/*
** dump_ast.c - SQLite SELECT AST to JSON serializer
**
** This program parses a SQL SELECT statement using the official SQLite parser
** and outputs the raw (pre-resolution) AST as JSON. It works by hooking into
** the parser's grammar action for "cmd ::= select(X)" to capture the Select*
** before it is modified by sqlite3Select() or deleted.
**
** Build: see Makefile (patches sqlite3.c to insert the hook call)
**
** Usage: dump_ast "SELECT 1"
**   Outputs JSON AST to stdout.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ----------------------------------------------------------------
 * Forward declaration of the hook function.
 * The patched amalgamation calls this from the grammar action
 * for "cmd ::= select(X)", passing the Select* as void*.
 * ---------------------------------------------------------------- */
void ast_capture_hook(void *select_ptr);

/* ----------------------------------------------------------------
 * Include the patched SQLite amalgamation.
 * This gives us access to all internal types (Select, Expr, etc.)
 * ---------------------------------------------------------------- */
#include "build/sqlite3_patched.c"

/* ================================================================
 * JSON Writer (pretty-printed with 2-space indentation)
 *
 * State machine:
 *   g_need_comma: next element needs a preceding comma
 *   g_after_key:  we just wrote "key": and the value follows inline
 *   g_indent:     current nesting depth for indentation
 * ================================================================ */

static char g_buf[4 * 1024 * 1024]; /* 4MB output buffer */
static int g_pos;
static int g_need_comma;
static int g_after_key;
static int g_indent;

static void jw_init(void) {
    g_pos = 0;
    g_buf[0] = 0;
    g_need_comma = 0;
    g_after_key = 0;
    g_indent = 0;
}

static void jw_raw(const char *s) {
    int len = (int)strlen(s);
    if (g_pos + len < (int)sizeof(g_buf) - 1) {
        memcpy(g_buf + g_pos, s, len);
        g_pos += len;
        g_buf[g_pos] = 0;
    }
}

static void jw_rawf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_buf + g_pos, sizeof(g_buf) - g_pos, fmt, ap);
    va_end(ap);
    if (n > 0) g_pos += n;
}

static void jw_newline(void) {
    jw_raw("\n");
    for (int i = 0; i < g_indent; i++) jw_raw("  ");
}

/*
** Before writing a new element (value, object, or array), call this
** to handle commas and newlines. After a key, values go inline.
*/
static void jw_element_prefix(void) {
    if (g_after_key) {
        g_after_key = 0;
        /* Value follows "key": inline, no newline */
    } else {
        if (g_need_comma) jw_raw(",");
        jw_newline();
    }
    g_need_comma = 0;
}

/* Write a JSON-escaped string (with quotes) - raw, no prefix handling */
static void jw_quoted_string(const char *s) {
    jw_raw("\"");
    if (s) {
        for (const char *p = s; *p; p++) {
            switch (*p) {
                case '"':  jw_raw("\\\""); break;
                case '\\': jw_raw("\\\\"); break;
                case '\b': jw_raw("\\b");  break;
                case '\f': jw_raw("\\f");  break;
                case '\n': jw_raw("\\n");  break;
                case '\r': jw_raw("\\r");  break;
                case '\t': jw_raw("\\t");  break;
                default:
                    if ((unsigned char)*p < 0x20) {
                        jw_rawf("\\u%04x", (unsigned char)*p);
                    } else {
                        char c[2] = {*p, 0};
                        jw_raw(c);
                    }
            }
        }
    }
    jw_raw("\"");
}

static void jw_obj_start(void) {
    jw_element_prefix();
    jw_raw("{");
    g_indent++;
    g_need_comma = 0;
}

static void jw_obj_end(void) {
    g_indent--;
    g_after_key = 0;
    jw_newline();
    jw_raw("}");
    g_need_comma = 1;
}

static void jw_arr_start(void) {
    jw_element_prefix();
    jw_raw("[");
    g_indent++;
    g_need_comma = 0;
}

static void jw_arr_end(void) {
    g_indent--;
    g_after_key = 0;
    jw_newline();
    jw_raw("]");
    g_need_comma = 1;
}

static void jw_key(const char *k) {
    if (g_need_comma) jw_raw(",");
    jw_newline();
    jw_quoted_string(k);
    jw_raw(": ");
    g_need_comma = 0;
    g_after_key = 1;
}

