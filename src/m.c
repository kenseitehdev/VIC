// vic

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
// [CHANGE 1] MAX_LINES now controls the initial capacity of the dynamic line
// pointer array.  The array grows as needed; see buf_ensure_capacity().
#define INITIAL_LINE_CAP 1024
#define MAX_LINE_LEN  2048

#define UNDO_MAX      25
#define REDO_MAX      25
#define CMDHIST_MAX   25

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
    // [CHANGE 1] lines is now a dynamically-sized heap array; line_cap tracks
    // the allocated capacity so we can realloc when needed.
    char **lines;
    int line_count;
    int line_cap;           // allocated slots in lines[]

    char filepath[1024];    // empty => [No Name]
    Language lang;
    int scroll_offset;      // top logical line
    int is_active;
    int dirty;

    char *undo[UNDO_MAX];
    int undo_len;

    char *redo[REDO_MAX];
    int redo_len;
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

    // --- needed by your insert-mode undo coalescing ---
    int insert_undo_armed;

    // [CHANGE 4/5] track whether a '%' was typed as a count prefix in normal
    // mode (i.e. :%y / :%d meaning "all lines").
    int percent_pending;
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

static void cmd_show_help(ViewerState *st);
static Language detect_language(const char *filepath);
static void set_status(ViewerState *st, const char *msg) {
    if (!st) return;
    snprintf(st->status_msg, sizeof(st->status_msg), "%s", msg ? msg : "");
    st->status_ticks = 80;
}

static const char *basename_path(const char *p) {
    if (!p || !*p) return "[No Name]";
    const char *s = strrchr(p, '/');
    return s ? (s + 1) : p;
}

static int file_exists(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0;
}

static void strip_overstrikes(char *s) {
    char *dst = s;
    for (char *src = s; *src; src++) {
        if (*src == '\b') {
            if (dst > s) dst--;
        } else {
            *dst++ = *src;
        }
    }
    *dst = '\0';
}

static int check_command_exists(const char *cmd) {
    char test[256];
    snprintf(test, sizeof(test), "command -v %s >/dev/null 2>&1", cmd);
    return system(test) == 0;
}

// --- tiny helper to trim newline(s) ---
static void trim_newlines(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

static void manual_buffer_list_fallback(ViewerState *st) {
    def_prog_mode();
    endwin();

    printf("\n=== Buffer List ===\n\n");
    for (int i = 0; i < st->buffer_count; i++) {
        Buffer *b = &st->buffers[i];
        const char *indicator = (i == st->current_buffer) ? "*" : " ";
        const char *modified  = b->dirty ? " [+]" : "";
        const char *name      = basename_path(b->filepath);

        printf("%s %2d: %s%s (%d lines)\n",
               indicator, i + 1, name, modified, b->line_count);
    }

    printf("\nPress any key to continue...");
    fflush(stdout);

    struct termios old_tio, new_tio;
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);

    reset_prog_mode();
    refresh();
}
static void get_cwd(char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (!getcwd(out, out_len)) {
        snprintf(out, out_len, ".");
    }
}
static void strip_ansi(char *s) {
    char *d = s;
    for (char *p = s; *p; ) {
        if ((unsigned char)*p == 0x1B) {
            p++;
            if (*p == '[') {
                p++;
                while (*p && !(*p >= '@' && *p <= '~')) p++;
                if (*p) p++;
                continue;
            } else if (*p == ']') {
                p++;
                while (*p && (unsigned char)*p != 0x07) p++;
                if (*p) p++;
                continue;
            } else {
                if (*p) p++;
                continue;
            }
        }
        *d++ = *p++;
    }
    *d = '\0';
}

static void rtrim(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
}

static Language detect_language(const char *filepath) {
    if (!filepath) return LANG_NONE;
    const char *ext = strrchr(filepath, '.');
    if (!ext) return LANG_NONE;
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0) return LANG_C;
    if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 || strcmp(ext, ".hpp") == 0 || strcmp(ext, ".cxx") == 0) return LANG_CPP;
    if (strcmp(ext, ".py") == 0) return LANG_PYTHON;
    if (strcmp(ext, ".java") == 0) return LANG_JAVA;
    if (strcmp(ext, ".js") == 0) return LANG_JS;
    if (strcmp(ext, ".ts") == 0 || strcmp(ext, ".tsx") == 0) return LANG_TS;
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return LANG_HTML;
    if (strcmp(ext, ".css") == 0) return LANG_CSS;
    if (strcmp(ext, ".sh") == 0 || strcmp(ext, ".bash") == 0 || strcmp(ext, ".zsh") == 0) return LANG_SHELL;
    if (strcmp(ext, ".md") == 0 || strcmp(ext, ".markdown") == 0) return LANG_MARKDOWN;
    if (strcmp(ext, ".rs") == 0) return LANG_RUST;
    if (strcmp(ext, ".go") == 0) return LANG_GO;
    if (strcmp(ext, ".rb") == 0) return LANG_RUBY;
    if (strcmp(ext, ".php") == 0) return LANG_PHP;
    if (strcmp(ext, ".sql") == 0) return LANG_SQL;
    if (strcmp(ext, ".json") == 0) return LANG_JSON;
    if (strcmp(ext, ".xml") == 0) return LANG_XML;
    if (strcmp(ext, ".yaml") == 0 || strcmp(ext, ".yml") == 0) return LANG_YAML;
    if (strstr(filepath, "/man/") || strstr(filepath, ".man")) return LANG_MAN;
    return LANG_NONE;
}

static WrappedLine wrap_line(const char *line, int width) {
    WrappedLine result = (WrappedLine){0, NULL};
    if (!line || width <= 0) return result;

    int len = (int)strlen(line);
    if (len == 0) {
        result.segments = (char**)malloc(sizeof(char*));
        result.segments[0] = strdup("");
        result.count = 1;
        return result;
    }

    int max_segments = (len / width) + 2;
    result.segments = (char**)malloc((size_t)max_segments * sizeof(char*));

    int pos = 0;
    while (pos < len) {
        int remaining = len - pos;
        int take = (remaining > width) ? width : remaining;

        char *seg = (char*)malloc((size_t)take + 1);
        memcpy(seg, line + pos, (size_t)take);
        seg[take] = '\0';
        result.segments[result.count++] = seg;
        pos += take;
    }
    return result;
}

static void free_wrapped_line(WrappedLine *wl) {
    if (!wl || !wl->segments) return;
    for (int i = 0; i < wl->count; i++) free(wl->segments[i]);
    free(wl->segments);
    wl->segments = NULL;
    wl->count = 0;
}

static int is_c_keyword(const char *word) {
    static const char *kw[] = {
        "auto","break","case","char","const","continue","default","do","double","else","enum","extern",
        "float","for","goto","if","int","long","register","return","short","signed","sizeof","static",
        "struct","switch","typedef","union","unsigned","void","volatile","while", NULL
    };
    for (int i = 0; kw[i]; i++) if (strcmp(word, kw[i]) == 0) return 1;
    return 0;
}

static int is_python_keyword(const char *word) {
    static const char *kw[] = {
        "False","None","True","and","as","assert","async","await","break","class","continue","def","del",
        "elif","else","except","finally","for","from","global","if","import","in","is","lambda","nonlocal",
        "not","or","pass","raise","return","try","while","with","yield", NULL
    };
    for (int i = 0; kw[i]; i++) if (strcmp(word, kw[i]) == 0) return 1;
    return 0;
}

static int is_js_keyword(const char *word) {
    static const char *kw[] = {
        "async","await","break","case","catch","class","const","continue","debugger","default","delete","do",
        "else","export","extends","finally","for","function","if","import","in","instanceof","let","new",
        "return","super","switch","this","throw","try","typeof","var","void","while","with","yield", NULL
    };
    for (int i = 0; kw[i]; i++) if (strcmp(word, kw[i]) == 0) return 1;
    return 0;
}

static int is_sql_keyword(const char *word) {
    static const char *kw[] = {
        "SELECT","FROM","WHERE","INSERT","UPDATE","DELETE","CREATE","DROP","ALTER","TABLE","JOIN",
        "INNER","LEFT","RIGHT","OUTER","ON","AND","OR","NOT","NULL","IS","IN","LIKE","ORDER","BY",
        "GROUP","HAVING","LIMIT","OFFSET","AS","DISTINCT", NULL
    };
    for (int i = 0; kw[i]; i++) if (strcasecmp(word, kw[i]) == 0) return 1;
    return 0;
}

static void highlight_line(const char *line, Language lang, int y, int start_x, int line_width,
                          const char *search_term, int do_search_hl) {
    if (!line) return;
    int len = (int)strlen(line);
    int i = 0;
    int col = start_x;
    int stlen = (do_search_hl && search_term && *search_term) ? (int)strlen(search_term) : 0;

    while (i < len && col < line_width) {
        if (stlen > 0) {
            if (i + stlen <= len && strncmp(&line[i], search_term, (size_t)stlen) == 0) {
                attron(COLOR_PAIR(COLOR_SEARCH_HL) | A_BOLD);
                for (int k = 0; k < stlen && col < line_width; k++) mvaddch(y, col++, line[i++]);
                attroff(COLOR_PAIR(COLOR_SEARCH_HL) | A_BOLD);
                continue;
            }
        }

        char ch = line[i];

        if ((lang == LANG_C || lang == LANG_CPP || lang == LANG_JAVA || lang == LANG_JS || lang == LANG_TS ||
             lang == LANG_CSS || lang == LANG_PHP || lang == LANG_GO || lang == LANG_RUST) &&
            i + 1 < len && line[i] == '/' && line[i+1] == '/') {
            attron(COLOR_PAIR(COLOR_COMMENT));
            while (i < len && col < line_width) mvaddch(y, col++, line[i++]);
            attroff(COLOR_PAIR(COLOR_COMMENT));
            break;
        }

        if ((lang == LANG_PYTHON || lang == LANG_SHELL || lang == LANG_RUBY || lang == LANG_YAML || lang == LANG_PHP) &&
            ch == '#') {
            attron(COLOR_PAIR(COLOR_COMMENT));
            while (i < len && col < line_width) mvaddch(y, col++, line[i++]);
            attroff(COLOR_PAIR(COLOR_COMMENT));
            break;
        }

        if (lang == LANG_SQL && i + 1 < len && line[i] == '-' && line[i+1] == '-') {
            attron(COLOR_PAIR(COLOR_COMMENT));
            while (i < len && col < line_width) mvaddch(y, col++, line[i++]);
            attroff(COLOR_PAIR(COLOR_COMMENT));
            break;
        }

        if (ch == '"' || ch == '\'') {
            char quote = ch;
            attron(COLOR_PAIR(COLOR_STRING));
            mvaddch(y, col++, line[i++]);
            while (i < len && col < line_width) {
                ch = line[i];
                mvaddch(y, col++, ch);
                if (ch == quote && (i == 0 || line[i-1] != '\\')) { i++; break; }
                i++;
            }
            attroff(COLOR_PAIR(COLOR_STRING));
            continue;
        }

        if (isdigit((unsigned char)ch)) {
            attron(COLOR_PAIR(COLOR_NUMBER));
            while (i < len && col < line_width &&
                   (isdigit((unsigned char)line[i]) || line[i] == '.' || line[i] == 'x' || line[i] == 'X' ||
                    (line[i] >= 'a' && line[i] <= 'f') || (line[i] >= 'A' && line[i] <= 'F'))) {
                mvaddch(y, col++, line[i++]);
            }
            attroff(COLOR_PAIR(COLOR_NUMBER));
            continue;
        }

        if (isalpha((unsigned char)ch) || ch == '_') {
            char word[128];
            int w = 0;
            int start = i;
            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '_') && w < 127) {
                word[w++] = line[i++];
            }
            word[w] = '\0';

            int iskw = 0;
            if (lang == LANG_C || lang == LANG_CPP) iskw = is_c_keyword(word);
            else if (lang == LANG_PYTHON) iskw = is_python_keyword(word);
            else if (lang == LANG_JS || lang == LANG_TS) iskw = is_js_keyword(word);
            else if (lang == LANG_SQL) iskw = is_sql_keyword(word);

            if (iskw) attron(COLOR_PAIR(COLOR_KEYWORD) | A_BOLD);
            for (int k = 0; k < w && col < line_width; k++) mvaddch(y, col++, line[start + k]);
            if (iskw) attroff(COLOR_PAIR(COLOR_KEYWORD) | A_BOLD);
            continue;
        }

        mvaddch(y, col++, line[i++]);
    }
}

