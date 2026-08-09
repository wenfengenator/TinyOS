/* SPDX-License-Identifier: GPL-2.0-only */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "tglob.h"

enum { T_WORD, T_SEMI, T_AND, T_OR, T_PIPE, T_IN, T_OUT, T_APP,
       T_LP, T_RP, T_LB, T_RB, T_EOF };
typedef struct { int type; char *text; int line; } Token;
typedef struct Node Node;
enum { N_SIMPLE, N_LIST, N_AND, N_OR, N_PIPE, N_IF, N_WHILE, N_FOR,
       N_FUNC, N_GROUP };

struct Node {
    int type;
    Node *a, *b, *c;
    Node **v;
    char **w;
    int nv, nw;
    char *name;
};

typedef struct Var { char *name, *value; struct Var *next; } Var;
typedef struct Func { char *name; Node *body; struct Func *next; } Func;
typedef struct {
    Var *vars;
    Func *funcs;
    char **pos;
    int npos, status, exit_requested, exit_status;
    int return_requested, break_requested, continue_requested;
} Shell;

static Token *tokens;
static int ntokens, token_cap, at, parse_error;

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { write(2, "bash: out of memory\n", 20); _exit(2); }
    return p;
}
static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p;
}
static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

static void add_token(int type, const char *s, size_t n, int line) {
    if (ntokens == token_cap) {
        token_cap = token_cap ? token_cap * 2 : 128;
        tokens = realloc(tokens, (size_t)token_cap * sizeof *tokens);
        if (!tokens) _exit(2);
    }
    tokens[ntokens++] = (Token){ type, s ? xstrndup(s, n) : 0, line };
}

static int operator_at(const char *s, int *length) {
    if (s[0] == '&' && s[1] == '&') { *length = 2; return T_AND; }
    if (s[0] == '|' && s[1] == '|') { *length = 2; return T_OR; }
    if (s[0] == '>' && s[1] == '>') { *length = 2; return T_APP; }
    *length = 1;
    switch (*s) {
    case ';': return T_SEMI; case '|': return T_PIPE;
    case '<': return T_IN; case '>': return T_OUT;
    case '(': return T_LP; case ')': return T_RP;
    case '{': return T_LB; case '}': return T_RB;
    default: return -1;
    }
}

static int lex(const char *source) {
    int line = 1;
    for (size_t i = 0; source[i]; ) {
        if (source[i] == ' ' || source[i] == '\t' || source[i] == '\r') { i++; continue; }
        if (source[i] == '\n') {
            if (!ntokens || tokens[ntokens - 1].type != T_SEMI) add_token(T_SEMI, 0, 0, line);
            line++; i++; continue;
        }
        if (source[i] == '#') { while (source[i] && source[i] != '\n') i++; continue; }
        int length, type = operator_at(source + i, &length);
        if (type >= 0) { add_token(type, 0, 0, line); i += (size_t)length; continue; }
        size_t start = i;
        int single = 0, dbl = 0;
        while (source[i]) {
            char ch = source[i];
            if (!single && ch == '"') { dbl = !dbl; i++; continue; }
            if (!dbl && ch == '\'') { single = !single; i++; continue; }
            if (!single && ch == '\\' && source[i + 1]) { i += 2; continue; }
            if (!single && !dbl) {
                if (isspace((unsigned char)ch) || operator_at(source + i, &length) >= 0) break;
                if (ch == '#') break;
            }
            i++;
        }
        if (single || dbl) { fprintf(stderr, "bash: line %d: unterminated quote\n", line); return -1; }
        if (i > start) add_token(T_WORD, source + start, i - start, line);
        else i++;
    }
    add_token(T_EOF, 0, 0, line);
    return 0;
}

