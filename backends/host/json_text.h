#ifndef FF8_HOST_JSON_TEXT_H
#define FF8_HOST_JSON_TEXT_H
#include <stddef.h>
/* Keep a complete, valid UTF-8 prefix after a bounded upstream write. */
static inline void host_utf8_truncate(char *text, size_t capacity) {
    size_t i = 0;
    if (!capacity) return;
    text[capacity - 1] = 0;
    while (text[i]) {
        unsigned char ch = (unsigned char)text[i];
        size_t n = ch < 0x80 ? 1 : ch >= 0xc2 && ch <= 0xdf ? 2 :
            ch >= 0xe0 && ch <= 0xef ? 3 : ch >= 0xf0 && ch <= 0xf4 ? 4 : 0;
        size_t k;
        if (!n || n >= capacity - i) break;
        for (k = 1; k < n && ((unsigned char)text[i + k] & 0xc0) == 0x80; ++k) {}
        if (k != n || (ch == 0xe0 && (unsigned char)text[i + 1] < 0xa0) ||
            (ch == 0xed && (unsigned char)text[i + 1] >= 0xa0) ||
            (ch == 0xf0 && (unsigned char)text[i + 1] < 0x90) ||
            (ch == 0xf4 && (unsigned char)text[i + 1] >= 0x90)) break;
        i += n;
    }
    text[i] = 0;
}
/* Quote UTF-8 text as JSON. Return false rather than emitting truncated JSON. */
static inline int host_json_quote(char *out, size_t capacity, const char *text) {
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)text;
    if (capacity < 3) return 0;
    out[n++] = '"';
    while (*p) {
        unsigned char ch = *p++;
        size_t need = ch < 32 ? 6 : (ch == '"' || ch == '\\') ? 2 : 1;
        if (need + 2 > capacity - n) return 0;
        if (ch < 32) {
            out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0';
            out[n++] = hex[ch >> 4]; out[n++] = hex[ch & 15];
        } else { if (need == 2) out[n++] = '\\'; out[n++] = (char)ch; }
    }
    out[n++] = '"'; out[n] = 0;
    return 1;
}
#endif
