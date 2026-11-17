// json.cpp: a text JSON reader and writer, for the AI's HTTP calls, the API and the reminders
// the summary AI writes. Parsed values live in the arena they are read into; a missing key or index
// reads as a zeroed (null) value, so lookups can be chained without checks.

enum JsonType : u8 {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
};

struct Json {
    JsonType type;
    bool b;
    f64 num;
    Str str;      // JSON_STRING
    Json* items;  // JSON_ARRAY and JSON_OBJECT
    Str* keys;    // JSON_OBJECT, one per item
    i32 count;
};

static const Json json_none = {};

const Json* json_get(const Json* j, const char* key) {
    if (j && j->type == JSON_OBJECT) {
        Str k = str(key);
        for (i32 i = 0; i < j->count; i++)
            if (str_eq(j->keys[i], k)) return &j->items[i];
    }
    return &json_none;
}

const Json* json_at(const Json* j, i32 i) {
    if (j && j->type == JSON_ARRAY && i >= 0 && i < j->count) return &j->items[i];
    return &json_none;
}

// The string, or empty when it is not one.
Str json_text(const Json* j) { return j && j->type == JSON_STRING ? j->str : Str{"", 0}; }
// Items of an array (0 for anything else).
i32 json_len(const Json* j) { return j && j->type == JSON_ARRAY ? j->count : 0; }

// ---------------------------------------------------------------------------------------------
// Reading

struct JsonReader {
    Str s;
    i64 at;
    Arena* a;
    i32 depth;
    const char* error;
};

static void json_space(JsonReader* r) {
    while (r->at < r->s.len && (r->s.ptr[r->at] == ' ' || r->s.ptr[r->at] == '\t' || r->s.ptr[r->at] == '\n' || r->s.ptr[r->at] == '\r'))
        r->at++;
}

