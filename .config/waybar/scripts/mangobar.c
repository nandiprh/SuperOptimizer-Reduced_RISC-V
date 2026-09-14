/*
 * mangobar - Mango WM -> waybar bridge.
 * Usage: mangobar tags | mangobar focused
 *
 * Talks to the Mango WM IPC socket, streams the matching "watch"
 * event bus and emits one waybar custom-module JSON object per line.
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* ---------------- minimal JSON ---------------- */
static const char *to_roman(int n) {
    static const char *romans[] = {
        "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X",
        "XI", "XII", "XIII", "XIV", "XV", "XVI", "XVII", "XVIII", "XIX", "XX",
        "XXI", "XXII", "XXIII", "XXIV", "XXV", "XXVI", "XXVII", "XXVIII", "XXIX", "XXX",
        "XXXI", "XXXII", "XXXIII", "XXXIV", "XXXV", "XXXVI", "XXXVII", "XXXVIII", "XXXIX", "XL",
        "XLI", "XLII", "XLIII", "XLIV", "XLV", "XLVI", "XLVII", "XLVIII", "XLIX", "L"
    };
    if (n < 1 || n > 50) return "";
    return romans[n - 1];
}

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JNode JNode;
typedef struct {
    char *k;
    JNode *v;
} JPair;

struct JNode {
    JType t;
    char *s;
    double num;
    int b;
    JPair *pairs;
    size_t np, cap;
    JNode **arr;
    size_t na, acap;
};

static void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n);
    if (!r) {
        fprintf(stderr, "mangobar: out of memory\n");
        exit(1);
    }
    return r;
}

static JNode *jnode(JType t) {
    JNode *n = calloc(1, sizeof(JNode));
    if (!n) exit(1);
    n->t = t;
    return n;
}

static void jadd_kv(JNode *o, const char *k, JNode *v) {
    if (o->np == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 8;
        o->pairs = xrealloc(o->pairs, o->cap * sizeof(JPair));
    }
    o->pairs[o->np].k = strdup(k);
    o->pairs[o->np].v = v;
    o->np++;
}

static void jadd_arr(JNode *o, JNode *v) {
    if (o->na == o->acap) {
        o->acap = o->acap ? o->acap * 2 : 8;
        o->arr = xrealloc(o->arr, o->acap * sizeof(JNode *));
    }
    o->arr[o->na++] = v;
}

static void jfree(JNode *n) {
    if (!n) return;
    if (n->t == J_STR) free(n->s);
    if (n->t == J_OBJ) {
        for (size_t i = 0; i < n->np; i++) {
            free(n->pairs[i].k);
            jfree(n->pairs[i].v);
        }
        free(n->pairs);
    }
    if (n->t == J_ARR)
        for (size_t i = 0; i < n->na; i++) jfree(n->arr[i]);
    if (n->t == J_ARR) free(n->arr);
    free(n);
}

typedef struct {
    const char *p;
    const char *end;
} Parse;

static void skipws(Parse *P) {
    while (P->p < P->end &&
           (*P->p == ' ' || *P->p == '\t' || *P->p == '\n' || *P->p == '\r'))
        P->p++;
}