// [CHANGE 1] Ensure the line pointer array has room for at least `needed`
// entries, growing by doubling if necessary.
static int buf_ensure_capacity(Buffer *b, int needed) {
    if (needed <= b->line_cap) return 1;
    int new_cap = b->line_cap ? b->line_cap : INITIAL_LINE_CAP;
    while (new_cap < needed) new_cap *= 2;
    char **np = (char**)realloc(b->lines, (size_t)new_cap * sizeof(char*));
    if (!np) return 0;
    b->lines = np;
    b->line_cap = new_cap;
    return 1;
}

static void buffer_init_blank(Buffer *b, const char *filepath) {
    memset(b, 0, sizeof(*b));
    b->is_active = 1;
    // [CHANGE 1] allocate initial dynamic array
    b->line_cap = INITIAL_LINE_CAP;
    b->lines = (char**)calloc((size_t)b->line_cap, sizeof(char*));
    if (filepath && *filepath) {
        snprintf(b->filepath, sizeof(b->filepath), "%s", filepath);
        b->lang = detect_language(filepath);
    } else {
        b->filepath[0] = '\0';
        b->lang = LANG_NONE;
    }
    b->line_count = 1;
    b->lines[0] = strdup("");
    b->scroll_offset = 0;
    b->dirty = 0;
    b->undo_len = 0;
    b->redo_len = 0;
}

static void free_buffer(Buffer *b) {
    if (!b) return;
    for (int i = 0; i < b->line_count; i++) {
        free(b->lines[i]);
        b->lines[i] = NULL;
    }
    free(b->lines);          // [CHANGE 1] free the dynamic array itself
    b->lines = NULL;
    b->line_count = 0;
    b->line_cap = 0;
    b->is_active = 0;
    for (int i = 0; i < b->undo_len; i++) free(b->undo[i]);
    for (int i = 0; i < b->redo_len; i++) free(b->redo[i]);
    b->undo_len = 0;
    b->redo_len = 0;
}

static int load_file(Buffer *b, const char *filepath) {
    if (!file_exists(filepath)) {
        buffer_init_blank(b, filepath);
        b->dirty = 1;
        return 0;
    }

    FILE *f = fopen(filepath, "r");
    if (!f) return -1;

    memset(b, 0, sizeof(*b));
    b->is_active = 1;
    b->scroll_offset = 0;
    // [CHANGE 1] start with initial capacity; grows as needed
    b->line_cap = INITIAL_LINE_CAP;
    b->lines = (char**)calloc((size_t)b->line_cap, sizeof(char*));
    snprintf(b->filepath, sizeof(b->filepath), "%s", filepath);
    b->lang = detect_language(filepath);
    b->dirty = 0;

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), f)) {
        // [CHANGE 1] grow the array instead of silently stopping
        if (!buf_ensure_capacity(b, b->line_count + 1)) break;
        line[strcspn(line, "\n")] = 0;
        strip_overstrikes(line);
        strip_ansi(line);
        rtrim(line);
        b->lines[b->line_count++] = strdup(line);
    }
    fclose(f);

    if (b->line_count == 0) {
        b->line_count = 1;
        b->lines[0] = strdup("");
    }

    b->undo_len = 0;
    b->redo_len = 0;
    return 0;
}

static int load_stdin(Buffer *b) {
    memset(b, 0, sizeof(*b));
    b->is_active = 1;
    // [CHANGE 1] dynamic array
    b->line_cap = INITIAL_LINE_CAP;
    b->lines = (char**)calloc((size_t)b->line_cap, sizeof(char*));
    snprintf(b->filepath, sizeof(b->filepath), "%s", "<stdin>");
    b->lang = LANG_NONE;
    b->scroll_offset = 0;
    b->dirty = 0;

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), stdin)) {
        if (!buf_ensure_capacity(b, b->line_count + 1)) break;
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        strip_overstrikes(line);
        strip_ansi(line);
        rtrim(line);
        b->lines[b->line_count++] = strdup(line);
    }
    if (b->line_count == 0) return -1;

    b->undo_len = 0;
    b->redo_len = 0;
    return 0;
}

static char *buffer_serialize(const Buffer *b) {
    if (!b || b->line_count <= 0) return strdup("");
    size_t total = 0;
    for (int i = 0; i < b->line_count; i++) total += strlen(b->lines[i]) + 1;
    char *out = (char*)malloc(total + 1);
    if (!out) return NULL;
    char *wp = out;
    for (int i = 0; i < b->line_count; i++) {
        size_t len = strlen(b->lines[i]);
        memcpy(wp, b->lines[i], len);
        wp += len;
        if (i != b->line_count - 1) *wp++ = '\n';
    }
    *wp = '\0';
    return out;
}
static void buffer_deserialize(Buffer *b, const char *text) {
    if (!b) return;
    for (int i = 0; i < b->line_count; i++) { free(b->lines[i]); b->lines[i] = NULL; }
    b->line_count = 0;

    if (!text) {
        if (!buf_ensure_capacity(b, 1)) return;
        b->line_count = 1;
        b->lines[0] = strdup("");
        return;
    }

    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (!buf_ensure_capacity(b, b->line_count + 1)) break;
        char *line = (char*)malloc(len + 1);
        memcpy(line, p, len);
        line[len] = '\0';
        b->lines[b->line_count++] = line;
        if (!nl) break;
        p = nl + 1;
    }

    if (b->line_count == 0) {
        if (buf_ensure_capacity(b, 1)) {
            b->line_count = 1;
            b->lines[0] = strdup("");
        }
    }
}

static void undo_push(Buffer *b) {
    if (!b) return;
    char *snap = buffer_serialize(b);
    if (!snap) return;

    for (int i = 0; i < b->redo_len; i++) free(b->redo[i]);
    b->redo_len = 0;

    if (b->undo_len == UNDO_MAX) {
        free(b->undo[0]);
        memmove(&b->undo[0], &b->undo[1], sizeof(char*) * (UNDO_MAX - 1));
        b->undo_len--;
    }
    b->undo[b->undo_len++] = snap;
}

static void do_undo(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    if (b->undo_len <= 0) return;

    char *cur = buffer_serialize(b);
    if (!cur) return;

    if (b->redo_len == REDO_MAX) {
        free(b->redo[0]);
        memmove(&b->redo[0], &b->redo[1], sizeof(char*) * (REDO_MAX - 1));
        b->redo_len--;
    }
    b->redo[b->redo_len++] = cur;

    char *snap = b->undo[--b->undo_len];
    buffer_deserialize(b, snap);
    free(snap);
    b->dirty = 1;
}

static void do_redo(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    if (b->redo_len <= 0) return;

    char *cur = buffer_serialize(b);
    if (!cur) return;

    if (b->undo_len == UNDO_MAX) {
        free(b->undo[0]);
        memmove(&b->undo[0], &b->undo[1], sizeof(char*) * (UNDO_MAX - 1));
        b->undo_len--;
    }
    b->undo[b->undo_len++] = cur;

    char *snap = b->redo[--b->redo_len];
    buffer_deserialize(b, snap);
    free(snap);
    b->dirty = 1;
}

static void clipboard_copy_text(const char *text) {
    if (!text) return;
    FILE *pipe = popen("pbcopy 2>/dev/null || xclip -selection clipboard 2>/dev/null", "w");
    if (!pipe) return;
    fputs(text, pipe);
    pclose(pipe);
}

static char *clipboard_paste_text(void) {
    FILE *pipe = popen("pbpaste 2>/dev/null || xclip -selection clipboard -o 2>/dev/null", "r");
    if (!pipe) return NULL;

    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) { pclose(pipe); return NULL; }

    int c;
    while ((c = fgetc(pipe)) != EOF) {
        if (len + 2 >= cap) {
            cap *= 2;
            char *nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); pclose(pipe); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    pclose(pipe);

    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    return buf;
}

static void clear_search(ViewerState *st) {
    st->search_highlight = 0;
    st->search_term[0] = '\0';
    st->search_match_count = 0;
    st->current_match = 0;
}

static void find_all_matches(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    st->search_match_count = 0;
    st->current_match = 0;
    if (!st->search_highlight || st->search_term[0] == '\0') return;
    for (int i = 0; i < b->line_count; i++)
        if (strstr(b->lines[i], st->search_term)) st->search_match_count++;
}

static int search_buffer(ViewerState *st, const char *term, int start_line, int direction) {
    Buffer *b = &st->buffers[st->current_buffer];
    if (!term || term[0] == '\0') return -1;
    int line = start_line;
    for (int i = 0; i < b->line_count; i++) {
        if (line < 0) line = b->line_count - 1;
        if (line >= b->line_count) line = 0;
        if (strstr(b->lines[line], term)) return line;
        line += direction;
    }
    return -1;
}

static void jump_to_first_match(ViewerState *st) {
    if (!st->search_highlight || st->search_term[0] == '\0') return;
    int match = search_buffer(st, st->search_term, 0, 1);
    if (match >= 0) {
        st->cursor_line = match;
        st->cursor_col = 0;
    }
}

static void next_match(ViewerState *st) {
    if (!st->search_highlight || st->search_term[0] == '\0') return;
    Buffer *b = &st->buffers[st->current_buffer];
    int match = search_buffer(st, st->search_term, st->cursor_line + 1, 1);
    if (match >= 0) {
        st->cursor_line = match;
        st->cursor_col = 0;
        int count = 0;
        for (int i = 0; i < match; i++) if (strstr(b->lines[i], st->search_term)) count++;
        st->current_match = count;
    }
}

static void prev_match(ViewerState *st) {
    if (!st->search_highlight || st->search_term[0] == '\0') return;
    Buffer *b = &st->buffers[st->current_buffer];
    int match = search_buffer(st, st->search_term, st->cursor_line - 1, -1);
    if (match >= 0) {
        st->cursor_line = match;
        st->cursor_col = 0;
        int count = 0;
        for (int i = 0; i < match; i++) if (strstr(b->lines[i], st->search_term)) count++;
        st->current_match = count;
    }
}

static int content_height(void) {
    int max_y = getmaxy(stdscr);
    int h = max_y - 2;
    if (h < 1) h = 1;
    return h;
}

static void ensure_cursor_bounds(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    if (st->cursor_line < 0) st->cursor_line = 0;
    if (st->cursor_line >= b->line_count) st->cursor_line = b->line_count - 1;
    if (st->cursor_line < 0) st->cursor_line = 0;
    int ll = (int)strlen(b->lines[st->cursor_line]);
    if (st->cursor_col < 0) st->cursor_col = 0;
    if (st->cursor_col > ll) st->cursor_col = ll;
}

static int line_nr_width_for(ViewerState *st) {
    return st->show_line_numbers ? 6 : 0;
}

static int text_width_for(ViewerState *st) {
    int max_x = getmaxx(stdscr);
    int lnw = line_nr_width_for(st);
    int w = max_x - lnw - 1;
    if (w < 1) w = 1;
    return w;
}

static int wrapped_rows_for_line(ViewerState *st, const char *line) {
    if (!st->wrap_enabled) return 1;
    int w = text_width_for(st);
    int len = (int)strlen(line);
    int rows = (len <= 0) ? 1 : ((len + w - 1) / w);
    if (rows < 1) rows = 1;
    return rows;
}

static void ensure_cursor_visible(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    int h = content_height();

    if (!st->wrap_enabled) {
        if (st->cursor_line < b->scroll_offset)
            b->scroll_offset = st->cursor_line;
        if (st->cursor_line >= b->scroll_offset + h)
            b->scroll_offset = st->cursor_line - h + 1;
        if (b->scroll_offset < 0) b->scroll_offset = 0;
        return;
    }

    if (st->cursor_line < b->scroll_offset) {
        b->scroll_offset = st->cursor_line;
        if (b->scroll_offset < 0) b->scroll_offset = 0;
        return;
    }

    int rows = 0;
    for (int i = b->scroll_offset; i <= st->cursor_line && i < b->line_count; i++)
        rows += wrapped_rows_for_line(st, b->lines[i]);

    while (rows > h && b->scroll_offset < st->cursor_line) {
        rows -= wrapped_rows_for_line(st, b->lines[b->scroll_offset]);
        b->scroll_offset++;
    }

    if (b->scroll_offset < 0) b->scroll_offset = 0;
}

static void scroll_viewport(ViewerState *st, int delta) {
    Buffer *b = &st->buffers[st->current_buffer];
    int h = content_height();
    int max_scroll = b->line_count - h;
    if (max_scroll < 0) max_scroll = 0;
    int new_scroll = b->scroll_offset + delta;
    if (new_scroll < 0) new_scroll = 0;
    if (new_scroll > max_scroll) new_scroll = max_scroll;
    b->scroll_offset = new_scroll;
}

