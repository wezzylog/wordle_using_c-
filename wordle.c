/*
 * Wordle in C - Terminal version
 * Compile: gcc -o wordle wordle.c
 * Run:     ./wordle
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define WORD_LEN     5
#define MAX_GUESSES  6
#define WORD_COUNT   50

/* ANSI color codes */
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define GREEN   "\033[42m\033[30m"
#define YELLOW  "\033[43m\033[30m"
#define GRAY    "\033[100m\033[37m"
#define WHITE   "\033[107m\033[30m"

/* ── Word list ─────────────────────────────────────────────────────────── */
const char *WORDS[WORD_COUNT] = {
    "CRANE", "SLATE", "AUDIO", "EARTH", "MAGIC",
    "BLADE", "STORM", "LIGHT", "POWER", "CLOUD",
    "FLAME", "STONE", "GRACE", "PRIZE", "TRUST",
    "BLEND", "CRISP", "FLUTE", "GLOOM", "CHUNK",
    "BRISK", "SCALP", "WRATH", "SPOKE", "GRILL",
    "TROUT", "QUILT", "PLANK", "SWAMP", "KNELT",
    "DWARF", "YACHT", "CHOIR", "PRISM", "BLAZE",
    "GHOST", "CRAVE", "FROST", "SHINE", "BRAVE",
    "DRIFT", "GROAN", "SPILL", "CROWN", "FEAST",
    "CLIMB", "BRUTE", "GLINT", "WALTZ", "PIXEL"
};

/* ── Data structures ───────────────────────────────────────────────────── */
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
    LetterState keyboard[26];   /* A-Z state */
} Game;

/* ── Helpers ────────────────────────────────────────────────────────────── */
void to_upper(char *s) {
    for (; *s; s++) *s = toupper((unsigned char)*s);
}

int is_alpha_word(const char *s) {
    for (int i = 0; i < WORD_LEN; i++)
        if (!isalpha((unsigned char)s[i])) return 0;
    return s[WORD_LEN] == '\0';
}

int word_in_list(const char *word) {
    for (int i = 0; i < WORD_COUNT; i++)
        if (strcmp(word, WORDS[i]) == 0) return 1;
    return 0;
}