static void utf8_put(List<char>* out, u32 c) {
    if (c < 0x80) {
        list_push(out, (char)c);
    } else if (c < 0x800) {
        list_push(out, (char)(0xC0 | (c >> 6)));
        list_push(out, (char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        list_push(out, (char)(0xE0 | (c >> 12)));
        list_push(out, (char)(0x80 | ((c >> 6) & 0x3F)));
        list_push(out, (char)(0x80 | (c & 0x3F)));
    } else {
        list_push(out, (char)(0xF0 | (c >> 18)));
        list_push(out, (char)(0x80 | ((c >> 12) & 0x3F)));
        list_push(out, (char)(0x80 | ((c >> 6) & 0x3F)));
        list_push(out, (char)(0x80 | (c & 0x3F)));
    }
}

static bool json_hex4(JsonReader* r, u32* out) {
    if (r->at + 4 > r->s.len) return false;
    u32 v = 0;
    for (i32 i = 0; i < 4; i++) {
        char c = r->s.ptr[r->at++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (u32)(c - 'A' + 10);
        else return false;
    }
    *out = v;
    return true;
}

// At the opening quote. The string goes into the arena.
static bool json_string(JsonReader* r, Str* out) {
    r->at++;
    List<char> buf = {};
    bool ok = false;
    while (r->at < r->s.len) {
        char c = r->s.ptr[r->at++];
        if (c == '"') {
            ok = true;
            break;
        }
        if (c != '\\') {
            list_push(&buf, c);
            continue;
        }
        if (r->at >= r->s.len) break;
        char e = r->s.ptr[r->at++];
        if (e == 'n') list_push(&buf, '\n');
        else if (e == 't') list_push(&buf, '\t');
        else if (e == 'r') list_push(&buf, '\r');
        else if (e == 'b') list_push(&buf, '\b');
        else if (e == 'f') list_push(&buf, '\f');
        else if (e == '"' || e == '\\' || e == '/') list_push(&buf, e);
        else if (e == 'u') {
            u32 c1 = 0;
            if (!json_hex4(r, &c1)) break;
            // A surrogate pair is two \u escapes for one character.
            if (c1 >= 0xD800 && c1 < 0xDC00 && r->at + 1 < r->s.len && r->s.ptr[r->at] == '\\' && r->s.ptr[r->at + 1] == 'u') {
                r->at += 2;
                u32 c2 = 0;
                if (!json_hex4(r, &c2)) break;
                c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
            }
            utf8_put(&buf, c1);
        } else break;
    }
    if (ok) *out = str_copy(r->a, {buf.items, buf.count});
    else r->error = "a string that ends";
    list_free(&buf);
    return ok;
}

static bool json_value(JsonReader* r, Json* out) {
    json_space(r);
    if (r->at >= r->s.len) {
        r->error = "a value";
        return false;
    }
    if (++r->depth > 64) {
        r->error = "less nesting";
        return false;
    }
    char c = r->s.ptr[r->at];
    bool ok = true;
    *out = {};
    if (c == '{' || c == '[') {
        bool object = c == '{';
        out->type = object ? JSON_OBJECT : JSON_ARRAY;
        r->at++;
        List<Json> items = {};
        List<Str> keys = {};
        json_space(r);
        if (r->at < r->s.len && r->s.ptr[r->at] == (object ? '}' : ']')) {
            r->at++;
        } else {
            for (;;) {
                if (object) {
                    json_space(r);
                    Str key = {};
                    if (r->at >= r->s.len || r->s.ptr[r->at] != '"' || !json_string(r, &key)) {
                        if (!r->error) r->error = "a key";
                        ok = false;
                        break;
                    }
                    json_space(r);
                    if (r->at >= r->s.len || r->s.ptr[r->at] != ':') {
                        r->error = "':' after a key";
                        ok = false;
                        break;
                    }
                    r->at++;
                    list_push(&keys, key);
                }
                Json item = {};
                if (!json_value(r, &item)) {
                    ok = false;
                    break;
                }
                list_push(&items, item);
                json_space(r);
                if (r->at < r->s.len && r->s.ptr[r->at] == ',') {
                    r->at++;
                    continue;
                }
                if (r->at < r->s.len && r->s.ptr[r->at] == (object ? '}' : ']')) {
                    r->at++;
                    break;
                }
                r->error = object ? "',' or '}'" : "',' or ']'";
                ok = false;
                break;
            }
        }
        if (ok) {
            out->count = items.count;
            out->items = PUSH_ARRAY(r->a, Json, items.count);
            if (items.count) memcpy(out->items, items.items, sizeof(Json) * items.count);
            if (object) {
                out->keys = PUSH_ARRAY(r->a, Str, keys.count);
                if (keys.count) memcpy(out->keys, keys.items, sizeof(Str) * keys.count);
            }
        }
        list_free(&items);
        list_free(&keys);
    } else if (c == '"') {
        out->type = JSON_STRING;
        ok = json_string(r, &out->str);
    } else if (str_starts(str_sub(r->s, r->at), str("true"))) {
        out->type = JSON_BOOL;
        out->b = true;
        r->at += 4;
    } else if (str_starts(str_sub(r->s, r->at), str("false"))) {
        out->type = JSON_BOOL;
        r->at += 5;
    } else if (str_starts(str_sub(r->s, r->at), str("null"))) {
        r->at += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char num[64];
        i32 n = 0;
        while (r->at < r->s.len && n < 63 && strchr("+-.eE0123456789", r->s.ptr[r->at])) num[n++] = r->s.ptr[r->at++];
        num[n] = 0;
        out->type = JSON_NUMBER;
        out->num = strtod(num, 0);
    } else {
        r->error = "a value";
        ok = false;
    }
    r->depth--;
    return ok;
}

// Reads one JSON value from `s`. False (with `error` saying where) if it is not JSON.
bool json_parse(Arena* a, Str s, Json* out, char* error, i32 error_cap) {
    JsonReader r = {s, 0, a, 0, 0};
    *out = {};
    bool ok = json_value(&r, out);
    if (ok) {
        json_space(&r);
        if (r.at < s.len) {
            ok = false;
            r.error = "the end after the value";
        }
    }
    if (!ok && error) snprintf(error, error_cap, "expected %s at byte %lld", r.error ? r.error : "JSON", r.at);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Writing: text gathered in a Buf (heap), then copied out.

struct Buf {
    List<char> chars;
};

void buf_add(Buf* b, Str s) {
    for (i64 i = 0; i < s.len; i++) list_push(&b->chars, s.ptr[i]);
}
void buf_cstr(Buf* b, const char* s) { buf_add(b, str(s)); }
void buf_fmt(Buf* b, const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n < (int)sizeof(text)) {
        buf_add(b, {text, n});
        return;
    }
    char* big = (char*)malloc((size_t)n + 1);
    CHECK(big, "memory for %d characters", n);
    va_start(args, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, args);
    va_end(args);
    buf_add(b, {big, n});
    free(big);
}
i64 buf_len(Buf* b) { return b->chars.count; }
// Copies the text into `a` (zero-terminated) and frees the buffer.
Str buf_take(Buf* b, Arena* a) {
    Str s = str_copy(a, {b->chars.items, b->chars.count});
    list_free(&b->chars);
    return s;
}
void buf_free(Buf* b) { list_free(&b->chars); }

// `s` as a JSON string, quotes included.
void json_quote(Buf* b, Str s) {
    list_push(&b->chars, '"');
    for (i64 i = 0; i < s.len; i++) {
        u8 c = (u8)s.ptr[i];
        if (c == '"') buf_cstr(b, "\\\"");
        else if (c == '\\') buf_cstr(b, "\\\\");
        else if (c == '\n') buf_cstr(b, "\\n");
        else if (c == '\r') buf_cstr(b, "\\r");
        else if (c == '\t') buf_cstr(b, "\\t");
        else if (c < 0x20) buf_fmt(b, "\\u%04x", c);
        else list_push(&b->chars, (char)c);
    }
    list_push(&b->chars, '"');
}

// `s` as a JSON string, or null when it is empty.
void json_quote_or_null(Buf* b, const char* s) {
    if (s && s[0]) json_quote(b, str(s));
    else buf_cstr(b, "null");
}

void json_write(Buf* b, const Json* j) {
    switch (j->type) {
    case JSON_NULL: buf_cstr(b, "null"); break;
    case JSON_BOOL: buf_cstr(b, j->b ? "true" : "false"); break;
    case JSON_NUMBER:
        if (j->num == floor(j->num) && fabs(j->num) < 1e15) buf_fmt(b, "%lld", (long long)j->num);
        else buf_fmt(b, "%.17g", j->num);
        break;
    case JSON_STRING: json_quote(b, j->str); break;
    case JSON_ARRAY:
    case JSON_OBJECT:
        list_push(&b->chars, j->type == JSON_ARRAY ? '[' : '{');
        for (i32 i = 0; i < j->count; i++) {
            if (i) list_push(&b->chars, ',');
            if (j->type == JSON_OBJECT) {
                json_quote(b, j->keys[i]);
                list_push(&b->chars, ':');
            }
            json_write(b, &j->items[i]);
        }
        list_push(&b->chars, j->type == JSON_ARRAY ? ']' : '}');
        break;
    }
}