static void move_left(ViewerState *st) {
    if (st->cursor_col > 0) st->cursor_col--;
    else if (st->cursor_line > 0) {
        st->cursor_line--;
        Buffer *b = &st->buffers[st->current_buffer];
        st->cursor_col = (int)strlen(b->lines[st->cursor_line]);
    }
}

static void move_right(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    int ll = (int)strlen(b->lines[st->cursor_line]);
    if (st->cursor_col < ll) st->cursor_col++;
    else if (st->cursor_line < b->line_count - 1) {
        st->cursor_line++;
        st->cursor_col = 0;
    }
}

static void move_up(ViewerState *st) {
    if (st->cursor_line > 0) st->cursor_line--;
    Buffer *b = &st->buffers[st->current_buffer];
    int ll = (int)strlen(b->lines[st->cursor_line]);
    if (st->cursor_col > ll) st->cursor_col = ll;
}

static void move_down(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    if (st->cursor_line < b->line_count - 1) st->cursor_line++;
    int ll = (int)strlen(b->lines[st->cursor_line]);
    if (st->cursor_col > ll) st->cursor_col = ll;
}

static int is_open_br(char c) { return (c=='(' || c=='[' || c=='{'); }
static int is_close_br(char c){ return (c==')' || c==']' || c=='}'); }

static char match_for(char c) {
    switch (c) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        default: return 0;
    }
}

typedef struct { int line; int col; } Pos;

static int pos_valid(Buffer *b, Pos p) {
    return p.line >= 0 && p.line < b->line_count && p.col >= 0;
}

static char char_at(Buffer *b, Pos p) {
    if (!pos_valid(b, p)) return 0;
    int len = (int)strlen(b->lines[p.line]);
    if (p.col < len) return b->lines[p.line][p.col];
    return 0;
}

static Pos pos_next(Buffer *b, Pos p) {
    if (!pos_valid(b, p)) return p;
    int len = (int)strlen(b->lines[p.line]);
    if (p.col < len) { p.col++; return p; }
    if (p.line < b->line_count - 1) { p.line++; p.col = 0; }
    return p;
}

static Pos pos_prev(Buffer *b, Pos p) {
    if (!pos_valid(b, p)) return p;
    if (p.col > 0) { p.col--; return p; }
    if (p.line > 0) {
        p.line--;
        p.col = (int)strlen(b->lines[p.line]);
        if (p.col > 0) p.col--;
    }
    return p;
}

static int find_matching_bracket(Buffer *b, Pos start, Pos *out) {
    char c = char_at(b, start);
    if (!is_open_br(c) && !is_close_br(c)) return 0;
    char want = match_for(c);
    int dir = is_open_br(c) ? +1 : -1;
    int depth = 0;
    Pos p = start;
    while (1) {
        p = (dir > 0) ? pos_next(b, p) : pos_prev(b, p);
        if (!pos_valid(b, p)) break;
        char x = char_at(b, p);
        if (x == 0) continue;
        if (x == c) depth++;
        else if (x == want) {
            if (depth == 0) { *out = p; return 1; }
            depth--;
        }
    }
    return 0;
}

static int jump_percent(ViewerState *st, int *out_to_line, int *out_to_col, int *out_from_line, int *out_from_col) {
    Buffer *b = &st->buffers[st->current_buffer];
    Pos start = (Pos){ st->cursor_line, st->cursor_col };
    char c = char_at(b, start);

    if (!is_open_br(c) && !is_close_br(c)) {
        int len = (int)strlen(b->lines[start.line]);
        int found = 0;
        for (int i = start.col; i < len; i++) {
            char t = b->lines[start.line][i];
            if (is_open_br(t) || is_close_br(t)) {
                start.col = i;
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }

    Pos match;
    if (!find_matching_bracket(b, start, &match)) return 0;

    if (out_from_line) *out_from_line = start.line;
    if (out_from_col)  *out_from_col  = start.col;
    if (out_to_line)   *out_to_line   = match.line;
    if (out_to_col)    *out_to_col    = match.col;

    return 1;
}

static void insert_char_at(Buffer *b, int line, int col, char c) {
    char *s = b->lines[line];
    int len = (int)strlen(s);
    if (col < 0) col = 0;
    if (col > len) col = len;

    char *ns = (char*)malloc((size_t)len + 2);
    memcpy(ns, s, (size_t)col);
    ns[col] = c;
    memcpy(ns + col + 1, s + col, (size_t)(len - col));
    ns[len + 1] = '\0';

    free(b->lines[line]);
    b->lines[line] = ns;
    b->dirty = 1;
}

static void delete_char_before(Buffer *b, int *line_io, int *col_io) {
    int line = *line_io;
    int col = *col_io;
    if (line < 0 || line >= b->line_count) return;
    char *s = b->lines[line];
    int len = (int)strlen(s);

    if (col > 0) {
        char *ns = (char*)malloc((size_t)len);
        memcpy(ns, s, (size_t)(col - 1));
        memcpy(ns + (col - 1), s + col, (size_t)(len - col + 1));
        free(b->lines[line]);
        b->lines[line] = ns;
        *col_io = col - 1;
        b->dirty = 1;
        return;
    }

    if (line == 0) return;

    char *prev = b->lines[line - 1];
    int plen = (int)strlen(prev);
    char *joined = (char*)malloc((size_t)plen + (size_t)len + 1);
    memcpy(joined, prev, (size_t)plen);
    memcpy(joined + plen, s, (size_t)len + 1);

    free(b->lines[line - 1]);
    b->lines[line - 1] = joined;
    free(b->lines[line]);

    for (int i = line; i < b->line_count - 1; i++) b->lines[i] = b->lines[i + 1];
    b->lines[b->line_count - 1] = NULL;
    b->line_count--;

    *line_io = line - 1;
    *col_io = plen;
    b->dirty = 1;
}

static void insert_newline(Buffer *b, int *line_io, int *col_io) {
    int line = *line_io;
    int col = *col_io;
    if (line < 0 || line >= b->line_count) return;

    // [CHANGE 1] grow dynamically instead of hard limit
    if (!buf_ensure_capacity(b, b->line_count + 1)) return;

    char *s = b->lines[line];
    int len = (int)strlen(s);
    if (col < 0) col = 0;
    if (col > len) col = len;

    char *left = (char*)malloc((size_t)col + 1);
    memcpy(left, s, (size_t)col);
    left[col] = '\0';
    char *right = strdup(s + col);

    free(b->lines[line]);
    b->lines[line] = left;

    for (int i = b->line_count; i > line + 1; i--) b->lines[i] = b->lines[i - 1];
    b->lines[line + 1] = right;
    b->line_count++;

    *line_io = line + 1;
    *col_io = 0;
    b->dirty = 1;
}

static void paste_text_at_cursor(ViewerState *st, const char *text) {
    if (!text || !*text) return;
    Buffer *b = &st->buffers[st->current_buffer];
    undo_push(b);
    for (const char *p = text; *p; p++) {
        if (*p == '\n') insert_newline(b, &st->cursor_line, &st->cursor_col);
        else {
            insert_char_at(b, st->cursor_line, st->cursor_col, *p);
            st->cursor_col++;
        }
    }
}

static void delete_range_to_match(ViewerState *st, int aL, int aC, int bL, int bC, int also_yank) {
    Buffer *b = &st->buffers[st->current_buffer];
    int sL=aL, sC=aC, eL=bL, eC=bC;
    if (sL > eL || (sL==eL && sC > eC)) {
        int tL=sL,tC=sC; sL=eL; sC=eC; eL=tL; eC=tC;
    }
    size_t cap = 4096, len = 0;
    char *tmp = (char*)malloc(cap);
    if (!tmp) return;
    tmp[0] = '\0';
    for (int L = sL; L <= eL; L++) {
        const char *line = b->lines[L];
        int line_len = (int)strlen(line);
        int start = (L==sL) ? sC : 0;
        int end   = (L==eL) ? eC : (line_len-1);
        if (start < 0) start = 0;
        if (start > line_len) start = line_len;
        if (end < -1) end = -1;
        if (end >= line_len) end = line_len-1;
        if (end >= start) {
            int chunk = end - start + 1;
            if (len + (size_t)chunk + 2 >= cap) {
                while (len + (size_t)chunk + 2 >= cap) cap *= 2;
                char *nb = (char*)realloc(tmp, cap);
                if (!nb) { free(tmp); return; }
                tmp = nb;
            }
            memcpy(tmp + len, line + start, (size_t)chunk);
            len += (size_t)chunk;
            tmp[len] = '\0';
        }
        if (L != eL) {
            if (len + 2 >= cap) { cap *= 2; char *nb=(char*)realloc(tmp, cap); if(!nb){free(tmp);return;} tmp=nb; }
            tmp[len++] = '\n';
            tmp[len] = '\0';
        }
    }
    if (also_yank) clipboard_copy_text(tmp);
    undo_push(b);
    if (sL == eL) {
        char *line = b->lines[sL];
        int line_len = (int)strlen(line);
        if (sC < 0) sC = 0;
        if (eC >= line_len) eC = line_len - 1;
        if (eC >= sC) {
            int new_len = line_len - (eC - sC + 1);
            char *nl = (char*)malloc((size_t)new_len + 1);
            memcpy(nl, line, (size_t)sC);
            memcpy(nl + sC, line + eC + 1, (size_t)(line_len - (eC + 1) + 1));
            free(b->lines[sL]);
            b->lines[sL] = nl;
        }
        st->cursor_line = sL;
        st->cursor_col = sC;
        ensure_cursor_bounds(st);
        free(tmp);
        return;
    }
    char *start_line = b->lines[sL];
    char *end_line   = b->lines[eL];
    int slen = (int)strlen(start_line);
    int elen = (int)strlen(end_line);
    if (sC < 0) sC = 0;
    if (sC > slen) sC = slen;
    if (eC < -1) eC = -1;
    if (eC >= elen) eC = elen - 1;
    const char *prefix = start_line;
    int prefix_len = sC;
    const char *suffix = (eC + 1 <= elen) ? (end_line + eC + 1) : (end_line + elen);
    int suffix_len = (int)strlen(suffix);
    char *joined = (char*)malloc((size_t)prefix_len + (size_t)suffix_len + 1);
    memcpy(joined, prefix, (size_t)prefix_len);
    memcpy(joined + prefix_len, suffix, (size_t)suffix_len);
    joined[prefix_len + suffix_len] = '\0';
    for (int L = sL; L <= eL; L++) free(b->lines[L]);
    int dst = sL + 1;
    int src = eL + 1;
    while (src < b->line_count) {
        b->lines[dst++] = b->lines[src++];
    }
    b->lines[sL] = joined;
    int new_count = dst;
    for (int i = new_count; i < b->line_count; i++) b->lines[i] = NULL;
    b->line_count = new_count;
    st->cursor_line = sL;
    st->cursor_col = sC;
    ensure_cursor_bounds(st);
    free(tmp);
}

// [CHANGE 2/3] Delete visual selection lines (lo..hi inclusive).
// Returns a malloc'd string with the deleted content (caller frees).
static char *delete_visual_lines(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    int lo = st->vis_start < st->vis_end ? st->vis_start : st->vis_end;
    int hi = st->vis_start > st->vis_end ? st->vis_start : st->vis_end;

    if (lo < 0) lo = 0;
    if (hi >= b->line_count) hi = b->line_count - 1;

    // Build yanked text
    size_t total = 0;
    for (int i = lo; i <= hi; i++) total += strlen(b->lines[i]) + 1;
    char *out = (char*)malloc(total + 1);
    if (!out) return NULL;
    out[0] = '\0';
    for (int i = lo; i <= hi; i++) {
        strcat(out, b->lines[i]);
        if (i != hi) strcat(out, "\n");
    }

    // Push undo before mutating
    undo_push(b);

    // Free deleted lines
    for (int i = lo; i <= hi; i++) {
        free(b->lines[i]);
        b->lines[i] = NULL;
    }

    // Compact the array
    int deleted = hi - lo + 1;
    for (int i = lo; i < b->line_count - deleted; i++)
        b->lines[i] = b->lines[i + deleted];
    for (int i = b->line_count - deleted; i < b->line_count; i++)
        b->lines[i] = NULL;
    b->line_count -= deleted;

    // Always keep at least one line
    if (b->line_count == 0) {
        b->line_count = 1;
        b->lines[0] = strdup("");
    }

    b->dirty = 1;

    // Place cursor at first deleted line (clamped)
    st->cursor_line = lo;
    st->cursor_col  = 0;
    ensure_cursor_bounds(st);

    return out;
}

// [CHANGE 4/5] Yank or delete all lines in the buffer.
static void yank_all_lines(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    char *snap = buffer_serialize(b);
    if (!snap) return;
    clipboard_copy_text(snap);
    free(snap);
    set_status(st, "Yanked all lines");
}

static void delete_all_lines(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    char *snap = buffer_serialize(b);
    if (snap) { clipboard_copy_text(snap); free(snap); }
    undo_push(b);
    for (int i = 0; i < b->line_count; i++) { free(b->lines[i]); b->lines[i] = NULL; }
    b->line_count = 1;
    b->lines[0] = strdup("");
    b->dirty = 1;
    st->cursor_line = 0;
    st->cursor_col  = 0;
    set_status(st, "Deleted all lines");
}

static void close_current_buffer(ViewerState *st) {
    if (st->buffer_count <= 1) { set_status(st, "Cannot close the last buffer"); return; }
    int cur = st->current_buffer;
    free_buffer(&st->buffers[cur]);
    for (int i = cur; i < st->buffer_count - 1; i++) st->buffers[i] = st->buffers[i + 1];
    st->buffer_count--;
    if (st->current_buffer >= st->buffer_count) st->current_buffer = st->buffer_count - 1;
    st->cursor_line = 0;
    st->cursor_col = 0;
}

static void next_buffer(ViewerState *st) {
    if (st->buffer_count <= 1) return;
    st->current_buffer = (st->current_buffer + 1) % st->buffer_count;
    st->cursor_line = 0;
    st->cursor_col = 0;
}

static void prev_buffer(ViewerState *st) {
    if (st->buffer_count <= 1) return;
    st->current_buffer--;
    if (st->current_buffer < 0) st->current_buffer = st->buffer_count - 1;
    st->cursor_line = 0;
    st->cursor_col = 0;
}

static void add_blank_buffer(ViewerState *st) {
    if (st->buffer_count >= MAX_BUFFERS) { set_status(st, "Max buffers reached"); return; }
    buffer_init_blank(&st->buffers[st->buffer_count], "");
    st->current_buffer = st->buffer_count;
    st->buffer_count++;
    st->cursor_line = 0;
    st->cursor_col = 0;
    set_status(st, "New buffer");
}

static int add_buffer_from_path(ViewerState *st, const char *path) {
    if (!st || !path || !*path) return -1;
    if (st->buffer_count >= MAX_BUFFERS) { set_status(st, "Max buffers reached"); return -1; }

    int idx = st->buffer_count;
    if (load_file(&st->buffers[idx], path) != 0) {
        set_status(st, "Failed to load file");
        return -1;
    }

    st->buffer_count++;
    st->current_buffer = idx;
    st->cursor_line = 0;
    st->cursor_col  = 0;
    set_status(st, "Added buffer");
    return 0;
}
static void shell_quote_single(char *out, size_t out_len, const char *in) {
    size_t j = 0;
    if (!out || out_len == 0) return;

    out[j++] = '\'';
    for (size_t i = 0; in && in[i] != '\0' && j + 6 < out_len; i++) {
        if (in[i] == '\'') {
            const char *esc = "'\"'\"'";
            for (int k = 0; esc[k] && j + 1 < out_len; k++) out[j++] = esc[k];
        } else {
            out[j++] = in[i];
        }
    }
    if (j + 1 < out_len) out[j++] = '\'';
    out[j] = '\0';
}

static int in_tmux(void) {
    const char *t = getenv("TMUX");
    return (t && *t);
}


static int tmux_pane_alive(const char *pane_id) {
    if (!pane_id || !*pane_id) return 0;

    char qid[256];
    shell_quote_single(qid, sizeof(qid), pane_id);

    char cmd[512];
    // exits nonzero if pane doesn't exist
    snprintf(cmd, sizeof(cmd), "tmux list-panes -F '#{pane_id}' | grep -qx %s", qid);
    return system(cmd) == 0;
}

static int tmux_kill_pane(const char *pane_id) {
    if (!pane_id || !*pane_id) return -1;

    char qid[256];
    shell_quote_single(qid, sizeof(qid), pane_id);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "tmux kill-pane -t %s 2>/dev/null", qid);
    return system(cmd);
}

// Returns 0 on success, -1 on failure
static int tmux_create_bottom_terminal(const char *cwd, char *out_pane, size_t out_len) {
    if (!cwd || !*cwd || !out_pane || out_len == 0) return -1;

    char qcwd[2048];
    shell_quote_single(qcwd, sizeof(qcwd), cwd);

    // -P prints info about the new pane, -F chooses the format -> just the pane id
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "tmux split-window -v -p 30 -c %s -P -F '#{pane_id}'", qcwd);

    FILE *p = popen(cmd, "r");
    if (!p) return -1;

    char buf[128] = {0};
    if (!fgets(buf, sizeof(buf), p)) {
        pclose(p);
        return -1;
    }
    pclose(p);

    buf[strcspn(buf, "\r\n")] = '\0';
    if (!buf[0]) return -1;

    strncpy(out_pane, buf, out_len - 1);
    out_pane[out_len - 1] = '\0';

    // go back to the editor/file pane
    system("tmux last-pane 2>/dev/null");
    return 0;
}
static void buffer_cwd(const Buffer *b, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    // Prefer the directory of the current file if it has a slash.
    if (b && b->filepath[0] && strcmp(b->filepath, "<stdin>") != 0) {
        const char *slash = strrchr(b->filepath, '/');
        if (slash && slash != b->filepath) {
            size_t n = (size_t)(slash - b->filepath);
            if (n >= out_len) n = out_len - 1;
            memcpy(out, b->filepath, n);
            out[n] = '\0';
            return;
        }
    }

    // Fallback: process cwd.
    if (getcwd(out, out_len) == NULL) snprintf(out, out_len, ".");
}
static char g_terminal_pane_id[128] = {0};


