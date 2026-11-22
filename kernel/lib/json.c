/* AxiomOS - JSON reader and writer.
 *
 * The parser is a plain recursive descent over the document text.  Nodes come
 * from a fixed pool inside struct json_doc, string bytes from the caller's
 * arena, and numbers keep a pointer to their original text so that integers
 * larger than 64 bits can still be inspected as text by a policy checker.
 * The recursion depth is bounded by JSON_MAX_DEPTH, which is what makes it safe
 * to run on attacker-controlled files from the ESP.
 */

#include <axiom/lib.h>
#include <axiom/json.h>

/* ------------------------------------------------------------------ */
/* Reader                                                             */
/* ------------------------------------------------------------------ */

static const char *json_skip_ws(const char *p)
{
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
            continue;
        }
        /* Policy files are hand edited, so comments are tolerated. */
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') {
                p++;
            }
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                p++;
            }
            if (*p) {
                p += 2;
            }
            continue;
        }
        break;
    }
    return p;
}

static struct json_value *json_new_node(struct json_doc *doc)
{
    if (doc->node_count >= JSON_MAX_NODES) {
        return NULL_PTR;
    }
    struct json_value *node = &doc->nodes[doc->node_count++];
    ax_memset(node, 0, sizeof(*node));
    return node;
}

static struct json_child *json_new_children(struct json_doc *doc, u32 count)
{
    if (count > JSON_MAX_CHILDREN ||
        doc->child_count + count > JSON_MAX_CHILDREN) {
        return NULL_PTR;
    }
    struct json_child *children = &doc->children[doc->child_count];
    doc->child_count += count;
    ax_memset(children, 0, sizeof(*children) * count);
    return children;
}

/* Decode a quoted string into the arena.  Returns the NUL terminated copy or
 * NULL when the literal is malformed or the arena is full. */
static const char *json_decode_string(const char **pp, struct json_doc *doc)
{
    const char *p = json_skip_ws(*pp);
    usize start = doc->arena_used;

    if (*p != '"') {
        return NULL_PTR;
    }
    p++;
    while (*p && *p != '"') {
        char c = *p;

        if (c == '\\') {
            p++;
            switch (*p) {
            case '"':  c = '"';  p++; break;
            case '\\': c = '\\'; p++; break;
            case '/':  c = '/';  p++; break;
            case 'b':  c = '\b'; p++; break;
            case 'f':  c = '\f'; p++; break;
            case 'n':  c = '\n'; p++; break;
            case 'r':  c = '\r'; p++; break;
            case 't':  c = '\t'; p++; break;
            case 'u': {
                u32 code = 0;
                p++;
                for (u32 i = 0; i < 4 && ax_isxdigit(*p); i++) {
                    code = code * 16 + (u32)ax_digit_value(*p);
                    p++;
                }
                /* Only Basic Latin is representable in the console font. */
                c = code < 0x80 ? (char)code : '?';
                break;
            }
            default:
                return NULL_PTR;
            }
        } else {
            p++;
        }
        if (doc->arena_used + 2 > doc->arena_cap) {
            return NULL_PTR;
        }
        doc->arena[doc->arena_used++] = c;
    }
    if (*p != '"') {
        return NULL_PTR;
    }
    p++;
    if (doc->arena_used + 1 > doc->arena_cap) {
        return NULL_PTR;
    }
    doc->arena[doc->arena_used++] = 0;
    *pp = p;
    return doc->arena + start;
}

static bool_t json_match(const char **pp, const char *literal)
{
    const char *p = json_skip_ws(*pp);
    usize len = ax_strlen(literal);

    if (ax_strncmp(p, literal, len) != 0) {
        return false;
    }
    *pp = p + len;
    return true;
}

static struct json_value *json_parse_value(const char **pp, struct json_doc *doc,
                                           u32 depth);