/* Write a string value */
static void jw_str(const char *s) {
    jw_element_prefix();
    jw_quoted_string(s);
    g_need_comma = 1;
}

/* Write a null value */
static void jw_null(void) {
    jw_element_prefix();
    jw_raw("null");
    g_need_comma = 1;
}

/* Write a boolean value */
static void jw_bool(int v) {
    jw_element_prefix();
    jw_raw(v ? "true" : "false");
    g_need_comma = 1;
}

/* Write an integer value */
static void jw_int(int v) {
    jw_element_prefix();
    jw_rawf("%d", v);
    g_need_comma = 1;
}

/* Convenience: write "key": "value" or "key": null (value inline) */
static void jw_key_str(const char *k, const char *v) {
    jw_key(k);
    if (v) { jw_quoted_string(v); } else { jw_raw("null"); }
    g_need_comma = 1;
    g_after_key = 0;
}

/* Convenience: write "key": true/false */
static void jw_key_bool(const char *k, int v) {
    jw_key(k);
    jw_raw(v ? "true" : "false");
    g_need_comma = 1;
    g_after_key = 0;
}

/* Convenience: write "key": null */
static void jw_key_null(const char *k) {
    jw_key(k);
    jw_raw("null");
    g_need_comma = 1;
    g_after_key = 0;
}

/* ================================================================
 * AST Serialization - Forward Declarations
 * ================================================================ */

static void json_expr(const Expr *pExpr);
static void json_expr_list(const ExprList *pList);
static void json_select(const Select *p);
static void json_src_list(const SrcList *pSrc);
static void json_id_list(const IdList *pList);
static void json_with(const With *pWith);
#ifndef SQLITE_OMIT_WINDOWFUNC
static void json_window(const Window *pWin);
#endif

/* ================================================================
 * AST Serialization - Expressions
 * ================================================================ */

/* Map a TK_ binary operator to its SQL symbol */
static const char *binop_name(int op) {
    switch (op) {
        case TK_AND:     return "AND";
        case TK_OR:      return "OR";
        case TK_LT:      return "<";
        case TK_LE:      return "<=";
        case TK_GT:      return ">";
        case TK_GE:      return ">=";
        case TK_EQ:      return "=";
        case TK_NE:      return "!=";
        case TK_IS:      return "IS";
        case TK_ISNOT:   return "IS NOT";
        case TK_PLUS:    return "+";
        case TK_MINUS:   return "-";
        case TK_STAR:    return "*";
        case TK_SLASH:   return "/";
        case TK_REM:     return "%";
        case TK_BITAND:  return "&";
        case TK_BITOR:   return "|";
        case TK_LSHIFT:  return "<<";
        case TK_RSHIFT:  return ">>";
        case TK_CONCAT:  return "||";
        case TK_LIKE_KW: return "LIKE";
        case TK_MATCH:   return "MATCH";
        default: return NULL;
    }
}

