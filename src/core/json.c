/*
 * json.c - minimal JSON reader/writer.
 */
#include "json.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/error.h>
#include <libavutil/mem.h>

#define MAX_DEPTH 64

typedef struct Parser {
    const char *s;
    const char *start;
    char       *err;
    size_t      errlen;
    int         failed;
} Parser;

static void fail(Parser *p, const char *msg)
{
    int line = 1, col = 1;

    if (p->failed)
        return;
    p->failed = 1;
    for (const char *c = p->start; c < p->s; c++) {
        if (*c == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    if (p->err && p->errlen)
        snprintf(p->err, p->errlen, "line %d, column %d: %s", line, col, msg);
}

static void skip_ws(Parser *p)
{
    while (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')
        p->s++;
}

static void free_value(JsonValue *v)
{
    if (!v)
        return;
    av_freep(&v->string);
    for (int i = 0; i < v->nb_items; i++) {
        free_value(&v->items[i]);
        if (v->keys)
            av_freep(&v->keys[i]);
    }
    av_freep(&v->items);
    av_freep(&v->keys);
    v->nb_items = 0;
}

static int parse_value(Parser *p, JsonValue *v, int depth);

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;

    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    *out = v;
    return 0;
}

static void put_utf8(AVBPrint *bp, unsigned cp)
{
    if (cp < 0x80) {
        av_bprint_chars(bp, cp, 1);
    } else if (cp < 0x800) {
        av_bprintf(bp, "%c%c", 0xC0 | (cp >> 6), 0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        av_bprintf(bp, "%c%c%c", 0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F),
                   0x80 | (cp & 0x3F));
    } else {
        av_bprintf(bp, "%c%c%c%c", 0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
                   0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
    }
}

static int parse_string(Parser *p, char **out)
{
    AVBPrint bp;

    *out = NULL;
    if (*p->s != '"') {
        fail(p, "expected string");
        return AVERROR_INVALIDDATA;
    }
    p->s++;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);

    while (*p->s != '"') {
        unsigned char c = *p->s;

        if (!c) {
            fail(p, "unterminated string");
            goto error;
        }
        if (c < 0x20) {
            fail(p, "control character in string");
            goto error;
        }
        if (c != '\\') {
            av_bprint_chars(&bp, c, 1);
            p->s++;
            continue;
        }
        p->s++;
        switch (*p->s) {
        case '"':  av_bprint_chars(&bp, '"', 1);  break;
        case '\\': av_bprint_chars(&bp, '\\', 1); break;
        case '/':  av_bprint_chars(&bp, '/', 1);  break;
        case 'b':  av_bprint_chars(&bp, '\b', 1); break;
        case 'f':  av_bprint_chars(&bp, '\f', 1); break;
        case 'n':  av_bprint_chars(&bp, '\n', 1); break;
        case 'r':  av_bprint_chars(&bp, '\r', 1); break;
        case 't':  av_bprint_chars(&bp, '\t', 1); break;
        case 'u': {
            unsigned cp, lo;
            if (hex4(p->s + 1, &cp) < 0) {
                fail(p, "invalid \\u escape");
                goto error;
            }
            p->s += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {          /* surrogate pair */
                if (p->s[1] != '\\' || p->s[2] != 'u' || hex4(p->s + 3, &lo) < 0 ||
                    lo < 0xDC00 || lo > 0xDFFF) {
                    fail(p, "invalid surrogate pair");
                    goto error;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p->s += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                fail(p, "unpaired surrogate");
                goto error;
            }
            if (!cp) {
                fail(p, "\\u0000 is not supported");
                goto error;
            }
            put_utf8(&bp, cp);
            break;
        }
        default:
            fail(p, "invalid escape");
            goto error;
        }
        p->s++;
    }
    p->s++;

    if (!av_bprint_is_complete(&bp) || av_bprint_finalize(&bp, out) < 0) {
        fail(p, "out of memory");
        return AVERROR(ENOMEM);
    }
    return 0;

error:
    av_bprint_finalize(&bp, NULL);
    return AVERROR_INVALIDDATA;
}

/* Locale-independent number parsing (strtod honours LC_NUMERIC, which a
 * GUI toolkit may change). */
static int parse_number(Parser *p, double *out)
{
    const char *s = p->s;
    double v = 0, frac = 0.1;
    int neg = 0, exp = 0, exp_neg = 0;

    if (*s == '-') {
        neg = 1;
        s++;
    }
    if (*s == '0') {
        s++;
    } else if (*s >= '1' && *s <= '9') {
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (*s++ - '0');
    } else {
        fail(p, "invalid number");
        return AVERROR_INVALIDDATA;
    }
    if (*s == '.') {
        s++;
        if (*s < '0' || *s > '9') {
            fail(p, "invalid number");
            return AVERROR_INVALIDDATA;
        }
        while (*s >= '0' && *s <= '9') {
            v += (*s++ - '0') * frac;
            frac /= 10;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '+' || *s == '-')
            exp_neg = *s++ == '-';
        if (*s < '0' || *s > '9') {
            fail(p, "invalid number");
            return AVERROR_INVALIDDATA;
        }
        while (*s >= '0' && *s <= '9' && exp < 10000)
            exp = exp * 10 + (*s++ - '0');
        while (*s >= '0' && *s <= '9')
            s++;
        v *= pow(10, exp_neg ? -exp : exp);
    }
    p->s = s;
    *out = neg ? -v : v;
    return 0;
}

static int parse_array(Parser *p, JsonValue *v, int depth)
{
    int cap = 0, ret;

    v->type = JSON_ARRAY;
    p->s++;
    skip_ws(p);
    if (*p->s == ']') {
        p->s++;
        return 0;
    }
    for (;;) {
        if (v->nb_items == cap) {
            JsonValue *tmp;
            cap = cap ? cap * 2 : 4;
            if (!(tmp = av_realloc_array(v->items, cap, sizeof(*tmp)))) {
                fail(p, "out of memory");
                return AVERROR(ENOMEM);
            }
            v->items = tmp;
        }
        memset(&v->items[v->nb_items], 0, sizeof(*v->items));
        v->nb_items++;
        if ((ret = parse_value(p, &v->items[v->nb_items - 1], depth + 1)) < 0)
            return ret;
        skip_ws(p);
        if (*p->s == ',') {
            p->s++;
            continue;
        }
        if (*p->s == ']') {
            p->s++;
            return 0;
        }
        fail(p, "expected ',' or ']'");
        return AVERROR_INVALIDDATA;
    }
}

static int parse_object(Parser *p, JsonValue *v, int depth)
{
    int cap = 0, ret;

    v->type = JSON_OBJECT;
    p->s++;
    skip_ws(p);
    if (*p->s == '}') {
        p->s++;
        return 0;
    }
    for (;;) {
        char *key;

        skip_ws(p);
        if ((ret = parse_string(p, &key)) < 0)
            return ret;
        for (int i = 0; i < v->nb_items; i++) {
            if (!strcmp(v->keys[i], key)) {
                av_free(key);
                fail(p, "duplicate key");
                return AVERROR_INVALIDDATA;
            }
        }
        if (v->nb_items == cap) {
            JsonValue *items;
            char **keys;
            cap = cap ? cap * 2 : 4;
            items = av_realloc_array(v->items, cap, sizeof(*items));
            if (items)
                v->items = items;
            keys = items ? av_realloc_array(v->keys, cap, sizeof(*keys)) : NULL;
            if (!keys) {
                av_free(key);
                fail(p, "out of memory");
                return AVERROR(ENOMEM);
            }
            v->keys = keys;
        }
        memset(&v->items[v->nb_items], 0, sizeof(*v->items));
        v->keys[v->nb_items] = key;
        v->nb_items++;

        skip_ws(p);
        if (*p->s != ':') {
            fail(p, "expected ':'");
            return AVERROR_INVALIDDATA;
        }
        p->s++;
        if ((ret = parse_value(p, &v->items[v->nb_items - 1], depth + 1)) < 0)
            return ret;
        skip_ws(p);
        if (*p->s == ',') {
            p->s++;
            continue;
        }
        if (*p->s == '}') {
            p->s++;
            return 0;
        }
        fail(p, "expected ',' or '}'");
        return AVERROR_INVALIDDATA;
    }
}

static int parse_literal(Parser *p, const char *word)
{
    size_t n = strlen(word);

    if (strncmp(p->s, word, n)) {
        fail(p, "unexpected character");
        return AVERROR_INVALIDDATA;
    }
    p->s += n;
    return 0;
}

static int parse_value(Parser *p, JsonValue *v, int depth)
{
    if (depth > MAX_DEPTH) {
        fail(p, "nesting too deep");
        return AVERROR_INVALIDDATA;
    }
    skip_ws(p);
    switch (*p->s) {
    case '{': return parse_object(p, v, depth);
    case '[': return parse_array(p, v, depth);
    case '"':
        v->type = JSON_STRING;
        return parse_string(p, &v->string);
    case 't':
        v->type = JSON_BOOL;
        v->boolean = 1;
        return parse_literal(p, "true");
    case 'f':
        v->type = JSON_BOOL;
        return parse_literal(p, "false");
    case 'n':
        v->type = JSON_NULL;
        return parse_literal(p, "null");
    case '\0':
        fail(p, "unexpected end of input");
        return AVERROR_INVALIDDATA;
    default:
        v->type = JSON_NUMBER;
        return parse_number(p, &v->number);
    }
}

int json_parse(const char *text, JsonValue **out, char *err, size_t errlen)
{
    Parser p = { .s = text, .start = text, .err = err, .errlen = errlen };
    JsonValue *v;
    int ret;

    *out = NULL;
    /* tolerate a UTF-8 byte order mark (Windows editors add one) */
    if (!strncmp(p.s, "\xEF\xBB\xBF", 3))
        p.s += 3;

    if (!(v = av_mallocz(sizeof(*v))))
        return AVERROR(ENOMEM);
    ret = parse_value(&p, v, 0);
    if (ret >= 0) {
        skip_ws(&p);
        if (*p.s) {
            fail(&p, "trailing characters after JSON value");
            ret = AVERROR_INVALIDDATA;
        }
    }
    if (ret < 0) {
        json_free(&v);
        return ret;
    }
    *out = v;
    return 0;
}

void json_free(JsonValue **pv)
{
    if (!pv || !*pv)
        return;
    free_value(*pv);
    av_freep(pv);
}

const JsonValue *json_get(const JsonValue *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT)
        return NULL;
    for (int i = 0; i < obj->nb_items; i++)
        if (!strcmp(obj->keys[i], key))
            return &obj->items[i];
    return NULL;
}

const char *json_type_name(JsonType t)
{
    switch (t) {
    case JSON_NULL:   return "null";
    case JSON_BOOL:   return "boolean";
    case JSON_NUMBER: return "number";
    case JSON_STRING: return "string";
    case JSON_ARRAY:  return "array";
    case JSON_OBJECT: return "object";
    default:          return "?";
    }
}

void json_write_string(AVBPrint *bp, const char *s)
{
    av_bprint_chars(bp, '"', 1);
    for (; s && *s; s++) {
        unsigned char c = *s;
        switch (c) {
        case '"':  av_bprintf(bp, "\\\""); break;
        case '\\': av_bprintf(bp, "\\\\"); break;
        case '\n': av_bprintf(bp, "\\n");  break;
        case '\r': av_bprintf(bp, "\\r");  break;
        case '\t': av_bprintf(bp, "\\t");  break;
        default:
            if (c < 0x20)
                av_bprintf(bp, "\\u%04x", c);
            else
                av_bprint_chars(bp, c, 1);
        }
    }
    av_bprint_chars(bp, '"', 1);
}