/* ── Evaluation ─────────────────────────────────────────────────────────── */
void evaluate_guess(const char *guess, const char *target, LetterState *states) {
    char pool[WORD_LEN + 1];
    strcpy(pool, target);

    /* First pass: correct positions */
    for (int i = 0; i < WORD_LEN; i++) {
        states[i] = ABSENT;
        if (guess[i] == target[i]) {
            states[i] = CORRECT;
            pool[i] = '.';
        }
    }

    /* Second pass: present but wrong position */
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

/* ── Keyboard state update ──────────────────────────────────────────────── */
void update_keyboard(Game *g, const Guess *guess) {
    /* Priority: CORRECT > PRESENT > ABSENT > UNKNOWN */
    int priority[] = {0, 3, 2, 1};   /* indexed by LetterState */

    for (int i = 0; i < WORD_LEN; i++) {
        int idx = toupper((unsigned char)guess->letters[i]) - 'A';
        LetterState cur = g->keyboard[idx];
        LetterState new = guess->states[i];
        if (priority[new] > priority[cur])
            g->keyboard[idx] = new;
    }
}

/* ── Display ────────────────────────────────────────────────────────────── */
void print_colored_cell(char letter, LetterState state) {
    switch (state) {
        case CORRECT: printf(GREEN " %c " RESET, letter); break;
        case PRESENT: printf(YELLOW " %c " RESET, letter); break;
        case ABSENT:  printf(GRAY   " %c " RESET, letter); break;
        default:      printf(WHITE  " %c " RESET, letter); break;
    }
}

void print_board(const Game *g) {
    printf("\n");
    for (int r = 0; r < MAX_GUESSES; r++) {
        printf("  ");
        if (r < g->num_guesses) {
            for (int c = 0; c < WORD_LEN; c++)
                print_colored_cell(g->guesses[r].letters[c], g->guesses[r].states[c]);
        } else {
            for (int c = 0; c < WORD_LEN; c++)
                printf(WHITE "   " RESET);
        }
        printf("\n\n");
    }
}

void print_keyboard(const Game *g) {
    const char *rows[] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    printf("  Keyboard:\n");
    for (int r = 0; r < 3; r++) {
        printf("  ");
        for (int i = 0; rows[r][i]; i++) {
            int idx = rows[r][i] - 'A';
            print_colored_cell(rows[r][i], g->keyboard[idx]);
        }
        printf("\n");
    }
    printf("\n");
}

void clear_screen(void) {
    printf("\033[H\033[2J");
}

void print_header(void) {
    printf(BOLD "\n  ╔══════════════╗\n");
    printf(      "  ║  W O R D L E  ║\n");
    printf(      "  ╚══════════════╝\n" RESET);
    printf("  Guess the 5-letter word in 6 tries.\n");
    printf("  " GREEN " C " RESET " Correct   "
               YELLOW " P " RESET " Present   "
               GRAY   " A " RESET " Absent\n\n");
}

/* ── Game logic ─────────────────────────────────────────────────────────── */
void init_game(Game *g) {
    srand((unsigned)time(NULL));
    strncpy(g->target, WORDS[rand() % WORD_COUNT], WORD_LEN + 1);
    g->num_guesses = 0;
    g->solved = 0;
    memset(g->guesses, 0, sizeof(g->guesses));
    memset(g->keyboard, 0, sizeof(g->keyboard));
}

int process_guess(Game *g, const char *input) {
    if (!is_alpha_word(input)) {
        printf("  ✗ Please enter a 5-letter word.\n\n");
        return 0;
    }
    /* Uncomment the next block to enforce dictionary check: */
    /*
    if (!word_in_list(input)) {
        printf("  ✗ Not in word list.\n\n");
        return 0;
    }
    */

    Guess *guess = &g->guesses[g->num_guesses];
    memcpy(guess->letters, input, WORD_LEN);
    evaluate_guess(input, g->target, guess->states);
    update_keyboard(g, guess);
    g->num_guesses++;

    if (memcmp(guess->states,
               (LetterState[]){CORRECT,CORRECT,CORRECT,CORRECT,CORRECT},
               WORD_LEN * sizeof(LetterState)) == 0) {
        g->solved = 1;
    }
    return 1;
}

void play(void) {
    Game g;
    init_game(&g);

    char buf[64];

    while (!g.solved && g.num_guesses < MAX_GUESSES) {
        clear_screen();
        print_header();
        print_board(&g);
        print_keyboard(&g);

        printf("  Guess %d/%d: ", g.num_guesses + 1, MAX_GUESSES);
        if (!fgets(buf, sizeof(buf), stdin)) break;

        /* Strip newline */
        buf[strcspn(buf, "\r\n")] = '\0';
        to_upper(buf);

        process_guess(&g, buf);
    }

    /* Final screen */
    clear_screen();
    print_header();
    print_board(&g);

    if (g.solved) {
        const char *msgs[] = {
            "Genius!", "Magnificent!", "Impressive!",
            "Splendid!", "Great!", "Phew!"
        };
        printf("  🎉 %s  (%d/%d)\n\n", msgs[g.num_guesses - 1], g.num_guesses, MAX_GUESSES);
    } else {
        printf("  The word was: " BOLD "%s" RESET "\n\n", g.target);
    }
}

/* ── Main ───────────────────────────────────────────────────────────────── */
int main(void) {
    char again[8];
    do {
        play();
        printf("  Play again? (y/n): ");
        if (!fgets(again, sizeof(again), stdin)) break;
    } while (tolower((unsigned char)again[0]) == 'y');

    printf("  Thanks for playing!\n\n");
    return 0;
}
