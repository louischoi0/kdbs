#include <linux/kds.h>
#include <linux/kds_types.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/kernel.h>

/* ------------------------------------------------------------------
 * Small manual hex helpers for KDS_TYPE_FLOAT (no kernel hex2bin/
 * bin2hex dependency, to keep this self-contained and avoid any
 * uncertainty about which kernel versions export those helpers from
 * where).
 * ------------------------------------------------------------------ */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex_bytes(const char *str, u8 *out, size_t out_len)
{
    size_t i;

    if (strlen(str) != out_len * 2)
        return -EINVAL;

    for (i = 0; i < out_len; i++) {
        int hi = hex_nibble(str[i * 2]);
        int lo = hex_nibble(str[i * 2 + 1]);

        if (hi < 0 || lo < 0)
            return -EINVAL;

        out[i] = (u8)((hi << 4) | lo);
    }

    return 0;
}

static void format_hex_bytes(const u8 *data, size_t len, char *out)
{
    static const char hexdigits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < len; i++) {
        out[i * 2]     = hexdigits[data[i] >> 4];
        out[i * 2 + 1] = hexdigits[data[i] & 0xf];
    }
    out[len * 2] = '\0';
}

/* ------------------------------------------------------------------
 * Fixed-width signed integers (int8/16/32/64)
 *
 * Parsed via kstrtoll (always available, unlike the narrower
 * kstrtos8/16/32 helpers which were added to the kernel at different
 * points) and range-checked by hand before narrowing.
 * ------------------------------------------------------------------ */

static int encode_int_generic(const char *str_val, void *buf, size_t buf_size,
                               u16 *out_len, u8 width, s64 min, s64 max)
{
    s64 v;
    int ret;

    if (buf_size < width)
        return -ENOSPC;

    ret = kstrtoll(str_val, 10, &v);
    if (ret)
        return ret;

    if (v < min || v > max)
        return -ERANGE;

    switch (width) {
    case 1: { s8  v8  = (s8)v;  memcpy(buf, &v8,  1); break; }
    case 2: { s16 v16 = (s16)v; memcpy(buf, &v16, 2); break; }
    case 4: { s32 v32 = (s32)v; memcpy(buf, &v32, 4); break; }
    case 8: { memcpy(buf, &v, 8); break; }
    default: return -EINVAL;
    }

    *out_len = width;
    return 0;
}

static int decode_int_generic(const void *buf, u16 len, char *out, size_t out_size, u8 width)
{
    s64 v;

    if (len != width)
        return -EINVAL;

    switch (width) {
    case 1: { s8  v8;  memcpy(&v8,  buf, 1); v = v8;  break; }
    case 2: { s16 v16; memcpy(&v16, buf, 2); v = v16; break; }
    case 4: { s32 v32; memcpy(&v32, buf, 4); v = v32; break; }
    case 8: { memcpy(&v, buf, 8); break; }
    default: return -EINVAL;
    }

    return scnprintf(out, out_size, "%lld", v);
}

static int encode_int8(const char *s, void *b, size_t bs, u16 *l)
{ return encode_int_generic(s, b, bs, l, 1, -128, 127); }
static int decode_int8(const void *b, u16 l, char *o, size_t os)
{ return decode_int_generic(b, l, o, os, 1); }

static int encode_int16(const char *s, void *b, size_t bs, u16 *l)
{ return encode_int_generic(s, b, bs, l, 2, -32768, 32767); }
static int decode_int16(const void *b, u16 l, char *o, size_t os)
{ return decode_int_generic(b, l, o, os, 2); }

static int encode_int32(const char *s, void *b, size_t bs, u16 *l)
{ return encode_int_generic(s, b, bs, l, 4, S32_MIN, S32_MAX); }
static int decode_int32(const void *b, u16 l, char *o, size_t os)
{ return decode_int_generic(b, l, o, os, 4); }

static int encode_int64(const char *s, void *b, size_t bs, u16 *l)
{ return encode_int_generic(s, b, bs, l, 8, S64_MIN, S64_MAX); }
static int decode_int64(const void *b, u16 l, char *o, size_t os)
{ return decode_int_generic(b, l, o, os, 8); }