static void json_expr(const Expr *pExpr) {
    if (pExpr == NULL) {
        jw_null();
        return;
    }

    jw_obj_start();

    switch (pExpr->op) {

    case TK_INTEGER: {
        jw_key_str("type", "integer");
        jw_key("value");
        if (pExpr->flags & EP_IntValue) {
            jw_rawf("%d", pExpr->u.iValue);
            g_need_comma = 1;
        } else {
            jw_str(pExpr->u.zToken);
        }
        break;
    }

    case TK_FLOAT: {
        jw_key_str("type", "float");
        jw_key_str("value", pExpr->u.zToken);
        break;
    }

    case TK_STRING: {
        jw_key_str("type", "string");
        jw_key_str("value", pExpr->u.zToken);
        break;
    }

    case TK_BLOB: {
        jw_key_str("type", "blob");
        jw_key_str("value", pExpr->u.zToken);
        break;
    }

    case TK_NULL: {
        jw_key_str("type", "null");
        break;
    }

    case TK_TRUEFALSE: {
        jw_key_str("type", "boolean");
        jw_key_bool("value", sqlite3ExprTruthValue(pExpr));
        break;
    }

    case TK_ID: {
        jw_key_str("type", "name");
        jw_key_str("name", pExpr->u.zToken);
        break;
    }

    case TK_DOT: {
        jw_key_str("type", "dot");
        jw_key("left");
        json_expr(pExpr->pLeft);
        jw_key("right");
        json_expr(pExpr->pRight);
        break;
    }

    case TK_ASTERISK: {
        jw_key_str("type", "star");
        break;
    }

    case TK_VARIABLE: {
        jw_key_str("type", "parameter");
        jw_key_str("name", pExpr->u.zToken);
        break;
    }

    case TK_CAST: {
        jw_key_str("type", "cast");
        jw_key("expr");
        json_expr(pExpr->pLeft);
        jw_key_str("as", pExpr->u.zToken);
        break;
    }

    case TK_CASE: {
        jw_key_str("type", "case");
        jw_key("operand");
        json_expr(pExpr->pLeft);
        if (pExpr->x.pList) {
            int i;
            jw_key("when_clauses");
            jw_arr_start();
            for (i = 0; i + 1 < pExpr->x.pList->nExpr; i += 2) {
                jw_obj_start();
                jw_key("when");
                json_expr(pExpr->x.pList->a[i].pExpr);
                jw_key("then");
                json_expr(pExpr->x.pList->a[i + 1].pExpr);
                jw_obj_end();
            }
            jw_arr_end();
            /* The last item, if odd count, is ELSE */
            if (pExpr->x.pList->nExpr % 2 == 1) {
                jw_key("else");
                json_expr(pExpr->x.pList->a[pExpr->x.pList->nExpr - 1].pExpr);
            } else {
                jw_key_null("else");
            }
        }
        break;
    }

    case TK_BETWEEN: {
        jw_key_str("type", "between");
        jw_key("expr");
        json_expr(pExpr->pLeft);
        jw_key("low");
        json_expr(pExpr->x.pList->a[0].pExpr);
        jw_key("high");
        json_expr(pExpr->x.pList->a[1].pExpr);
        break;
    }

    case TK_IN: {
        jw_key_str("type", "in");
        jw_key("expr");
        json_expr(pExpr->pLeft);
        if (pExpr->flags & EP_xIsSelect) {
            jw_key("select");
            json_select(pExpr->x.pSelect);
        } else {
            jw_key("values");
            json_expr_list(pExpr->x.pList);
        }
        break;
    }

    case TK_EXISTS: {
        jw_key_str("type", "exists");
        jw_key("select");
        json_select(pExpr->x.pSelect);
        break;
    }

    case TK_SELECT: {
        jw_key_str("type", "subquery");
        jw_key("select");
        json_select(pExpr->x.pSelect);
        break;
    }

    case TK_COLLATE: {
        jw_key_str("type", "collate");
        jw_key("expr");
        json_expr(pExpr->pLeft);
        jw_key_str("collation", pExpr->u.zToken);
        break;
    }

    case TK_FUNCTION:
    case TK_AGG_FUNCTION: {
        jw_key_str("type", "function");
        jw_key_str("name", pExpr->u.zToken);
        jw_key("args");
        if (!ExprHasProperty(pExpr, EP_TokenOnly) && pExpr->x.pList) {
            json_expr_list(pExpr->x.pList);
        } else {
            jw_arr_start();
            jw_arr_end();
        }
        jw_key_bool("distinct",
            (pExpr->flags & EP_Distinct) ? 1 : 0);
        /* ORDER BY within aggregate function */
        if (pExpr->pLeft && pExpr->pLeft->op == TK_ORDER) {
            jw_key("order_by");
            json_expr_list(pExpr->pLeft->x.pList);
        }
#ifndef SQLITE_OMIT_WINDOWFUNC
        if (IsWindowFunc(pExpr) && pExpr->y.pWin) {
            jw_key("over");
            json_window(pExpr->y.pWin);
        }
#endif
        break;
    }

    case TK_UMINUS: {
        jw_key_str("type", "unary");
        jw_key_str("op", "-");
        jw_key("operand");
        json_expr(pExpr->pLeft);
        break;
    }

    case TK_UPLUS: {
        jw_key_str("type", "unary");
        jw_key_str("op", "+");
        jw_key("operand");
        json_expr(pExpr->pLeft);
        break;
    }

    case TK_BITNOT: {
        jw_key_str("type", "unary");
        jw_key_str("op", "~");
        jw_key("operand");
        json_expr(pExpr->pLeft);
        break;
    }

    case TK_NOT: {
        jw_key_str("type", "unary");
        jw_key_str("op", "NOT");
        jw_key("operand");
        json_expr(pExpr->pLeft);
        break;
    }

    case TK_ISNULL: {
        jw_key_str("type", "isnull");
        jw_key("operand");
        json_expr(pExpr->pLeft);
        break;
    }

    case TK_NOTNULL: {
        jw_key_str("type", "notnull");
        jw_key("operand");
        json_expr(pExpr->pLeft);
        break;
    }

    case TK_TRUTH: {
        /* IS TRUE, IS FALSE, IS NOT TRUE, IS NOT FALSE */
        int isNot = (pExpr->op2 == TK_ISNOT);
        int isTrue = sqlite3ExprTruthValue(pExpr->pRight);
        const char *ops[] = {
            "IS FALSE", "IS TRUE", "IS NOT FALSE", "IS NOT TRUE"
        };
        jw_key_str("type", "truth_test");
        jw_key_str("op", ops[isNot * 2 + isTrue]);
        jw_key("operand");
        json_expr(pExpr->pLeft);
        break;
    }

    case TK_RAISE: {
        jw_key_str("type", "raise");
        const char *zType = "unknown";
        switch (pExpr->affExpr) {
            case OE_Rollback: zType = "ROLLBACK"; break;
            case OE_Abort:    zType = "ABORT";    break;
            case OE_Fail:     zType = "FAIL";     break;
            case OE_Ignore:   zType = "IGNORE";   break;
        }
        jw_key_str("action", zType);
        if (pExpr->u.zToken) {
            jw_key_str("message", pExpr->u.zToken);
        }
        break;
    }

    case TK_VECTOR: {
        jw_key_str("type", "vector");
        jw_key("values");
        json_expr_list(pExpr->x.pList);
        break;
    }

    case TK_SPAN: {
        /* SPAN wraps an expression with its original SQL text */
        jw_key_str("type", "span");
        jw_key_str("text", pExpr->u.zToken);
        jw_key("expr");
        json_expr(pExpr->pLeft);
        break;
    }

    default: {
        /* Binary operators */
        const char *zOp = binop_name(pExpr->op);
        if (zOp && pExpr->pLeft && pExpr->pRight) {
            jw_key_str("type", "binary");
            jw_key_str("op", zOp);
            jw_key("left");
            json_expr(pExpr->pLeft);
            jw_key("right");
            json_expr(pExpr->pRight);
        } else {
            /* Fallback: output the opcode number */
            jw_key_str("type", "unknown");
            jw_key("op");
            jw_int(pExpr->op);
        }
        break;
    }

    } /* end switch */

    jw_obj_end();
}

