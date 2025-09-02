#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>

#define BUF_SIZE 16
#define da_append(da, item)                                                          \
    do {                                                                             \
        if ((da)->count >= (da)->capacity) {                                         \
            (da)->capacity = (da)->capacity == 0 ? BUF_SIZE : (da)->capacity*2;      \
            (da)->items = realloc((da)->items, (da)->capacity*sizeof(*(da)->items)); \
            assert((da)->items != NULL);                                             \
        }                                                                            \
                                                                                     \
        (da)->items[(da)->count++] = (item);                                         \
    } while (0)

#define SIZE 9
typedef int Board[SIZE * SIZE];

typedef struct {
    int from, to;
    int x, y;
} Op;

typedef struct {
    Op *items;
    int count;
    int capacity;
    int top;
} OpHistory;

#define BOARD_IDX(board, x, y) ((board)[(x) + (y)*SIZE])

struct termios save_termios;
int term_saved = 0;

Board board = { 0 };
Board locked = { 0 };
Board error = { 0 };
OpHistory op_hist;
int cursor_x = 0;
int cursor_y = 0;
int cheated;
int holes;

#define HL_ROW  0b001
#define HL_COL  0b010
#define HL_INT  0b100
int hl_mode = 0;
int highlight_x;
int highlight_y;
int highlight_val;

#define INDENT "  "
#define BOARD_INDENT "                  "
#define CSI "\033["

#define RESET CSI"0m"

#define ANSI_RED     CSI"31m"
#define ANSI_GREEN   CSI"32m"
#define ANSI_YELLOW  CSI"33m"
#define ANSI_CYAN    CSI"36m"
#define ANSI_GREY    CSI"90m"

#define ANSI_BOLD              CSI"1m"
#define ANSI_DIM               CSI"2m"
#define ANSI_ITALIC            CSI"3m"
#define ANSI_DOUBLE_UNDERLINE  CSI"21m"
#define ANSI_INVERT            CSI"7m"
#define KEY(k) ANSI_ITALIC ANSI_BOLD #k RESET

#define BAR_COLOUR  ANSI_GREY
#define ERR_COLOUR  ANSI_RED
#define VAL_COLOUR  ANSI_YELLOW
#define ROW_COLOUR  ANSI_CYAN

#define LOCKED_STYLE   ANSI_BOLD ANSI_DOUBLE_UNDERLINE
#define ACTIVE_STYLE   ANSI_INVERT
#define CHEATED_STYLE  ""
#define SUCCESS_STYLE  ANSI_GREEN

#define EMPTY_CELL ANSI_GREY"·"RESET

void push_op(op_hist, op)
    OpHistory *op_hist;
    Op op;
{
    if (op_hist->top < op_hist->count)
    {
        op_hist->count = op_hist->top;
    }
    da_append(op_hist, op);
    op_hist->top = op_hist->count;
}

int unpop_op(op_hist, op)
    OpHistory *op_hist;
    Op *op;
{
    if (op_hist->top == op_hist->count) return 0;
    *op = op_hist->items[op_hist->top++];
    return 1;
}

int pop_op(op_hist, op)
    OpHistory *op_hist;
    Op *op;
{
    if (op_hist->top == 0) return 0;
    *op = op_hist->items[--op_hist->top];
    return 1;
}

tty_raw(fd)
{
    struct termios buf;

    if (tcgetattr(fd, &save_termios) < 0) /* get the original state */
        return -1;

    buf = save_termios;

    /* echo off, canonical mode off, extended input processing off, signal chars off */
    buf.c_lflag &= ~(ECHO | ICANON);

    /* no SIGINT on BREAK, CR-toNL off, input parity check off, don't strip the 8th bit on input, ouput flow control off */
    buf.c_iflag &= ~(BRKINT | ICRNL | ISTRIP | IXON);

    /* clear size bits, parity checking off */
    buf.c_cflag &= ~(CSIZE | PARENB);

    /* set 8 bits/char */
    buf.c_cflag |= CS8;

    buf.c_cc[VMIN] = 1;  /* 1 byte at a time */
    buf.c_cc[VTIME] = 0; /* no timer on input */

    if (tcsetattr(fd, TCSAFLUSH, &buf) < 0)
        return -1;

    term_saved = 1;

    return 0;
}

tty_reset(fd)
{
    if (term_saved && tcsetattr(fd, TCSAFLUSH, &save_termios) < 0)
        return -1;
    return 0;
}