static int compare_int64_generic(const void *a, u16 a_len, const void *b, u16 b_len, u8 width)
{
    s64 av = 0, bv = 0;

    if (a_len != width || b_len != width)
        return 0; /* malformed -- treat as equal rather than guessing an order */

    switch (width) {
    case 1: { s8 x; memcpy(&x, a, 1); av = x; memcpy(&x, b, 1); bv = x; break; }
    case 2: { s16 x; memcpy(&x, a, 2); av = x; memcpy(&x, b, 2); bv = x; break; }
    case 4: { s32 x; memcpy(&x, a, 4); av = x; memcpy(&x, b, 4); bv = x; break; }
    case 8: memcpy(&av, a, 8); memcpy(&bv, b, 8); break;
    }

    return (av > bv) - (av < bv);
}

static int compare_int8(const void *a, u16 al, const void *b, u16 bl)  { return compare_int64_generic(a, al, b, bl, 1); }
static int compare_int16(const void *a, u16 al, const void *b, u16 bl) { return compare_int64_generic(a, al, b, bl, 2); }
static int compare_int32(const void *a, u16 al, const void *b, u16 bl) { return compare_int64_generic(a, al, b, bl, 4); }
static int compare_int64(const void *a, u16 al, const void *b, u16 bl) { return compare_int64_generic(a, al, b, bl, 8); }

/* ------------------------------------------------------------------
 * Bool
 * ------------------------------------------------------------------ */

static int encode_bool(const char *str_val, void *buf, size_t buf_size, u16 *out_len)
{
    u8 v;

    if (buf_size < 1)
        return -ENOSPC;

    v = (!strcmp(str_val, "1") || !strcmp(str_val, "true")) ? 1 : 0;
    memcpy(buf, &v, 1);
    *out_len = 1;
    return 0;
}

static int decode_bool(const void *buf, u16 len, char *out, size_t out_size)
{
    const u8 *v = buf;

    if (len != 1)
        return -EINVAL;

    return scnprintf(out, out_size, "%s", *v ? "true" : "false");
}

/* ------------------------------------------------------------------
 * Varchar / char -- variable length, raw bytes, no transformation.
 * (char is treated identically to varchar for now -- no fixed-blank-
 * padding semantics yet.)
 * ------------------------------------------------------------------ */

static int encode_varchar(const char *str_val, void *buf, size_t buf_size, u16 *out_len)
{
    size_t len = strlen(str_val);

    if (len > U16_MAX || len > buf_size)
        return -ENOSPC;

    memcpy(buf, str_val, len);
    *out_len = (u16)len;
    return 0;
}

static int decode_varchar(const void *buf, u16 len, char *out, size_t out_size)
{
    if ((size_t)len >= out_size)
        return -ENOSPC;

    memcpy(out, buf, len);
    out[len] = '\0';
    return len;
}

/* ------------------------------------------------------------------
 * Float -- NOT real IEEE 754 arithmetic. See kds_types.h's
 * file-level comment: the kernel side only stores/retrieves the raw
 * 4-byte bit pattern, exchanged as an 8-hex-character string. No FPU
 * use anywhere in this path.
 * ------------------------------------------------------------------ */

static int encode_float(const char *str_val, void *buf, size_t buf_size, u16 *out_len)
{
    if (buf_size < 4)
        return -ENOSPC;

    if (parse_hex_bytes(str_val, (u8 *)buf, 4))
        return -EINVAL;

    *out_len = 4;
    return 0;
}

static int decode_float(const void *buf, u16 len, char *out, size_t out_size)
{
    if (len != 4)
        return -EINVAL;
    if (out_size < 9) /* 8 hex chars + NUL */
        return -ENOSPC;

    format_hex_bytes(buf, 4, out);
    return 8;
}

/* ------------------------------------------------------------------
 * Decimal -- fixed-point s64, hardcoded scale (KDS_DECIMAL_SCALE
 * digits after the point). Parsed/formatted by hand; no floating
 * point used. See kds_types.h's file-level comment for the
 * per-column precision/scale limitation.
 * ------------------------------------------------------------------ */

