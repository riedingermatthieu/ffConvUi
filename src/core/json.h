/*
 * json.h - minimal JSON reader/writer for job files (RFC 8259 subset:
 * everything except numbers outside double range). No dependencies beyond
 * libavutil; parsing does not depend on the C locale.
 */
#ifndef CONV_JSON_H
#define CONV_JSON_H

#include <stddef.h>

#include <libavutil/bprint.h>

typedef enum JsonType {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonType;

typedef struct JsonValue {
    JsonType          type;
    int               boolean;
    double            number;
    char             *string;    /* JSON_STRING, UTF-8 */
    struct JsonValue *items;     /* JSON_ARRAY elements / JSON_OBJECT values */
    char            **keys;      /* JSON_OBJECT keys, parallel to items */
    int               nb_items;
} JsonValue;

/* Parse `text`. On error returns a negative AVERROR and writes
 * "line L, column C: message" to err. */
int  json_parse(const char *text, JsonValue **out, char *err, size_t errlen);
void json_free(JsonValue **v);

/* Object member lookup, NULL if absent or `obj` is not an object. */
const JsonValue *json_get(const JsonValue *obj, const char *key);
const char      *json_type_name(JsonType t);

/* Writer helpers: append a quoted, escaped JSON string. */
void json_write_string(AVBPrint *bp, const char *s);

#endif /* CONV_JSON_H */