static struct json_value *json_parse_object(const char **pp, struct json_doc *doc,
                                            u32 depth)
{
    struct json_value *node = json_new_node(doc);
    const char *p = json_skip_ws(*pp);
    u32 capacity = JSON_MAX_CHILDREN;

    if (!node) {
        return NULL_PTR;
    }
    node->type = JSON_OBJECT;
    node->as.aggregate.children = json_new_children(doc, capacity);
    if (!node->as.aggregate.children) {
        return NULL_PTR;
    }
    node->as.aggregate.count = 0;

    p++;      /* '{' */
    p = json_skip_ws(p);
    if (*p == '}') {
        *pp = p + 1;
        return node;
    }
    for (;;) {
        u32 index = node->as.aggregate.count;
        const char *key_start = p;
        const char *key = json_decode_string(&key_start, doc);
        if (!key) {
            return NULL_PTR;
        }
        p = json_skip_ws(key_start);
        if (*p != ':') {
            return NULL_PTR;
        }
        p++;
        struct json_value *value = json_parse_value(&p, doc, depth + 1);
        if (!value) {
            return NULL_PTR;
        }
        node->as.aggregate.children[index].key = key;
        node->as.aggregate.children[index].value = value;
        node->as.aggregate.count = index + 1;

        p = json_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            *pp = p + 1;
            return node;
        }
        return NULL_PTR;
    }
}

static struct json_value *json_parse_array(const char **pp, struct json_doc *doc,
                                           u32 depth)
{
    struct json_value *node = json_new_node(doc);
    const char *p = json_skip_ws(*pp);
    u32 capacity = JSON_MAX_CHILDREN;

    if (!node) {
        return NULL_PTR;
    }
    node->type = JSON_ARRAY;
    node->as.aggregate.children = json_new_children(doc, capacity);
    if (!node->as.aggregate.children) {
        return NULL_PTR;
    }
    node->as.aggregate.count = 0;

    p++;      /* '[' */
    p = json_skip_ws(p);
    if (*p == ']') {
        *pp = p + 1;
        return node;
    }
    for (;;) {
        u32 index = node->as.aggregate.count;
        struct json_value *item = json_parse_value(&p, doc, depth + 1);
        if (!item) {
            return NULL_PTR;
        }
        node->as.aggregate.children[index].key = NULL_PTR;
        node->as.aggregate.children[index].value = item;
        node->as.aggregate.count = index + 1;

        p = json_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']') {
            *pp = p + 1;
            return node;
        }
        return NULL_PTR;
    }
}

static struct json_value *json_parse_value(const char **pp, struct json_doc *doc,
                                           u32 depth)
{
    const char *p = json_skip_ws(*pp);
    struct json_value *node;

    if (depth > JSON_MAX_DEPTH) {
        return NULL_PTR;
    }

    switch (*p) {
    case '{':
        return json_parse_object(pp, doc, depth);
    case '[':
        return json_parse_array(pp, doc, depth);
    case '"': {
        const char *text = json_decode_string(&p, doc);
        if (!text) {
            return NULL_PTR;
        }
        node = json_new_node(doc);
        if (!node) {
            return NULL_PTR;
        }
        node->type = JSON_STRING;
        node->as.string = text;
        *pp = p;
        return node;
    }
    case 't':
        if (!json_match(&p, "true")) {
            return NULL_PTR;
        }
        node = json_new_node(doc);
        if (!node) {
            return NULL_PTR;
        }
        node->type = JSON_BOOL;
        node->as.boolean = true;
        *pp = p;
        return node;
    case 'f':
        if (!json_match(&p, "false")) {
            return NULL_PTR;
        }
        node = json_new_node(doc);
        if (!node) {
            return NULL_PTR;
        }
        node->type = JSON_BOOL;
        node->as.boolean = false;
        *pp = p;
        return node;
    case 'n':
        if (!json_match(&p, "null")) {
            return NULL_PTR;
        }
        node = json_new_node(doc);
        if (!node) {
            return NULL_PTR;
        }
        node->type = JSON_NULL;
        *pp = p;
        return node;
    default: {
        const char *start = p;
        bool_t digits = false;

        if (*p == '-' || *p == '+') {
            p++;
        }
        while (ax_isdigit(*p)) {
            digits = true;
            p++;
        }
        if (*p == '.') {
            p++;
            while (ax_isdigit(*p)) {
                digits = true;
                p++;
            }
        }
        if (*p == 'e' || *p == 'E') {
            p++;
            if (*p == '-' || *p == '+') {
                p++;
            }
            while (ax_isdigit(*p)) {
                p++;
            }
        }
        if (!digits) {
            return NULL_PTR;
        }
        node = json_new_node(doc);
        if (!node) {
            return NULL_PTR;
        }
        node->type = JSON_NUMBER;
        node->as.number.text = start;
        node->as.number.length = (u32)(p - start);
        *pp = p;
        return node;
    }
    }
}