static char *json_string(Parse *P) {
    /* P->p points at the opening quote */
    P->p++;
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) exit(1);
    while (P->p < P->end) {
        unsigned char c = (unsigned char)*P->p;
        if (c == '"') {
            P->p++;
            out[len] = 0;
            return out;
        }
        if (c == '\\') {
            P->p++;
            if (P->p >= P->end) break;
            char e = *P->p++;
            const char *rep = NULL;
            switch (e) {
                case '"': rep = "\""; break;
                case '\\': rep = "\\"; break;
                case '/': rep = "/"; break;
                case 'b': rep = "\b"; break;
                case 'f': rep = "\f"; break;
                case 'n': rep = "\n"; break;
                case 'r': rep = "\r"; break;
                case 't': rep = "\t"; break;
                case 'u': {
                    if (P->end - P->p < 4) {
                        P->p = P->end;
                        break;
                    }
                    unsigned v = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = P->p[i];
                        unsigned d;
                        if (h >= '0' && h <= '9') d = h - '0';
                        else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                        else d = 0;
                        v = (v << 4) | d;
                    }
                    P->p += 4;
                    char utf8[4];
                    int n = 0;
                    if (v < 0x80) utf8[n++] = (char)v;
                    else if (v < 0x800) {
                        utf8[n++] = (char)(0xC0 | (v >> 6));
                        utf8[n++] = (char)(0x80 | (v & 0x3F));
                    } else {
                        utf8[n++] = (char)(0xE0 | (v >> 12));
                        utf8[n++] = (char)(0x80 | ((v >> 6) & 0x3F));
                        utf8[n++] = (char)(0x80 | (v & 0x3F));
                    }
                    if (len + n + 1 >= cap) {
                        cap = (cap + n + 1) * 2;
                        out = xrealloc(out, cap);
                    }
                    memcpy(out + len, utf8, n);
                    len += n;
                    continue;
                }
                default: rep = "?"; break;
            }
            if (rep) {
                size_t rl = strlen(rep);
                if (len + rl + 1 >= cap) {
                    cap = (cap + rl + 1) * 2;
                    out = xrealloc(out, cap);
                }
                memcpy(out + len, rep, rl);
                len += rl;
            }
            continue;
        }
        if (len + 2 >= cap) {
            cap *= 2;
            out = xrealloc(out, cap);
        }
        out[len++] = (char)c;
        P->p++;
    }
    out[len] = 0;
    return out;
}

static JNode *json_value(Parse *P) {
    skipws(P);
    if (P->p >= P->end) return NULL;
    char c = *P->p;
    if (c == '{') {
        P->p++;
        JNode *o = jnode(J_OBJ);
        skipws(P);
        if (P->p < P->end && *P->p == '}') { P->p++; return o; }
        for (;;) {
            skipws(P);
            char *k = json_string(P);
            skipws(P);
            if (P->p < P->end && *P->p == ':') P->p++;
            JNode *v = json_value(P);
            jadd_kv(o, k, v);
            free(k);
            skipws(P);
            if (P->p >= P->end) break;
            if (*P->p == ',') { P->p++; continue; }
            if (*P->p == '}') { P->p++; break; }
        }
        return o;
    }
    if (c == '[') {
        P->p++;
        JNode *o = jnode(J_ARR);
        skipws(P);
        if (P->p < P->end && *P->p == ']') { P->p++; return o; }
        for (;;) {
            JNode *v = json_value(P);
            jadd_arr(o, v);
            skipws(P);
            if (P->p >= P->end) break;
            if (*P->p == ',') { P->p++; continue; }
            if (*P->p == ']') { P->p++; break; }
        }
        return o;
    }
    if (c == '"') {
        JNode *n = jnode(J_STR);
        n->s = json_string(P);
        return n;
    }
    if (strncmp(P->p, "true", 4) == 0) { P->p += 4; JNode *n = jnode(J_BOOL); n->b = 1; return n; }
    if (strncmp(P->p, "false", 5) == 0) { P->p += 5; JNode *n = jnode(J_BOOL); n->b = 0; return n; }
    if (strncmp(P->p, "null", 4) == 0) { P->p += 4; return jnode(J_NULL); }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *endp;
        double d = strtod(P->p, &endp);
        P->p = endp;
        JNode *n = jnode(J_NUM);
        n->num = d;
        return n;
    }
    return jnode(J_NULL);
}

static JNode *json_parse(const char *s) {
    Parse P = {s, s + strlen(s)};
    JNode *v = json_value(&P);
    skipws(&P);
    return v;
}