/* ================================================================
 * AST Serialization - Expression Lists
 * ================================================================ */

static void json_expr_list(const ExprList *pList) {
    if (pList == NULL) {
        jw_null();
        return;
    }
    jw_arr_start();
    for (int i = 0; i < pList->nExpr; i++) {
        json_expr(pList->a[i].pExpr);
    }
    jw_arr_end();
}

/* ================================================================
 * AST Serialization - Result Columns
 * (Like ExprList but includes alias info)
 * ================================================================ */

static void json_result_columns(const ExprList *pList) {
    if (pList == NULL) {
        jw_null();
        return;
    }
    jw_arr_start();
    for (int i = 0; i < pList->nExpr; i++) {
        jw_obj_start();
        jw_key("expr");
        json_expr(pList->a[i].pExpr);
        /* Alias: only output if this is an explicit AS name */
        if (pList->a[i].zEName && pList->a[i].fg.eEName == ENAME_NAME) {
            jw_key_str("alias", pList->a[i].zEName);
        } else {
            jw_key_null("alias");
        }
        jw_obj_end();
    }
    jw_arr_end();
}

/* ================================================================
 * AST Serialization - ORDER BY Columns
 * (Like ExprList but includes direction)
 * ================================================================ */

static void json_order_by(const ExprList *pList) {
    if (pList == NULL) {
        jw_null();
        return;
    }
    jw_arr_start();
    for (int i = 0; i < pList->nExpr; i++) {
        jw_obj_start();
        jw_key("expr");
        json_expr(pList->a[i].pExpr);
        if (pList->a[i].fg.sortFlags & KEYINFO_ORDER_DESC) {
            jw_key_str("direction", "DESC");
        } else {
            jw_key_str("direction", "ASC");
        }
        if (pList->a[i].fg.bNulls) {
            if (pList->a[i].fg.sortFlags & KEYINFO_ORDER_BIGNULL) {
                jw_key_str("nulls", "LAST");
            } else {
                jw_key_str("nulls", "FIRST");
            }
        }
        jw_obj_end();
    }
    jw_arr_end();
}