static void chomp(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
}
static void sh_quote(const char *in, char *out, size_t out_sz) {
    // output: '...'
    // replaces ' with '"'"'
    size_t j = 0;
    if (out_sz < 3) { if (out_sz) out[0] = 0; return; }

    out[j++] = '\'';
    for (size_t i = 0; in && in[i]; i++) {
        if (j + 6 >= out_sz) break;
        if (in[i] == '\'') {
            // close ', insert '"'"', reopen '
            out[j++] = '\'';
            out[j++] = '"';
            out[j++] = '\'';
            out[j++] = '"';
            out[j++] = '\'';
        } else {
            out[j++] = in[i];
        }
    }
    out[j++] = '\'';
    out[j] = '\0';
}
static int tmux_pane_exists_simple(const char *pane_id) {
    if (!pane_id || !*pane_id) return 0;
    char cmd[256];
    // exact match in current server
    snprintf(cmd, sizeof(cmd),
             "tmux list-panes -a -F '#{pane_id}' 2>/dev/null | grep -qx '%s'",
             pane_id);
    return system(cmd) == 0;
}

static int tmux_get_window_opt(const char *opt, char *out, size_t out_len) {
    if (!opt || !out || out_len == 0) return -1;
    out[0] = '\0';

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tmux show-options -w -qv %s 2>/dev/null", opt);

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    if (fgets(out, (int)out_len, fp)) chomp(out);
    pclose(fp);

    return 0;
}

static void tmux_set_window_opt(const char *opt, const char *val) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "tmux set-option -w %s '%s' 2>/dev/null", opt, val ? val : "");
    system(cmd);
}

static void tmux_unset_window_opt(const char *opt) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tmux unset-option -w %s 2>/dev/null", opt);
    system(cmd);
}
void tmux_toggle_lldb(const char *cwd) {
    if (!getenv("TMUX")) return;
    char pane[64] = {0};
    tmux_get_window_opt("@vic_lldb_pane", pane, sizeof(pane));
    // TOGGLE OFF
    if (pane[0] && tmux_pane_exists_simple(pane)) {
        char qpane[256];
        shell_quote_single(qpane, sizeof(qpane), pane);
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "tmux kill-pane -t %s 2>/dev/null", qpane);
        system(cmd);
        return;
    }
    // stale option (pane died)
    if (pane[0] && !tmux_pane_exists_simple(pane)) {
        tmux_unset_window_opt("@vic_lldb_pane");
        pane[0] = '\0';
    }
    // TOGGLE ON
    char qcwd[2048];
    sh_quote((cwd && *cwd) ? cwd : ".", qcwd, sizeof(qcwd));
    // create the pane (it becomes active)
    {
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "tmux split-window -h -p 10 -c %s 2>/dev/null", qcwd);
        system(cmd);
    }
    // read the active pane id (that's the new one), then go back
    {
        FILE *fp = popen("tmux display-message -p '#{pane_id}' 2>/dev/null", "r");
        if (!fp) return;
        char newpane[64] = {0};
        if (fgets(newpane, sizeof(newpane), fp)) {
            chomp(newpane);
            if (newpane[0]) {
                tmux_set_window_opt("@vic_lldb_pane", newpane);
                // Send the lldb command to the new pane
                char lldb_cmd[512];
char qnewpane[256];
                shell_quote_single(qnewpane, sizeof(qnewpane), newpane);
                snprintf(lldb_cmd, sizeof(lldb_cmd),
                         "tmux send-keys -t %s 'lldb ./bin/*' Enter 2>/dev/null", qnewpane);                system(lldb_cmd);
            }
        }
        pclose(fp);
        system("tmux last-pane 2>/dev/null");
    }
}
void tmux_toggle_db(const char *cwd) {
    if (!getenv("TMUX")) return;
    char pane[64] = {0};
    tmux_get_window_opt("@vic_db_pane", pane, sizeof(pane));
    // TOGGLE OFF
    if (pane[0] && tmux_pane_exists_simple(pane)) {
        char qpane[256];
        shell_quote_single(qpane, sizeof(qpane), pane);
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "tmux kill-pane -t %s 2>/dev/null", qpane);
        system(cmd);
        tmux_unset_window_opt("@vic_db_pane");
        return;
    }
    // stale option (pane died)
    if (pane[0] && !tmux_pane_exists_simple(pane)) {
        tmux_unset_window_opt("@vic_db_pane");
        pane[0] = '\0';
    }
    // TOGGLE ON
    char qcwd[2048];
    sh_quote((cwd && *cwd) ? cwd : ".", qcwd, sizeof(qcwd));
    // create the pane (it becomes active)
    {
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "tmux split-window -h -p 40 -c %s 2>/dev/null", qcwd);
        system(cmd);
    }
    // read the active pane id (that's the new one), then go back
    {
        FILE *fp = popen("tmux display-message -p '#{pane_id}' 2>/dev/null", "r");
        if (!fp) return;
        char newpane[64] = {0};
        if (fgets(newpane, sizeof(newpane), fp)) {
            chomp(newpane);
            if (newpane[0]) {
                tmux_set_window_opt("@vic_db_pane", newpane);
                // Send the data command to the new pane
                char db_cmd[512];
                char qnewpane[256];
                shell_quote_single(qnewpane, sizeof(qnewpane), newpane);
                snprintf(db_cmd, sizeof(db_cmd),
                         "tmux send-keys -t %s 'data-tui' Enter 2>/dev/null", qnewpane);
                system(db_cmd);
            }
        }
        pclose(fp);
        system("tmux last-pane 2>/dev/null");
    }
}
void tmux_toggle_peek(const char *cwd) {
    if (!getenv("TMUX")) return;
    char pane[64] = {0};
    char shell_pane[64] = {0};
    tmux_get_window_opt("@vic_peek_pane", pane, sizeof(pane));
    tmux_get_window_opt("@vic_peek_shell_pane", shell_pane, sizeof(shell_pane));

    // TOGGLE OFF
    if (pane[0] && tmux_pane_exists_simple(pane)) {
        char cmd[256];
        // Kill shell pane first if it exists
if (shell_pane[0] && tmux_pane_exists_simple(shell_pane)) {
            char qshell[256];
            shell_quote_single(qshell, sizeof(qshell), shell_pane);
            snprintf(cmd, sizeof(cmd), "tmux kill-pane -t %s 2>/dev/null", qshell);
            system(cmd);
        }
        // Then kill the main peek pane
        char qpane[256];
        shell_quote_single(qpane, sizeof(qpane), pane);
        snprintf(cmd, sizeof(cmd), "tmux kill-pane -t %s 2>/dev/null", qpane);
        system(cmd);
        tmux_unset_window_opt("@vic_peek_pane");
        tmux_unset_window_opt("@vic_peek_shell_pane");
        return;
    }

    // stale option (pane died)
    if (pane[0] && !tmux_pane_exists_simple(pane)) {
        tmux_unset_window_opt("@vic_peek_pane");
        tmux_unset_window_opt("@vic_peek_shell_pane");
        pane[0] = '\0';
    }

    // TOGGLE ON
    char qcwd[2048];
    sh_quote((cwd && *cwd) ? cwd : ".", qcwd, sizeof(qcwd));
    // create the main peek pane (it becomes active)
    {
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "tmux split-window -h -p 30 -c %s 2>/dev/null", qcwd);
        system(cmd);
    }
    // read the active pane id (that's the peek pane)
    char newpane[64] = {0};
    {
        FILE *fp = popen("tmux display-message -p '#{pane_id}' 2>/dev/null", "r");
        if (!fp) return;
        if (fgets(newpane, sizeof(newpane), fp)) {
            chomp(newpane);
            if (newpane[0]) tmux_set_window_opt("@vic_peek_pane", newpane);
        }
        pclose(fp);
    }
    // send peek command to the main pane
    if (newpane[0]) {
        char peek_cmd[512];
        char qnewpane[256];
        shell_quote_single(qnewpane, sizeof(qnewpane), newpane);
        snprintf(peek_cmd, sizeof(peek_cmd),
                 "tmux send-keys -t %s 'peek todo.md' Enter 2>/dev/null", qnewpane);
        system(peek_cmd);
    }
    // split the peek pane vertically to create bottom shell (20% of peek pane)
    {
        char split_cmd[512];
        char qnewpane[256];
snprintf(split_cmd, sizeof(split_cmd),
                 "tmux split-window -t %s -v -p 30 -c %s 2>/dev/null", qnewpane, qcwd);
        system(split_cmd);
    }
    // get the new shell pane id and save it
    {
        FILE *fp = popen("tmux display-message -p '#{pane_id}' 2>/dev/null", "r");
        if (!fp) return;
        char new_shell_pane[64] = {0};
        if (fgets(new_shell_pane, sizeof(new_shell_pane), fp)) {
            chomp(new_shell_pane);
            if (new_shell_pane[0]) tmux_set_window_opt("@vic_peek_shell_pane", new_shell_pane);
        }
        pclose(fp);
    }
    // go back to the original pane
    system("tmux last-pane 2>/dev/null");
}
void tmux_toggle_terminal(const char *cwd) {
    if (!getenv("TMUX")) return;

    char pane[64] = {0};
    tmux_get_window_opt("@vic_term_pane", pane, sizeof(pane));

    // TOGGLE OFF
    if (pane[0] && tmux_pane_exists_simple(pane)) {
        char qpane[256];
        shell_quote_single(qpane, sizeof(qpane), pane);
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "tmux kill-pane -t %s 2>/dev/null", qpane);
        system(cmd);
        tmux_unset_window_opt("@vic_term_pane");
        return;
    }

    // stale option (pane died)
    if (pane[0] && !tmux_pane_exists_simple(pane)) {
        tmux_unset_window_opt("@vic_term_pane");
        pane[0] = '\0';
    }

    // TOGGLE ON
    char qcwd[2048];
    sh_quote((cwd && *cwd) ? cwd : ".", qcwd, sizeof(qcwd));

    // create the pane (it becomes active)
    {
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "tmux split-window -v -l 30%% -c %s 2>/dev/null", qcwd);
        system(cmd);
    }

    // read the active pane id (that's the new one), then go back
    {
        FILE *fp = popen("tmux display-message -p '#{pane_id}' 2>/dev/null", "r");
        if (!fp) return;

        char newpane[64] = {0};
        if (fgets(newpane, sizeof(newpane), fp)) {
            chomp(newpane);
            if (newpane[0]) tmux_set_window_opt("@vic_term_pane", newpane);
        }
        pclose(fp);

        system("tmux last-pane 2>/dev/null");
    }
}
// very small shell quote helper for paths