static Token *peek(void) { return &tokens[at]; }
static int take(int type) { if (peek()->type == type) { at++; return 1; } return 0; }
static int word_is(const char *s) { return peek()->type == T_WORD && !strcmp(peek()->text, s); }
static int take_word(const char *s) { if (word_is(s)) { at++; return 1; } return 0; }
static void syntax(const char *want) {
    fprintf(stderr, "bash: line %d: expected %s\n", peek()->line, want); parse_error = 1;
}
static Node *node(int type) { Node *n = calloc(1, sizeof *n); if (!n) _exit(2); n->type = type; return n; }
static void push_node(Node *n, Node *value) {
    n->v = realloc(n->v, (size_t)(n->nv + 1) * sizeof *n->v); n->v[n->nv++] = value;
}
static void push_word(Node *n, char *value) {
    n->w = realloc(n->w, (size_t)(n->nw + 1) * sizeof *n->w); n->w[n->nw++] = value;
}

static Node *parse_list(const char *, const char *, const char *);

static int stopped(const char *a, const char *b, const char *c) {
    return (a && word_is(a)) || (b && word_is(b)) || (c && word_is(c)) ||
           peek()->type == T_EOF || peek()->type == T_RB || peek()->type == T_RP;
}

static Node *parse_command(void) {
    if (take_word("if")) {
        Node *n = node(N_IF), *tail = n;
        tail->a = parse_list("then", 0, 0);
        if (!take_word("then")) { syntax("then"); return n; }
        tail->b = parse_list("elif", "else", "fi");
        while (take_word("elif")) {
            Node *next = node(N_IF); tail->c = next; tail = next;
            tail->a = parse_list("then", 0, 0);
            if (!take_word("then")) { syntax("then"); return n; }
            tail->b = parse_list("elif", "else", "fi");
        }
        if (take_word("else")) tail->c = parse_list("fi", 0, 0);
        if (!take_word("fi")) syntax("fi");
        return n;
    }
    if (take_word("while") || take_word("until")) {
        int until = !strcmp(tokens[at - 1].text, "until");
        Node *n = node(N_WHILE); n->nw = until;
        n->a = parse_list("do", 0, 0);
        if (!take_word("do")) { syntax("do"); return n; }
        n->b = parse_list("done", 0, 0);
        if (!take_word("done")) syntax("done");
        return n;
    }
    if (take_word("for")) {
        Node *n = node(N_FOR);
        if (peek()->type != T_WORD) { syntax("loop variable"); return n; }
        n->name = xstrdup(peek()->text); at++;
        if (take_word("in")) while (peek()->type == T_WORD) push_word(n, xstrdup(tokens[at++].text));
        take(T_SEMI);
        if (!take_word("do")) { syntax("do"); return n; }
        n->a = parse_list("done", 0, 0);
        if (!take_word("done")) syntax("done");
        return n;
    }
    if (take_word("function")) {
        Node *n = node(N_FUNC);
        if (peek()->type != T_WORD) { syntax("function name"); return n; }
        n->name = xstrdup(peek()->text); at++; take(T_LP); take(T_RP);
        if (!take(T_LB)) { syntax("{"); return n; }
        n->a = parse_list(0, 0, 0); take(T_SEMI);
        if (!take(T_RB)) syntax("}");
        return n;
    }
    if (peek()->type == T_WORD && tokens[at + 1].type == T_LP && tokens[at + 2].type == T_RP) {
        Node *n = node(N_FUNC); n->name = xstrdup(peek()->text); at += 3;
        if (!take(T_LB)) { syntax("{"); return n; }
        n->a = parse_list(0, 0, 0); take(T_SEMI);
        if (!take(T_RB)) syntax("}");
        return n;
    }
    if (take(T_LB) || take(T_LP)) {
        int close = tokens[at - 1].type == T_LB ? T_RB : T_RP;
        Node *n = node(N_GROUP); n->a = parse_list(0, 0, 0); take(T_SEMI);
        if (!take(close)) syntax(close == T_RB ? "}" : ")");
        return n;
    }
    Node *n = node(N_SIMPLE);
    while (peek()->type == T_WORD || peek()->type == T_IN || peek()->type == T_OUT || peek()->type == T_APP)
        push_word(n, peek()->type == T_WORD ? xstrdup(peek()->text) : xstrdup(peek()->type == T_IN ? "<" : peek()->type == T_OUT ? ">" : ">>")), at++;
    if (!n->nw) { syntax("command"); }
    return n;
}