/* ================================================================
 * AST Serialization - Id List (for USING clauses)
 * ================================================================ */

static void json_id_list(const IdList *pList) {
    if (pList == NULL) {
        jw_null();
        return;
    }
    jw_arr_start();
    for (int i = 0; i < pList->nId; i++) {
        jw_str(pList->a[i].zName);
    }
    jw_arr_end();
}

/* ================================================================
 * AST Serialization - FROM Clause (SrcList)
 * ================================================================ */

static const char *join_type_name(u8 jt) {
    if (jt == 0) return NULL; /* no explicit join, just comma-separated */

    /* Check for FULL OUTER JOIN first */
    if ((jt & (JT_LEFT | JT_RIGHT)) == (JT_LEFT | JT_RIGHT)) {
        if (jt & JT_NATURAL) return "NATURAL FULL OUTER JOIN";
        return "FULL OUTER JOIN";
    }
    if (jt & JT_LEFT) {
        if (jt & JT_NATURAL) return "NATURAL LEFT JOIN";
        return "LEFT JOIN";
    }
    if (jt & JT_RIGHT) {
        if (jt & JT_NATURAL) return "NATURAL RIGHT JOIN";
        return "RIGHT JOIN";
    }
    if (jt & JT_CROSS) {
        return "CROSS JOIN";
    }
    if (jt & JT_NATURAL) {
        return "NATURAL JOIN";
    }
    if (jt & JT_INNER) {
        return "JOIN";
    }
    return NULL;
}

static void json_src_list(const SrcList *pSrc) {
    if (pSrc == NULL || pSrc->nSrc == 0) {
        jw_null();
        return;
    }
    jw_arr_start();
    for (int i = 0; i < pSrc->nSrc; i++) {
        const SrcItem *pItem = &pSrc->a[i];
        jw_obj_start();

        if (pItem->fg.isSubquery) {
            jw_key_str("type", "subquery");
            jw_key("select");
            json_select(pItem->u4.pSubq->pSelect);
        } else {
            jw_key_str("type", "table");
            jw_key_str("name", pItem->zName);
            if (pItem->u4.zDatabase && !pItem->fg.fixedSchema) {
                jw_key_str("schema", pItem->u4.zDatabase);
            }
        }

        jw_key_str("alias", pItem->zAlias);

        /* Join type */
        const char *joinName = join_type_name(pItem->fg.jointype);
        jw_key_str("join_type", joinName);

        /* ON clause */
        if (pItem->fg.isOn || pItem->u3.pOn) {
            jw_key("on");
            json_expr(pItem->u3.pOn);
        }

        /* USING clause */
        if (pItem->fg.isUsing && pItem->u3.pUsing) {
            jw_key("using");
            json_id_list(pItem->u3.pUsing);
        }

        /* Table-valued function arguments */
        if (pItem->fg.isTabFunc && pItem->u1.pFuncArg) {
            jw_key("args");
            json_expr_list(pItem->u1.pFuncArg);
        }

        jw_obj_end();
    }
    jw_arr_end();
}

/* ================================================================
 * AST Serialization - WITH / CTE
 * ================================================================ */

static void json_with(const With *pWith) {
    if (pWith == NULL) {
        jw_null();
        return;
    }
    jw_arr_start();
    for (int i = 0; i < pWith->nCte; i++) {
        const Cte *pCte = &pWith->a[i];
        jw_obj_start();
        jw_key_str("name", pCte->zName);
        /* Column list */
        if (pCte->pCols && pCte->pCols->nExpr > 0) {
            jw_key("columns");
            jw_arr_start();
            for (int j = 0; j < pCte->pCols->nExpr; j++) {
                jw_str(pCte->pCols->a[j].zEName);
            }
            jw_arr_end();
        }
        /* Materialization hint */
        if (pCte->eM10d == M10d_Yes) {
            jw_key_str("materialized", "MATERIALIZED");
        } else if (pCte->eM10d == M10d_No) {
            jw_key_str("materialized", "NOT MATERIALIZED");
        }
        /* The CTE body */
        jw_key("select");
        json_select(pCte->pSelect);
        jw_obj_end();
    }
    jw_arr_end();
}