static JNode *jobj_get(JNode *o, const char *key) {
    if (!o || o->t != J_OBJ) return NULL;
    for (size_t i = 0; i < o->np; i++)
        if (strcmp(o->pairs[i].k, key) == 0) return o->pairs[i].v;
    return NULL;
}

static const char *jstr(JNode *o) { return (o && o->t == J_STR) ? o->s : NULL; }

static int jint(JNode *o, int def) { return (o && o->t == J_NUM) ? (int)o->num : def; }

static int jbool(JNode *o, int def) { return (o && o->t == J_BOOL) ? o->b : def; }

static char *jescape(const char *s) {
    if (!s) return strdup("");
    size_t cap = strlen(s) + 8, len = 0;
    char *out = malloc(cap);
    if (!out) exit(1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (len + 8 >= cap) { cap *= 2; out = xrealloc(out, cap); }
        switch (*p) {
            case '"': memcpy(out + len, "\\\"", 2); len += 2; break;
            case '\\': memcpy(out + len, "\\\\", 2); len += 2; break;
            case '\n': memcpy(out + len, "\\n", 2); len += 2; break;
            case '\t': memcpy(out + len, "\\t", 2); len += 2; break;
            case '\r': memcpy(out + len, "\\r", 2); len += 2; break;
            default:
                if (*p < 0x20) { sprintf(out + len, "\\u%04x", *p); len += 6; }
                else out[len++] = (char)*p;
        }
    }
    out[len] = 0;
    return out;
}

static void emit(const char *text, const char *cls, const char *alt,
                 const char *tooltip) {
    char *t = jescape(text);
    char *c = jescape(cls);
    char *a = jescape(alt);
    char *tt = jescape(tooltip);
    printf("{\"text\":\"%s\",\"class\":\"%s\",\"alt\":\"%s\",\"tooltip\":\"%s\"}\n",
           t, c, a, tt);
    free(t); free(c); free(a); free(tt);
    fflush(stdout);
}

/* ---------------- IPC ---------------- */

static char *find_socket(void) {
    const char *sig = getenv("MANGO_INSTANCE_SIGNATURE");
    if (sig && sig[0] && access(sig, F_OK) == 0) return strdup(sig);
    char pat[256];
    snprintf(pat, sizeof(pat), "%s/mango-*.sock",
             getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp");
    glob_t g;
    char *res = NULL;
    if (glob(pat, 0, NULL, &g) == 0 && g.gl_pathc > 0)
        res = strdup(g.gl_pathv[g.gl_pathc - 1]);
    globfree(&g);
    return res;
}

static int connect_mango(char **err) {
    char *path = find_socket();
    if (!path) {
        if (err) *err = strdup("mango IPC socket not found");
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) *err = strdup(strerror(errno));
        free(path);
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (err) *err = strdup(strerror(errno));
        close(fd);
        free(path);
        return -1;
    }
    free(path);
    return fd;
}

static void send_cmd(int fd, const char *cmd) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s\n", cmd);
    (void)!write(fd, buf, (size_t)n);
}

/*
 * Read the next complete line from the socket.  Returns a freshly
 * allocated NUL-terminated string, or NULL on EOF/error.
 */
static char *read_line(int fd) {
    static char buf[1 << 20];
    static size_t len = 0;
    for (;;) {
        char *nl = memchr(buf, '\n', len);
        if (nl) {
            size_t sz = (size_t)(nl - buf);
            char *line = malloc(sz + 1);
            if (!line) exit(1);
            memcpy(line, buf, sz);
            line[sz] = 0;
            memmove(buf, nl + 1, len - sz - 1);
            len -= sz + 1;
            return line;
        }
        if (len >= sizeof(buf) - 1) {
            len = 0;
            return strdup("{}");
        }
        ssize_t r = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
        if (r <= 0) return NULL;
        len += (size_t)r;
        buf[len] = 0;
    }
}

