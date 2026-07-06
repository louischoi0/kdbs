#ifndef __KDS_PARSER_H
#define __KDS_PARSER_H

#include <linux/kds.h>
#include <linux/kds_catalog.h>   /* kds_clustered_type_t, KDS_SCHEMA_MAX_COLUMNS */
#include <linux/kds_types.h>     /* kds_type_val_t */

/*
 * KDS SQL subset parser.
 *
 * Supported grammar:
 *
 *   CREATE TABLE <name> (<col> <type> [, ...]) [HEAP | BTREE];
 *   INSERT INTO  <name> VALUES (<val> [, ...]);
 *   SELECT *     FROM   <name> [WHERE <cond> [AND <cond>]*];
 *   SHOW META;
 *   SHOW ALLOC;
 *
 * <type>  ::= INT8 | INT16 | INT32 | INT64 | FLOAT | DECIMAL
 *           | BOOL | VARCHAR | CHAR
 *
 * <cond>  ::= <col> <op> <val>
 * <op>    ::= = | != | < | <= | > | >=
 * <val>   ::= integer literal | 'string literal' | NULL
 *
 * Limitations (deliberate, to keep the kernel-side implementation
 * small):
 *   - No JOINs, subqueries, GROUP BY, ORDER BY, or aggregates.
 *   - WHERE conditions are ANDs only (no OR, no NOT, no nesting).
 *   - SELECT column list is always * (projection is deferred).
 *   - String literals may not contain single quotes (no escaping).
 *   - Identifiers are case-insensitive; string values are not.
 */

/* ------------------------------------------------------------------
 * Size limits
 * ------------------------------------------------------------------ */

#define KDS_PARSER_NAME_MAX    64
#define KDS_PARSER_VAL_MAX    256   /* max length of one value literal */
#define KDS_PARSER_MAX_COLS    KDS_SCHEMA_MAX_COLUMNS
#define KDS_PARSER_MAX_VALS    KDS_SCHEMA_MAX_COLUMNS
#define KDS_PARSER_MAX_CONDS   16
#define KDS_PARSER_ERR_MAX    256

/* ------------------------------------------------------------------
 * Token types (lexer output)
 * ------------------------------------------------------------------ */

typedef enum {
    KDS_TOK_EOF = 0,

    /* literals */
    KDS_TOK_IDENT,          /* table/column name, keyword        */
    KDS_TOK_INT_LIT,        /* 42, -7                            */
    KDS_TOK_STR_LIT,        /* 'hello'  (quotes stripped)        */
    KDS_TOK_NULL,           /* NULL                              */

    /* punctuation */
    KDS_TOK_LPAREN,         /* (  */
    KDS_TOK_RPAREN,         /* )  */
    KDS_TOK_COMMA,          /* ,  */
    KDS_TOK_SEMICOLON,      /* ;  */
    KDS_TOK_STAR,           /* *  */

    /* comparison operators */
    KDS_TOK_EQ,             /* =  */
    KDS_TOK_NEQ,            /* != */
    KDS_TOK_LT,             /* <  */
    KDS_TOK_LTE,            /* <= */
    KDS_TOK_GT,             /* >  */
    KDS_TOK_GTE,            /* >= */

    KDS_TOK_ERROR,          /* unrecognised character            */
} kds_token_type_t;

typedef struct {
    kds_token_type_t type;
    char             text[KDS_PARSER_VAL_MAX]; /* raw token text */
    s64              int_val;                   /* valid when type == INT_LIT */
} kds_token_t;

/* ------------------------------------------------------------------
 * AST value node (used in both INSERT values and WHERE conditions)
 * ------------------------------------------------------------------ */

typedef enum {
    KDS_VAL_INT,
    KDS_VAL_STR,
    KDS_VAL_NULL,
} kds_val_type_t;

typedef struct {
    kds_val_type_t type;
    s64            int_val;                    /* KDS_VAL_INT  */
    char           str_val[KDS_PARSER_VAL_MAX]; /* KDS_VAL_STR */
} kds_ast_val_t;