static int tmux_run_capture(const char *cmd, char *out, size_t outlen) {
    if (!out || outlen == 0) return -1;
    out[0] = '\0';

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    if (!fgets(out, (int)outlen, fp)) {
        pclose(fp);
        return -1;
    }
    pclose(fp);

    // trim \r\n
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] ? 0 : -1;
}

static int tmux_pane_exists_grep(const char *pane_id) {
    if (!pane_id || !*pane_id) return 0;

    char qid[256];
    shell_quote_single(qid, sizeof(qid), pane_id);

    char cmd[512];
    // -a = all sessions, safe; -F = only pane_id; grep exact whole-line match
    snprintf(cmd, sizeof(cmd),
             "tmux list-panes -a -F '#{pane_id}' 2>/dev/null | grep -Fxq -- %s",
             qid);
    return system(cmd) == 0;
}
static int tmux_capture_first_line(const char *cmd, char *out, size_t outlen) {
    if (!out || outlen == 0) return -1;
    out[0] = '\0';

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    if (!fgets(out, (int)outlen, fp)) {
        pclose(fp);
        return -1;
    }
    pclose(fp);

    out[strcspn(out, "\r\n")] = '\0';
    return out[0] ? 0 : -1;
}

static char *pick_file(void) {
    int have_nfzf = check_command_exists("ff");
    int have_fzf  = check_command_exists("fzf");

    if (!have_nfzf && !have_fzf) {
        char chosen[2048] = {0};

        def_prog_mode();
        endwin();

        printf("\nOpen file: ");
        fflush(stdout);

        if (!fgets(chosen, sizeof(chosen), stdin)) {
            reset_prog_mode();
            refresh();
            return NULL;
        }

        reset_prog_mode();
        refresh();

        trim_newlines(chosen);
        if (chosen[0] == '\0') return NULL;
        return strdup(chosen);
    }

    char tmp_template[] = "/tmp/nbl_vic_pick_XXXXXX";
    int fdout = mkstemp(tmp_template);
    if (fdout < 0) return NULL;
    close(fdout);

    int has_fd = check_command_exists("fd");
    char cmd[8192];

    if (has_fd) {
        if (have_nfzf) {
            snprintf(cmd, sizeof(cmd),
                "fd -L -t f . "
                "--exclude .git --exclude node_modules --exclude build --exclude dist --exclude .cache "
                "2>/dev/null | ff > \"%s\" 2>/dev/null",
                tmp_template
            );
        } else {
            snprintf(cmd, sizeof(cmd),
                "fd -L -t f . "
                "--exclude .git --exclude node_modules --exclude build --exclude dist --exclude .cache "
                "2>/dev/null | fzf --height=80%% --layout=reverse --border "
                "< /dev/tty > \"%s\" 2> /dev/tty",
                tmp_template
            );
        }
    } else {
        const char *find_cmd =
            "find . -type d \\( -name .git -o -name node_modules -o -name build -o -name dist -o -name .cache \\) "
            "-prune -o -type f -print 2>/dev/null";

        if (have_nfzf) {
            snprintf(cmd, sizeof(cmd),
                "%s | ff > \"%s\" 2>/dev/null",
                find_cmd, tmp_template
            );
        } else {
            snprintf(cmd, sizeof(cmd),
                "%s | fzf --height=80%% --layout=reverse --border "
                "< /dev/tty > \"%s\" 2> /dev/tty",
                find_cmd, tmp_template
            );
        }
    }

    def_prog_mode();
    endwin();
    int rc = system(cmd);
    reset_prog_mode();
    refresh();

    if (rc != 0) {
        unlink(tmp_template);
        return NULL;
    }

    FILE *f = fopen(tmp_template, "r");
    if (!f) { unlink(tmp_template); return NULL; }

    char buf[2048] = {0};
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        unlink(tmp_template);
        return NULL;
    }
    fclose(f);
    unlink(tmp_template);

    trim_newlines(buf);
    if (buf[0] == '\0') return NULL;
    return strdup(buf);
}

static void add_buffer_from_file(ViewerState *st, const char *path) {
    if (!st) return;
    if (path && *path) {
        (void)add_buffer_from_path(st, path);
        return;
    }
    set_status(st, "No path provided");
}

// --- Buffers list picker: nfzf -> fzf -> manual fallback ---
static void cmd_list_buffers(ViewerState *st) {
    if (!st) return;

    int have_nfzf = check_command_exists("ff");
    int have_fzf  = check_command_exists("fzf");

    if (!have_nfzf && !have_fzf) {
        manual_buffer_list_fallback(st);
        return;
    }

    char list_template[] = "/tmp/nbl_vic_buflist_XXXXXX";
    int fd = mkstemp(list_template);
    if (fd < 0) { set_status(st, "Failed to create temp file"); return; }

    FILE *list_file = fdopen(fd, "w");
    if (!list_file) {
        close(fd);
        unlink(list_template);
        set_status(st, "Failed to open temp file");
        return;
    }

    for (int i = 0; i < st->buffer_count; i++) {
        Buffer *b = &st->buffers[i];
        const char *indicator = (i == st->current_buffer) ? "*" : " ";
        const char *modified  = b->dirty ? "[+]" : "   ";
        const char *name      = basename_path(b->filepath);

        fprintf(list_file, "%d|%s %s %s (%d lines)\n",
                i + 1, indicator, modified, name, b->line_count);
    }
    fflush(list_file);
    fclose(list_file);

    char result_template[] = "/tmp/nbl_vic_bufpick_XXXXXX";
    int result_fd = mkstemp(result_template);
    if (result_fd < 0) {
        unlink(list_template);
        set_status(st, "Failed to create result file");
        return;
    }
    close(result_fd);

    char cmd[8192];
    if (have_nfzf) {
        snprintf(cmd, sizeof(cmd),
                 "ff < \"%s\" > \"%s\" 2> /dev/null",
                 list_template, result_template);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "fzf --height=80%% --layout=reverse --border "
                 "--header='Select buffer (ESC to cancel)' "
                 "< \"%s\" > \"%s\" 2> /dev/tty",
                 list_template, result_template);
    }

    def_prog_mode();
    endwin();
    int rc = system(cmd);
    reset_prog_mode();
    refresh();

    unlink(list_template);

    if (rc != 0) {
        unlink(result_template);
        return;
    }

    FILE *result_file = fopen(result_template, "r");
    if (!result_file) {
        unlink(result_template);
        set_status(st, "Failed to read selection");
        return;
    }

    char buf[2048] = {0};
    if (!fgets(buf, sizeof(buf), result_file)) {
        fclose(result_file);
        unlink(result_template);
        return;
    }
    fclose(result_file);
    unlink(result_template);

    int buffer_num = atoi(buf);
    if (buffer_num >= 1 && buffer_num <= st->buffer_count) {
        st->current_buffer = buffer_num - 1;
        st->cursor_line = 0;
        st->cursor_col  = 0;
        set_status(st, "Switched buffer");
    } else {
        set_status(st, "Invalid buffer");
    }
}

static int write_buffer_to_path(Buffer *b, const char *path) {
    if (!b || !path || !*path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (int i = 0; i < b->line_count; i++) {
        fputs(b->lines[i], f);
        if (i != b->line_count - 1) fputc('\n', f);
    }
    fclose(f);
    return 0;
}

static void cmd_write(ViewerState *st, const char *arg) {
    Buffer *b = &st->buffers[st->current_buffer];
    char target[1024] = {0};

    if (arg && *arg) {
        snprintf(target, sizeof(target), "%s", arg);
    } else {
        if (b->filepath[0] == '\0' || strcmp(b->filepath, "<stdin>") == 0 || b->filepath[0] == '[') {
            set_status(st, "No file name (use :w path)");
            return;
        }
        snprintf(target, sizeof(target), "%s", b->filepath);
    }

    if (write_buffer_to_path(b, target) == 0) {
        if (b->filepath[0] == '\0' || strcmp(b->filepath, "<stdin>") == 0 || b->filepath[0] == '[') {
            snprintf(b->filepath, sizeof(b->filepath), "%s", target);
            b->lang = detect_language(b->filepath);
        }
        b->dirty = 0;
        set_status(st, "Wrote file");
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "Write failed: %s", strerror(errno));
        set_status(st, msg);
    }
}

