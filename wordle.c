#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define WORD_LEN     5
#define MAX_GUESSES  6
#define FRAME_W      41

#define RESET       "\033[0m"
#define BOLD        "\033[1m"
#define EOL         "\033[K\n"

#define TILE_GREEN  "\033[48;5;28m\033[1;97m"
#define TILE_YELLOW "\033[48;5;178m\033[1;30m"
#define TILE_GRAY   "\033[48;5;239m\033[1;97m"
#define KEY_UNUSED  "\033[48;5;252m\033[1;30m"
#define KEY_ELIM    "\033[48;5;236m\033[38;5;242m"

#define FG_GREEN    "\033[1;38;5;40m"
#define FG_YELLOW   "\033[1;38;5;220m"
#define FG_RED      "\033[1;38;5;203m"
#define FG_DIM      "\033[38;5;240m"
#define FG_MID      "\033[38;5;245m"
#define FG_LIGHT    "\033[38;5;252m"
#define FG_WHITE    "\033[1;97m"

static const char *WORDS[] = {
    "CRANE","SLATE","AUDIO","EARTH","MAGIC","BLADE","STORM","LIGHT","POWER","CLOUD",
    "FLAME","STONE","GRACE","PRIZE","TRUST","BLEND","CRISP","FLUTE","GLOOM","CHUNK",
    "BRISK","SCALP","WRATH","SPOKE","GRILL","TROUT","QUILT","PLANK","SWAMP","KNELT",
    "DWARF","YACHT","CHOIR","PRISM","BLAZE","GHOST","CRAVE","FROST","SHINE","BRAVE",
    "DRIFT","GROAN","SPILL","CROWN","FEAST","CLIMB","BRUTE","GLINT","WALTZ","PIXEL",
    "APPLE","BEACH","BRICK","CHAIR","DREAM","EAGLE","FIELD","GIANT","HEART","IVORY",
    "JELLY","KNIFE","LEMON","MANGO","NIGHT","OCEAN","PIANO","QUEEN","RIVER","SUGAR",
    "TIGER","UNCLE","VIVID","WATER","YOUTH","ZEBRA","PLANT","SMILE","TRAIN","WORLD",
    "HOUSE","MUSIC","DANCE","SPACE","FLOOR","BREAD","CLOCK","DRINK","FAIRY","GRAPE",
    "HORSE","JOKER","LUCKY","MAPLE","NOBLE","OLIVE","PEARL","QUIET","ROBIN","SHEEP",
    "THUMB","UNITY","VOICE","WHEAT","YEARN"
};
#define WORD_COUNT ((int)(sizeof WORDS / sizeof WORDS[0]))

static char (*g_dict)[WORD_LEN + 1];
static size_t g_dict_n;

typedef enum { UNKNOWN, CORRECT, PRESENT, ABSENT } LetterState;

typedef struct {
    char        letters[WORD_LEN];
    LetterState states[WORD_LEN];
} Guess;

typedef struct {
    char        target[WORD_LEN + 1];
    Guess       guesses[MAX_GUESSES];
    int         num_guesses;
    int         solved;
    LetterState keyboard[26];
} Game;

typedef struct {
    int played, wins, streak, best;
    int dist[MAX_GUESSES];
} Stats;

enum { K_NONE = 0, K_ENTER = -1, K_BACK = -2, K_QUIT = -3 };
enum { T_EMPTY, T_TYPED, T_FILLED };

static struct termios g_orig, g_raw;
static int g_active;
static int g_pad;

static void term_restore(void)
{
    if (!g_active) return;
    g_active = 0;
    static const char s[] = "\033[0m\033[?25h\033[?1049l";
    ssize_t r = write(STDOUT_FILENO, s, sizeof s - 1);
    (void)r;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
}

static void on_signal(int sig)
{
    term_restore();
    _exit(128 + sig);
}

