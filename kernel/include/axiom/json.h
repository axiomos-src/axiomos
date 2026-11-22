/* AxiomOS - JSON reader interface.
 *
 * The tree keeps its policy files in JSON: the HVCI allow list, the network
 * service registry, the GUI theme overrides and the boot measurement manifest.
 * Parsing is zero-copy for numbers (the text span is preserved) and arena
 * based for strings, and all nodes live in one document object so that a
 * policy file can be dropped by freeing a single block.
 */
#ifndef AXIOM_JSON_H
#define AXIOM_JSON_H

#include <axiom/types.h>

#define JSON_MAX_DEPTH      12
#define JSON_MAX_NODES      96
#define JSON_MAX_CHILDREN   96
#define JSON_MAX_NUMBER     40

enum json_type {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
};

struct json_value;

struct json_child {
    const char        *key;      /* NULL for array elements */
    struct json_value *value;
};

struct json_value {
    u8 type;
    union {
        bool_t boolean;
        struct {
            const char *text;
            u32         length;
        } number;
        const char *string;
        struct {
            struct json_child *children;
            u32                count;
        } aggregate;   /* arrays and objects */
    } as;
};

struct json_doc {
    struct json_value  nodes[JSON_MAX_NODES];
    u32                node_count;
    struct json_child  children[JSON_MAX_CHILDREN];
    u32                child_count;
    char              *arena;
    usize              arena_cap;
    usize              arena_used;
};

/* Parse `text` into `doc`.  The document must be provided by the caller; the
 * arena holds the decoded string bytes.  Returns 0 on success, -1 on error. */
int json_parse(struct json_doc *doc, const char *text, char *arena, usize arena_cap);

struct json_value       *json_root(struct json_doc *doc);
const struct json_value *json_object_get(const struct json_value *object, const char *key);
u32                      json_object_size(const struct json_value *object);
const char              *json_object_key_at(const struct json_value *object, u32 index);
const struct json_value *json_object_value_at(const struct json_value *object, u32 index);
const struct json_value *json_array_at(const struct json_value *array, u32 index);
u32                      json_array_size(const struct json_value *array);
bool_t                   json_is_type(const struct json_value *v, u8 type);

const char *json_as_string(const struct json_value *v);
bool_t      json_as_bool(const struct json_value *v, bool_t fallback);
i64         json_as_i64(const struct json_value *v, i64 fallback);
u64         json_as_u64(const struct json_value *v, u64 fallback);
u64         json_as_size(const struct json_value *v, u64 fallback);

/* Writer: emits a compact document into a fixed buffer. */
struct json_writer {
    char  *dst;
    usize  cap;
    usize  used;
    bool_t overflow;
    bool_t need_comma;
    u32    depth;
};

void json_writer_init(struct json_writer *w, char *dst, usize cap);
void json_writer_begin_object(struct json_writer *w);
void json_writer_end_object(struct json_writer *w);
void json_writer_begin_array(struct json_writer *w);
void json_writer_end_array(struct json_writer *w);
void json_writer_key(struct json_writer *w, const char *key);
void json_writer_string(struct json_writer *w, const char *value);
void json_writer_u64(struct json_writer *w, u64 value);
void json_writer_i64(struct json_writer *w, i64 value);
void json_writer_bool(struct json_writer *w, bool_t value);
void json_writer_null(struct json_writer *w);
int  json_writer_finish(struct json_writer *w);

/* Escape a string for inclusion in a document; returns the bytes stored. */
usize json_escape(const char *text, char *dst, usize cap);

#endif /* AXIOM_JSON_H */