static int encode_decimal(const char *str_val, void *buf, size_t buf_size, u16 *out_len)
{
    const char *p = str_val;
    bool neg = false;
    s64 int_part = 0;
    s64 frac_part = 0;
    int frac_digits = 0;
    s64 result;

    if (buf_size < 8)
        return -ENOSPC;

    if (*p == '-') { neg = true; p++; }
    else if (*p == '+') { p++; }

    if (!*p)
        return -EINVAL;

    while (*p >= '0' && *p <= '9') {
        int_part = int_part * 10 + (*p - '0');
        p++;
    }

    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9' && frac_digits < KDS_DECIMAL_SCALE) {
            frac_part = frac_part * 10 + (*p - '0');
            frac_digits++;
            p++;
        }
        /* Extra fractional digits beyond KDS_DECIMAL_SCALE are
         * silently truncated (not rounded) -- acceptable for a v1
         * fixed-scale implementation; revisit if rounding behavior
         * ever matters to a caller. */
        while (*p >= '0' && *p <= '9')
            p++;
    }

    if (*p != '\0')
        return -EINVAL; /* trailing garbage */

    while (frac_digits < KDS_DECIMAL_SCALE) {
        frac_part *= 10;
        frac_digits++;
    }

    result = int_part * KDS_DECIMAL_SCALE_MUL + frac_part;
    if (neg)
        result = -result;

    memcpy(buf, &result, 8);
    *out_len = 8;
    return 0;
}

static int decode_decimal(const void *buf, u16 len, char *out, size_t out_size)
{
    s64 v;
    s64 int_part, frac_part;
    bool neg = false;

    if (len != 8)
        return -EINVAL;

    memcpy(&v, buf, 8);

    if (v < 0) {
        neg = true;
        v = -v;
    }

    int_part = v / KDS_DECIMAL_SCALE_MUL;
    frac_part = v % KDS_DECIMAL_SCALE_MUL;

    return scnprintf(out, out_size, "%s%lld.%0*lld",
                      neg ? "-" : "", int_part, KDS_DECIMAL_SCALE, frac_part);
}

/* ------------------------------------------------------------------
 * Registry
 * ------------------------------------------------------------------ */

static const kds_type_desc_t kds_type_table[] = {
    { KDS_TYPE_INT8,    "int8",    1, encode_int8,    decode_int8,    compare_int8 },
    { KDS_TYPE_INT16,   "int16",   2, encode_int16,   decode_int16,   compare_int16 },
    { KDS_TYPE_INT32,   "int32",   4, encode_int32,   decode_int32,   compare_int32 },
    { KDS_TYPE_INT64,   "int64",   8, encode_int64,   decode_int64,   compare_int64 },
    { KDS_TYPE_FLOAT,   "float",   4, encode_float,   decode_float,   NULL },
    { KDS_TYPE_DECIMAL, "decimal", 8, encode_decimal, decode_decimal, NULL },
    { KDS_TYPE_BOOL,    "bool",    1, encode_bool,    decode_bool,    NULL },
    { KDS_TYPE_VARCHAR, "varchar", 0, encode_varchar, decode_varchar, NULL },
    { KDS_TYPE_CHAR,    "char",    0, encode_varchar, decode_varchar, NULL },
};

#define KDS_TYPE_TABLE_COUNT (sizeof(kds_type_table) / sizeof(kds_type_table[0]))

const kds_type_desc_t *kds_type_lookup_by_name(const char *name)
{
    u32 i;

    if (!name)
        return NULL;

    for (i = 0; i < KDS_TYPE_TABLE_COUNT; i++) {
        if (!strcmp(kds_type_table[i].name, name))
            return &kds_type_table[i];
    }

    return NULL;
}

const kds_type_desc_t *kds_type_lookup_by_val(kds_type_val_t type_val)
{
    u32 i;

    for (i = 0; i < KDS_TYPE_TABLE_COUNT; i++) {
        if (kds_type_table[i].type_val == type_val)
            return &kds_type_table[i];
    }

    return NULL;
}