static void visual_copy_to_clipboard(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    int a = st->vis_start;
    int c = st->vis_end;
    int lo = (a < c) ? a : c;
    int hi = (a > c) ? a : c;

    if (lo < 0) lo = 0;
    if (hi >= b->line_count) hi = b->line_count - 1;

    size_t total = 0;
    for (int i = lo; i <= hi; i++) total += strlen(b->lines[i]) + 1;

    char *out = (char*)malloc(total + 1);
    if (!out) return;

    out[0] = '\0';
    for (int i = lo; i <= hi; i++) {
        strcat(out, b->lines[i]);
        if (i != hi) strcat(out, "\n");
    }

    clipboard_copy_text(out);
    free(out);
    set_status(st, "Copied to clipboard");
}

static const char *mode_name(InputMode m) {
    switch (m) {
        case MODE_INSERT: return "INSERT";
        case MODE_VISUAL: return "VISUAL";
        case MODE_COMMAND: return "COMMAND";
        default: return "NORMAL";
    }
}

static void draw_status_bar(ViewerState *st) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);
    Buffer *b = &st->buffers[st->current_buffer];

    attron(COLOR_PAIR(COLOR_NORMAL));
    mvhline(max_y - 2, 0, ACS_HLINE, max_x);
    attroff(COLOR_PAIR(COLOR_NORMAL));

    move(max_y - 1, 0);
    clrtoeol();

    attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);

    const char *name = basename_path(b->filepath);
    int percent = b->line_count > 0 ? (b->scroll_offset * 100) / b->line_count : 0;

    char left[768];
    snprintf(left, sizeof(left),
             "NBL VIC | %s [%d/%d] | %s | %d%% | %d/%d | (%d,%d) | L:%s W:%s%s",
             name,
             st->current_buffer + 1, st->buffer_count,
             mode_name(st->mode),
             percent,
             st->cursor_line + 1, b->line_count,
             st->cursor_line + 1, st->cursor_col + 1,
             st->show_line_numbers ? "ON" : "OFF",
             st->wrap_enabled ? "ON" : "OFF",
             b->dirty ? " | +modified" : "");

    mvprintw(max_y - 1, 1, "%.*s", max_x - 2, left);

    if (st->mode == MODE_COMMAND) {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), ":%s", st->cmdline);

        int left_len = (int)strlen(left);
        int left_end_x = 1 + left_len;
        int right_margin = 1;
        int max_right_end = max_x - right_margin;
        int gap = 1;
        int avail = max_right_end - (left_end_x + gap) + 1;

        if (avail > 0) {
            int cmdlen = (int)strlen(cmd);
            const char *view = cmd;
            int viewlen = cmdlen;
            if (viewlen > avail) {
                view = cmd + (viewlen - avail);
                viewlen = avail;
            }
            int start_x = max_right_end - viewlen + 1;
            int min_x = left_end_x + gap;
            if (start_x < min_x) start_x = min_x;
            mvprintw(max_y - 1, start_x, "%.*s", viewlen, view);
        }
    } else if (st->status_ticks > 0 && st->status_msg[0]) {
        mvprintw(max_y - 1, max_x - (int)strlen(st->status_msg) - 2, "%s", st->status_msg);
    } else if (st->search_highlight && st->search_term[0] != '\0') {
        char right[256];
        snprintf(right, sizeof(right), "Search: \"%s\" [%d/%d] ",
                 st->search_term, st->current_match + 1, st->search_match_count);
        mvprintw(max_y - 1, max_x - (int)strlen(right) - 2, "%s", right);
    }

    attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
}

static void draw_buffer(ViewerState *st) {
    Buffer *b = &st->buffers[st->current_buffer];
    int max_x = getmaxx(stdscr);
    int h = content_height();
    int line_nr_width = line_nr_width_for(st);
    int do_search_hl = (st->search_highlight && st->search_term[0] != '\0');

    for (int y = 0; y < h; y++) {
        move(y, 0);
        clrtoeol();
    }

    if (st->wrap_enabled) {
        int y = 0;
        int logical = b->scroll_offset;
        int text_width = text_width_for(st);

        while (y < h && logical < b->line_count) {
            WrappedLine wl = wrap_line(b->lines[logical], text_width);
            for (int seg = 0; seg < wl.count && y < h; seg++) {
                if (st->show_line_numbers && seg == 0) {
                    attron(COLOR_PAIR(COLOR_LINENR));
                    mvprintw(y, 1, "%4d ", logical + 1);
                    attroff(COLOR_PAIR(COLOR_LINENR));
                } else if (st->show_line_numbers) {
                    mvprintw(y, 1, "     ");
                }

                int in_sel = 0;
                if (st->mode == MODE_VISUAL) {
                    int lo = st->vis_start < st->vis_end ? st->vis_start : st->vis_end;
                    int hi = st->vis_start > st->vis_end ? st->vis_start : st->vis_end;
                    in_sel = (logical >= lo && logical <= hi);
                }

                if (in_sel) attron(COLOR_PAIR(COLOR_COPY_SELECT) | A_REVERSE);
                highlight_line(wl.segments[seg], b->lang, y, line_nr_width + 1, max_x,
                               st->search_term, do_search_hl);
                if (in_sel) attroff(COLOR_PAIR(COLOR_COPY_SELECT) | A_REVERSE);

                y++;
            }
            free_wrapped_line(&wl);
            logical++;
        }
    } else {
        for (int i = 0; i < h; i++) {
            int line_idx = b->scroll_offset + i;
            if (line_idx >= b->line_count) continue;

            if (st->show_line_numbers) {
                attron(COLOR_PAIR(COLOR_LINENR));
                mvprintw(i, 1, "%4d ", line_idx + 1);
                attroff(COLOR_PAIR(COLOR_LINENR));
            }

            int in_sel = 0;
            if (st->mode == MODE_VISUAL) {
                int lo = st->vis_start < st->vis_end ? st->vis_start : st->vis_end;
                int hi = st->vis_start > st->vis_end ? st->vis_start : st->vis_end;
                in_sel = (line_idx >= lo && line_idx <= hi);
            }

            if (in_sel) attron(COLOR_PAIR(COLOR_COPY_SELECT) | A_REVERSE);
            highlight_line(b->lines[line_idx], b->lang, i, line_nr_width + 1, max_x,
                           st->search_term, do_search_hl);
            if (in_sel) attroff(COLOR_PAIR(COLOR_COPY_SELECT) | A_REVERSE);
        }
    }
}

static void cursor_to_screen(ViewerState *st, int *out_y, int *out_x) {
    Buffer *b = &st->buffers[st->current_buffer];
    int max_x = getmaxx(stdscr);
    int h = content_height();
    int line_nr_width = line_nr_width_for(st);
    int y = 0;
    int x = 0;

    if (!st->wrap_enabled) {
        y = st->cursor_line - b->scroll_offset;
        if (y < 0) y = 0;
        if (y >= h) y = h - 1;
        x = line_nr_width + 1 + st->cursor_col;
    } else {
        int w = text_width_for(st);
        int row = 0;
        for (int L = b->scroll_offset; L < st->cursor_line && L < b->line_count; L++) {
            row += wrapped_rows_for_line(st, b->lines[L]);
        }
        int seg = (w > 0) ? (st->cursor_col / w) : 0;
        int segcol = (w > 0) ? (st->cursor_col % w) : 0;
        row += seg;
        y = row;
        if (y < 0) y = 0;
        if (y >= h) y = h - 1;
        x = line_nr_width + 1 + segcol;
    }

    if (x < line_nr_width + 1) x = line_nr_width + 1;
    if (x >= max_x) x = max_x - 1;

    *out_y = y;
    *out_x = x;
}

static void draw_ui(ViewerState *st) {
    draw_buffer(st);
    draw_status_bar(st);
    if (st->status_ticks > 0) st->status_ticks--;

    int cy, cx;
    cursor_to_screen(st, &cy, &cx);

    if (st->mode == MODE_COMMAND) {
        int max_y = getmaxy(stdscr);
        int y = max_y - 1;
        int x = 2 + st->cmdlen; // ":" at x=1 visually
        if (x >= getmaxx(stdscr)) x = getmaxx(stdscr) - 1;
        move(y, x);
    } else {
        move(cy, cx);
    }
    refresh();
}

static void cmdhist_add(ViewerState *st, const char *cmd) {
    if (!st || !cmd) return;
    while (*cmd && isspace((unsigned char)*cmd)) cmd++;
    if (*cmd == '\0') return;

    if (st->cmdhist_len > 0 && strcmp(st->cmdhist[st->cmdhist_len - 1], cmd) == 0) {
        st->cmdhist_pos = st->cmdhist_len;
        return;
    }

    if (st->cmdhist_len == CMDHIST_MAX) {
        free(st->cmdhist[0]);
        memmove(&st->cmdhist[0], &st->cmdhist[1], sizeof(char*) * (CMDHIST_MAX - 1));
        st->cmdhist_len--;
    }

    st->cmdhist[st->cmdhist_len++] = strdup(cmd);
    st->cmdhist_pos = st->cmdhist_len;
}

static void cmdhist_prev(ViewerState *st) {
    if (st->cmdhist_len == 0) return;
    if (st->cmdhist_pos <= 0) st->cmdhist_pos = 0;
    else st->cmdhist_pos--;
    snprintf(st->cmdline, sizeof(st->cmdline), "%s", st->cmdhist[st->cmdhist_pos]);
    st->cmdlen = (int)strlen(st->cmdline);
}

static void cmdhist_next(ViewerState *st) {
    if (st->cmdhist_len == 0) return;
    if (st->cmdhist_pos >= st->cmdhist_len - 1) {
        st->cmdhist_pos = st->cmdhist_len;
        st->cmdline[0] = '\0';
        st->cmdlen = 0;
        return;
    }
    st->cmdhist_pos++;
    snprintf(st->cmdline, sizeof(st->cmdline), "%s", st->cmdhist[st->cmdhist_pos]);
    st->cmdlen = (int)strlen(st->cmdline);
}

static int any_dirty(ViewerState *st) {
    for (int i = 0; i < st->buffer_count; i++) {
        if (st->buffers[i].dirty) return 1;
    }
    return 0;
}

static int dirty_count(ViewerState *st) {
    int n = 0;
    for (int i = 0; i < st->buffer_count; i++) if (st->buffers[i].dirty) n++;
    return n;
}

static void exec_command(ViewerState *st, const char *cmd, int *running) {
    if (!cmd) return;
    while (*cmd && isspace((unsigned char)*cmd)) cmd++;
    if (*cmd == '\0') return;

    Buffer *b = &st->buffers[st->current_buffer];

    if (isdigit((unsigned char)cmd[0])) {
        long n = strtol(cmd, NULL, 10);
        if (n > 0) {
            st->cursor_line = (int)n - 1;
            st->cursor_col = 0;
            ensure_cursor_bounds(st);
            ensure_cursor_visible(st);
        }
        return;
    }

    char tok[64] = {0};
    int ti = 0;
    const char *p = cmd;
    while (*p && !isspace((unsigned char)*p) && ti < (int)sizeof(tok)-1) tok[ti++] = *p++;
    tok[ti] = '\0';
    while (*p && isspace((unsigned char)*p)) p++;

    if (strcmp(tok, "qa") == 0 || strcmp(tok, "qall") == 0) {
        if (!any_dirty(st)) { *running = 0; return; }
        int n = dirty_count(st);
        char msg[128];
        snprintf(msg, sizeof(msg), "%d modified buffer%s (use :qa! to quit anyway)", n, (n==1?"":"s"));
        set_status(st, msg);
        return;
    }
    if (strcmp(tok, "?") == 0) { cmd_show_help(st); return; }
    if (strcmp(tok, "qa!") == 0 || strcmp(tok, "qall!") == 0) { *running = 0; return; }

    if (strcmp(tok, "q") == 0) {
        Buffer *cur = &st->buffers[st->current_buffer];
        if (!cur->dirty) { close_current_buffer(st); return; }
        set_status(st, "No write since last change (use :q! to quit, :w to save)");
        return;
    }
    if (strcmp(tok, "q!") == 0) { close_current_buffer(st); return; }
    if (strcmp(tok, "w") == 0)  { cmd_write(st, p); return; }
    if (strcmp(tok, "noh") == 0 || strcmp(tok, "nohlsearch") == 0) { clear_search(st); set_status(st,"noh"); return; }

    if (strcmp(tok, "p") == 0) {
        char *clip = clipboard_paste_text();
        if (!clip) { set_status(st, "Clipboard empty/unavailable"); return; }
        paste_text_at_cursor(st, clip);
        free(clip);
        set_status(st, "Pasted");
        return;
    }

    if (strcmp(tok, "ls") == 0 || strcmp(tok, "buffers") == 0) { cmd_list_buffers(st); return; }

    if (strcmp(tok, "b") == 0) {
        // :b new
        if (strncmp(p, "new", 3) == 0 && (p[3] == '\0' || isspace((unsigned char)p[3]))) {
            add_blank_buffer(st);
            return;
        }

        // :b n
        if (strncmp(p, "n", 1) == 0 && (p[1] == '\0' || isspace((unsigned char)p[1]))) {
            next_buffer(st);
            return;
        }

        // :b p
        if (strncmp(p, "p", 1) == 0 && (p[1] == '\0' || isspace((unsigned char)p[1]))) {
            prev_buffer(st);
            return;
        }

        // :b add / :b a
        if (strncmp(p, "add", 3) == 0 || (strncmp(p, "a", 1) == 0 && (p[1] == '\0' || isspace((unsigned char)p[1])))) {
            char *picked = pick_file();
            if (!picked) {
                set_status(st, "No file selected");
                return;
            }
            add_buffer_from_file(st, picked);
            free(picked);
            return;
        }

        // :b <num>
        if (*p && isdigit((unsigned char)*p)) {
            long n = strtol(p, NULL, 10);
            if (n >= 1 && n <= st->buffer_count) {
                st->current_buffer = (int)n - 1;
                st->cursor_line = 0;
                st->cursor_col = 0;
                set_status(st, "Switched buffer");
            } else set_status(st, "Bad buffer number");
            return;
        }

        set_status(st, "Usage: :b new|n|p|add|a|<num>");
        return;
    }

    (void)b;
    set_status(st, "Unknown command");
}