static int term_init(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &g_orig) < 0) return -1;

    g_raw = g_orig;
    g_raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    g_raw.c_cc[VMIN]  = 1;
    g_raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_raw) < 0) return -1;

    g_active = 1;
    atexit(term_restore);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);

    printf("\033[?1049h\033[?25l\033[2J");
    fflush(stdout);
    return 0;
}

static int read_key(void)
{
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return K_QUIT;

    if (c == 27) {
        struct termios t = g_raw;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        unsigned char b;
        while (read(STDIN_FILENO, &b, 1) == 1) { }
        tcsetattr(STDIN_FILENO, TCSANOW, &g_raw);
        return K_NONE;
    }
    if (c == '\n' || c == '\r')  return K_ENTER;
    if (c == 127  || c == 8)     return K_BACK;
    if (c == 4)                  return K_QUIT;
    if (isalpha(c))              return toupper(c);
    return K_NONE;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int cmp_word(const void *a, const void *b) { return strcmp(a, b); }

static void load_dict(void)
{
    const char *paths[] = { "/usr/share/dict/words", "/usr/share/dict/american-english",
                            "/usr/share/dict/british-english", NULL };
    for (int p = 0; paths[p]; p++) {
        FILE *f = fopen(paths[p], "r");
        if (!f) continue;

        size_t cap = 0;
        char line[128];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strlen(line) != WORD_LEN) continue;
            int ok = 1;
            for (int i = 0; i < WORD_LEN; i++)
                if (!islower((unsigned char)line[i])) { ok = 0; break; }
            if (!ok) continue;

            if (g_dict_n == cap) {
                cap = cap ? cap * 2 : 4096;
                char (*tmp)[WORD_LEN + 1] = realloc(g_dict, cap * sizeof *g_dict);
                if (!tmp) break;
                g_dict = tmp;
            }
            for (int i = 0; i <= WORD_LEN; i++)
                g_dict[g_dict_n][i] = (char)toupper((unsigned char)line[i]);
            g_dict_n++;
        }
        fclose(f);
        break;
    }
    if (g_dict_n < 1000) { free(g_dict); g_dict = NULL; g_dict_n = 0; }
    else qsort(g_dict, g_dict_n, sizeof *g_dict, cmp_word);
}

static int valid_word(const char *w)
{
    for (int i = 0; i < WORD_COUNT; i++)
        if (strcmp(w, WORDS[i]) == 0) return 1;
    if (g_dict_n == 0) return 1;
    return bsearch(w, g_dict, g_dict_n, sizeof *g_dict, cmp_word) != NULL;
}

static void evaluate_guess(const char *guess, const char *target, LetterState *states)
{
    char pool[WORD_LEN];
    memcpy(pool, target, WORD_LEN);

    for (int i = 0; i < WORD_LEN; i++) {
        states[i] = ABSENT;
        if (guess[i] == target[i]) {
            states[i] = CORRECT;
            pool[i] = '.';
        }
    }
    for (int i = 0; i < WORD_LEN; i++) {
        if (states[i] == CORRECT) continue;
        for (int j = 0; j < WORD_LEN; j++) {
            if (pool[j] == guess[i]) {
                states[i] = PRESENT;
                pool[j] = '.';
                break;
            }
        }
    }
}

static void update_keyboard(Game *g, const Guess *guess)
{
    static const int priority[] = { 0, 3, 2, 1 };
    for (int i = 0; i < WORD_LEN; i++) {
        int idx = guess->letters[i] - 'A';
        if (priority[guess->states[i]] > priority[g->keyboard[idx]])
            g->keyboard[idx] = guess->states[i];
    }
}

static void init_game(Game *g)
{
    static int used[sizeof WORDS / sizeof WORDS[0]];
    int left = 0;
    for (int i = 0; i < WORD_COUNT; i++) if (!used[i]) left++;
    if (left == 0) { memset(used, 0, sizeof used); }

    int idx;
    do { idx = rand() % WORD_COUNT; } while (used[idx]);
    used[idx] = 1;

    memset(g, 0, sizeof *g);
    strcpy(g->target, WORDS[idx]);
}

static void ind(int extra) { printf("%*s", g_pad + extra, ""); }