static void reconnect_loop(int *fd) {
    for (;;) {
        char *err = NULL;
        int f = connect_mango(&err);
        if (f >= 0) {
            *fd = f;
            return;
        }
        free(err);
        usleep(1000000);
    }
}

/* ---------------- tags (workspaces) ---------------- */

static const char *ROMAN[] = {"", "I",  "II",  "III", "IV",  "V",
                              "VI", "VII", "VIII", "IX",  "X",   "XI",
                              "XII"};

static void run_tags(void) {
    int fd = -1;
    reconnect_loop(&fd);
    send_cmd(fd, "watch all-tags");
    for (;;) {
        char *line = read_line(fd);
        if (!line) { close(fd); reconnect_loop(&fd); send_cmd(fd, "watch all-tags"); continue; }
        JNode *root = json_parse(line);
        free(line);
        JNode *all = jobj_get(root, "all_tags");
        JNode *entry = NULL;
        if (all && all->t == J_ARR) {
            for (size_t i = 0; i < all->na; i++) {
                JNode *e = all->arr[i];
                JNode *tags = jobj_get(e, "tags");
                int has_active = 0;
                if (tags && tags->t == J_ARR)
                    for (size_t j = 0; j < tags->na; j++)
                        if (jbool(jobj_get(tags->arr[j], "is_active"), 0))
                            has_active = 1;
                if (has_active) { entry = e; break; }
            }
            if (!entry && all->na > 0) entry = all->arr[0];
        }
        JNode *tags = entry ? jobj_get(entry, "tags") : NULL;
        if (tags && tags->t == J_ARR && tags->na > 0) {
            char text[1024] = {0};
            size_t used = 0;
            int any_active = 0, any_urgent = 0;
            char tooltip[512] = {0};
            for (size_t i = 0; i < tags->na; i++) {
                JNode *t = tags->arr[i];
                int idx = jint(jobj_get(t, "index"), (int)i + 1);
                int active = jbool(jobj_get(t, "is_active"), 0);
                int urgent = jbool(jobj_get(t, "is_urgent"), 0);
                int count = jint(jobj_get(t, "client_count"), 0);
                const char *num = (idx > 0 && (size_t)idx < sizeof(ROMAN) / sizeof(ROMAN[0]))
                                      ? ROMAN[idx]
                                      : (idx > 0 ? "" : "");
                char nb[16];
                if (!num[0]) { snprintf(nb, sizeof(nb), "%d", idx); num = nb; }
                const char *color;
                if (active) { color = "#ff5c68"; any_active = 1; }
                else if (urgent) { color = "#ff8fa3"; any_urgent = 1; }
                else if (count > 0) color = "#cfd3ea";
                else color = "#565a75";
                int n = snprintf(text + used, sizeof(text) - used,
                                 "%s<span foreground=\"%s\"%s>%s</span>",
                                 used ? " " : "", color,
                                 (active || urgent) ? " weight=\"bold\"" : "", num);
                if (n < 0 || used + (size_t)n >= sizeof(text)) break;
                used += (size_t)n;
            }
            if (any_active)
                snprintf(tooltip, sizeof(tooltip), "active workspace is highlighted in red");
            else if (any_urgent)
                snprintf(tooltip, sizeof(tooltip), "urgent workspace");
            else
                snprintf(tooltip, sizeof(tooltip), "mango tags");
            const char *cls = any_active ? "active" : any_urgent ? "urgent" : "idle";
            emit(text, cls, cls, tooltip);
        }
        jfree(root);
    }
}

/*
 * Emit a single workspace (tag) as its own waybar module so each one
 * can be styled as an individual button/circle.
 */
