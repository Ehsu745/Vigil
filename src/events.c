/* Follows the events file. The hook writes flat JSON objects, one per line, so the reader only
 * needs to pull a handful of string/number fields out of a line. */
#include "dodeca.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define HISTORY_BYTES 524288

void tail_init(Tail *t, const char *path) {
    memset(t, 0, sizeof *t);
    snprintf(t->path, sizeof t->path, "%s", path);
}

/* the value of "key" as text (strings unescaped, numbers as written); 0 if the key is missing */
static int field(const char *s, const char *end, const char *key, char *out, size_t cap) {
    char pat[40];
    int klen = snprintf(pat, sizeof pat, "\"%s\"", key);
    out[0] = 0;
    for (const char *p = s; p + klen < end; p++) {
        if (memcmp(p, pat, (size_t)klen) != 0) continue;
        if (p > s && p[-1] == '\\') continue; /* an escaped quote inside some value */
        const char *q = p + klen;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end || *q != ':') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        size_t n = 0;
        if (q < end && *q == '"') {
            for (q++; q < end && *q != '"'; q++) {
                char ch = *q;
                if (ch == '\\' && q + 1 < end) {
                    q++;
                    if (*q == 'u') { q += 4; ch = '?'; } /* non-ASCII: the label font has no glyph for it anyway */
                    else ch = *q == 'n' || *q == 't' ? ' ' : *q;
                }
                if (n + 1 < cap) out[n++] = ch;
            }
        } else {
            for (; q < end && *q != ',' && *q != '}' && *q != ' '; q++)
                if (n + 1 < cap) out[n++] = *q;
        }
        out[n] = 0;
        return 1;
    }
    return 0;
}

int event_parse(const char *line, size_t n, Event *e) {
    const char *end = line + n;
    char num[40];
    memset(e, 0, sizeof *e);
    if (n < 2 || line[0] != '{') return 0;
    if (!field(line, end, "t", num, sizeof num)) return 0;
    e->t = strtod(num, NULL);
    if (!field(line, end, "sid", e->sid, sizeof e->sid) || !e->sid[0]) return 0;
    if (!field(line, end, "ev", e->ev, sizeof e->ev) || !e->ev[0]) return 0;
    field(line, end, "cwd", e->cwd, sizeof e->cwd);
    field(line, end, "tool", e->tool, sizeof e->tool);
    field(line, end, "n", e->ntype, sizeof e->ntype);
    if (field(line, end, "pid", num, sizeof num)) e->pid = atoi(num);
    return e->t > 0;
}

static int parse_lines(const char *d, size_t n, Event *out, int max, double since) {
    int count = 0;
    size_t i = 0;
    while (i < n && count < max) {
        size_t j = i;
        while (j < n && d[j] != '\n') j++;
        Event e;
        if (j > i && event_parse(d + i, j - i, &e) && e.t >= since) out[count++] = e;
        i = j + 1;
    }
    return count;
}

int tail_history(Tail *t, double since, Event *out, int max) {
    FILE *f = fopen(t->path, "rb");
    if (!f) { t->pos = 0; return 0; }
    struct stat st;
    fstat(fileno(f), &st);
    long size = (long)st.st_size, start = size > HISTORY_BYTES ? size - HISTORY_BYTES : 0;
    char *d = malloc((size_t)(size - start) + 1);
    fseek(f, start, SEEK_SET);
    size_t n = fread(d, 1, (size_t)(size - start), f);
    fclose(f);
    size_t off = 0, end = n;
    if (start) { while (off < n && d[off] != '\n') off++; if (off < n) off++; } /* the first line is cut in half */
    while (end > off && d[end - 1] != '\n') end--;                              /* a half-written last line waits for poll */
    t->pos = start + (long)end;
    int count = parse_lines(d + off, end - off, out, max, since);
    free(d);
    return count;
}

int tail_poll(Tail *t, Event *out, int max) {
    struct stat st;
    if (stat(t->path, &st) != 0) return 0;
    long size = (long)st.st_size;
    if (size < t->pos) { t->pos = 0; t->buflen = 0; } /* truncated or replaced */
    if (size == t->pos) return 0;
    FILE *f = fopen(t->path, "rb");
    if (!f) return 0;
    fseek(f, t->pos, SEEK_SET);
    size_t want = (size_t)(size - t->pos);
    char *d = malloc(t->buflen + want + 1);
    if (t->buflen) memcpy(d, t->buf, t->buflen);
    size_t got = fread(d + t->buflen, 1, want, f);
    fclose(f);
    t->pos += (long)got;
    size_t total = t->buflen + got, end = total;
    while (end > 0 && d[end - 1] != '\n') end--; /* keep the unfinished line for next time */
    int count = parse_lines(d, end, out, max, 0);
    free(t->buf);
    t->buflen = total - end;
    t->buf = t->buflen ? malloc(t->buflen) : NULL;
    if (t->buflen) memcpy(t->buf, d + end, t->buflen);
    free(d);
    return count;
}