void print_highlight(x, y, val)
{
    if (BOARD_IDX(error, x, y))
    {
        printf(ERR_COLOUR);
    } else if ((hl_mode & HL_INT) && highlight_val == val)
    {
        printf(VAL_COLOUR);
    }
    else if (
        (hl_mode & HL_ROW) && highlight_y == y
        || (hl_mode & HL_COL) && highlight_x == x
    )
    {
        printf(ROW_COLOUR);
    }
}

void print_controls(void)
{

    printf("\n");
    printf(INDENT"Controls:\n");
    printf(INDENT"  "KEY(h)", "KEY(j)", "KEY(k)", "KEY(l)"      - Move cursor left, down, up, or right\n");
    printf(INDENT"  "KEY(1)", "KEY(2)", "KEY(3)", ..., "KEY(9)" - Set number in active cell\n");
    printf(INDENT"  "KEY(x)" - Delete number at current position\n");
    printf(INDENT"  "KEY(u)" - Undo last move\n");
    printf(INDENT"  "KEY(r)" - Redo last undone move\n");
    printf(INDENT"  "KEY(s)" - Automatically find the solution\n");
    printf(INDENT"  "KEY(z)" - Start a new game\n");
    printf(INDENT"  "KEY(q)" - Quit game\n");
    printf("\n");
    printf(INDENT"Key:\n");
    printf(INDENT"  "LOCKED_STYLE"1"RESET" - This cell is generated and can not be changed\n");
    printf(INDENT"  "  ERR_COLOUR"1"RESET" - This cell has an error\n");
}

void count_usages(board, counts)
    Board board;
    int counts[9];
{
    int i = 0;
    while (i < SIZE*SIZE)
    {
        if (board[i]) counts[board[i] - 1]++;
        i += 1;
    }
}

void print_usages(counts)
    int counts[9];
{
    int i = 0;
    printf(BOARD_INDENT "   ");
    while (i < 9)
    {
        if (counts[i] >= 9) {
            printf(ANSI_GREY"%d "RESET, i + 1);
        } else {
            printf("%d ", i + 1);
        }
        i += 1;
    }
    printf("\n");
}

void print_board(void)
{
    int x, y = 0;

    int counts[9] = { 0 };
    printf("\n");
    count_usages(board, counts);
    print_usages(counts);

    printf(BAR_COLOUR BOARD_INDENT "       ╷       ╷       " RESET "\n");
    while (y < SIZE)
    {
        printf(BOARD_INDENT" ");
        x = 0;
        while (x < SIZE)
        {
            if (x == cursor_x && y == cursor_y)
            {
                printf(ACTIVE_STYLE);
            }

            if (BOARD_IDX(locked, x, y))
            {
                printf(LOCKED_STYLE);
            }

            int val = BOARD_IDX(board, x, y);
            if (val == 0)
            {
                printf(EMPTY_CELL);
            }
            else
            {
                print_highlight(x, y, val);
                printf("%d", val);
            }

            printf(RESET " ");

            if (x % 3 == 2 && x != SIZE - 1)
            {
                printf(BAR_COLOUR"│ "RESET);
            }

            x += 1;
        }

        if (y % 3 == 2 && y != SIZE - 1) {
            printf(BAR_COLOUR"\n"BOARD_INDENT"───────┼───────┼───────" RESET);
        }

        printf("\n");
        y += 1;
    }
    printf(BAR_COLOUR BOARD_INDENT "       ╵       ╵       " RESET "\n");
}

row_count(board, y, val)
    Board board;
{
    int ret = 0;
    int x = 0;
    while (x < SIZE) if (BOARD_IDX(board, x++, y) == val) ret += 1;
    return ret;
}

col_count(board, x, val)
    Board board;
{
    int ret = 0;
    int y = 0;
    while (y < SIZE) if (BOARD_IDX(board, x, y++) == val) ret += 1;
    return ret;
}

sub_board_count(board, sx, sy, val)
    Board board;
{
    int ret = 0;
    int y = sy * 3;
    while (y < (sy*3 + 3)) {
        int x = sx * 3;
        while (x < (sx*3 + 3)) {
            if (BOARD_IDX(board, x, y) == val) ret += 1;
            x += 1;
        }
        y += 1;
    }
    return ret;
}