/* ================================================================
 * AST Serialization - Window Definitions
 * ================================================================ */

#ifndef SQLITE_OMIT_WINDOWFUNC
static const char *frame_bound_name(u8 bound) {
    switch (bound) {
        case TK_UNBOUNDED: return "UNBOUNDED";
        case TK_CURRENT:   return "CURRENT ROW";
        case TK_PRECEDING: return "PRECEDING";
        case TK_FOLLOWING: return "FOLLOWING";
        default: return "unknown";
    }
}

static void json_window(const Window *pWin) {
    if (pWin == NULL) {
        jw_null();
        return;
    }
    jw_obj_start();
    jw_key_str("name", pWin->zName);
    jw_key_str("base", pWin->zBase);

    if (pWin->pPartition) {
        jw_key("partition_by");
        json_expr_list(pWin->pPartition);
    }

    if (pWin->pOrderBy) {
        jw_key("order_by");
        json_order_by(pWin->pOrderBy);
    }

    if (pWin->eFrmType != 0 && pWin->eFrmType != TK_FILTER) {
        jw_key("frame");
        jw_obj_start();
        const char *zFrmType = "ROWS";
        if (pWin->eFrmType == TK_RANGE) zFrmType = "RANGE";
        if (pWin->eFrmType == TK_GROUPS) zFrmType = "GROUPS";
        jw_key_str("type", zFrmType);

        jw_key("start");
        jw_obj_start();
        jw_key_str("type", frame_bound_name(pWin->eStart));
        if (pWin->pStart) {
            jw_key("expr");
            json_expr(pWin->pStart);
        }
        jw_obj_end();

        jw_key("end");
        jw_obj_start();
        jw_key_str("type", frame_bound_name(pWin->eEnd));
        if (pWin->pEnd) {
            jw_key("expr");
            json_expr(pWin->pEnd);
        }
        jw_obj_end();

        if (pWin->eExclude) {
            const char *zExclude = "unknown";
            switch (pWin->eExclude) {
                case TK_NO:      zExclude = "NO OTHERS"; break;
                case TK_CURRENT: zExclude = "CURRENT ROW"; break;
                case TK_GROUP:   zExclude = "GROUP"; break;
                case TK_TIES:    zExclude = "TIES"; break;
            }
            jw_key_str("exclude", zExclude);
        }
        jw_obj_end();
    }

    if (pWin->pFilter) {
        jw_key("filter");
        json_expr(pWin->pFilter);
    }

    jw_obj_end();
}
#endif /* SQLITE_OMIT_WINDOWFUNC */

/* ================================================================
 * AST Serialization - SELECT Statement
 * ================================================================ */

