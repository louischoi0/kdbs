/* parser.c */
#include <linux/kds.h>
#include <linux/kds_parser.h>
#include <linux/kds_types.h>
#include <linux/kds_catalog.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/errno.h>

/*
 * Recursive descent parser for the KDS SQL subset.
 *
 * Structure:
 *   Lexer   -- kds_lexer_t, driven by kds_lex_next()
 *   Parser  -- kds_parser_t, one parse_*() function per grammar rule
 *
 * Every parse_*() function follows the same contract:
 *   - Returns 0 on success, -EINVAL on syntax error.
 *   - On error, calls parser_err() to fill p->err_buf and returns
 *     immediately; callers propagate the return value upward without
 *     writing additional error messages (the first error wins).
 *   - Consumes tokens from the lexer as it goes; on error the lexer
 *     state is undefined (no backtracking needed since we stop).
 */

/* ------------------------------------------------------------------
 * Lexer
 * ------------------------------------------------------------------ */

typedef struct {
    const char  *src;     /* original input, never modified          */
    const char  *pos;     /* current scan position                   */
    kds_token_t  peek;    /* one-token lookahead, filled by lex_advance() */
    bool         peeked;  /* true when peek holds a valid token      */
} kds_lexer_t;

static void lexer_init(kds_lexer_t *l, const char *sql)
{
    l->src    = sql;
    l->pos    = sql;
    l->peeked = false;
}

/*
 * Skips whitespace and C-style -- line comments (SQL standard).
 */
static void lexer_skip_ws(kds_lexer_t *l)
{
    for (;;) {
        while (*l->pos && isspace((unsigned char)*l->pos))
            l->pos++;

        /* -- line comment */
        if (l->pos[0] == '-' && l->pos[1] == '-') {
            while (*l->pos && *l->pos != '\n')
                l->pos++;
            continue;
        }
        break;
    }
}

/*
 * Reads the next raw token from the input stream into *tok.
 * Does not use the peek buffer.
 */
static void lexer_read(kds_lexer_t *l, kds_token_t *tok)
{
    const char *start;
    size_t      len;

    lexer_skip_ws(l);

    memset(tok, 0, sizeof(*tok));

    if (!*l->pos) {
        tok->type = KDS_TOK_EOF;
        return;
    }

    /* String literal: 'value' */
    if (*l->pos == '\'') {
        start = ++l->pos;
        while (*l->pos && *l->pos != '\'')
            l->pos++;
        len = min_t(size_t, (size_t)(l->pos - start), KDS_PARSER_VAL_MAX - 1);
        memcpy(tok->text, start, len);
        tok->text[len] = '\0';
        tok->type = KDS_TOK_STR_LIT;
        if (*l->pos == '\'')
            l->pos++; /* consume closing quote */
        return;
    }

    /* Integer literal (optional leading minus) */
    if (isdigit((unsigned char)*l->pos) ||
        (*l->pos == '-' && isdigit((unsigned char)l->pos[1]))) {
        start = l->pos;
        if (*l->pos == '-')
            l->pos++;
        while (isdigit((unsigned char)*l->pos))
            l->pos++;
        len = min_t(size_t, (size_t)(l->pos - start), KDS_PARSER_VAL_MAX - 1);
        memcpy(tok->text, start, len);
        tok->text[len] = '\0';
        tok->type    = KDS_TOK_INT_LIT;

        /* kstrtos64 not available in all kernel configs; use simple atoi */
        {
            s64  v   = 0;
            int  neg = (tok->text[0] == '-');
            const char *p = tok->text + (neg ? 1 : 0);

            while (*p)
                v = v * 10 + (*p++ - '0');
            tok->int_val = neg ? -v : v;
        }
        return;
    }

    /* Identifier or keyword */
    if (isalpha((unsigned char)*l->pos) || *l->pos == '_') {
        start = l->pos;
        while (isalnum((unsigned char)*l->pos) || *l->pos == '_')
            l->pos++;
        len = min_t(size_t, (size_t)(l->pos - start), KDS_PARSER_VAL_MAX - 1);
        memcpy(tok->text, start, len);
        tok->text[len] = '\0';
        tok->type = KDS_TOK_IDENT;

        /* Check for NULL keyword (case-insensitive) */
        if (strcasecmp(tok->text, "NULL") == 0)
            tok->type = KDS_TOK_NULL;
        return;
    }

    /* Two-character operators */
    if (*l->pos == '!' && l->pos[1] == '=') {
        tok->type = KDS_TOK_NEQ;
        strcpy(tok->text, "!=");
        l->pos += 2;
        return;
    }
    if (*l->pos == '<' && l->pos[1] == '=') {
        tok->type = KDS_TOK_LTE;
        strcpy(tok->text, "<=");
        l->pos += 2;
        return;
    }
    if (*l->pos == '>' && l->pos[1] == '=') {
        tok->type = KDS_TOK_GTE;
        strcpy(tok->text, ">=");
        l->pos += 2;
        return;
    }

    /* Single-character tokens */
    tok->text[0] = *l->pos;
    tok->text[1] = '\0';
    switch (*l->pos++) {
    case '(':  tok->type = KDS_TOK_LPAREN;    break;
    case ')':  tok->type = KDS_TOK_RPAREN;    break;
    case ',':  tok->type = KDS_TOK_COMMA;     break;
    case ';':  tok->type = KDS_TOK_SEMICOLON; break;
    case '*':  tok->type = KDS_TOK_STAR;      break;
    case '=':  tok->type = KDS_TOK_EQ;        break;
    case '<':  tok->type = KDS_TOK_LT;        break;
    case '>':  tok->type = KDS_TOK_GT;        break;
    default:   tok->type = KDS_TOK_ERROR;     break;
    }
}

