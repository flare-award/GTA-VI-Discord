/*
 * json.c - growable string buffer + JSON string escaping.
 */
#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sbuf_init(sbuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = false;
}

void sbuf_reset(sbuf *b)
{
    b->len = 0;
    b->oom = false;
    if (b->data != NULL) {
        b->data[0] = '\0';
    }
}

void sbuf_free(sbuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static bool sbuf_reserve(sbuf *b, size_t extra)
{
    size_t need;
    size_t cap;
    char *grown;

    if (b->oom) {
        return false;
    }
    need = b->len + extra + 1;
    if (b->data != NULL && need <= b->cap) {
        return true;
    }
    cap = (b->cap != 0) ? b->cap : 256;
    while (cap < need) {
        cap *= 2;
    }
    grown = (char *)realloc(b->data, cap);
    if (grown == NULL) {
        b->oom = true;
        return false;
    }
    b->data = grown;
    b->cap = cap;
    return true;
}

void sbuf_addn(sbuf *b, const char *s, size_t n)
{
    if (s == NULL || n == 0) {
        return;
    }
    if (!sbuf_reserve(b, n)) {
        return;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sbuf_add(sbuf *b, const char *s)
{
    if (s == NULL) {
        return;
    }
    sbuf_addn(b, s, strlen(s));
}

void sbuf_addf(sbuf *b, const char *fmt, ...)
{
    char stack[512];
    va_list args;
    int written;

    va_start(args, fmt);
    written = vsnprintf(stack, sizeof(stack), fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }
    if ((size_t)written < sizeof(stack)) {
        sbuf_addn(b, stack, (size_t)written);
        return;
    }

    {
        size_t need = (size_t)written + 1;
        char *heap = (char *)malloc(need);
        if (heap == NULL) {
            b->oom = true;
            return;
        }
        va_start(args, fmt);
        vsnprintf(heap, need, fmt, args);
        va_end(args);
        sbuf_addn(b, heap, (size_t)written);
        free(heap);
    }
}

void sbuf_json_str(sbuf *b, const char *s)
{
    static const char hex[] = "0123456789abcdef";

    sbuf_add(b, "\"");
    if (s != NULL) {
        for (; *s != '\0'; s++) {
            unsigned char c = (unsigned char)*s;
            switch (c) {
            case '"':  sbuf_add(b, "\\\""); break;
            case '\\': sbuf_add(b, "\\\\"); break;
            case '\b': sbuf_add(b, "\\b");  break;
            case '\f': sbuf_add(b, "\\f");  break;
            case '\n': sbuf_add(b, "\\n");  break;
            case '\r': sbuf_add(b, "\\r");  break;
            case '\t': sbuf_add(b, "\\t");  break;
            default:
                if (c < 0x20) {
                    char esc[7];
                    esc[0] = '\\';
                    esc[1] = 'u';
                    esc[2] = '0';
                    esc[3] = '0';
                    esc[4] = hex[(c >> 4) & 0x0F];
                    esc[5] = hex[c & 0x0F];
                    esc[6] = '\0';
                    sbuf_add(b, esc);
                } else {
                    sbuf_addn(b, (const char *)&c, 1);
                }
                break;
            }
        }
    }
    sbuf_add(b, "\"");
}

const char *sbuf_cstr(const sbuf *b)
{
    return (b->data != NULL) ? b->data : "";
}