static void json_select(const Select *p) {
    if (p == NULL) {
        jw_null();
        return;
    }

    /*
    ** For compound selects (UNION, INTERSECT, EXCEPT), walk the chain.
    ** The chain via pPrior goes: rightmost → ... → leftmost.
    ** We want to output in left-to-right order, so first collect them.
    */
    if (p->pPrior) {
        /* Count the chain */
        int count = 0;
        const Select *q;
        for (q = p; q != NULL; q = q->pPrior) count++;

        /* Collect pointers in order */
        const Select **arr = sqlite3_malloc64(count * sizeof(Select *));
        if (arr == NULL) { jw_null(); return; }
        int idx = count;
        for (q = p; q != NULL; q = q->pPrior) arr[--idx] = q;

        jw_obj_start();
        jw_key_str("type", "compound");
        jw_key("body");
        jw_arr_start();
        for (int i = 0; i < count; i++) {
            jw_obj_start();
            if (i > 0) {
                /* The operator is stored on the right side of the compound */
                const char *zOp = "UNION";
                switch (arr[i]->op) {
                    case TK_ALL:       zOp = "UNION ALL"; break;
                    case TK_INTERSECT: zOp = "INTERSECT"; break;
                    case TK_EXCEPT:    zOp = "EXCEPT";    break;
                }
                jw_key_str("operator", zOp);
            }
            jw_key("select");
            /* Output this individual select (non-compound parts) */
            jw_obj_start();
            jw_key_str("type", "select");
            jw_key_bool("distinct", (arr[i]->selFlags & SF_Distinct) ? 1 : 0);
            jw_key_bool("all", (arr[i]->selFlags & SF_All) ? 1 : 0);
            jw_key("columns");
            json_result_columns(arr[i]->pEList);
            jw_key("from");
            json_src_list(arr[i]->pSrc);
            jw_key("where");
            json_expr(arr[i]->pWhere);
            jw_key("group_by");
            json_expr_list(arr[i]->pGroupBy);
            jw_key("having");
            json_expr(arr[i]->pHaving);
            /* Note: ORDER BY and LIMIT are on the outermost select only */
            jw_obj_end();
            jw_obj_end();
        }
        jw_arr_end();
        /* ORDER BY and LIMIT apply to the whole compound */
        jw_key("order_by");
        json_order_by(p->pOrderBy);
        if (p->pLimit) {
            jw_key("limit");
            json_expr(p->pLimit->pLeft);
            jw_key("offset");
            if (p->pLimit->pRight) {
                json_expr(p->pLimit->pRight);
            } else {
                jw_null();
            }
        } else {
            jw_key_null("limit");
        }
        jw_obj_end();
        sqlite3_free(arr);
        return;
    }

    /* Simple (non-compound) select */
    jw_obj_start();
    jw_key_str("type", "select");
    jw_key_bool("distinct", (p->selFlags & SF_Distinct) ? 1 : 0);
    jw_key_bool("all", (p->selFlags & SF_All) ? 1 : 0);

    /* WITH clause */
    if (p->pWith) {
        jw_key("with");
        json_with(p->pWith);
    }

    /* Result columns */
    jw_key("columns");
    json_result_columns(p->pEList);

    /* FROM clause */
    jw_key("from");
    json_src_list(p->pSrc);

    /* WHERE clause */
    jw_key("where");
    json_expr(p->pWhere);

    /* GROUP BY */
    jw_key("group_by");
    json_expr_list(p->pGroupBy);

    /* HAVING */
    jw_key("having");
    json_expr(p->pHaving);

#ifndef SQLITE_OMIT_WINDOWFUNC
    /* Named window definitions (WINDOW w AS (...)) */
    if (p->pWinDefn) {
        jw_key("window_definitions");
        jw_arr_start();
        for (const Window *pW = p->pWinDefn; pW; pW = pW->pNextWin) {
            json_window(pW);
        }
        jw_arr_end();
    }
#endif

    /* ORDER BY */
    jw_key("order_by");
    json_order_by(p->pOrderBy);

    /* LIMIT / OFFSET */
    if (p->pLimit) {
        jw_key("limit");
        json_expr(p->pLimit->pLeft);
        jw_key("offset");
        if (p->pLimit->pRight) {
            json_expr(p->pLimit->pRight);
        } else {
            jw_null();
        }
    } else {
        jw_key_null("limit");
    }

    jw_obj_end();
}

/* ================================================================
 * Hook Function - Called from patched grammar action
 * ================================================================ */

/* Flags to control AST capture */
static int g_capture_enabled = 0;
static int g_captured = 0;

void ast_capture_hook(void *select_ptr) {
    if (!g_capture_enabled) return;
    if (g_captured) return;  /* Only capture the first SELECT (the user's query) */
    g_captured = 1;
    Select *p = (Select *)select_ptr;
    jw_init();
    json_select(p);
}

/* ================================================================
 * Main Program
 * ================================================================ */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: dump_ast 'SQL query'\n");
        fprintf(stderr, "Outputs the parsed AST as JSON to stdout.\n");
        return 1;
    }

    const char *sql = argv[1];
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    /* Enable AST capture */
    g_capture_enabled = 1;
    g_captured = 0;
    jw_init();

    /*
    ** Call prepare to trigger the parser. The patched grammar action will
    ** call ast_capture_hook() with the raw Select* before any resolution.
    ** We don't care if prepare fails (e.g., tables don't exist) - we only
    ** care about the parse tree.
    */
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

    if (g_pos == 0) {
        /* No AST was captured - probably a parse error */
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Parse error: %s\n", sqlite3_errmsg(db));
        } else {
            fprintf(stderr, "No SELECT statement found in input\n");
        }
        sqlite3_close(db);
        return 1;
    }

    /* Output the JSON */
    printf("%s\n", g_buf);

    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}