/* Peek at the next token without consuming it. */
static const kds_token_t *lexer_peek(kds_lexer_t *l)
{
    if (!l->peeked) {
        lexer_read(l, &l->peek);
        l->peeked = true;
    }
    return &l->peek;
}

/* Consume and return the next token. */
static kds_token_t lexer_next(kds_lexer_t *l)
{
    if (l->peeked) {
        l->peeked = false;
        return l->peek;
    }
    {
        kds_token_t tok;
        lexer_read(l, &tok);
        return tok;
    }
}

/* ------------------------------------------------------------------
 * Parser context
 * ------------------------------------------------------------------ */

typedef struct {
    kds_lexer_t  lex;
    char        *err_buf;
    size_t       err_buf_size;
} kds_parser_t;

static void parser_init(kds_parser_t *p, const char *sql,
                         char *err_buf, size_t err_buf_size)
{
    lexer_init(&p->lex, sql);
    p->err_buf      = err_buf;
    p->err_buf_size = err_buf_size;
}

/*
 * Records the first error message. Subsequent calls are no-ops so
 * the first (most specific) error is preserved.
 */
static void parser_err(kds_parser_t *p, const char *fmt, ...)
{
    va_list ap;

    if (!p->err_buf || p->err_buf_size == 0)
        return;
    if (p->err_buf[0] != '\0') /* already have an error */
        return;

    va_start(ap, fmt);
    vscnprintf(p->err_buf, p->err_buf_size, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------
 * Low-level parser helpers
 * ------------------------------------------------------------------ */

/*
 * Expect the next token to be an identifier matching `keyword`
 * (case-insensitive). Consumes the token on success.
 */
static int expect_keyword(kds_parser_t *p, const char *keyword)
{
    kds_token_t tok = lexer_next(&p->lex);

    if (tok.type != KDS_TOK_IDENT || strcasecmp(tok.text, keyword) != 0) {
        parser_err(p, "expected '%s', got '%s'", keyword,
                   tok.type == KDS_TOK_EOF ? "<end of input>" : tok.text);
        return -EINVAL;
    }
    return 0;
}

/*
 * Expect a specific punctuation token type. Consumes on success.
 */
static int expect_token(kds_parser_t *p, kds_token_type_t type,
                         const char *desc)
{
    kds_token_t tok = lexer_next(&p->lex);

    if (tok.type != type) {
        parser_err(p, "expected %s, got '%s'", desc,
                   tok.type == KDS_TOK_EOF ? "<end of input>" : tok.text);
        return -EINVAL;
    }
    return 0;
}

/*
 * Read an identifier (table or column name) into buf.
 */
static int parse_ident(kds_parser_t *p, char *buf, size_t buf_size)
{
    kds_token_t tok = lexer_next(&p->lex);

    if (tok.type != KDS_TOK_IDENT) {
        parser_err(p, "expected identifier, got '%s'",
                   tok.type == KDS_TOK_EOF ? "<end of input>" : tok.text);
        return -EINVAL;
    }

    strncpy(buf, tok.text, buf_size - 1);
    buf[buf_size - 1] = '\0';
    return 0;
}

/*
 * Optionally consume a semicolon at end of statement. Not required;
 * the parser accepts statements with or without a trailing ';'.
 */
static void consume_optional_semicolon(kds_parser_t *p)
{
    const kds_token_t *peek = lexer_peek(&p->lex);

    if (peek->type == KDS_TOK_SEMICOLON)
        lexer_next(&p->lex);
}

/* ------------------------------------------------------------------
 * Value parser (shared by INSERT and WHERE)
 * ------------------------------------------------------------------ */

static int parse_value(kds_parser_t *p, kds_ast_val_t *val)
{
    kds_token_t tok = lexer_next(&p->lex);

    switch (tok.type) {
    case KDS_TOK_INT_LIT:
        val->type    = KDS_VAL_INT;
        val->int_val = tok.int_val;
        return 0;

    case KDS_TOK_STR_LIT:
        val->type = KDS_VAL_STR;
        strncpy(val->str_val, tok.text, KDS_PARSER_VAL_MAX - 1);
        val->str_val[KDS_PARSER_VAL_MAX - 1] = '\0';
        return 0;

    case KDS_TOK_NULL:
        val->type = KDS_VAL_NULL;
        return 0;

    default:
        parser_err(p, "expected value (integer, 'string', or NULL), got '%s'",
                   tok.type == KDS_TOK_EOF ? "<end of input>" : tok.text);
        return -EINVAL;
    }
}

/* ------------------------------------------------------------------
 * Comparison operator parser (used in WHERE)
 * ------------------------------------------------------------------ */

static int parse_cond_op(kds_parser_t *p, kds_cond_op_t *op)
{
    kds_token_t tok = lexer_next(&p->lex);

    switch (tok.type) {
    case KDS_TOK_EQ:  *op = KDS_OP_EQ;  return 0;
    case KDS_TOK_NEQ: *op = KDS_OP_NEQ; return 0;
    case KDS_TOK_LT:  *op = KDS_OP_LT;  return 0;
    case KDS_TOK_LTE: *op = KDS_OP_LTE; return 0;
    case KDS_TOK_GT:  *op = KDS_OP_GT;  return 0;
    case KDS_TOK_GTE: *op = KDS_OP_GTE; return 0;
    default:
        parser_err(p, "expected comparison operator (=, !=, <, <=, >, >=), got '%s'",
                   tok.type == KDS_TOK_EOF ? "<end of input>" : tok.text);
        return -EINVAL;
    }
}

/* ------------------------------------------------------------------
 * Statement parsers
 * ------------------------------------------------------------------ */

/*
 * CREATE TABLE <name> (<col> <type> [, ...]) [HEAP | BTREE];
 *
 * Cursor is positioned after the CREATE keyword on entry.
 */
static int parse_create_table(kds_parser_t *p, kds_stmt_create_table_t *out)
{
    int ret;

    ret = expect_keyword(p, "TABLE");
    if (ret) return ret;

    ret = parse_ident(p, out->table_name, sizeof(out->table_name));
    if (ret) return ret;

    ret = expect_token(p, KDS_TOK_LPAREN, "'('");
    if (ret) return ret;

    out->nr_cols   = 0;
    out->clustered = KDS_CLUSTERED_HEAP;

    /* Column definitions: name type [, name type]* */
    for (;;) {
        const kds_token_t *peek;
        kds_ast_col_def_t *col;
        const kds_type_desc_t *type_desc;
        char type_name[KDS_PARSER_NAME_MAX];

        if (out->nr_cols >= KDS_PARSER_MAX_COLS) {
            parser_err(p, "too many columns (max %d)", KDS_PARSER_MAX_COLS);
            return -EINVAL;
        }

        col = &out->cols[out->nr_cols];

        ret = parse_ident(p, col->name, sizeof(col->name));
        if (ret) return ret;

        /* Type name */
        ret = parse_ident(p, type_name, sizeof(type_name));
        if (ret) return ret;

        type_desc = kds_type_lookup_by_name(type_name);
        if (!type_desc) {
            parser_err(p, "unknown column type '%s'", type_name);
            return -EINVAL;
        }
        col->type_val = type_desc->type_val;
        col->len      = type_desc->fixed_len;

        out->nr_cols++;

        /* Comma → more columns; RPAREN → done */
        peek = lexer_peek(&p->lex);
        if (peek->type == KDS_TOK_COMMA) {
            lexer_next(&p->lex);
            continue;
        }
        break;
    }

    ret = expect_token(p, KDS_TOK_RPAREN, "')'");
    if (ret) return ret;

    if (out->nr_cols == 0) {
        parser_err(p, "CREATE TABLE requires at least one column");
        return -EINVAL;
    }

    /*
     * Optional storage clause: HEAP | BTREE
     * Peek ahead -- if neither keyword follows, default to HEAP.
     */
    {
        const kds_token_t *peek = lexer_peek(&p->lex);

        if (peek->type == KDS_TOK_IDENT) {
            if (strcasecmp(peek->text, "HEAP") == 0) {
                lexer_next(&p->lex);
                out->clustered = KDS_CLUSTERED_HEAP;
            } else if (strcasecmp(peek->text, "BTREE") == 0) {
                lexer_next(&p->lex);
                out->clustered = KDS_CLUSTERED_BTREE;
            }
            /* any other identifier is left for the semicolon/EOF check */
        }
    }

    consume_optional_semicolon(p);
    return 0;
}

/*
 * INSERT INTO <name> VALUES (<val> [, ...]);
 *
 * Cursor is positioned after INSERT on entry.
 */
static int parse_insert(kds_parser_t *p, kds_stmt_insert_t *out)
{
    int ret;

    ret = expect_keyword(p, "INTO");
    if (ret) return ret;

    ret = parse_ident(p, out->table_name, sizeof(out->table_name));
    if (ret) return ret;

    ret = expect_keyword(p, "VALUES");
    if (ret) return ret;

    ret = expect_token(p, KDS_TOK_LPAREN, "'('");
    if (ret) return ret;

    out->nr_values = 0;

    for (;;) {
        const kds_token_t *peek;

        if (out->nr_values >= KDS_PARSER_MAX_VALS) {
            parser_err(p, "too many values (max %d)", KDS_PARSER_MAX_VALS);
            return -EINVAL;
        }

        ret = parse_value(p, &out->values[out->nr_values]);
        if (ret) return ret;

        out->nr_values++;

        peek = lexer_peek(&p->lex);
        if (peek->type == KDS_TOK_COMMA) {
            lexer_next(&p->lex);
            continue;
        }
        break;
    }

    ret = expect_token(p, KDS_TOK_RPAREN, "')'");
    if (ret) return ret;

    if (out->nr_values == 0) {
        parser_err(p, "INSERT VALUES requires at least one value");
        return -EINVAL;
    }

    consume_optional_semicolon(p);
    return 0;
}

/*
 * Parses one WHERE condition: <col> <op> <val>
 */
static int parse_one_cond(kds_parser_t *p, kds_ast_cond_t *cond)
{
    int ret;

    ret = parse_ident(p, cond->col_name, sizeof(cond->col_name));
    if (ret) return ret;

    ret = parse_cond_op(p, &cond->op);
    if (ret) return ret;

    ret = parse_value(p, &cond->val);
    if (ret) return ret;

    return 0;
}

/*
 * SELECT * FROM <name> [WHERE <cond> [AND <cond>]*];
 *
 * Cursor is positioned after SELECT on entry.
 */
static int parse_select(kds_parser_t *p, kds_stmt_select_t *out)
{
    int ret;

    /* SELECT list: we only support * for now */
    {
        kds_token_t tok = lexer_next(&p->lex);

        if (tok.type != KDS_TOK_STAR) {
            parser_err(p, "only SELECT * is supported, got '%s'", tok.text);
            return -EINVAL;
        }
    }

    ret = expect_keyword(p, "FROM");
    if (ret) return ret;

    ret = parse_ident(p, out->table_name, sizeof(out->table_name));
    if (ret) return ret;

    out->has_where = false;
    out->nr_conds  = 0;

    /* Optional WHERE clause */
    {
        const kds_token_t *peek = lexer_peek(&p->lex);

        if (peek->type == KDS_TOK_IDENT &&
            strcasecmp(peek->text, "WHERE") == 0) {
            lexer_next(&p->lex); /* consume WHERE */
            out->has_where = true;

            /* One or more conditions joined by AND */
            for (;;) {
                const kds_token_t *next_peek;

                if (out->nr_conds >= KDS_PARSER_MAX_CONDS) {
                    parser_err(p, "too many WHERE conditions (max %d)",
                               KDS_PARSER_MAX_CONDS);
                    return -EINVAL;
                }

                ret = parse_one_cond(p, &out->conds[out->nr_conds]);
                if (ret) return ret;
                out->nr_conds++;

                /* AND → more conditions */
                next_peek = lexer_peek(&p->lex);
                if (next_peek->type == KDS_TOK_IDENT &&
                    strcasecmp(next_peek->text, "AND") == 0) {
                    lexer_next(&p->lex);
                    continue;
                }
                break;
            }
        }
    }

    consume_optional_semicolon(p);
    return 0;
}

/*
 * SHOW META | SHOW ALLOC
 *
 * Cursor is positioned after SHOW on entry.
 */
static int parse_show(kds_parser_t *p, kds_stmt_t *out)
{
    kds_token_t tok = lexer_next(&p->lex);
    int ret = 0;

    if (tok.type != KDS_TOK_IDENT) {
        parser_err(p, "expected META or ALLOC after SHOW, got '%s'", tok.text);
        return -EINVAL;
    }

    if (strcasecmp(tok.text, "META") == 0) {
        out->type = KDS_STMT_SHOW_META;
    } else if (strcasecmp(tok.text, "ALLOC") == 0) {
        out->type = KDS_STMT_SHOW_ALLOC;
    } else {
        parser_err(p, "unknown SHOW target '%s' (expected META or ALLOC)",
                   tok.text);
        return -EINVAL;
    }

    consume_optional_semicolon(p);
    return ret;
}

/* ------------------------------------------------------------------
 * Top-level entry point
 * ------------------------------------------------------------------ */

int kds_parse(const char *sql, kds_stmt_t *out_stmt,
              char *err_buf, size_t err_buf_size)
{
    kds_parser_t p;
    kds_token_t  tok;
    int          ret = 0;

    if (!sql || !out_stmt)
        return -EINVAL;

    if (err_buf && err_buf_size > 0)
        err_buf[0] = '\0';

    memset(out_stmt, 0, sizeof(*out_stmt));
    parser_init(&p, sql, err_buf, err_buf_size);

    tok = lexer_next(&p.lex);

    if (tok.type == KDS_TOK_EOF) {
        parser_err(&p, "empty statement");
        return -EINVAL;
    }

    if (tok.type != KDS_TOK_IDENT) {
        parser_err(&p, "expected SQL keyword, got '%s'", tok.text);
        return -EINVAL;
    }

    if (strcasecmp(tok.text, "CREATE") == 0) {
        out_stmt->type = KDS_STMT_CREATE_TABLE;
        ret = parse_create_table(&p, &out_stmt->create_table);

    } else if (strcasecmp(tok.text, "INSERT") == 0) {
        out_stmt->type = KDS_STMT_INSERT;
        ret = parse_insert(&p, &out_stmt->insert);

    } else if (strcasecmp(tok.text, "SELECT") == 0) {
        out_stmt->type = KDS_STMT_SELECT;
        ret = parse_select(&p, &out_stmt->select);

    } else if (strcasecmp(tok.text, "SHOW") == 0) {
        ret = parse_show(&p, out_stmt); /* sets out_stmt->type internally */

    } else {
        parser_err(&p, "unknown SQL keyword '%s' "
                   "(supported: CREATE, INSERT, SELECT, SHOW)", tok.text);
        ret = -EINVAL;
    }

    if (ret)
        return ret;

    /*
     * After a successful parse, the only acceptable remaining token
     * is EOF (or an already-consumed semicolon). Anything else means
     * the user typed extra garbage after a valid statement.
     */
    {
        const kds_token_t *tail = lexer_peek(&p.lex);

        if (tail->type != KDS_TOK_EOF) {
            parser_err(&p, "unexpected token '%s' after end of statement",
                       tail->text);
            return -EINVAL;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------
 * Utility functions
 * ------------------------------------------------------------------ */

const char *kds_stmt_type_name(kds_stmt_type_t type)
{
    switch (type) {
    case KDS_STMT_CREATE_TABLE: return "CREATE TABLE";
    case KDS_STMT_INSERT:       return "INSERT";
    case KDS_STMT_SELECT:       return "SELECT";
    case KDS_STMT_SHOW_META:    return "SHOW META";
    case KDS_STMT_SHOW_ALLOC:   return "SHOW ALLOC";
    default:                    return "UNKNOWN";
    }
}

const char *kds_cond_op_name(kds_cond_op_t op)
{
    switch (op) {
    case KDS_OP_EQ:  return "=";
    case KDS_OP_NEQ: return "!=";
    case KDS_OP_LT:  return "<";
    case KDS_OP_LTE: return "<=";
    case KDS_OP_GT:  return ">";
    case KDS_OP_GTE: return ">=";
    default:         return "?";
    }
}