int json_parse(struct json_doc *doc, const char *text, char *arena, usize arena_cap)
{
    const char *p;

    if (!doc || !text || !arena || arena_cap == 0) {
        return -1;
    }
    ax_memset(doc, 0, sizeof(*doc));
    doc->arena = arena;
    doc->arena_cap = arena_cap;

    p = text;
    if (!json_parse_value(&p, doc, 0)) {
        return -1;
    }
    p = json_skip_ws(p);
    if (*p) {
        return -1;
    }
    return 0;
}

struct json_value *json_root(struct json_doc *doc)
{
    if (!doc || doc->node_count == 0) {
        return NULL_PTR;
    }
    return &doc->nodes[0];
}

const struct json_value *json_object_get(const struct json_value *object, const char *key)
{
    if (!object || object->type != JSON_OBJECT || !key) {
        return NULL_PTR;
    }
    for (u32 i = 0; i < object->as.aggregate.count; i++) {
        const struct json_child *c = &object->as.aggregate.children[i];
        if (c->key && ax_strcmp(c->key, key) == 0) {
            return c->value;
        }
    }
    return NULL_PTR;
}

u32 json_object_size(const struct json_value *object)
{
    if (!object || object->type != JSON_OBJECT) {
        return 0;
    }
    return object->as.aggregate.count;
}

const char *json_object_key_at(const struct json_value *object, u32 index)
{
    if (!object || object->type != JSON_OBJECT || index >= object->as.aggregate.count) {
        return NULL_PTR;
    }
    return object->as.aggregate.children[index].key;
}

const struct json_value *json_object_value_at(const struct json_value *object, u32 index)
{
    if (!object || object->type != JSON_OBJECT || index >= object->as.aggregate.count) {
        return NULL_PTR;
    }
    return object->as.aggregate.children[index].value;
}

const struct json_value *json_array_at(const struct json_value *array, u32 index)
{
    if (!array || array->type != JSON_ARRAY || index >= array->as.aggregate.count) {
        return NULL_PTR;
    }
    return array->as.aggregate.children[index].value;
}

u32 json_array_size(const struct json_value *array)
{
    if (!array || array->type != JSON_ARRAY) {
        return 0;
    }
    return array->as.aggregate.count;
}

bool_t json_is_type(const struct json_value *v, u8 type)
{
    return v && v->type == type;
}

const char *json_as_string(const struct json_value *v)
{
    return (v && v->type == JSON_STRING) ? v->as.string : NULL_PTR;
}

bool_t json_as_bool(const struct json_value *v, bool_t fallback)
{
    if (!v) {
        return fallback;
    }
    if (v->type == JSON_BOOL) {
        return v->as.boolean;
    }
    if (v->type == JSON_STRING) {
        bool_t out = fallback;
        if (ax_parse_bool(v->as.string, &out) == 0) {
            return out;
        }
    }
    return fallback;
}

/* Numbers are text spans; copy them into a scratch buffer before conversion so
 * that ax_strtoll can terminate the digit run. */
static bool_t json_number_text(const struct json_value *v, char *buf, usize cap)
{
    if (!v || v->type != JSON_NUMBER || cap < 2) {
        return false;
    }
    usize n = v->as.number.length;
    if (n >= cap) {
        n = cap - 1;
    }
    ax_memcpy(buf, v->as.number.text, n);
    buf[n] = 0;
    return true;
}

i64 json_as_i64(const struct json_value *v, i64 fallback)
{
    char buf[JSON_MAX_NUMBER + 1];

    if (v && v->type == JSON_STRING) {
        return ax_strtoll(v->as.string, NULL_PTR, 0);
    }
    if (!json_number_text(v, buf, sizeof(buf))) {
        return fallback;
    }
    return ax_strtoll(buf, NULL_PTR, 0);
}

u64 json_as_u64(const struct json_value *v, u64 fallback)
{
    char buf[JSON_MAX_NUMBER + 1];

    if (v && v->type == JSON_STRING) {
        return ax_strtoull(v->as.string, NULL_PTR, 0);
    }
    if (!json_number_text(v, buf, sizeof(buf))) {
        return fallback;
    }
    return ax_strtoull(buf, NULL_PTR, 0);
}

/* Size strings accept the ax_parse_size suffixes ("4M", "512K"). */
u64 json_as_size(const struct json_value *v, u64 fallback)
{
    u64 out = 0;
    char buf[JSON_MAX_NUMBER + 1];

    if (v && v->type == JSON_STRING) {
        if (ax_parse_size(v->as.string, &out) == 0) {
            return out;
        }
        return fallback;
    }
    if (json_number_text(v, buf, sizeof(buf))) {
        return ax_strtoull(buf, NULL_PTR, 0);
    }
    return fallback;
}

