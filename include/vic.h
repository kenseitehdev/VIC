#ifndef VIC_H
#define VIC_H
#define _POSIX_C_SOURCE 200809L
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <locale.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdbool.h>

#define MAX_BUFFERS   20
#define MAX_LINES     100000
#define MAX_LINE_LEN  2048
#define UNDO_MAX      5
#define REDO_MAX      5
#define CMDHIST_MAX   5
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} DynamicLine;
typedef enum {
    LANG_NONE = 0,
    LANG_C, LANG_CPP, LANG_PYTHON, LANG_JAVA, LANG_JS, LANG_TS,
    LANG_HTML, LANG_CSS, LANG_SHELL, LANG_MARKDOWN, LANG_MAN,
    LANG_RUST, LANG_GO, LANG_RUBY, LANG_PHP, LANG_SQL, LANG_JSON,
    LANG_XML, LANG_YAML
} Language;

typedef struct {
    int count;
    char **segments;
} WrappedLine;

typedef struct {
    DynamicLine *lines;   
    int line_count;
    int line_capacity;         
    char filepath[1024];
    Language lang;
    int scroll_offset;
    int is_active;
    int dirty;
    char **undo;             
    int undo_len;
    int undo_capacity;         
    char **redo;               
    int redo_len;
    int redo_capacity;         
} Buffer;
typedef enum {
    MODE_NORMAL = 0,
    MODE_INSERT,
    MODE_VISUAL,
    MODE_COMMAND
} InputMode;

typedef enum {
    OP_NONE = 0,
    OP_YANK,
    OP_DELETE
} Operator;

typedef struct {
    Buffer buffers[MAX_BUFFERS];
    int buffer_count;
    int current_buffer;
    int cursor_line;
    int cursor_col;
    int vis_start;
    int vis_end;
    char search_term[256];
    int search_match_count;
    int current_match;
    int search_highlight;
    int show_line_numbers;
    int wrap_enabled;
    InputMode mode;
    int g_pending;
    char cmdline[512];
    int cmdlen;
    char *cmdhist[CMDHIST_MAX];
    int cmdhist_len;
    int cmdhist_pos;
    char status_msg[256];
    int status_ticks;
    Operator op_pending;
    int op_start_line;
    int op_start_col;
    int free_scroll;
    char terminal_pane_id[128];
    int left_insert_mode;        
} ViewerState;
#define COLOR_NORMAL       1
#define COLOR_KEYWORD      2
#define COLOR_STRING       3
#define COLOR_COMMENT      4
#define COLOR_NUMBER       5
#define COLOR_LINENR       6
#define COLOR_STATUS       7
#define COLOR_COPY_SELECT  8
#define COLOR_SEARCH_HL    9
#define LINE_CSTR(b,i)  dline_cstr(&(b)->lines[(i)])
#define LINE_LEN(b,i)   ((int)(b)->lines[(i)].length)
static DynamicLine* dline_create(const char *str);
static void dline_free(DynamicLine *dl);
static int dline_ensure_capacity(DynamicLine *dl, size_t needed);
static int dline_insert_char(DynamicLine *dl, size_t pos, char c);
static int dline_delete_char(DynamicLine *dl, size_t pos);
static int dline_set(DynamicLine *dl, const char *str);
static const char* dline_cstr(const DynamicLine *dl);
static int buffer_ensure_capacity(Buffer *b, int needed);
static void cmd_show_help(ViewerState *st);
static Language detect_language(const char *filepath);

#endif