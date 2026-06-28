#ifndef __KDS_TYPES_H
#define __KDS_TYPES_H

#include <linux/kds.h>

/*
 * Centralized scalar type registry. catalog.c only ever needs to
 * store/compare a type_val (an opaque u32 identifying a row in
 * sys.types); it should never need to know how many bytes a given
 * type takes or how to parse/format one -- that's exactly the
 * responsibility this file exists to isolate, so adding a new type
 * doesn't require touching catalog.c, dshell.c, and (eventually)
 * the executor/btree key comparison code all at once.
 *
 * IMPORTANT, FLOAT: the kernel does not freely permit floating-point
 * arithmetic (FPU state isn't safe to touch without explicit
 * kernel_fpu_begin()/kernel_fpu_end() bracketing, which is heavy and
 * generally avoided). KDS_TYPE_FLOAT therefore does NOT do any IEEE
 * 754 math in-kernel -- it stores/retrieves the raw 4-byte bit
 * pattern verbatim, exchanged as an 8-character hex string at the
 * dshell text boundary (e.g. "3f800000" for 1.0f). Interpreting or
 * computing on that bit pattern as an actual float is the client's
 * job, not the kernel module's.
 *
 * IMPORTANT, DECIMAL: no arbitrary-precision or per-column
 * precision/scale support yet -- v1 stores a fixed-point s64 with a
 * hardcoded scale of KDS_DECIMAL_SCALE digits, parsed/formatted by
 * hand (no floating point involved either). Per-column
 * precision/scale would need an extra field on kds_sys_column_t
 * (kds_catalog.h) -- not added here to avoid changing a struct other
 * code already depends on without being asked to.
 */

#define KDS_DECIMAL_SCALE       4               /* fixed digits after the decimal point, v1 */
#define KDS_DECIMAL_SCALE_MUL   10000LL          /* 10^KDS_DECIMAL_SCALE */

typedef enum kds_type_val {
    KDS_TYPE_INT8    = 0,
    KDS_TYPE_INT16   = 1,
    KDS_TYPE_INT32   = 2,
    KDS_TYPE_INT64   = 3,
    KDS_TYPE_FLOAT   = 4,
    KDS_TYPE_DECIMAL = 5,
    KDS_TYPE_BOOL    = 6,
    KDS_TYPE_VARCHAR = 7,
    KDS_TYPE_CHAR    = 8,
} kds_type_val_t;

/*
 * Encodes `str_val` (a NUL-terminated text token, e.g. straight from
 * a dshell command argument) into `buf`. Writes *out_len bytes.
 * Returns 0 on success, negative errno on a parse error or if it
 * doesn't fit in buf_size.
 */
typedef int (*kds_type_encode_fn)(const char *str_val, void *buf,
                                   size_t buf_size, u16 *out_len);

/*
 * Decodes `len` bytes at `buf` (as written by the matching encode_fn)
 * into a human-readable NUL-terminated string in `out`. Returns the
 * number of characters written (not counting the NUL), or negative
 * errno.
 */
typedef int (*kds_type_decode_fn)(const void *buf, u16 len,
                                   char *out, size_t out_size);

/*
 * Three-way comparison for two encoded values of this type, for
 * future btree key ordering. Not wired up anywhere yet -- exists so
 * a type's comparison rule lives next to its encode/decode rule
 * instead of being reinvented at each call site later.
 */
typedef int (*kds_type_compare_fn)(const void *a, u16 a_len,
                                    const void *b, u16 b_len);

typedef struct kds_type_desc {
    kds_type_val_t          type_val;
    const char              *name;
    u32                      fixed_len;   /* 0 = variable-length (varchar/char) */
    kds_type_encode_fn       encode;
    kds_type_decode_fn       decode;
    kds_type_compare_fn      compare;     /* may be NULL if not yet needed */
} kds_type_desc_t;

const kds_type_desc_t *kds_type_lookup_by_name(const char *name);
const kds_type_desc_t *kds_type_lookup_by_val(kds_type_val_t type_val);

#endif /* __KDS_TYPES_H */