/* ------------------------------------------------------------------ */
/* Writer                                                             */
/* ------------------------------------------------------------------ */

static void writer_putc(struct json_writer *w, char c)
{
    if (w->used + 1 >= w->cap) {
        w->overflow = true;
        return;
    }
    w->dst[w->used++] = c;
    w->dst[w->used] = 0;
}

static void writer_puts(struct json_writer *w, const char *s)
{
    while (*s) {
        writer_putc(w, *s++);
    }
}

usize json_escape(const char *text, char *dst, usize cap)
{
    usize used = 0;

    if (!text) {
        text = "";
    }
    for (const char *p = text; *p; p++) {
        const char *rep = NULL_PTR;
        switch (*p) {
        case '"':  rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n"; break;
        case '\r': rep = "\\r"; break;
        case '\t': rep = "\\t"; break;
        default: break;
        }
        if (rep) {
            usize n = ax_strlen(rep);
            if (used + n + 1 >= cap) {
                break;
            }
            ax_memcpy(dst + used, rep, n);
            used += n;
        } else {
            if (used + 2 >= cap) {
                break;
            }
            if ((u8)*p < 0x20) {
                dst[used++] = '?';
            } else {
                dst[used++] = *p;
            }
        }
    }
    dst[used] = 0;
    return used;
}

void json_writer_init(struct json_writer *w, char *dst, usize cap)
{
    w->dst = dst;
    w->cap = cap;
    w->used = 0;
    w->overflow = false;
    w->need_comma = false;
    w->depth = 0;
    if (dst && cap) {
        dst[0] = 0;
    }
}

static void writer_separator(struct json_writer *w)
{
    if (w->need_comma) {
        writer_putc(w, ',');
    }
    w->need_comma = true;
}

void json_writer_begin_object(struct json_writer *w)
{
    writer_separator(w);
    writer_putc(w, '{');
    w->need_comma = false;
    w->depth++;
}

void json_writer_end_object(struct json_writer *w)
{
    writer_putc(w, '}');
    w->need_comma = true;
    if (w->depth) {
        w->depth--;
    }
}

void json_writer_begin_array(struct json_writer *w)
{
    writer_separator(w);
    writer_putc(w, '[');
    w->need_comma = false;
    w->depth++;
}

void json_writer_end_array(struct json_writer *w)
{
    writer_putc(w, ']');
    w->need_comma = true;
    if (w->depth) {
        w->depth--;
    }
}

void json_writer_key(struct json_writer *w, const char *key)
{
    char escaped[128];

    writer_separator(w);
    writer_putc(w, '"');
    json_escape(key, escaped, sizeof(escaped));
    writer_puts(w, escaped);
    writer_puts(w, "\":");
    w->need_comma = false;
}

void json_writer_string(struct json_writer *w, const char *value)
{
    char escaped[256];

    if (!w->need_comma && w->depth == 0) {
        /* first top level value needs no comma */
    } else {
        writer_separator(w);
    }
    writer_putc(w, '"');
    json_escape(value, escaped, sizeof(escaped));
    writer_puts(w, escaped);
    writer_putc(w, '"');
    w->need_comma = true;
}

void json_writer_u64(struct json_writer *w, u64 value)
{
    char buf[24];

    if (w->need_comma) {
        writer_separator(w);
    } else if (w->depth) {
        w->need_comma = true;
    }
    ax_utoa(value, buf);
    writer_puts(w, buf);
    w->need_comma = true;
}

void json_writer_i64(struct json_writer *w, i64 value)
{
    char buf[24];

    if (w->need_comma) {
        writer_separator(w);
    } else if (w->depth) {
        w->need_comma = true;
    }
    ax_itoa(value, buf);
    writer_puts(w, buf);
    w->need_comma = true;
}

void json_writer_bool(struct json_writer *w, bool_t value)
{
    if (w->need_comma) {
        writer_separator(w);
    } else if (w->depth) {
        w->need_comma = true;
    }
    writer_puts(w, value ? "true" : "false");
    w->need_comma = true;
}

void json_writer_null(struct json_writer *w)
{
    if (w->need_comma) {
        writer_separator(w);
    }
    writer_puts(w, "null");
    w->need_comma = true;
}

int json_writer_finish(struct json_writer *w)
{
    if (!w->dst || w->overflow) {
        return -1;
    }
    writer_putc(w, '\n');
    return (int)w->used;
}