static Node *parse_pipeline(void) {
    Node *first = parse_command();
    if (!take(T_PIPE)) return first;
    Node *n = node(N_PIPE); push_node(n, first);
    do push_node(n, parse_command()); while (take(T_PIPE));
    return n;
}

static Node *parse_andor(void) {
    Node *left = parse_pipeline();
    while (peek()->type == T_AND || peek()->type == T_OR) {
        int type = take(T_AND) ? N_AND : (take(T_OR), N_OR);
        Node *n = node(type); n->a = left; n->b = parse_pipeline(); left = n;
    }
    return left;
}

static Node *parse_list(const char *stop_a, const char *stop_b, const char *stop_c) {
    while (take(T_SEMI)) {}
    Node *result = 0;
    while (!stopped(stop_a, stop_b, stop_c)) {
        Node *part = parse_andor();
        if (!result) result = part;
        else { Node *seq = node(N_LIST); seq->a = result; seq->b = part; result = seq; }
        if (!take(T_SEMI) && !stopped(stop_a, stop_b, stop_c)) { syntax("command separator"); break; }
        while (take(T_SEMI)) {}
    }
    return result;
}

static Var *find_var(Shell *sh, const char *name) {
    for (Var *v = sh->vars; v; v = v->next) if (!strcmp(v->name, name)) return v;
    return 0;
}
static const char *get_var(Shell *sh, const char *name) {
    Var *v = find_var(sh, name); if (v) return v->value;
    const char *e = getenv(name); return e ? e : "";
}
static void set_var(Shell *sh, const char *name, const char *value) {
    Var *v = find_var(sh, name);
    if (!v) { v = xmalloc(sizeof *v); v->name = xstrdup(name); v->next = sh->vars; sh->vars = v; }
    else free(v->value);
    v->value = xstrdup(value); setenv(name, value, 1);
}
static void unset_var(Shell *sh, const char *name) {
    Var **link = &sh->vars;
    while (*link) {
        Var *v = *link;
        if (!strcmp(v->name, name)) {
            *link = v->next; free(v->name); free(v->value); free(v); break;
        }
        link = &v->next;
    }
    unsetenv(name);
}
static int valid_name(const char *s, size_t n) {
    if (!n || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (size_t i = 1; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    return 1;
}
static void append(char **out, size_t *len, size_t *cap, const char *s) {
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) { while (*len + n + 1 > *cap) *cap *= 2; *out = realloc(*out, *cap); }
    memcpy(*out + *len, s, n); *len += n; (*out)[*len] = 0;
}
static char *expand(Shell *sh, const char *raw) {
    size_t cap = 64, len = 0; char *out = xmalloc(cap); out[0] = 0; int single = 0, dbl = 0;
    for (size_t i = 0; raw[i]; ) {
        char ch = raw[i++];
        if (!single && ch == '"') { dbl = !dbl; continue; }
        if (!dbl && ch == '\'') { single = !single; continue; }
        if (!single && ch == '\\' && raw[i]) { char t[2] = { raw[i++], 0 }; append(&out, &len, &cap, t); continue; }
        if (!single && ch == '$') {
            char name[64]; size_t n = 0;
            if (raw[i] == '?') { i++; snprintf(name, sizeof name, "%d", sh->status); append(&out, &len, &cap, name); continue; }
            if (raw[i] == '$') { i++; snprintf(name, sizeof name, "%d", getpid()); append(&out, &len, &cap, name); continue; }
            if (raw[i] == '#' ) { i++; snprintf(name, sizeof name, "%d", sh->npos); append(&out, &len, &cap, name); continue; }
            if (raw[i] >= '0' && raw[i] <= '9') { int p = raw[i++] - '0'; append(&out, &len, &cap, p <= sh->npos ? sh->pos[p] : ""); continue; }
            if (raw[i] == '*' || raw[i] == '@') {
                i++; for (int p = 1; p <= sh->npos; p++) { if (p > 1) append(&out, &len, &cap, " "); append(&out, &len, &cap, sh->pos[p]); } continue;
            }
            if (raw[i] == '{') { i++; while (raw[i] && raw[i] != '}' && n < sizeof name - 1) name[n++] = raw[i++]; if (raw[i] == '}') i++; }
            else while ((isalnum((unsigned char)raw[i]) || raw[i] == '_') && n < sizeof name - 1) name[n++] = raw[i++];
            name[n] = 0; if (n) append(&out, &len, &cap, get_var(sh, name)); else append(&out, &len, &cap, "$");
            continue;
        }
        char t[2] = { ch, 0 }; append(&out, &len, &cap, t);
    }
    return out;
}

static int glob_allowed(const char *raw) {
    int single = 0, dbl = 0;
    for (size_t i = 0; raw[i]; i++) {
        if (!dbl && raw[i] == '\'') { single = !single; continue; }
        if (!single && raw[i] == '"') { dbl = !dbl; continue; }
        if (!single && raw[i] == '\\' && raw[i + 1]) { i++; continue; }
        if (!single && !dbl && (raw[i] == '*' || raw[i] == '?' || raw[i] == '[' || raw[i] == '$')) return 1;
    }
    return 0;
}

static int push_arg(char ***argv, int *argc, int *capacity, char *value) {
    if (*argc + 1 >= *capacity) {
        *capacity *= 2;
        char **grown = realloc(*argv, (size_t)*capacity * sizeof **argv);
        if (!grown) return -1;
        *argv = grown;
    }
    (*argv)[(*argc)++] = value;
    (*argv)[*argc] = 0;
    return 0;
}

static int is_mbb_applet(const char *s) {
    static const char *names[] = { "cat","echo","printf","head","tail","wc","grep","sed","awk","seq","cut","sort","uniq","tr","ls","tree","cp","mv","rm","mkdir","rmdir","touch","cmp","stat","basename","dirname","uname","sleep","date","id","whoami","hostname","env","clear","free","sync","kill","expr","vi","ping","web","ifconfig" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (!strcmp(s, names[i])) return 1;
    return 0;
}
static void exec_external(char **argv) {
    if (is_mbb_applet(argv[0])) {
        int n = 0; while (argv[n]) n++;
        char **av = xmalloc((size_t)(n + 2) * sizeof *av); av[0] = (char *)"/MBB.BIN";
        for (int i = 0; i <= n; i++) av[i + 1] = argv[i];
        execv(av[0], av);
    }
    if (strchr(argv[0], '/')) execv(argv[0], argv);
    char path[256];
    snprintf(path, sizeof path, "/%s", argv[0]); execv(path, argv);
    snprintf(path, sizeof path, "/%s.bin", argv[0]); execv(path, argv);
    snprintf(path, sizeof path, "/bin/%s", argv[0]); execv(path, argv);
    snprintf(path, sizeof path, "/bin/%s.bin", argv[0]); execv(path, argv);
    execvp(argv[0], argv);
}

static int execute(Shell *, Node *);
static int run_source(Shell *, const char *);
static char *read_all(const char *);
static Func *find_func(Shell *sh, const char *name) {
    for (Func *f = sh->funcs; f; f = f->next)
        if (!strcmp(f->name, name)) return f;
    return 0;
}
static int truth_test(int argc, char **argv) {
    if (argc > 0 && !strcmp(argv[argc - 1], "]")) argc--;
    if (argc == 0) return 1;
    if (argc == 1) return !argv[0][0];
    if (argc == 2 && !strcmp(argv[0], "-n")) return !argv[1][0];
    if (argc == 2 && !strcmp(argv[0], "-z")) return argv[1][0] != 0;
    if (argc == 2 && !strcmp(argv[0], "-f")) { struct stat st; return stat(argv[1], &st) || !S_ISREG(st.st_mode); }
    if (argc == 2 && !strcmp(argv[0], "-d")) { struct stat st; return stat(argv[1], &st) || !S_ISDIR(st.st_mode); }
    if (argc == 3 && (!strcmp(argv[1], "=") || !strcmp(argv[1], "=="))) return strcmp(argv[0], argv[2]) != 0;
    if (argc == 3 && !strcmp(argv[1], "!=")) return strcmp(argv[0], argv[2]) == 0;
    if (argc == 3 && !strcmp(argv[1], "-eq")) return strtol(argv[0], 0, 0) != strtol(argv[2], 0, 0);
    if (argc == 3 && !strcmp(argv[1], "-ne")) return strtol(argv[0], 0, 0) == strtol(argv[2], 0, 0);
    if (argc == 3 && !strcmp(argv[1], "-lt")) return strtol(argv[0], 0, 0) >= strtol(argv[2], 0, 0);
    if (argc == 3 && !strcmp(argv[1], "-gt")) return strtol(argv[0], 0, 0) <= strtol(argv[2], 0, 0);
    return 1;
}

static int builtin(Shell *sh, int argc, char **argv, int *handled) {
    *handled = 1;
    if (!strcmp(argv[0], ":") || !strcmp(argv[0], "true")) return 0;
    if (!strcmp(argv[0], "false")) return 1;
    if (!strcmp(argv[0], "echo")) { for (int i = 1; i < argc; i++) printf("%s%s", i > 1 ? " " : "", argv[i]); putchar('\n'); fflush(stdout); return 0; }
    if (!strcmp(argv[0], "pwd")) { char b[256]; if (getcwd(b, sizeof b)) { puts(b); return 0; } perror("pwd"); return 1; }
    if (!strcmp(argv[0], "cd")) { const char *p = argc > 1 ? argv[1] : "/"; if (!chdir(p)) { set_var(sh, "PWD", p); return 0; } perror(p); return 1; }
    if (!strcmp(argv[0], "exit")) { sh->exit_requested = 1; sh->exit_status = argc > 1 ? atoi(argv[1]) : sh->status; return sh->exit_status; }
    if (!strcmp(argv[0], "return")) { sh->return_requested = 1; return argc > 1 ? atoi(argv[1]) : sh->status; }
    if (!strcmp(argv[0], "break")) { sh->break_requested = 1; return 0; }
    if (!strcmp(argv[0], "continue")) { sh->continue_requested = 1; return 0; }
    if (!strcmp(argv[0], "test") || !strcmp(argv[0], "[")) return truth_test(argc - 1, argv + 1);
    if (!strcmp(argv[0], "export")) { for (int i = 1; i < argc; i++) { char *eq = strchr(argv[i], '='); if (eq) { *eq = 0; set_var(sh, argv[i], eq + 1); } else setenv(argv[i], get_var(sh, argv[i]), 1); } return 0; }
    if (!strcmp(argv[0], "unset")) { for (int i = 1; i < argc; i++) unset_var(sh, argv[i]); return 0; }
    if (!strcmp(argv[0], "read")) { char b[512]; if (!fgets(b, sizeof b, stdin)) return 1; b[strcspn(b, "\r\n")] = 0; set_var(sh, argc > 1 ? argv[1] : "REPLY", b); return 0; }
    if (!strcmp(argv[0], "source") || !strcmp(argv[0], ".")) {
        if (argc < 2) { fprintf(stderr, "%s: filename required\n", argv[0]); return 2; }
        char *source = read_all(argv[1]);
        if (!source) { perror(argv[1]); return 1; }
        int rc = run_source(sh, source); free(source); return rc;
    }
    *handled = 0; return 0;
}

static int run_simple(Shell *sh, Node *n, int child) {
    int capacity = n->nw + 8;
    char **argv = xmalloc((size_t)capacity * sizeof *argv); int argc = 0, in = -1, out = -1;
    int command_seen = 0;
    for (int i = 0; i < n->nw; i++) {
        if ((!strcmp(n->w[i], "<") || !strcmp(n->w[i], ">") || !strcmp(n->w[i], ">>")) && i + 1 < n->nw) {
            int kind = n->w[i][0] == '<' ? 0 : n->w[i][1] ? 2 : 1; char *path = expand(sh, n->w[++i]);
            int fd = open(path, kind == 0 ? O_RDONLY : O_WRONLY | O_CREAT | (kind == 2 ? O_APPEND : O_TRUNC), 0666);
            free(path); if (fd < 0) { perror("bash: redirect"); return 1; }
            if (kind == 0) in = fd; else out = fd; continue;
        }
        char *value = expand(sh, n->w[i]);
        char *equals = !command_seen ? strchr(value, '=') : 0;
        int assignment = equals && valid_name(value, (size_t)(equals - value));
        if (!assignment) command_seen = 1;
        if (!assignment && glob_allowed(n->w[i]) && tglob_has_meta(value)) {
            tglob_result matches;
            int count = tglob_expand(value, &matches);
            if (count < 0) { free(value); fprintf(stderr, "bash: glob: out of memory\n"); return 1; }
            if (count > 0) {
                free(value);
                for (int j = 0; j < count; j++) {
                    if (push_arg(&argv, &argc, &capacity, xstrdup(matches.items[j])) != 0) { tglob_free(&matches); return 1; }
                }
                tglob_free(&matches);
                continue;
            }
        }
        if (push_arg(&argv, &argc, &capacity, value) != 0) return 1;
    }
    argv[argc] = 0;
    if (!argc) return 0;
    int assignments = 0;
    while (assignments < argc) {
        char *eq = strchr(argv[assignments], '=');
        if (!eq || !valid_name(argv[assignments], (size_t)(eq - argv[assignments]))) break;
        *eq = 0; set_var(sh, argv[assignments], eq + 1); assignments++;
    }
    if (assignments == argc) { if (in >= 0) close(in); if (out >= 0) close(out); return 0; }
    argv += assignments; argc -= assignments;
    int saved_in = in >= 0 && !child ? dup(0) : -1;
    int saved_out = out >= 0 && !child ? dup(1) : -1;
    if (in >= 0) { dup2(in, 0); close(in); }
    if (out >= 0) { dup2(out, 1); close(out); }
    Func *f = find_func(sh, argv[0]);
    if (f && !child) {
        char **oldpos = sh->pos; int oldnpos = sh->npos; sh->pos = argv; sh->npos = argc - 1;
        int rc = execute(sh, f->body); sh->return_requested = 0; sh->pos = oldpos; sh->npos = oldnpos;
        if (saved_in >= 0) { dup2(saved_in, 0); close(saved_in); }
        if (saved_out >= 0) { fflush(stdout); dup2(saved_out, 1); close(saved_out); }
        return rc;
    }
    int handled, rc = builtin(sh, argc, argv, &handled);
    if (handled && !child) {
        if (saved_in >= 0) { dup2(saved_in, 0); close(saved_in); }
        if (saved_out >= 0) { fflush(stdout); dup2(saved_out, 1); close(saved_out); }
        return rc;
    }
    if (!child) {
        int pid = fork(); if (pid < 0) { perror("fork"); return 1; }
        if (!pid) { signal(SIGINT, SIG_DFL); if (f) _exit(execute(sh, f->body)); if (handled) _exit(rc); exec_external(argv); perror(argv[0]); _exit(127); }
        if (saved_in >= 0) { dup2(saved_in, 0); close(saved_in); }
        if (saved_out >= 0) { dup2(saved_out, 1); close(saved_out); }
        int status; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    }
    if (f) _exit(execute(sh, f->body));
    if (handled) _exit(rc);
    exec_external(argv); fprintf(stderr, "%s: command not found\n", argv[0]); _exit(127);
}

static int execute(Shell *sh, Node *n) {
    if (!n || sh->exit_requested) return sh->status;
    int rc = 0;
    switch (n->type) {
    case N_SIMPLE: rc = run_simple(sh, n, 0); break;
    case N_LIST: rc = execute(sh, n->a); if (!sh->exit_requested && !sh->return_requested && !sh->break_requested && !sh->continue_requested) rc = execute(sh, n->b); break;
    case N_AND: rc = execute(sh, n->a); if (!rc) rc = execute(sh, n->b); break;
    case N_OR: rc = execute(sh, n->a); if (rc) rc = execute(sh, n->b); break;
    case N_PIPE: {
        int prev = -1, last = -1;
        for (int i = 0; i < n->nv; i++) {
            int p[2] = {-1,-1}; if (i + 1 < n->nv && pipe(p)) { perror("pipe"); return 1; }
            int pid = fork(); if (!pid) { if (prev >= 0) { dup2(prev, 0); close(prev); } if (p[1] >= 0) { dup2(p[1], 1); close(p[0]); close(p[1]); } if (n->v[i]->type == N_SIMPLE) run_simple(sh, n->v[i], 1); _exit(execute(sh, n->v[i])); }
            if (prev >= 0) close(prev);
            if (p[1] >= 0) close(p[1]);
            prev = p[0]; last = pid;
        }
        if (prev >= 0) close(prev);
        int status, pid;
        while ((pid = wait(&status)) > 0)
            if (pid == last) rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        break;
    }
    case N_IF: rc = execute(sh, n->a); rc = !rc ? execute(sh, n->b) : execute(sh, n->c); break;
    case N_WHILE:
        while (!sh->exit_requested) { int ok = execute(sh, n->a) == 0; if (n->nw) ok = !ok; if (!ok) break; rc = execute(sh, n->b); if (sh->break_requested) { sh->break_requested = 0; break; } if (sh->continue_requested) sh->continue_requested = 0; if (sh->return_requested) break; }
        break;
    case N_FOR: {
        int count = n->nw ? n->nw : sh->npos;
        for (int i = 0; i < count && !sh->exit_requested; i++) { char *value = n->nw ? expand(sh, n->w[i]) : xstrdup(sh->pos[i + 1]); set_var(sh, n->name, value); free(value); rc = execute(sh, n->a); if (sh->break_requested) { sh->break_requested = 0; break; } if (sh->continue_requested) sh->continue_requested = 0; if (sh->return_requested) break; }
        break;
    }
    case N_FUNC: { Func *f = find_func(sh, n->name); if (!f) { f = xmalloc(sizeof *f); f->name = xstrdup(n->name); f->next = sh->funcs; sh->funcs = f; } f->body = n->a; rc = 0; break; }
    case N_GROUP: rc = execute(sh, n->a); break;
    }
    sh->status = rc; return rc;
}

static int run_source(Shell *sh, const char *source) {
    tokens = 0; ntokens = token_cap = at = parse_error = 0;
    if (lex(source)) return 2;
    Node *root = parse_list(0, 0, 0);
    if (parse_error || peek()->type != T_EOF) return 2;
    return execute(sh, root);
}
static char *read_all(const char *path) {
    FILE *f = !strcmp(path, "-") ? stdin : fopen(path, "rb");
    if (!f) return 0;
    size_t cap = 4096, n = 0; char *b = xmalloc(cap);
    for (;;) { if (n + 2048 + 1 > cap) { cap *= 2; b = realloc(b, cap); } size_t r = fread(b + n, 1, 2048, f); n += r; if (r < 2048) break; }
    if (f != stdin) fclose(f);
    b[n] = 0; return b;
}

int main(int argc, char **argv) {
    Shell sh; memset(&sh, 0, sizeof sh); sh.pos = argv; sh.npos = argc - 1;
    set_var(&sh, "PATH", "/:/bin"); signal(SIGINT, SIG_IGN);
    if (argc > 2 && !strcmp(argv[1], "-c")) { sh.pos = argv + 2; sh.npos = argc - 3; int rc = run_source(&sh, argv[2]); return sh.exit_requested ? sh.exit_status : rc; }
    if (argc > 1) {
        char *source = read_all(argv[1]); if (!source) { perror(argv[1]); return 1; }
        sh.pos = argv + 1; sh.npos = argc - 2; int rc = run_source(&sh, source); free(source);
        return sh.exit_requested ? sh.exit_status : rc;
    }
    puts("TinyOS bash-like shell 1.0 (type 'exit' to leave)");
    char line[1024];
    while (!sh.exit_requested) {
        char cwd[256]; if (getcwd(cwd, sizeof cwd)) printf("%s", cwd); printf(" $ "); fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        run_source(&sh, line);
    }
    return sh.exit_requested ? sh.exit_status : sh.status;
}