static void enter_command_mode(ViewerState *st) {
    st->mode = MODE_COMMAND;
    st->cmdline[0] = '\0';
    st->cmdlen = 0;
    st->cmdhist_pos = st->cmdhist_len;
}

static void exit_command_mode(ViewerState *st) {
    st->mode = MODE_NORMAL;
    st->cmdline[0] = '\0';
    st->cmdlen = 0;
}

static void cmd_show_help(ViewerState *st) {
    int have_nfzf = check_command_exists("ff");
    int have_fzf = check_command_exists("fzf");

    if (!have_nfzf && !have_fzf) {
        set_status(st, "fzf or ff required for help menu");
        return;
    }

    char help_template[] = "/tmp/nbl_vic_help_XXXXXX";
    int fd = mkstemp(help_template);
    if (fd < 0) {
        set_status(st, "Failed to create temp file");
        return;
    }

    FILE *help_file = fdopen(fd, "w");
    if (!help_file) {
        close(fd);
        unlink(help_template);
        set_status(st, "Failed to open temp file");
        return;
    }

    fprintf(help_file, "=== NAVIGATION ===\n");
    fprintf(help_file, "h / LEFT        | Move cursor left\n");
    fprintf(help_file, "j / DOWN        | Move cursor down\n");
    fprintf(help_file, "k / UP          | Move cursor up\n");
    fprintf(help_file, "l / RIGHT       | Move cursor right\n");
    fprintf(help_file, "gg              | Jump to first line\n");
    fprintf(help_file, "G               | Jump to last line\n");
    fprintf(help_file, "%%               | Jump to matching bracket\n");
    fprintf(help_file, "Ctrl+D          | Half page down\n");
    fprintf(help_file, "Ctrl+U          | Half page up\n");
    fprintf(help_file, "Ctrl+E          | Scroll down one line\n");
    fprintf(help_file, "Ctrl+Y          | Scroll up one line\n");
    fprintf(help_file, "\n");
    fprintf(help_file, "=== EDITING ===\n");
    fprintf(help_file, "i               | Enter insert mode\n");
    fprintf(help_file, "ESC             | Exit insert mode / clear search\n");
    fprintf(help_file, "u               | Undo\n");
    fprintf(help_file, "Ctrl+R          | Redo\n");
    fprintf(help_file, "yy              | Yank (copy) current line\n");
    fprintf(help_file, "y%%              | Yank to matching bracket\n");
    fprintf(help_file, "dd              | Delete current line\n");
    fprintf(help_file, "d%%              | Delete to matching bracket\n");
    fprintf(help_file, "p               | Paste from clipboard\n");
    fprintf(help_file, "p (in visual)   | Replace selection with clipboard\n");
    fprintf(help_file, "\n");
    fprintf(help_file, "=== VISUAL MODE ===\n");
    fprintf(help_file, "V               | Enter visual line mode\n");
    fprintf(help_file, "y (in visual)   | Yank selected lines\n");
    fprintf(help_file, "d (in visual)   | Delete selected lines\n");
    fprintf(help_file, "p (in visual)   | Replace selected lines with clipboard\n");
    fprintf(help_file, "ESC (in visual) | Exit visual mode\n");
    fprintf(help_file, "\n");
    fprintf(help_file, "=== SEARCH ===\n");
    fprintf(help_file, "/               | Search forward\n");
    fprintf(help_file, "n               | Next search match\n");
    fprintf(help_file, "N               | Previous search match\n");
    fprintf(help_file, ":noh            | Clear search highlighting\n");
    fprintf(help_file, "\n");
    fprintf(help_file, "=== ALL-LINES OPERATIONS ===\n");
    fprintf(help_file, "%%y              | Yank all lines to clipboard\n");
    fprintf(help_file, "%%d              | Delete all lines (yanks first)\n");
    fprintf(help_file, "\n");
    fprintf(help_file, "=== BUFFERS ===\n");
    fprintf(help_file, ":ls             | List and switch buffers (fzf)\n");
    fprintf(help_file, ":b new          | Create new blank buffer\n");
    fprintf(help_file, ":b add / :b a   | Add file to buffer (fzf picker)\n");
    fprintf(help_file, ":b n            | Next buffer\n");
    fprintf(help_file, ":b p            | Previous buffer\n");
    fprintf(help_file, ":b <num>        | Switch to buffer number\n");
    fprintf(help_file, "gt              | Next buffer (when g pending)\n");
    fprintf(help_file, "gT              | Previous buffer (when g pending)\n");
    fprintf(help_file, "x               | Close current buffer\n");
    fprintf(help_file, "\n");
    fprintf(help_file, "=== FILE OPERATIONS ===\n");
    fprintf(help_file, ":w              | Write current buffer to file\n");
    fprintf(help_file, ":w <path>       | Write to specific path\n");
    fprintf(help_file, ":q              | Quit current buffer (warns if dirty)\n");
    fprintf(help_file, ":q!             | Force quit current buffer\n");
    fprintf(help_file, ":qa             | Quit all buffers (warns if dirty)\n");
    fprintf(help_file, ":qa!            | Force quit all buffers\n");
    fprintf(help_file, "\n");
    fprintf(help_file, "=== SETTINGS ===\n");
    fprintf(help_file, "L               | Toggle line numbers\n");
    fprintf(help_file, "T               | Toggle line wrapping\n");
    fprintf(help_file, "\n");
    fprintf(help_file, "=== OTHER ===\n");
    fprintf(help_file, ":p              | Paste from clipboard\n");
    fprintf(help_file, ":<num>          | Jump to line number\n");
    fprintf(help_file, ":?              | Show this help\n");
    fprintf(help_file, "q               | Quit (in normal mode)\n");

    fflush(help_file);
    fclose(help_file);

    char cmd[8192];
    if (have_nfzf) {
        snprintf(cmd, sizeof(cmd),
                 "ff < \"%s\" > /dev/null 2>/dev/null",
                 help_template);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "fzf --height=100%% --layout=reverse --border "
                 "--header='Help - Search commands (ESC to close)' "
                 "< \"%s\" > /dev/null 2> /dev/tty",
                 help_template);
    }

    def_prog_mode();
    endwin();
    system(cmd);
    reset_prog_mode();
    refresh();

    unlink(help_template);
}

static void handle_command_key(ViewerState *st, int ch, int *running) {
    if (ch == 27) { exit_command_mode(st); return; }
    if (ch == KEY_UP) { cmdhist_prev(st); return; }
    if (ch == KEY_DOWN) { cmdhist_next(st); return; }

    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (st->cmdlen > 0) st->cmdline[--st->cmdlen] = '\0';
        return;
    }

    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        cmdhist_add(st, st->cmdline);
        char cmdcopy[512];
        snprintf(cmdcopy, sizeof(cmdcopy), "%s", st->cmdline);
        exit_command_mode(st);
        exec_command(st, cmdcopy, running);
        return;
    }

    if (isprint(ch)) {
        if (st->cmdlen < (int)sizeof(st->cmdline) - 1) {
            st->cmdline[st->cmdlen++] = (char)ch;
            st->cmdline[st->cmdlen] = '\0';
        }
    }
}