/* ------------------------------------------------------------------
 * AST: column definition (used in CREATE TABLE)
 * ------------------------------------------------------------------ */

typedef struct {
    char          name[KDS_PARSER_NAME_MAX];
    kds_type_val_t type_val;   /* resolved by the parser via kds_type_lookup_by_name() */
    u32           len;         /* fixed_len from type descriptor; 0 for variable-width */
} kds_ast_col_def_t;

/* ------------------------------------------------------------------
 * AST: WHERE condition
 *
 * Each condition is a simple <col> <op> <val> triple. Multiple
 * conditions are combined with implicit AND (array in the SELECT
 * statement node below).
 * ------------------------------------------------------------------ */

typedef enum {
    KDS_OP_EQ = 0,
    KDS_OP_NEQ,
    KDS_OP_LT,
    KDS_OP_LTE,
    KDS_OP_GT,
    KDS_OP_GTE,
} kds_cond_op_t;

typedef struct {
    char          col_name[KDS_PARSER_NAME_MAX];
    kds_cond_op_t op;
    kds_ast_val_t val;
} kds_ast_cond_t;

/* ------------------------------------------------------------------
 * Statement types
 * ------------------------------------------------------------------ */

typedef enum {
    KDS_STMT_CREATE_TABLE,
    KDS_STMT_INSERT,
    KDS_STMT_SELECT,
    KDS_STMT_SHOW_META,
    KDS_STMT_SHOW_ALLOC,
} kds_stmt_type_t;

/* ------------------------------------------------------------------
 * AST: statement nodes (one per statement type)
 *
 * All string storage is inline (no heap pointers) so the whole AST
 * lives in one kds_stmt_t allocation and can be freed with a single
 * kfree() / stack unwind.
 * ------------------------------------------------------------------ */

typedef struct {
    char                 table_name[KDS_PARSER_NAME_MAX];
    kds_ast_col_def_t    cols[KDS_PARSER_MAX_COLS];
    u32                  nr_cols;
    kds_clustered_type_t clustered;   /* default: KDS_CLUSTERED_HEAP */
} kds_stmt_create_table_t;

typedef struct {
    char          table_name[KDS_PARSER_NAME_MAX];
    kds_ast_val_t values[KDS_PARSER_MAX_VALS];
    u32           nr_values;
} kds_stmt_insert_t;

typedef struct {
    char           table_name[KDS_PARSER_NAME_MAX];
    bool           has_where;
    kds_ast_cond_t conds[KDS_PARSER_MAX_CONDS];  /* AND-combined */
    u32            nr_conds;
} kds_stmt_select_t;

/* ------------------------------------------------------------------
 * Top-level statement (tagged union)
 * ------------------------------------------------------------------ */

typedef struct {
    kds_stmt_type_t type;
    union {
        kds_stmt_create_table_t create_table;
        kds_stmt_insert_t       insert;
        kds_stmt_select_t       select;
        /* show_meta / show_alloc carry no fields */
    };
} kds_stmt_t;

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

/*
 * kds_parse(): parse one SQL statement from `sql` into *out_stmt.
 *
 * Returns 0 on success. On error, returns -EINVAL and writes a
 * human-readable description of the problem into err_buf (up to
 * err_buf_size bytes including the NUL terminator). err_buf may be
 * NULL if the caller doesn't need the message.
 *
 * *out_stmt is stack- or caller-allocated; this function never
 * allocates heap memory of its own, so the caller doesn't need to
 * free anything on either success or failure.
 */
int kds_parse(const char *sql, kds_stmt_t *out_stmt,
              char *err_buf, size_t err_buf_size);

/*
 * kds_stmt_type_name(): human-readable statement type, for logging.
 */
const char *kds_stmt_type_name(kds_stmt_type_t type);

/*
 * kds_cond_op_name(): human-readable operator, for logging / EXPLAIN.
 */
const char *kds_cond_op_name(kds_cond_op_t op);

#endif /* __KDS_PARSER_H */