static void tile_line(int kind, LetterState st, char ch, int line, int tall)
{
    if (kind == T_FILLED) {
        const char *style = st == CORRECT ? TILE_GREEN
                          : st == PRESENT ? TILE_YELLOW : TILE_GRAY;
        if (line == 1) printf("%s  %c  " RESET, style, ch);
        else           printf("%s     " RESET, style);
        return;
    }
    const char *col = kind == T_TYPED ? FG_LIGHT : FG_DIM;
    const char *ls  = kind == T_TYPED ? FG_WHITE : "";
    if (tall) {
        if      (line == 0) printf("%s┌───┐" RESET, col);
        else if (line == 2) printf("%s└───┘" RESET, col);
        else                printf("%s│%s %c %s│" RESET, col, ls, ch, col);
    } else {
        printf("%s[%s %c %s]" RESET, col, ls, ch, col);
    }
}

static void draw_keyboard(const Game *g)
{
    static const char *rows[] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    static const int   off[]  = { 0, 2, 6 };
    for (int r = 0; r < 3; r++) {
        ind(1 + off[r]);
        for (int i = 0; rows[r][i]; i++) {
            LetterState s = g->keyboard[rows[r][i] - 'A'];
            const char *style = s == CORRECT ? TILE_GREEN
                              : s == PRESENT ? TILE_YELLOW
                              : s == ABSENT  ? KEY_ELIM : KEY_UNUSED;
            printf("%s %c " RESET " ", style, rows[r][i]);
        }
        printf(EOL);
    }
}

static void draw_stats(const Game *g, const Stats *st, int rows)
{
    ind(1);
    printf(BOLD "Played %d   Won %d%%   Streak %d   Best %d" RESET EOL,
           st->played, st->played ? st->wins * 100 / st->played : 0,
           st->streak, st->best);

    if (rows < 28) return;

    static const char FULL[] = "████████████████████";
    int max = 1;
    for (int i = 0; i < MAX_GUESSES; i++) if (st->dist[i] > max) max = st->dist[i];

    for (int i = 0; i < MAX_GUESSES; i++) {
        int n = st->dist[i] * 20 / max;
        if (st->dist[i] > 0 && n == 0) n = 1;
        int hl = g->solved && i == g->num_guesses - 1;
        ind(1);
        printf(FG_MID "%d " RESET "%s%.*s" RESET " %d" EOL,
               i + 1, hl ? FG_GREEN : FG_MID, n * 3, FULL, st->dist[i]);
    }
}

static void draw(const Game *g, const char *input, int len,
                 const Guess *pend, int reveal,
                 const char *msg, const char *footer, const Stats *stats)
{
    struct winsize ws;
    int cols = 80, rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    g_pad = cols > FRAME_W ? (cols - FRAME_W) / 2 : 0;
    int tall = rows >= 36;

    printf("\033[H");

    ind(11); printf(FG_LIGHT "╔═════════════════╗" RESET EOL);
    ind(11); printf(FG_LIGHT "║   " FG_GREEN "W " FG_YELLOW "O " FG_GREEN "R "
                    FG_MID "D " FG_YELLOW "L " FG_GREEN "E" FG_LIGHT "   ║" RESET EOL);
    ind(11); printf(FG_LIGHT "╚═════════════════╝" RESET EOL);

    ind(2);
    printf(TILE_GREEN "  " RESET FG_LIGHT " correct   " RESET
           TILE_YELLOW "  " RESET FG_LIGHT " misplaced  " RESET
           TILE_GRAY "  " RESET FG_LIGHT " absent" RESET EOL);
    printf(EOL);

    for (int r = 0; r < MAX_GUESSES; r++) {
        for (int line = tall ? 0 : 1; line <= (tall ? 2 : 1); line++) {
            ind(6);
            for (int c = 0; c < WORD_LEN; c++) {
                int kind = T_EMPTY;
                LetterState st = UNKNOWN;
                char ch = ' ';

                if (r < g->num_guesses) {
                    kind = T_FILLED;
                    st = g->guesses[r].states[c];
                    ch = g->guesses[r].letters[c];
                } else if (r == g->num_guesses) {
                    if (pend) {
                        ch = pend->letters[c];
                        if (c < reveal) { kind = T_FILLED; st = pend->states[c]; }
                        else            { kind = T_TYPED; }
                    } else if (c < len) {
                        kind = T_TYPED;
                        ch = input[c];
                    }
                }
                tile_line(kind, st, ch, line, tall);
                if (c < WORD_LEN - 1) printf(" ");
            }
            printf(EOL);
        }
        if (r < MAX_GUESSES - 1) printf(EOL);
    }

    ind(6);
    printf("%s" EOL, msg);
    printf(EOL);

    if (stats) draw_stats(g, stats, rows);
    else       draw_keyboard(g);

    printf(EOL);
    ind(1);
    printf(FG_DIM "%s" RESET EOL, footer);

    printf("\033[J");
    fflush(stdout);
}

