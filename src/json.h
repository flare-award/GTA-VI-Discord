/*
 * json.h - growable string buffer with just enough JSON on top of it.
 *
 * The program only ever *writes* JSON (handshake, SET_ACTIVITY, control
 * replies), so a full parser would be dead weight.  Strings coming from the
 * config are escaped properly, everything else is literal.
 */
#ifndef GTAVI_JSON_H
#define GTAVI_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} sbuf;

void sbuf_init(sbuf *b);
void sbuf_reset(sbuf *b);
void sbuf_free(sbuf *b);

void sbuf_add(sbuf *b, const char *s);
void sbuf_addn(sbuf *b, const char *s, size_t n);
void sbuf_addf(sbuf *b, const char *fmt, ...);

/* Appends a properly escaped, quoted JSON string ("..." -> "\"...\"") */
void sbuf_json_str(sbuf *b, const char *s);

const char *sbuf_cstr(const sbuf *b);

#endif /* GTAVI_JSON_H */