static void run_one_tag(int which) {
    int fd = -1;
    reconnect_loop(&fd);
    send_cmd(fd, "watch all-tags");
    for (;;) {
        char *line = read_line(fd);
        if (!line) { close(fd); reconnect_loop(&fd); send_cmd(fd, "watch all-tags"); continue; }
        JNode *root = json_parse(line);
        free(line);
        JNode *all = jobj_get(root, "all_tags");
        JNode *entry = NULL;
        if (all && all->t == J_ARR) {
            for (size_t i = 0; i < all->na; i++) {
                JNode *e = all->arr[i];
                JNode *tags = jobj_get(e, "tags");
                int has_active = 0;
                if (tags && tags->t == J_ARR)
                    for (size_t j = 0; j < tags->na; j++)
                        if (jbool(jobj_get(tags->arr[j], "is_active"), 0))
                            has_active = 1;
                if (has_active) { entry = e; break; }
            }
            if (!entry && all->na > 0) entry = all->arr[0];
        }
        JNode *tags = entry ? jobj_get(entry, "tags") : NULL;
        if (tags && tags->t == J_ARR && tags->na > 0) {
            int found = 0;
            char num[16] = {0};
            int active = 0, urgent = 0, count = 0;
            for (size_t j = 0; j < tags->na; j++) {
                JNode *t = tags->arr[j];
                int idx = jint(jobj_get(t, "index"), (int)j + 1);
                if (idx == which) {
                    found = 1;
                    active = jbool(jobj_get(t, "is_active"), 0);
                    urgent = jbool(jobj_get(t, "is_urgent"), 0);
                    count = jint(jobj_get(t, "client_count"), 0);
                    snprintf(num, sizeof(num), "%.10s", to_roman(idx));
                    break;
                }
            }
            if (found) {
                const char *cls = active ? "active"
                                 : urgent ? "urgent"
                                 : (count > 0 ? "occupied" : "idle");
                char tooltip[128];
                snprintf(tooltip, sizeof(tooltip), "workspace %s%s", to_roman(which),
                         active ? " (focused)" : urgent ? " (urgent)" : "");
                emit(num, cls, cls, tooltip);
            } else {
                emit("", "idle", "idle", "");
            }
        }
        jfree(root);
    }
}

/* ---------------- focused window ---------------- */

static void run_focused(void) {
    int fd = -1;
    reconnect_loop(&fd);
    send_cmd(fd, "watch focusing-client");
    for (;;) {
        char *line = read_line(fd);
        if (!line) { close(fd); reconnect_loop(&fd); send_cmd(fd, "watch focusing-client"); continue; }
        JNode *root = json_parse(line);
        free(line);
        JNode *id = jobj_get(root, "id");
        if (id && id->t != J_NULL) {
            const char *title = jstr(jobj_get(root, "title"));
            const char *appid = jstr(jobj_get(root, "appid"));
            char text[256] = {0};
            if (title && title[0]) {
                int len = strlen(title);
                if (len > 60) {
                    snprintf(text, sizeof(text), "%.57s...", title);
                } else {
                    snprintf(text, sizeof(text), "%.240s", title);
                }
            } else if (appid && appid[0]) {
                int len = strlen(appid);
                if (len > 60) {
                    snprintf(text, sizeof(text), "%.57s...", appid);
                } else {
                    snprintf(text, sizeof(text), "%.240s", appid);
                }
            } else {
                snprintf(text, sizeof(text), "—");
            }
            char tooltip[512] = {0};
            snprintf(tooltip, sizeof(tooltip), "%s\n%s",
                     appid ? appid : "", title ? title : "");
            emit(text, "window-active", "window", tooltip);
        } else {
            emit("", "window-idle", "window", "");
        }
        jfree(root);
    }
}

static void usage(void) {
    fprintf(stderr, "usage: mangobar <tags|focused|tag N> [-n]\n");
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    const char *mode = argv[1];
    if (strcmp(mode, "tags") == 0) run_tags();
    else if (strcmp(mode, "focused") == 0) run_focused();
    else if (strcmp(mode, "tag") == 0 && argc >= 3)
        run_one_tag(atoi(argv[2]));
    else usage();
    return 0;
}