has_conflict(board, x, y, val)
    Board board;
{
    int i;

    /* check row */
    i = 0;
    while (i < SIZE)
    {
        if (x != i && BOARD_IDX(board, i, y) == val) return 1;
        i += 1;
    }

    /* check col */
    i = 0;
    while (i < SIZE)
    {
        if (y != i && BOARD_IDX(board, x, i) == val) return 1;
        i += 1;
    }

    /* check square */
    i = 0;
    while (i < SIZE)
    {
        int ix = (x/3)*3 + i % 3;
        int iy = (y/3)*3 + i / 3;
        if (ix != x && iy != y && BOARD_IDX(board, ix, iy) == val) return 1;
        i += 1;
    }

    return 0;
}

validate_board(error, board)
    Board *error, board;
{
    int c;
    int i = 0;
    int failed = 0;

    memset(error, 0, sizeof(*error));
    while (i < SIZE*SIZE)
    {
        int x = i % SIZE;
        int y = i / SIZE;

        if (board[i]) {
            c = row_count(board, y, board[i]);
            if (c > 1) {
                int x = 0;
                while (x < SIZE)
                    if (BOARD_IDX(board, x++, y) == board[i])
                        (*error)[i] = (failed |= 1);
            }

            c = col_count(board, x, board[i]);
            if (c > 1) {
                int y = 0;
                while (y < SIZE)
                    if (BOARD_IDX(board, x, y++) == board[i])
                        (*error)[i] = (failed |= 1);
            }

            c = sub_board_count(board, x / 3, y / 3, board[i]);
            if (c > 1) {
                int sx = x / 3;
                int sy = y / 3;
                int yi = sy * 3;
                while (yi < (sy + 1) * 3) {
                    int xi = sx * 3;
                    while (xi < (sx + 1) * 3) {
                        if (BOARD_IDX(board, xi, yi) == board[i])
                            (*error)[i] = (failed |= 1);
                        xi += 1;
                    }
                    yi += 1;
                }
            }
        }

        ++i;
    }
    return failed;
}

is_board_full(board)
    Board board;
{
    int i = 0;
    while (i < SIZE*SIZE)
    {
        if (!board[i++]) return 0;
    }
    return 1;
}

void into_locked_board(locked, b)
    Board locked, b;
{
    int i = 0;
    while (i < SIZE*SIZE)
    {
        locked[i] = !!b[i];
        ++i;
    }
}

void shuffle(items, count)
    int *items, count;
{
    int n = 0;
    while (n < count)
    {
        int i = rand()%count;
        int j = rand()%count;
        int t = items[i];
        items[i] = items[j];
        items[j] = t;
        n += 1;
    }
}

fill_remaining(board, x, y)
    Board board;
{
    if (y == SIZE) return 1;
    if (x == SIZE) return fill_remaining(board, 0, y + 1);
    if (BOARD_IDX(board, x, y)) return fill_remaining(board, x + 1, y);

    int n = 1;
    while (n <= 9)
    {
        if (!has_conflict(board, x, y, n)) {
            BOARD_IDX(board, x, y) = n;
            if (fill_remaining(board, x + 1, y)) {
                return 1;
            }
            BOARD_IDX(board, x, y) = 0; 
        }
        n += 1;
    }
    return 0;
}

void poke_holes(board, n)
    Board board;
{
    while (n)
    {
        int i = rand() % (SIZE*SIZE);
        if (board[i])
        {
            board[i] = 0;
            n -= 1;
        }
    }
}

void randomise_square(board, sx, sy)
    Board board;
    int sx, sy;
{
    int y = 0;
    while (y < 3)
    {
        int x = 0;
        while (x < 3)
        {
            BOARD_IDX(board, sx*3 + x, sy*3 + y) = y*3 + x + 1;
            x += 1;
        }
        y += 1;
    }

    int n = 0;
    while (n < SIZE)
    {
        int i = rand() % SIZE;
        int j = rand() % SIZE;

        int ix = sx*3 + (i % 3);
        int iy = sy*3 + (i / 3);

        int jx = sx*3 + (j % 3);
        int jy = sy*3 + (j / 3);

        int t = BOARD_IDX(board, ix, iy);
        BOARD_IDX(board, ix, iy) = BOARD_IDX(board, jx, jy);
        BOARD_IDX(board, jx, jy) = t;

        n += 1;
    }
}

void gen_board(board)
    Board board;
{
    randomise_square(board, 0, 0);
    randomise_square(board, 1, 1);
    randomise_square(board, 2, 2);

    fill_remaining(board, 0, 0);
}