#define FOOT_PLAY "Enter submit · Backspace delete · Ctrl-C quit"
#define FOOT_END  "Enter: play again · Q: quit"

static int play(Stats *st)
{
    static const char *praise[] = {
        "Genius!", "Magnificent!", "Impressive!", "Splendid!", "Great!", "Phew!"
    };

    Game g;
    init_game(&g);

    char input[WORD_LEN + 1] = "";
    int  len = 0;
    char msg[128] = "";

    printf("\033[2J");

    while (!g.solved && g.num_guesses < MAX_GUESSES) {
        draw(&g, input, len, NULL, 0, msg, FOOT_PLAY, NULL);
        msg[0] = '\0';

        int k = read_key();
        if (k == K_QUIT) return 0;

        if (k >= 'A' && k <= 'Z') {
            if (len < WORD_LEN) { input[len++] = (char)k; input[len] = '\0'; }
        } else if (k == K_BACK) {
            if (len > 0) input[--len] = '\0';
        } else if (k == K_ENTER) {
            if (len < WORD_LEN) {
                snprintf(msg, sizeof msg, FG_RED "Not enough letters" RESET);
            } else if (!valid_word(input)) {
                snprintf(msg, sizeof msg, FG_RED "Not in word list" RESET);
            } else {
                Guess pend;
                memcpy(pend.letters, input, WORD_LEN);
                evaluate_guess(input, g.target, pend.states);

                for (int r = 1; r <= WORD_LEN; r++) {
                    draw(&g, input, len, &pend, r, "", FOOT_PLAY, NULL);
                    sleep_ms(280);
                }
                tcflush(STDIN_FILENO, TCIFLUSH);

                g.guesses[g.num_guesses] = pend;
                update_keyboard(&g, &pend);
                g.num_guesses++;

                g.solved = 1;
                for (int i = 0; i < WORD_LEN; i++)
                    if (pend.states[i] != CORRECT) g.solved = 0;

                len = 0;
                input[0] = '\0';
            }
        }
    }

    st->played++;
    if (g.solved) {
        st->wins++;
        st->streak++;
        if (st->streak > st->best) st->best = st->streak;
        st->dist[g.num_guesses - 1]++;
        snprintf(msg, sizeof msg, FG_GREEN "%s" RESET " (%d/%d)",
                 praise[g.num_guesses - 1], g.num_guesses, MAX_GUESSES);
    } else {
        st->streak = 0;
        snprintf(msg, sizeof msg, "The word was " BOLD "%s" RESET, g.target);
    }

    draw(&g, "", 0, NULL, 0, msg, FOOT_END, st);

    for (;;) {
        int k = read_key();
        if (k == K_ENTER) return 1;
        if (k == K_QUIT || k == 'Q') return 0;
    }
}

int main(void)
{
    if (term_init() < 0) {
        fprintf(stderr, "wordle: needs an interactive terminal.\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    load_dict();

    Stats stats = {0};
    while (play(&stats)) { }

    term_restore();
    printf("Thanks for playing!\n");
    free(g_dict);
    return 0;
}