static void prompt_search(ViewerState *st) {
    int max_y = getmaxy(stdscr);
    move(max_y - 1, 0);
    clrtoeol();
    attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    mvprintw(max_y - 1, 1, "/");
    attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    refresh();

    echo();
    char input[256] = {0};
    getnstr(input, sizeof(input) - 1);
    noecho();

    int len = (int)strlen(input);
    while (len > 0 && isspace((unsigned char)input[len-1])) input[--len] = '\0';
    if (input[0] == '\0') return;

    snprintf(st->search_term, sizeof(st->search_term), "%s", input);
    st->search_highlight = 1;
    find_all_matches(st);
    jump_to_first_match(st);
}
static inline void insert_undo_maybe_push(ViewerState *st, Buffer *b) {
    if (!st->insert_undo_armed) {
        undo_push(b);
        st->insert_undo_armed = 1;
    }
}
static void handle_insert_key(ViewerState *st, int ch) {
    Buffer *b = &st->buffers[st->current_buffer];

    if (ch == KEY_LEFT)  { move_left(st);  return; }
    if (ch == KEY_RIGHT) { move_right(st); return; }
    if (ch == KEY_UP)    { move_up(st);    return; }
    if (ch == KEY_DOWN)  { move_down(st);  return; }

    if (ch == 27) { // ESC
        st->mode = MODE_NORMAL;
        st->insert_undo_armed = 0;
        return;
    }

    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        insert_undo_maybe_push(st, b);
        delete_char_before(b, &st->cursor_line, &st->cursor_col);
        ensure_cursor_bounds(st);
        return;
    }

    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        insert_undo_maybe_push(st, b);
        insert_newline(b, &st->cursor_line, &st->cursor_col);
        ensure_cursor_bounds(st);
        return;
    }

    if (ch == '\t') {
        insert_undo_maybe_push(st, b);
        for (int i = 0; i < 4; i++) {
            insert_char_at(b, st->cursor_line, st->cursor_col, ' ');
            st->cursor_col++;
        }
        return;
    }

    if (isprint(ch)) {
        insert_undo_maybe_push(st, b);
        insert_char_at(b, st->cursor_line, st->cursor_col, (char)ch);
        st->cursor_col++;
        return;
    }
}
static void handle_input(ViewerState *st, int *running) {
    int ch = getch();

    if (st->mode == MODE_COMMAND) {
        handle_command_key(st, ch, running);
        return;
    }

    if (st->mode != MODE_INSERT && ch == ':') {
        enter_command_mode(st);
        return;
    }

    if (st->mode == MODE_INSERT) {
        handle_insert_key(st, ch);
        ensure_cursor_visible(st);
        return;
    }

    Buffer *b = &st->buffers[st->current_buffer];

    // -------------------------------------------------------
    // VISUAL MODE
    // -------------------------------------------------------
    if (st->mode == MODE_VISUAL) {
        if (ch == 27) {
            st->mode = MODE_NORMAL;
            st->op_pending = OP_NONE;
            st->percent_pending = 0;
            return;
        }

        // [CHANGE 3] d in visual: delete the selected lines
        if (ch == 'd') {
            char *deleted = delete_visual_lines(st);
            if (deleted) {
                clipboard_copy_text(deleted);
                free(deleted);
            }
            st->mode = MODE_NORMAL;
            st->op_pending = OP_NONE;
            ensure_cursor_visible(st);
            set_status(st, "Deleted selection");
            return;
        }

        // [CHANGE 2] p in visual: replace selection with clipboard
        if (ch == 'p') {
            char *clip = clipboard_paste_text();
            // Delete selection first (this also pushes undo)
            char *deleted = delete_visual_lines(st);
            free(deleted);
            st->mode = MODE_NORMAL;
            st->op_pending = OP_NONE;
            if (clip) {
                // paste_text_at_cursor calls undo_push internally; since we
                // already pushed in delete_visual_lines, collapse to one step
                // by inserting directly without a second undo snapshot.
                for (const char *p = clip; *p; p++) {
                    if (*p == '\n') insert_newline(b, &st->cursor_line, &st->cursor_col);
                    else {
                        insert_char_at(b, st->cursor_line, st->cursor_col, *p);
                        st->cursor_col++;
                    }
                }
                free(clip);
                b->dirty = 1;
            }
            ensure_cursor_visible(st);
            set_status(st, "Replaced selection");
            return;
        }

        if (ch == 'y') {
            visual_copy_to_clipboard(st);
            st->mode = MODE_NORMAL;
            st->op_pending = OP_NONE;
            return;
        }
        // Handle movement in visual mode to update selection
        if (ch == 'j' || ch == KEY_DOWN) {
            move_down(st);
            st->vis_end = st->cursor_line;
            ensure_cursor_visible(st);
            return;
        }
        if (ch == 'k' || ch == KEY_UP) {
            move_up(st);
            st->vis_end = st->cursor_line;
            ensure_cursor_visible(st);
            return;
        }
        if (ch == 'g') {
            st->cursor_line = 0;
            st->cursor_col = 0;
            st->vis_end = st->cursor_line;
            ensure_cursor_visible(st);
            return;
        }
        if (ch == 'G') {
            st->cursor_line = b->line_count - 1;
            st->cursor_col = 0;
            st->vis_end = st->cursor_line;
            ensure_cursor_visible(st);
            return;
        }
        return; // swallow unhandled keys in visual mode
    }

    // -------------------------------------------------------
    // NORMAL MODE — g-prefix
    // -------------------------------------------------------
    if (st->g_pending) {
        st->g_pending = 0;
        if (ch == 'g') { st->cursor_line = 0; st->cursor_col = 0; ensure_cursor_visible(st); return; }
        if (ch == 't') { next_buffer(st); return; }
        if (ch == 'T') { prev_buffer(st); return; }
        return;
    }

    // -------------------------------------------------------
    // NORMAL MODE — %-prefix  (%y = yank all, %d = delete all)
    // -------------------------------------------------------
    // [CHANGE 4/5]
    if (st->percent_pending) {
        st->percent_pending = 0;
        if (ch == 'y') { yank_all_lines(st); return; }
        if (ch == 'd') { delete_all_lines(st); ensure_cursor_visible(st); return; }
        // Not a recognised follow-up: fall through and treat % as bracket jump
        // (handled below in op_pending or switch)
    }

    // -------------------------------------------------------
    // NORMAL MODE — operator pending (y / d already pressed)
    // -------------------------------------------------------
    if (st->op_pending != OP_NONE) {
        if (ch == 27) { st->op_pending = OP_NONE; return; }

        // Handle yy (yank line)
        if (ch == 'y' && st->op_pending == OP_YANK) {
            clipboard_copy_text(b->lines[st->cursor_line]);
            set_status(st, "Yanked line");
            st->op_pending = OP_NONE;
            return;
        }

        // Handle dd (delete line)
        if (ch == 'd' && st->op_pending == OP_DELETE) {
            clipboard_copy_text(b->lines[st->cursor_line]);
            undo_push(b);
            free(b->lines[st->cursor_line]);
            for (int i = st->cursor_line; i < b->line_count - 1; i++) {
                b->lines[i] = b->lines[i + 1];
            }
            b->lines[b->line_count - 1] = NULL;
            b->line_count--;
            if (b->line_count <= 0) {
                b->line_count = 1;
                b->lines[0] = strdup("");
                st->cursor_line = 0;
            }
            if (st->cursor_line >= b->line_count) {
                st->cursor_line = b->line_count - 1;
            }
            st->cursor_col = 0;
            b->dirty = 1;
            set_status(st, "Deleted line");
            st->op_pending = OP_NONE;
            ensure_cursor_visible(st);
            return;
        }

        if (ch == '%') {
            int toL,toC,fromL,fromC;
            if (jump_percent(st, &toL,&toC,&fromL,&fromC)) {
                if (st->op_pending == OP_YANK) {
                    delete_range_to_match(st, fromL, fromC, toL, toC, 1);
                    set_status(st, "Yanked %");
                } else if (st->op_pending == OP_DELETE) {
                    delete_range_to_match(st, fromL, fromC, toL, toC, 1);
                    set_status(st, "Deleted %");
                }
            } else {
                set_status(st, "No match");
            }
            st->op_pending = OP_NONE;
            ensure_cursor_visible(st);
            return;
        }
        st->op_pending = OP_NONE;
    }

    // -------------------------------------------------------
    // NORMAL MODE — main key dispatch
    // -------------------------------------------------------
    switch (ch) {
        case 'q': *running = 0; return;
        case 27: clear_search(st); st->op_pending = OP_NONE; st->percent_pending = 0; set_status(st, "noh"); return;
        case 'x': close_current_buffer(st); return;
        case 'i':
            st->mode = MODE_INSERT;
            st->insert_undo_armed = 0;
            return;

        case 'V':
            st->mode = MODE_VISUAL;
            st->vis_start = st->cursor_line;
            st->vis_end = st->cursor_line;
            return;
        case '/': prompt_search(st); ensure_cursor_visible(st); return;
        case 'n': next_match(st); ensure_cursor_visible(st); return;
        case 'N': prev_match(st); ensure_cursor_visible(st); return;
        case 'u': do_undo(st); ensure_cursor_visible(st); return;
        case 't': {
            char cwd[1024];
            get_cwd(cwd, sizeof(cwd));
            def_prog_mode();
            endwin();
            tmux_toggle_terminal(cwd);
            reset_prog_mode();
            refresh();
            return;
        }
        case 'D': {
            char cwd[1024];
            get_cwd(cwd, sizeof(cwd));
            def_prog_mode();
            endwin();
            tmux_toggle_db(cwd);
            reset_prog_mode();
            refresh();
            return;
        }
        case 'P': {
            char cwd[1024];
            get_cwd(cwd, sizeof(cwd));
            def_prog_mode();
            endwin();
            tmux_toggle_peek(cwd);
            reset_prog_mode();
            refresh();
            return;
        }
        case '!': {
            char cwd[1024];
            get_cwd(cwd, sizeof(cwd));
            def_prog_mode();
            endwin();
            tmux_toggle_lldb(cwd);
            reset_prog_mode();
            refresh();
            return;
        }
        case 18:  do_redo(st); ensure_cursor_visible(st); return; // Ctrl+R
        case 'p': {
            char *clip = clipboard_paste_text();
            if (!clip) { set_status(st, "Clipboard empty/unavailable"); return; }
            paste_text_at_cursor(st, clip);
            free(clip);
            ensure_cursor_visible(st);
            return;
        }
        case 'g': st->g_pending = 1; return;
        case 'G':
            st->cursor_line = b->line_count - 1;
            st->cursor_col = 0;
            ensure_cursor_visible(st);
            return;
        case 'h':
        case KEY_LEFT:
            st->free_scroll = 0;
            move_left(st);
            ensure_cursor_visible(st);
            return;
        case 'l':
        case KEY_RIGHT:
            st->free_scroll = 0;
            move_right(st);
            ensure_cursor_visible(st);
            return;
        case 'k':
        case KEY_UP:
            st->free_scroll = 0;
            move_up(st);
            ensure_cursor_visible(st);
            return;
        case 'j':
        case KEY_DOWN:
            st->free_scroll = 0;
            move_down(st);
            ensure_cursor_visible(st);
            return;
        case 'L': st->show_line_numbers = !st->show_line_numbers; return;
        case 'T': st->wrap_enabled = !st->wrap_enabled; ensure_cursor_visible(st); return;
        case 5:  // Ctrl+E
            st->free_scroll = 1;
            scroll_viewport(st, +1);
            return;
        case 25: // Ctrl+Y
            st->free_scroll = 1;
            scroll_viewport(st, -1);
            return;
        case 'y':
            st->op_pending = OP_YANK;
            st->op_start_line = st->cursor_line;
            st->op_start_col  = st->cursor_col;
            return;
        case 'd':
            st->op_pending = OP_DELETE;
            st->op_start_line = st->cursor_line;
            st->op_start_col  = st->cursor_col;
            return;
        // [CHANGE 4/5] '%' in normal mode: set percent_pending so the next
        // key ('y' or 'd') can trigger yank-all / delete-all.  If the next
        // key is something else, fall through to the bracket-jump behaviour.
        case '%': {
            st->percent_pending = 1;
            return;
        }
        default:
            return;
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [--no-wrap] <file1> [file2 ...]\n"
        "  %s -           (read from stdin)\n",
        prog, prog
    );
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    ViewerState *st = (ViewerState*)calloc(1, sizeof(ViewerState));
    if (!st) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }

    st->show_line_numbers = 1;
    st->wrap_enabled = 1;
    st->mode = MODE_NORMAL;
    st->g_pending = 0;
    st->percent_pending = 0;
    st->search_highlight = 0;
    st->op_pending = OP_NONE;

    int stdin_is_pipe = !isatty(STDIN_FILENO);
    int loaded_anything = 0;
    int arg_start = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-wrap") == 0) {
            st->wrap_enabled = 0;
            arg_start = i + 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            free(st);
            return 0;
        } else break;
    }

    int effective_argc = argc - (arg_start - 1);
    if (effective_argc < 2) {
        if (!stdin_is_pipe) {
            usage(argv[0]);
            free(st);
            return 1;
        }
        if (load_stdin(&st->buffers[st->buffer_count]) == 0) {
            st->buffer_count++;
            loaded_anything = 1;
        } else {
            fprintf(stderr, "No data on stdin\n");
            free(st);
            return 1;
        }
    } else {
        for (int i = arg_start; i < argc && st->buffer_count < MAX_BUFFERS; i++) {
            if (strcmp(argv[i], "-") == 0) {
                if (load_stdin(&st->buffers[st->buffer_count]) == 0) {
                    st->buffer_count++;
                    loaded_anything = 1;
                }
                continue;
            }
            if (load_file(&st->buffers[st->buffer_count], argv[i]) == 0) {
                st->buffer_count++;
                loaded_anything = 1;
            } else {
                fprintf(stderr, "Failed to load %s\n", argv[i]);
            }
        }
    }

    if (!loaded_anything || st->buffer_count == 0) {
        fprintf(stderr, "Failed to load anything\n");
        free(st);
        return 1;
    }

    FILE *tty_in = NULL;
    SCREEN *screen = NULL;

    if (stdin_is_pipe) {
        tty_in = fopen("/dev/tty", "r");
        if (!tty_in) {
            fprintf(stderr, "Failed to open /dev/tty: %s\n", strerror(errno));
            free(st);
            return 1;
        }
        screen = newterm(NULL, stdout, tty_in);
        if (!screen) {
            fprintf(stderr, "newterm failed\n");
            fclose(tty_in);
            free(st);
            return 1;
        }
        set_term(screen);
    } else {
        initscr();
    }

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COLOR_NORMAL,      COLOR_WHITE,  -1);
        init_pair(COLOR_KEYWORD,     COLOR_MAGENTA,-1);
        init_pair(COLOR_STRING,      COLOR_GREEN,  -1);
        init_pair(COLOR_COMMENT,     COLOR_CYAN,   -1);
        init_pair(COLOR_NUMBER,      COLOR_YELLOW, -1);
        init_pair(COLOR_LINENR,      COLOR_YELLOW, -1);
        init_pair(COLOR_STATUS,      COLOR_WHITE,  -1);
        init_pair(COLOR_COPY_SELECT, COLOR_WHITE,  COLOR_BLUE);
        init_pair(COLOR_SEARCH_HL,   COLOR_BLACK,  COLOR_YELLOW);
    }

    for (int i = 0; i < st->buffer_count; i++) {
        undo_push(&st->buffers[i]);
    }

    st->cursor_line = 0;
    st->cursor_col = 0;
    ensure_cursor_bounds(st);
    ensure_cursor_visible(st);

    int running = 1;
    while (running) {
        ensure_cursor_bounds(st);
        draw_ui(st);
        handle_input(st, &running);
        ensure_cursor_bounds(st);
        if (!st->free_scroll) ensure_cursor_visible(st);
    }

    for (int i = 0; i < st->buffer_count; i++) free_buffer(&st->buffers[i]);
    for (int i = 0; i < st->cmdhist_len; i++) free(st->cmdhist[i]);

    endwin();
    if (screen) delscreen(screen);
    if (tty_in) fclose(tty_in);

    free(st);
    return 0;
}