void reset(void)
{
    memset(board, 0, sizeof(Board));
    memset(error, 0, sizeof(Board));
    memset(locked, 0, sizeof(Board));

    hl_mode = HL_ROW | HL_COL | HL_INT;
    highlight_val = 0;
    highlight_x = cursor_x = 0;
    highlight_y = cursor_y = 0;


    gen_board(board);
    poke_holes(board, holes);
    into_locked_board(locked, board);
    op_hist.count = op_hist.top = 0;
}

void print_usage(program)
    const char *program;
{
    fprintf(stderr, ERR_COLOUR"Usage: %s [easy|medium|hard]\n", program);
}

int handle_args(argc, argv)
    int argc;
    char **argv;
{
    if (argc == 1)
    {
        holes = 50;
        return 1;
    }

    if (argc != 2) return 0;
    
    if (!strcasecmp(argv[1], "easy"))
    {
        holes = 40;
    }
    else if (!strcasecmp(argv[1], "medium"))
    {
        holes = 50;
    }
    else if (!strcasecmp(argv[1], "hard"))
    {
        holes = 60;
    }
    else
    {
        return 0;
    }
    return 1;
}

main(argc, argv)
    int argc;
    char **argv;
{
    char c;

    if (!handle_args(argc, argv)) {
        print_usage(argv[0]);
        return 1;
    }

    srand(time(0) + getpid());
    reset();

    if (tty_raw(STDIN_FILENO))
    {
        fprintf(stderr, "Unable to set terminal raw mode.\n");
        return 1;
    }

    print_controls();
    print_board();
    while (1) {
        printf(
            CSI"?25l" /* hide cursor */
            CSI"15A"  /* move up 15 lines */
            CSI"0J"   /* clear lines below cursor */
        );
        if (!read(STDIN_FILENO, &c, 1)) {
            fprintf(stderr, "Failed to read input\n");
            break;
        }
        switch (c)
        {
            case 'k': {
                if (cursor_y > 0) cursor_y -= 1;
            }; break;
            case 'j': {
                if (cursor_y < SIZE - 1) cursor_y += 1;
            }; break;
            case 'h': {
                if (cursor_x > 0) cursor_x -= 1;
            }; break;
            case 'l': {
                if (cursor_x < SIZE - 1) cursor_x += 1;
            }; break;

            case 'z': {
                reset();
            }; break;

            case 's': {
                cheated = 1;
                if (!fill_remaining(board, 0, 0)) {
                    fprintf(stderr, ERR_COLOUR"Failed to find solution.\n");
                }
            }; break;
            case 'q': {
                print_board();
                goto done;
            }; break;
            case 'x': {
                if (!BOARD_IDX(locked, cursor_x, cursor_y))
                {
                    Op op = {
                        .from = BOARD_IDX(board, cursor_x, cursor_y),
                        .to = 0,
                        .x = cursor_x,
                        .y = cursor_y,
                    };
                    push_op(&op_hist, op);
                    BOARD_IDX(board, cursor_x, cursor_y) = 0;
                }
            }; break;
            case 'r': {
                Op undo;
                if (unpop_op(&op_hist, &undo))
                    BOARD_IDX(board, undo.x, undo.y) = undo.to;
            }; break;
            case 'u': {
                Op undo;
                if (pop_op(&op_hist, &undo))
                    BOARD_IDX(board, undo.x, undo.y) = undo.from;
            }; break;

            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9': {
                int n = c - '0';
                if (!BOARD_IDX(locked, cursor_x, cursor_y))
                {
                    Op op = {
                        .from = BOARD_IDX(board, cursor_x, cursor_y),
                        .to = n,
                        .x = cursor_x,
                        .y = cursor_y,
                    };
                    push_op(&op_hist, op);
                    BOARD_IDX(board, cursor_x, cursor_y) = n;
                }
            }; break;
        }
        highlight_val = BOARD_IDX(board, cursor_x, cursor_y);
        highlight_x = cursor_x;
        highlight_y = cursor_y;
        int has_error = validate_board(&error, board);
        int is_full = is_board_full(board);
        if (!has_error && is_full) {
            if (!cheated)
            {
                printf(SUCCESS_STYLE"Congratulations, you solved it!\n"RESET);
            }
            print_board();
            printf(SUCCESS_STYLE"Press "KEY(q) SUCCESS_STYLE" to quit, or any key to play again!\n"RESET);
            read(STDIN_FILENO, &c, 1);
            if (c == 'q') break;
            reset();
            print_board();
        } else {
            print_board();
        }
    }

done:
    if (tty_reset(STDIN_FILENO))
    {
        fprintf(stderr, "Unable to restore terminal mode.\n");
        return 1;
    }
    return 0;
}
