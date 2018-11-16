/**
 * @file tree_schema.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Schema tree implementation
 *
 * Copyright (c) 2015 - 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "libyang.h"
#include "context.h"
#include "tree_schema_internal.h"
#include "xpath.h"

/**
 * @brief Duplicate string into dictionary
 * @param[in] CTX libyang context of the dictionary.
 * @param[in] ORIG String to duplicate.
 * @param[out] DUP Where to store the result.
 */
#define DUP_STRING(CTX, ORIG, DUP) if (ORIG) {DUP = lydict_insert(CTX, ORIG, 0);}

#define COMPILE_ARRAY_GOTO(CTX, ARRAY_P, ARRAY_C, OPTIONS, ITER, FUNC, RET, GOTO) \
    if (ARRAY_P) { \
        LY_ARRAY_CREATE_GOTO((CTX)->ctx, ARRAY_C, LY_ARRAY_SIZE(ARRAY_P), RET, GOTO); \
        for (ITER = 0; ITER < LY_ARRAY_SIZE(ARRAY_P); ++ITER) { \
            LY_ARRAY_INCREMENT(ARRAY_C); \
            RET = FUNC(CTX, &(ARRAY_P)[ITER], OPTIONS, &(ARRAY_C)[ITER]); \
            LY_CHECK_GOTO(RET != LY_SUCCESS, GOTO); \
        } \
    }

#define COMPILE_MEMBER_GOTO(CTX, MEMBER_P, MEMBER_C, OPTIONS, FUNC, RET, GOTO) \
    if (MEMBER_P) { \
        MEMBER_C = calloc(1, sizeof *(MEMBER_C)); \
        LY_CHECK_ERR_GOTO(!(MEMBER_C), LOGMEM((CTX)->ctx); RET = LY_EMEM, GOTO); \
        RET = FUNC(CTX, MEMBER_P, OPTIONS, MEMBER_C); \
        LY_CHECK_GOTO(RET != LY_SUCCESS, GOTO); \
    }

const char* ly_data_type2str[LY_DATA_TYPE_COUNT] = {"unknown", "binary", "bits", "boolean", "decimal64", "empty", "enumeration",
    "identityref", "instance-identifier", "leafref", "string", "union", "8bit integer", "8bit unsigned integer", "16bit integer",
    "16bit unsigned integer", "32bit integer", "32bit unsigned integer", "64bit integer", "64bit unsigned integer"
};

static struct lysc_ext_instance *
lysc_ext_instance_dup(struct ly_ctx *ctx, struct lysc_ext_instance *orig)
{
    /* TODO */
    (void) ctx;
    (void) orig;
    return NULL;
}

static struct lysc_pattern*
lysc_pattern_dup(struct lysc_pattern *orig)
{
    ++orig->refcount;
    return orig;
}

static struct lysc_pattern**
lysc_patterns_dup(struct ly_ctx *ctx, struct lysc_pattern **orig)
{
    struct lysc_pattern **dup;
    unsigned int u;

    LY_ARRAY_CREATE_RET(ctx, dup, LY_ARRAY_SIZE(orig), NULL);
    LY_ARRAY_FOR(orig, u) {
        dup[u] = lysc_pattern_dup(orig[u]);
        LY_ARRAY_INCREMENT(dup);
    }
    return dup;
}

struct lysc_range*
lysc_range_dup(struct ly_ctx *ctx, const struct lysc_range *orig)
{
    struct lysc_range *dup;
    LY_ERR ret;

    dup = calloc(1, sizeof *dup);
    LY_CHECK_ERR_RET(!dup, LOGMEM(ctx), NULL);
    if (orig->parts) {
        LY_ARRAY_CREATE_GOTO(ctx, dup->parts, LY_ARRAY_SIZE(orig->parts), ret, cleanup);
        LY_ARRAY_SIZE(dup->parts) = LY_ARRAY_SIZE(orig->parts);
        memcpy(dup->parts, orig->parts, LY_ARRAY_SIZE(dup->parts) * sizeof *dup->parts);
    }
    DUP_STRING(ctx, orig->eapptag, dup->eapptag);
    DUP_STRING(ctx, orig->emsg, dup->emsg);
    dup->exts = lysc_ext_instance_dup(ctx, orig->exts);

    return dup;
cleanup:
    free(dup);
    (void) ret; /* set but not used due to the return type */
    return NULL;
}

struct iff_stack {
    int size;
    int index;     /* first empty item */
    uint8_t *stack;
};

static LY_ERR
iff_stack_push(struct iff_stack *stack, uint8_t value)
{
    if (stack->index == stack->size) {
        stack->size += 4;
        stack->stack = ly_realloc(stack->stack, stack->size * sizeof *stack->stack);
        LY_CHECK_ERR_RET(!stack->stack, LOGMEM(NULL); stack->size = 0, LY_EMEM);
    }
    stack->stack[stack->index++] = value;
    return LY_SUCCESS;
}

static uint8_t
iff_stack_pop(struct iff_stack *stack)
{
    stack->index--;
    return stack->stack[stack->index];
}

static void
iff_stack_clean(struct iff_stack *stack)
{
    stack->size = 0;
    free(stack->stack);
}

static void
iff_setop(uint8_t *list, uint8_t op, int pos)
{
    uint8_t *item;
    uint8_t mask = 3;

    assert(pos >= 0);
    assert(op <= 3); /* max 2 bits */

    item = &list[pos / 4];
    mask = mask << 2 * (pos % 4);
    *item = (*item) & ~mask;
    *item = (*item) | (op << 2 * (pos % 4));
}

#define LYS_IFF_LP 0x04 /* ( */
#define LYS_IFF_RP 0x08 /* ) */

static struct lysc_feature *
lysc_feature_find(struct lysc_module *mod, const char *name, size_t len)
{
    size_t i;
    struct lysc_feature *f;

    for (i = 0; i < len; ++i) {
        if (name[i] == ':') {
            /* we have a prefixed feature */
            mod = lysc_module_find_prefix(mod, name, i);
            LY_CHECK_RET(!mod, NULL);

            name = &name[i + 1];
            len = len - i - 1;
        }
    }

    /* we have the correct module, get the feature */
    LY_ARRAY_FOR(mod->features, i) {
        f = &mod->features[i];
        if (!strncmp(f->name, name, len) && f->name[len] == '\0') {
            return f;
        }
    }

    return NULL;
}

static LY_ERR
lys_compile_ext(struct lysc_ctx *ctx, struct lysp_ext_instance *ext_p, int UNUSED(options), struct lysc_ext_instance *ext)
{
    const char *name;
    unsigned int u;
    const struct lys_module *mod;
    struct lysp_ext *edef = NULL;

    DUP_STRING(ctx->ctx, ext_p->argument, ext->argument);
    ext->insubstmt = ext_p->insubstmt;
    ext->insubstmt_index = ext_p->insubstmt_index;

    /* get module where the extension definition should be placed */
    for (u = 0; ext_p->name[u] != ':'; ++u);
    mod = lys_module_find_prefix(ctx->mod, ext_p->name, u);
    LY_CHECK_ERR_RET(!mod, LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                                  "Invalid prefix \"%.*s\" used for extension instance identifier.", u, ext_p->name),
                     LY_EVALID);
    LY_CHECK_ERR_RET(!mod->parsed->extensions,
                     LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                            "Extension instance \"%s\" refers \"%s\" module that does not contain extension definitions.",
                            ext_p->name, mod->parsed->name),
                     LY_EVALID);
    name = &ext_p->name[u + 1];
    /* find the extension definition there */
    for (ext = NULL, u = 0; u < LY_ARRAY_SIZE(mod->parsed->extensions); ++u) {
        if (!strcmp(name, mod->parsed->extensions[u].name)) {
            edef = &mod->parsed->extensions[u];
            break;
        }
    }
    LY_CHECK_ERR_RET(!edef, LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                                   "Extension definition of extension instance \"%s\" not found.", ext_p->name),
                     LY_EVALID);
    /* TODO plugins */

    return LY_SUCCESS;
}

static LY_ERR
lys_compile_iffeature(struct lysc_ctx *ctx, const char **value, int UNUSED(options), struct lysc_iffeature *iff)
{
    const char *c = *value;
    int r, rc = EXIT_FAILURE;
    int i, j, last_not, checkversion = 0;
    unsigned int f_size = 0, expr_size = 0, f_exp = 1;
    uint8_t op;
    struct iff_stack stack = {0, 0, NULL};
    struct lysc_feature *f;

    assert(c);

    /* pre-parse the expression to get sizes for arrays, also do some syntax checks of the expression */
    for (i = j = last_not = 0; c[i]; i++) {
        if (c[i] == '(') {
            j++;
            checkversion = 1;
            continue;
        } else if (c[i] == ')') {
            j--;
            continue;
        } else if (isspace(c[i])) {
            checkversion = 1;
            continue;
        }

        if (!strncmp(&c[i], "not", r = 3) || !strncmp(&c[i], "and", r = 3) || !strncmp(&c[i], "or", r = 2)) {
            if (c[i + r] == '\0') {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid value \"%s\" of if-feature - unexpected end of expression.", *value);
                return LY_EVALID;
            } else if (!isspace(c[i + r])) {
                /* feature name starting with the not/and/or */
                last_not = 0;
                f_size++;
            } else if (c[i] == 'n') { /* not operation */
                if (last_not) {
                    /* double not */
                    expr_size = expr_size - 2;
                    last_not = 0;
                } else {
                    last_not = 1;
                }
            } else { /* and, or */
                f_exp++;
                /* not a not operation */
                last_not = 0;
            }
            i += r;
        } else {
            f_size++;
            last_not = 0;
        }
        expr_size++;

        while (!isspace(c[i])) {
            if (!c[i] || c[i] == ')') {
                i--;
                break;
            }
            i++;
        }
    }
    if (j || f_exp != f_size) {
        /* not matching count of ( and ) */
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
               "Invalid value \"%s\" of if-feature - non-matching opening and closing parentheses.", *value);
        return LY_EVALID;
    }

    if (checkversion || expr_size > 1) {
        /* check that we have 1.1 module */
        if (ctx->mod->compiled->version != LYS_VERSION_1_1) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                   "Invalid value \"%s\" of if-feature - YANG 1.1 expression in YANG 1.0 module.", *value);
            return LY_EVALID;
        }
    }

    /* allocate the memory */
    LY_ARRAY_CREATE_RET(ctx->ctx, iff->features, f_size, LY_EMEM);
    iff->expr = calloc((j = (expr_size / 4) + ((expr_size % 4) ? 1 : 0)), sizeof *iff->expr);
    stack.stack = malloc(expr_size * sizeof *stack.stack);
    LY_CHECK_ERR_GOTO(!stack.stack || !iff->expr, LOGMEM(ctx->ctx), error);

    stack.size = expr_size;
    f_size--; expr_size--; /* used as indexes from now */

    for (i--; i >= 0; i--) {
        if (c[i] == ')') {
            /* push it on stack */
            iff_stack_push(&stack, LYS_IFF_RP);
            continue;
        } else if (c[i] == '(') {
            /* pop from the stack into result all operators until ) */
            while((op = iff_stack_pop(&stack)) != LYS_IFF_RP) {
                iff_setop(iff->expr, op, expr_size--);
            }
            continue;
        } else if (isspace(c[i])) {
            continue;
        }

        /* end of operator or operand -> find beginning and get what is it */
        j = i + 1;
        while (i >= 0 && !isspace(c[i]) && c[i] != '(') {
            i--;
        }
        i++; /* go back by one step */

        if (!strncmp(&c[i], "not", 3) && isspace(c[i + 3])) {
            if (stack.index && stack.stack[stack.index - 1] == LYS_IFF_NOT) {
                /* double not */
                iff_stack_pop(&stack);
            } else {
                /* not has the highest priority, so do not pop from the stack
                 * as in case of AND and OR */
                iff_stack_push(&stack, LYS_IFF_NOT);
            }
        } else if (!strncmp(&c[i], "and", 3) && isspace(c[i + 3])) {
            /* as for OR - pop from the stack all operators with the same or higher
             * priority and store them to the result, then push the AND to the stack */
            while (stack.index && stack.stack[stack.index - 1] <= LYS_IFF_AND) {
                op = iff_stack_pop(&stack);
                iff_setop(iff->expr, op, expr_size--);
            }
            iff_stack_push(&stack, LYS_IFF_AND);
        } else if (!strncmp(&c[i], "or", 2) && isspace(c[i + 2])) {
            while (stack.index && stack.stack[stack.index - 1] <= LYS_IFF_OR) {
                op = iff_stack_pop(&stack);
                iff_setop(iff->expr, op, expr_size--);
            }
            iff_stack_push(&stack, LYS_IFF_OR);
        } else {
            /* feature name, length is j - i */

            /* add it to the expression */
            iff_setop(iff->expr, LYS_IFF_F, expr_size--);

            /* now get the link to the feature definition */
            f = lysc_feature_find(ctx->mod->compiled, &c[i], j - i);
            LY_CHECK_ERR_GOTO(!f,
                              LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                                     "Invalid value \"%s\" of if-feature - unable to find feature \"%.*s\".", *value, j - i, &c[i]);
                              rc = LY_EVALID,
                              error)
            iff->features[f_size] = f;
            LY_ARRAY_INCREMENT(iff->features);
            f_size--;
        }
    }
    while (stack.index) {
        op = iff_stack_pop(&stack);
        iff_setop(iff->expr, op, expr_size--);
    }

    if (++expr_size || ++f_size) {
        /* not all expected operators and operands found */
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
               "Invalid value \"%s\" of if-feature - processing error.", *value);
        rc = LY_EINT;
    } else {
        rc = LY_SUCCESS;
    }

error:
    /* cleanup */
    iff_stack_clean(&stack);

    return rc;
}

static LY_ERR
lys_compile_when(struct lysc_ctx *ctx, struct lysp_when *when_p, int options, struct lysc_when *when)
{
    unsigned int u;
    LY_ERR ret = LY_SUCCESS;

    when->cond = lyxp_expr_parse(ctx->ctx, when_p->cond);
    LY_CHECK_ERR_GOTO(!when->cond, ret = ly_errcode(ctx->ctx), done);
    COMPILE_ARRAY_GOTO(ctx, when_p->exts, when->exts, options, u, lys_compile_ext, ret, done);

done:
    return ret;
}

static LY_ERR
lys_compile_must(struct lysc_ctx *ctx, struct lysp_restr *must_p, int options, struct lysc_must *must)
{
    unsigned int u;
    LY_ERR ret = LY_SUCCESS;

    must->cond = lyxp_expr_parse(ctx->ctx, must_p->arg);
    LY_CHECK_ERR_GOTO(!must->cond, ret = ly_errcode(ctx->ctx), done);

    DUP_STRING(ctx->ctx, must_p->eapptag, must->eapptag);
    DUP_STRING(ctx->ctx, must_p->emsg, must->emsg);
    COMPILE_ARRAY_GOTO(ctx, must_p->exts, must->exts, options, u, lys_compile_ext, ret, done);

done:
    return ret;
}

static LY_ERR
lys_compile_import(struct lysc_ctx *ctx, struct lysp_import *imp_p, int options, struct lysc_import *imp)
{
    unsigned int u;
    struct lys_module *mod = NULL;
    struct lysc_module *comp;
    LY_ERR ret = LY_SUCCESS;

    DUP_STRING(ctx->ctx, imp_p->prefix, imp->prefix);
    COMPILE_ARRAY_GOTO(ctx, imp_p->exts, imp->exts, options, u, lys_compile_ext, ret, done);
    imp->module = imp_p->module;

    /* make sure that we have both versions (lysp_ and lysc_) of the imported module. To import groupings or
     * typedefs, the lysp_ is needed. To augment or deviate imported module, we need the lysc_ structure */
    if (!imp->module->parsed) {
        comp = imp->module->compiled;
        /* try to get filepath from the compiled version */
        if (comp->filepath) {
            mod = (struct lys_module*)lys_parse_path(ctx->ctx, comp->filepath,
                                 !strcmp(&comp->filepath[strlen(comp->filepath - 4)], ".yin") ? LYS_IN_YIN : LYS_IN_YANG);
            if (mod != imp->module) {
                LOGERR(ctx->ctx, LY_EINT, "Filepath \"%s\" of the module \"%s\" does not match.",
                       comp->filepath, comp->name);
                mod = NULL;
            }
        }
        if (!mod) {
            if (lysp_load_module(ctx->ctx, comp->name, comp->revision, 0, 1, &mod)) {
                LOGERR(ctx->ctx, LY_ENOTFOUND, "Unable to reload \"%s\" module to import it into \"%s\", source data not found.",
                       comp->name, ctx->mod->compiled->name);
                return LY_ENOTFOUND;
            }
        }
    } else if (!imp->module->compiled) {
        return lys_compile(imp->module, options);
    }

done:
    return ret;
}

static LY_ERR
lys_compile_identity(struct lysc_ctx *ctx, struct lysp_ident *ident_p, int options, struct lysc_ident *ident)
{
    unsigned int u;
    LY_ERR ret = LY_SUCCESS;

    DUP_STRING(ctx->ctx, ident_p->name, ident->name);
    COMPILE_ARRAY_GOTO(ctx, ident_p->iffeatures, ident->iffeatures, options, u, lys_compile_iffeature, ret, done);
    /* backlings (derived) can be added no sooner than when all the identities in the current module are present */
    COMPILE_ARRAY_GOTO(ctx, ident_p->exts, ident->exts, options, u, lys_compile_ext, ret, done);
    ident->flags = ident_p->flags;

done:
    return ret;
}

static LY_ERR
lys_compile_identities_derived(struct lysc_ctx *ctx, struct lysp_ident *idents_p, struct lysc_ident *idents)
{
    unsigned int i, u, v;
    const char *s, *name;
    struct lysc_module *mod;
    struct lysc_ident **dident;

    for (i = 0; i < LY_ARRAY_SIZE(idents_p); ++i) {
        if (!idents_p[i].bases) {
            continue;
        }
        for (u = 0; u < LY_ARRAY_SIZE(idents_p[i].bases); ++u) {
            s = strchr(idents_p[i].bases[u], ':');
            if (s) {
                /* prefixed identity */
                name = &s[1];
                mod = lysc_module_find_prefix(ctx->mod->compiled, idents_p[i].bases[u], s - idents_p[i].bases[u]);
            } else {
                name = idents_p[i].bases[u];
                mod = ctx->mod->compiled;
            }
            LY_CHECK_ERR_RET(!mod, LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                                          "Invalid prefix used for base (%s) of identity \"%s\".", idents_p[i].bases[u], idents[i].name),
                             LY_EVALID);
            if (mod->identities) {
                for (v = 0; v < LY_ARRAY_SIZE(mod->identities); ++v) {
                    if (!strcmp(name, mod->identities[v].name)) {
                        /* we have match! store the backlink */
                        LY_ARRAY_NEW_RET(ctx->ctx, mod->identities[v].derived, dident, LY_EMEM);
                        *dident = &idents[i];
                        break;
                    }
                }
            }
            LY_CHECK_ERR_RET(!dident, LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                                             "Unable to find base (%s) of identity \"%s\".", idents_p[i].bases[u], idents[i].name),
                             LY_EVALID);
        }
    }
    return LY_SUCCESS;
}

static LY_ERR
lys_compile_feature(struct lysc_ctx *ctx, struct lysp_feature *feature_p, int options, struct lysc_feature *feature)
{
    unsigned int u, v;
    LY_ERR ret = LY_SUCCESS;
    struct lysc_feature **df;

    DUP_STRING(ctx->ctx, feature_p->name, feature->name);
    feature->flags = feature_p->flags;

    COMPILE_ARRAY_GOTO(ctx, feature_p->exts, feature->exts, options, u, lys_compile_ext, ret, done);
    COMPILE_ARRAY_GOTO(ctx, feature_p->iffeatures, feature->iffeatures, options, u, lys_compile_iffeature, ret, done);
    if (feature->iffeatures) {
        for (u = 0; u < LY_ARRAY_SIZE(feature->iffeatures); ++u) {
            if (feature->iffeatures[u].features) {
                for (v = 0; v < LY_ARRAY_SIZE(feature->iffeatures[u].features); ++v) {
                    /* add itself into the dependants list */
                    LY_ARRAY_NEW_RET(ctx->ctx, feature->iffeatures[u].features[v]->depfeatures, df, LY_EMEM);
                    *df = feature;
                }
                /* TODO check for circular dependency */
            }
        }
    }
done:
    return ret;
}

static LY_ERR
range_part_check_value_syntax(struct lysc_ctx *ctx, LY_DATA_TYPE basetype, uint8_t frdigits, const char *value, size_t *len, char **valcopy)
{
    size_t fraction = 0, size;

    *len = 0;

    assert(value);
    /* parse value */
    if (!isdigit(value[*len]) && (value[*len] != '-') && (value[*len] != '+')) {
        return LY_EVALID;
    }

    if ((value[*len] == '-') || (value[*len] == '+')) {
        ++(*len);
    }

    while (isdigit(value[*len])) {
        ++(*len);
    }

    if ((basetype != LY_TYPE_DEC64) || (value[*len] != '.') || !isdigit(value[*len + 1])) {
        if (basetype == LY_TYPE_DEC64) {
            goto decimal;
        } else {
            *valcopy = strndup(value, *len);
            return LY_SUCCESS;
        }
    }
    fraction = *len;

    ++(*len);
    while (isdigit(value[*len])) {
        ++(*len);
    }

    if (basetype == LY_TYPE_DEC64) {
decimal:
        assert(frdigits);
        if (*len - 1 - fraction > frdigits) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                   "Range boundary \"%.*s\" of decimal64 type exceeds defined number (%u) of fraction digits.",
                   *len, value, frdigits);
            return LY_EINVAL;
        }
        if (fraction) {
            size = (*len) + (frdigits - ((*len) - 1 - fraction));
        } else {
            size = (*len) + frdigits + 1;
        }
        *valcopy = malloc(size * sizeof **valcopy);
        LY_CHECK_ERR_RET(!(*valcopy), LOGMEM(ctx->ctx), LY_EMEM);

        (*valcopy)[size - 1] = '\0';
        if (fraction) {
            memcpy(&(*valcopy)[0], &value[0], fraction);
            memcpy(&(*valcopy)[fraction], &value[fraction + 1], (*len) - 1 - (fraction));
            memset(&(*valcopy)[(*len) - 1], '0', frdigits - ((*len) - 1 - fraction));
        } else {
            memcpy(&(*valcopy)[0], &value[0], *len);
            memset(&(*valcopy)[*len], '0', frdigits);
        }
    }
    return LY_SUCCESS;
}

static LY_ERR
range_part_check_ascendance(int unsigned_value, int64_t value, int64_t prev_value)
{
    if (unsigned_value) {
        if ((uint64_t)prev_value >= (uint64_t)value) {
            return LY_EEXIST;
        }
    } else {
        if (prev_value >= value) {
            return LY_EEXIST;
        }
    }
    return LY_SUCCESS;
}

static LY_ERR
range_part_minmax(struct lysc_ctx *ctx, struct lysc_range_part *part, int max, int64_t prev, LY_DATA_TYPE basetype, int first, int length_restr,
                  uint8_t frdigits, const char **value)
{
    LY_ERR ret = LY_SUCCESS;
    char *valcopy = NULL;
    size_t len;

    if (value) {
        ret = range_part_check_value_syntax(ctx, basetype, frdigits, *value, &len, &valcopy);
        LY_CHECK_GOTO(ret, error);
    }

    switch (basetype) {
    case LY_TYPE_BINARY: /* length */
        if (valcopy) {
            ret = ly_parse_uint(valcopy, UINT64_C(18446744073709551615), 10, max ? &part->max_u64 : &part->min_u64);
        } else if (max) {
            part->max_u64 = UINT64_C(18446744073709551615);
        } else {
            part->min_u64 = UINT64_C(0);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendance(1, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_DEC64: /* range */
        if (valcopy) {
            ret = ly_parse_int(valcopy, INT64_C(-9223372036854775807) - INT64_C(1), INT64_C(9223372036854775807), 10,
                               max ? &part->max_64 : &part->min_64);
        } else if (max) {
            part->max_64 = INT64_C(9223372036854775807);
        } else {
            part->min_64 = INT64_C(-9223372036854775807) - INT64_C(1);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendance(0, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_INT8: /* range */
        if (valcopy) {
            ret = ly_parse_int(valcopy, INT64_C(-128), INT64_C(127), 10, max ? &part->max_64 : &part->min_64);
        } else if (max) {
            part->max_64 = INT64_C(127);
        } else {
            part->min_64 = INT64_C(-128);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendance(0, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_INT16: /* range */
        if (valcopy) {
            ret = ly_parse_int(valcopy, INT64_C(-32768), INT64_C(32767), 10, max ? &part->max_64 : &part->min_64);
        } else if (max) {
            part->max_64 = INT64_C(32767);
        } else {
            part->min_64 = INT64_C(-32768);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendance(0, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_INT32: /* range */
        if (valcopy) {
            ret = ly_parse_int(valcopy, INT64_C(-2147483648), INT64_C(2147483647), 10, max ? &part->max_64 : &part->min_64);
        } else if (max) {
            part->max_64 = INT64_C(2147483647);
        } else {
            part->min_64 = INT64_C(-2147483648);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendance(0, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_INT64: /* range */
        if (valcopy) {
            ret = ly_parse_int(valcopy, INT64_C(-9223372036854775807) - INT64_C(1), INT64_C(9223372036854775807), 10,
                               max ? &part->max_64 : &part->min_64);
        } else if (max) {
            part->max_64 = INT64_C(9223372036854775807);
        } else {
            part->min_64 = INT64_C(-9223372036854775807) - INT64_C(1);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendance(0, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_UINT8: /* range */
        if (valcopy) {
            ret = ly_parse_uint(valcopy, UINT64_C(255), 10, max ? &part->max_u64 : &part->min_u64);
        } else if (max) {
            part->max_u64 = UINT64_C(255);
        } else {
            part->min_u64 = UINT64_C(0);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendance(1, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_UINT16: /* range */
        if (valcopy) {
            ret = ly_parse_uint(valcopy, UINT64_C(65535), 10, max ? &part->max_u64 : &part->min_u64);
        } else if (max) {
            part->max_u64 = UINT64_C(65535);
        } else {
            part->min_u64 = UINT64_C(0);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendance(1, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_UINT32: /* range */
        if (valcopy) {
            ret = ly_parse_uint(valcopy, UINT64_C(4294967295), 10, max ? &part->max_u64 : &part->min_u64);
        } else if (max) {
            part->max_u64 = UINT64_C(4294967295);
        } else {
            part->min_u64 = UINT64_C(0);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendance(1, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_UINT64: /* range */
        if (valcopy) {
            ret = ly_parse_uint(valcopy, UINT64_C(18446744073709551615), 10, max ? &part->max_u64 : &part->min_u64);
        } else if (max) {
            part->max_u64 = UINT64_C(18446744073709551615);
        } else {
            part->min_u64 = UINT64_C(0);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendance(1, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_STRING: /* length */
        if (valcopy) {
            ret = ly_parse_uint(valcopy, UINT64_C(18446744073709551615), 10, max ? &part->max_u64 : &part->min_u64);
        } else if (max) {
            part->max_u64 = UINT64_C(18446744073709551615);
        } else {
            part->min_u64 = UINT64_C(0);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendance(1, max ? part->max_64 : part->min_64, prev);
        }
        break;
    default:
        LOGINT(ctx->ctx);
        ret = LY_EINT;
    }

error:
    if (ret == LY_EDENIED) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
               "Invalid %s restriction - value \"%s\" does not fit the type limitations.",
               length_restr ? "length" : "range", valcopy ? valcopy : *value);
    } else if (ret == LY_EVALID) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
               "Invalid %s restriction - invalid value \"%s\".",
               length_restr ? "length" : "range", valcopy ? valcopy : *value);
    } else if (ret == LY_EEXIST) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid %s restriction - values are not in ascending order (%s).",
                       length_restr ? "length" : "range",
                       (valcopy && basetype != LY_TYPE_DEC64) ? valcopy : *value);
    } else if (!ret && value) {
        *value = *value + len;
    }
    free(valcopy);
    return ret;
}

static LY_ERR
lys_compile_type_range(struct lysc_ctx *ctx, struct lysp_restr *range_p, LY_DATA_TYPE basetype, int length_restr, uint8_t frdigits,
                       struct lysc_range *base_range, struct lysc_range **range)
{
    LY_ERR ret = LY_EVALID;
    const char *expr;
    struct lysc_range_part *parts = NULL, *part;
    int range_expected = 0, uns;
    unsigned int parts_done = 0, u, v;

    assert(range);
    assert(range_p);

    expr = range_p->arg;
    while(1) {
        if (isspace(*expr)) {
            ++expr;
        } else if (*expr == '\0') {
            if (range_expected) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid %s restriction - unexpected end of the expression after \"..\" (%s).",
                       length_restr ? "length" : "range", range_p->arg);
                goto cleanup;
            } else if (!parts || parts_done == LY_ARRAY_SIZE(parts)) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid %s restriction - unexpected end of the expression (%s).",
                       length_restr ? "length" : "range", range_p->arg);
                goto cleanup;
            }
            parts_done++;
            break;
        } else if (!strncmp(expr, "min", 3)) {
            if (parts) {
                /* min cannot be used elsewhere than in the first part */
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid %s restriction - unexpected data before min keyword (%.*s).", length_restr ? "length" : "range",
                       expr - range_p->arg, range_p->arg);
                goto cleanup;
            }
            expr += 3;

            LY_ARRAY_NEW_GOTO(ctx->ctx, parts, part, ret, cleanup);
            LY_CHECK_GOTO(range_part_minmax(ctx, part, 0, 0, basetype, 1, length_restr, frdigits, NULL), cleanup);
            part->max_64 = part->min_64;
        } else if (*expr == '|') {
            if (!parts || range_expected) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid %s restriction - unexpected beginning of the expression (%s).", length_restr ? "length" : "range", expr);
                goto cleanup;
            }
            expr++;
            parts_done++;
            /* process next part of the expression */
        } else if (!strncmp(expr, "..", 2)) {
            expr += 2;
            while (isspace(*expr)) {
                expr++;
            }
            if (!parts || LY_ARRAY_SIZE(parts) == parts_done) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid %s restriction - unexpected \"..\" without a lower bound.", length_restr ? "length" : "range");
                goto cleanup;
            }
            /* continue expecting the upper boundary */
            range_expected = 1;
        } else if (isdigit(*expr) || (*expr == '-') || (*expr == '+')) {
            /* number */
            if (range_expected) {
                part = &parts[LY_ARRAY_SIZE(parts) - 1];
                LY_CHECK_GOTO(range_part_minmax(ctx, part, 1, part->min_64, basetype, 0, length_restr, frdigits, &expr), cleanup);
                range_expected = 0;
            } else {
                LY_ARRAY_NEW_GOTO(ctx->ctx, parts, part, ret, cleanup);
                LY_CHECK_GOTO(range_part_minmax(ctx, part, 0, parts_done ? parts[LY_ARRAY_SIZE(parts) - 2].max_64 : 0,
                                                basetype, parts_done ? 0 : 1, length_restr, frdigits, &expr), cleanup);
                part->max_64 = part->min_64;
            }

            /* continue with possible another expression part */
        } else if (!strncmp(expr, "max", 3)) {
            expr += 3;
            while (isspace(*expr)) {
                expr++;
            }
            if (*expr != '\0') {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Invalid %s restriction - unexpected data after max keyword (%s).",
                       length_restr ? "length" : "range", expr);
                goto cleanup;
            }
            if (range_expected) {
                part = &parts[LY_ARRAY_SIZE(parts) - 1];
                LY_CHECK_GOTO(range_part_minmax(ctx, part, 1, part->min_64, basetype, 0, length_restr, frdigits, NULL), cleanup);
                range_expected = 0;
            } else {
                LY_ARRAY_NEW_GOTO(ctx->ctx, parts, part, ret, cleanup);
                LY_CHECK_GOTO(range_part_minmax(ctx, part, 1, parts_done ? parts[LY_ARRAY_SIZE(parts) - 2].max_64 : 0,
                                                basetype, parts_done ? 0 : 1, length_restr, frdigits, NULL), cleanup);
                part->min_64 = part->max_64;
            }
        } else {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Invalid %s restriction - unexpected data (%s).",
                   length_restr ? "length" : "range", expr);
            goto cleanup;
        }
    }

    /* check with the previous range/length restriction */
    if (base_range) {
        switch (basetype) {
        case LY_TYPE_BINARY:
        case LY_TYPE_UINT8:
        case LY_TYPE_UINT16:
        case LY_TYPE_UINT32:
        case LY_TYPE_UINT64:
        case LY_TYPE_STRING:
            uns = 1;
            break;
        case LY_TYPE_DEC64:
        case LY_TYPE_INT8:
        case LY_TYPE_INT16:
        case LY_TYPE_INT32:
        case LY_TYPE_INT64:
            uns = 0;
            break;
        default:
            LOGINT(ctx->ctx);
            ret = LY_EINT;
            goto cleanup;
        }
        for (u = v = 0; u < parts_done && v < LY_ARRAY_SIZE(base_range->parts); ++u) {
            if ((uns && parts[u].min_u64 < base_range->parts[v].min_u64) || (!uns && parts[u].min_64 < base_range->parts[v].min_64)) {
                goto baseerror;
            }
            /* current lower bound is not lower than the base */
            if (base_range->parts[v].min_64 == base_range->parts[v].max_64) {
                /* base has single value */
                if (base_range->parts[v].min_64 == parts[u].min_64) {
                    /* both lower bounds are the same */
                    if (parts[u].min_64 != parts[u].max_64) {
                        /* current continues with a range */
                        goto baseerror;
                    } else {
                        /* equal single values, move both forward */
                        ++v;
                        continue;
                    }
                } else {
                    /* base is single value lower than current range, so the
                     * value from base range is removed in the current,
                     * move only base and repeat checking */
                    ++v;
                    --u;
                    continue;
                }
            } else {
                /* base is the range */
                if (parts[u].min_64 == parts[u].max_64) {
                    /* current is a single value */
                    if ((uns && parts[u].max_u64 > base_range->parts[v].max_u64) || (!uns && parts[u].max_64 > base_range->parts[v].max_64)) {
                        /* current is behind the base range, so base range is omitted,
                         * move the base and keep the current for further check */
                        ++v;
                        --u;
                    } /* else it is within the base range, so move the current, but keep the base */
                    continue;
                } else {
                    /* both are ranges - check the higher bound, the lower was already checked */
                    if ((uns && parts[u].max_u64 > base_range->parts[v].max_u64) || (!uns && parts[u].max_64 > base_range->parts[v].max_64)) {
                        /* higher bound is higher than the current higher bound */
                        if ((uns && parts[u].min_u64 > base_range->parts[v].max_u64) || (!uns && parts[u].min_64 > base_range->parts[v].max_64)) {
                            /* but the current lower bound is also higher, so the base range is omitted,
                             * continue with the same current, but move the base */
                            --u;
                            ++v;
                            continue;
                        }
                        /* current range starts within the base range but end behind it */
                        goto baseerror;
                    } else {
                        /* current range is smaller than the base,
                         * move current, but stay with the base */
                        continue;
                    }
                }
            }
        }
        if (u != parts_done) {
baseerror:
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                   "Invalid %s restriction - the derived restriction (%s) is not equally or more limiting.",
                   length_restr ? "length" : "range", range_p->arg);
            goto cleanup;
        }
    }

    if (!(*range)) {
        *range = calloc(1, sizeof **range);
        LY_CHECK_ERR_RET(!(*range), LOGMEM(ctx->ctx), LY_EMEM);
    }

    if (range_p->eapptag) {
        lydict_remove(ctx->ctx, (*range)->eapptag);
        (*range)->eapptag = lydict_insert(ctx->ctx, range_p->eapptag, 0);
    }
    if (range_p->emsg) {
        lydict_remove(ctx->ctx, (*range)->emsg);
        (*range)->emsg = lydict_insert(ctx->ctx, range_p->emsg, 0);
    }
    /* extensions are taken only from the last range by the caller */

    (*range)->parts = parts;
    parts = NULL;
    ret = LY_SUCCESS;
cleanup:
    /* TODO clean up */
    LY_ARRAY_FREE(parts);

    return ret;
}

/**
 * @brief Checks pattern syntax.
 *
 * @param[in] pattern Pattern to check.
 * @param[out] pcre_precomp Precompiled PCRE pattern. Can be NULL.
 * @return EXIT_SUCCESS on success, EXIT_FAILURE otherwise.
 */
static LY_ERR
lys_compile_type_pattern_check(struct lysc_ctx *ctx, const char *pattern, pcre **pcre_precomp)
{
    int idx, idx2, start, end, err_offset, count;
    char *perl_regex, *ptr;
    const char *err_msg, *orig_ptr;
    pcre *precomp;
#define URANGE_LEN 19
    char *ublock2urange[][2] = {
        {"BasicLatin", "[\\x{0000}-\\x{007F}]"},
        {"Latin-1Supplement", "[\\x{0080}-\\x{00FF}]"},
        {"LatinExtended-A", "[\\x{0100}-\\x{017F}]"},
        {"LatinExtended-B", "[\\x{0180}-\\x{024F}]"},
        {"IPAExtensions", "[\\x{0250}-\\x{02AF}]"},
        {"SpacingModifierLetters", "[\\x{02B0}-\\x{02FF}]"},
        {"CombiningDiacriticalMarks", "[\\x{0300}-\\x{036F}]"},
        {"Greek", "[\\x{0370}-\\x{03FF}]"},
        {"Cyrillic", "[\\x{0400}-\\x{04FF}]"},
        {"Armenian", "[\\x{0530}-\\x{058F}]"},
        {"Hebrew", "[\\x{0590}-\\x{05FF}]"},
        {"Arabic", "[\\x{0600}-\\x{06FF}]"},
        {"Syriac", "[\\x{0700}-\\x{074F}]"},
        {"Thaana", "[\\x{0780}-\\x{07BF}]"},
        {"Devanagari", "[\\x{0900}-\\x{097F}]"},
        {"Bengali", "[\\x{0980}-\\x{09FF}]"},
        {"Gurmukhi", "[\\x{0A00}-\\x{0A7F}]"},
        {"Gujarati", "[\\x{0A80}-\\x{0AFF}]"},
        {"Oriya", "[\\x{0B00}-\\x{0B7F}]"},
        {"Tamil", "[\\x{0B80}-\\x{0BFF}]"},
        {"Telugu", "[\\x{0C00}-\\x{0C7F}]"},
        {"Kannada", "[\\x{0C80}-\\x{0CFF}]"},
        {"Malayalam", "[\\x{0D00}-\\x{0D7F}]"},
        {"Sinhala", "[\\x{0D80}-\\x{0DFF}]"},
        {"Thai", "[\\x{0E00}-\\x{0E7F}]"},
        {"Lao", "[\\x{0E80}-\\x{0EFF}]"},
        {"Tibetan", "[\\x{0F00}-\\x{0FFF}]"},
        {"Myanmar", "[\\x{1000}-\\x{109F}]"},
        {"Georgian", "[\\x{10A0}-\\x{10FF}]"},
        {"HangulJamo", "[\\x{1100}-\\x{11FF}]"},
        {"Ethiopic", "[\\x{1200}-\\x{137F}]"},
        {"Cherokee", "[\\x{13A0}-\\x{13FF}]"},
        {"UnifiedCanadianAboriginalSyllabics", "[\\x{1400}-\\x{167F}]"},
        {"Ogham", "[\\x{1680}-\\x{169F}]"},
        {"Runic", "[\\x{16A0}-\\x{16FF}]"},
        {"Khmer", "[\\x{1780}-\\x{17FF}]"},
        {"Mongolian", "[\\x{1800}-\\x{18AF}]"},
        {"LatinExtendedAdditional", "[\\x{1E00}-\\x{1EFF}]"},
        {"GreekExtended", "[\\x{1F00}-\\x{1FFF}]"},
        {"GeneralPunctuation", "[\\x{2000}-\\x{206F}]"},
        {"SuperscriptsandSubscripts", "[\\x{2070}-\\x{209F}]"},
        {"CurrencySymbols", "[\\x{20A0}-\\x{20CF}]"},
        {"CombiningMarksforSymbols", "[\\x{20D0}-\\x{20FF}]"},
        {"LetterlikeSymbols", "[\\x{2100}-\\x{214F}]"},
        {"NumberForms", "[\\x{2150}-\\x{218F}]"},
        {"Arrows", "[\\x{2190}-\\x{21FF}]"},
        {"MathematicalOperators", "[\\x{2200}-\\x{22FF}]"},
        {"MiscellaneousTechnical", "[\\x{2300}-\\x{23FF}]"},
        {"ControlPictures", "[\\x{2400}-\\x{243F}]"},
        {"OpticalCharacterRecognition", "[\\x{2440}-\\x{245F}]"},
        {"EnclosedAlphanumerics", "[\\x{2460}-\\x{24FF}]"},
        {"BoxDrawing", "[\\x{2500}-\\x{257F}]"},
        {"BlockElements", "[\\x{2580}-\\x{259F}]"},
        {"GeometricShapes", "[\\x{25A0}-\\x{25FF}]"},
        {"MiscellaneousSymbols", "[\\x{2600}-\\x{26FF}]"},
        {"Dingbats", "[\\x{2700}-\\x{27BF}]"},
        {"BraillePatterns", "[\\x{2800}-\\x{28FF}]"},
        {"CJKRadicalsSupplement", "[\\x{2E80}-\\x{2EFF}]"},
        {"KangxiRadicals", "[\\x{2F00}-\\x{2FDF}]"},
        {"IdeographicDescriptionCharacters", "[\\x{2FF0}-\\x{2FFF}]"},
        {"CJKSymbolsandPunctuation", "[\\x{3000}-\\x{303F}]"},
        {"Hiragana", "[\\x{3040}-\\x{309F}]"},
        {"Katakana", "[\\x{30A0}-\\x{30FF}]"},
        {"Bopomofo", "[\\x{3100}-\\x{312F}]"},
        {"HangulCompatibilityJamo", "[\\x{3130}-\\x{318F}]"},
        {"Kanbun", "[\\x{3190}-\\x{319F}]"},
        {"BopomofoExtended", "[\\x{31A0}-\\x{31BF}]"},
        {"EnclosedCJKLettersandMonths", "[\\x{3200}-\\x{32FF}]"},
        {"CJKCompatibility", "[\\x{3300}-\\x{33FF}]"},
        {"CJKUnifiedIdeographsExtensionA", "[\\x{3400}-\\x{4DB5}]"},
        {"CJKUnifiedIdeographs", "[\\x{4E00}-\\x{9FFF}]"},
        {"YiSyllables", "[\\x{A000}-\\x{A48F}]"},
        {"YiRadicals", "[\\x{A490}-\\x{A4CF}]"},
        {"HangulSyllables", "[\\x{AC00}-\\x{D7A3}]"},
        {"PrivateUse", "[\\x{E000}-\\x{F8FF}]"},
        {"CJKCompatibilityIdeographs", "[\\x{F900}-\\x{FAFF}]"},
        {"AlphabeticPresentationForms", "[\\x{FB00}-\\x{FB4F}]"},
        {"ArabicPresentationForms-A", "[\\x{FB50}-\\x{FDFF}]"},
        {"CombiningHalfMarks", "[\\x{FE20}-\\x{FE2F}]"},
        {"CJKCompatibilityForms", "[\\x{FE30}-\\x{FE4F}]"},
        {"SmallFormVariants", "[\\x{FE50}-\\x{FE6F}]"},
        {"ArabicPresentationForms-B", "[\\x{FE70}-\\x{FEFE}]"},
        {"HalfwidthandFullwidthForms", "[\\x{FF00}-\\x{FFEF}]"},
        {NULL, NULL}
    };

    /* adjust the expression to a Perl equivalent
     * http://www.w3.org/TR/2004/REC-xmlschema-2-20041028/#regexs */

    /* we need to replace all "$" with "\$", count them now */
    for (count = 0, ptr = strpbrk(pattern, "^$"); ptr; ++count, ptr = strpbrk(ptr + 1, "^$"));

    perl_regex = malloc((strlen(pattern) + 4 + count) * sizeof(char));
    LY_CHECK_ERR_RET(!perl_regex, LOGMEM(ctx->ctx), LY_EMEM);
    perl_regex[0] = '\0';

    ptr = perl_regex;

    if (strncmp(pattern + strlen(pattern) - 2, ".*", 2)) {
        /* we will add line-end anchoring */
        ptr[0] = '(';
        ++ptr;
    }

    for (orig_ptr = pattern; orig_ptr[0]; ++orig_ptr) {
        if (orig_ptr[0] == '$') {
            ptr += sprintf(ptr, "\\$");
        } else if (orig_ptr[0] == '^') {
            ptr += sprintf(ptr, "\\^");
        } else {
            ptr[0] = orig_ptr[0];
            ++ptr;
        }
    }

    if (strncmp(pattern + strlen(pattern) - 2, ".*", 2)) {
        ptr += sprintf(ptr, ")$");
    } else {
        ptr[0] = '\0';
        ++ptr;
    }

    /* substitute Unicode Character Blocks with exact Character Ranges */
    while ((ptr = strstr(perl_regex, "\\p{Is"))) {
        start = ptr - perl_regex;

        ptr = strchr(ptr, '}');
        if (!ptr) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_INREGEXP,
                   pattern, perl_regex + start + 2, "unterminated character property");
            free(perl_regex);
            return LY_EVALID;
        }
        end = (ptr - perl_regex) + 1;

        /* need more space */
        if (end - start < URANGE_LEN) {
            perl_regex = ly_realloc(perl_regex, strlen(perl_regex) + (URANGE_LEN - (end - start)) + 1);
            LY_CHECK_ERR_RET(!perl_regex, LOGMEM(ctx->ctx); free(perl_regex), LY_EMEM);
        }

        /* find our range */
        for (idx = 0; ublock2urange[idx][0]; ++idx) {
            if (!strncmp(perl_regex + start + 5, ublock2urange[idx][0], strlen(ublock2urange[idx][0]))) {
                break;
            }
        }
        if (!ublock2urange[idx][0]) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_INREGEXP,
                   pattern, perl_regex + start + 5, "unknown block name");
            free(perl_regex);
            return LY_EVALID;
        }

        /* make the space in the string and replace the block (but we cannot include brackets if it was already enclosed in them) */
        for (idx2 = 0, count = 0; idx2 < start; ++idx2) {
            if ((perl_regex[idx2] == '[') && (!idx2 || (perl_regex[idx2 - 1] != '\\'))) {
                ++count;
            }
            if ((perl_regex[idx2] == ']') && (!idx2 || (perl_regex[idx2 - 1] != '\\'))) {
                --count;
            }
        }
        if (count) {
            /* skip brackets */
            memmove(perl_regex + start + (URANGE_LEN - 2), perl_regex + end, strlen(perl_regex + end) + 1);
            memcpy(perl_regex + start, ublock2urange[idx][1] + 1, URANGE_LEN - 2);
        } else {
            memmove(perl_regex + start + URANGE_LEN, perl_regex + end, strlen(perl_regex + end) + 1);
            memcpy(perl_regex + start, ublock2urange[idx][1], URANGE_LEN);
        }
    }

    /* must return 0, already checked during parsing */
    precomp = pcre_compile(perl_regex, PCRE_UTF8 | PCRE_ANCHORED | PCRE_DOLLAR_ENDONLY | PCRE_NO_AUTO_CAPTURE,
                           &err_msg, &err_offset, NULL);
    if (!precomp) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_INREGEXP, pattern, perl_regex + err_offset, err_msg);
        free(perl_regex);
        return LY_EVALID;
    }
    free(perl_regex);

    if (pcre_precomp) {
        *pcre_precomp = precomp;
    } else {
        free(precomp);
    }

    return LY_SUCCESS;

#undef URANGE_LEN
}

static LY_ERR
lys_compile_type_patterns(struct lysc_ctx *ctx, struct lysp_restr *patterns_p, int options,
                          struct lysc_pattern **base_patterns, struct lysc_pattern ***patterns)
{
    struct lysc_pattern **pattern;
    unsigned int u, v;
    const char *err_msg;
    LY_ERR ret = LY_SUCCESS;

    /* first, copy the patterns from the base type */
    if (base_patterns) {
        *patterns = lysc_patterns_dup(ctx->ctx, base_patterns);
        LY_CHECK_ERR_RET(!(*patterns), LOGMEM(ctx->ctx), LY_EMEM);
    }

    LY_ARRAY_FOR(patterns_p, u) {
        LY_ARRAY_NEW_RET(ctx->ctx, (*patterns), pattern, LY_EMEM);
        *pattern = calloc(1, sizeof **pattern);
        ++(*pattern)->refcount;

        ret = lys_compile_type_pattern_check(ctx, &patterns_p[u].arg[1], &(*pattern)->expr);
        LY_CHECK_RET(ret);
        (*pattern)->expr_extra = pcre_study((*pattern)->expr, 0, &err_msg);
        if (err_msg) {
            LOGWRN(ctx->ctx, "Studying pattern \"%s\" failed (%s).", pattern, err_msg);
        }

        if (patterns_p[u].arg[0] == 0x15) {
            (*pattern)->inverted = 1;
        }
        DUP_STRING(ctx->ctx, patterns_p[u].eapptag, (*pattern)->eapptag);
        DUP_STRING(ctx->ctx, patterns_p[u].emsg, (*pattern)->emsg);
        COMPILE_ARRAY_GOTO(ctx, patterns_p[u].exts, (*pattern)->exts,
                           options, v, lys_compile_ext, ret, done);
    }
done:
    return ret;
}

static uint16_t type_substmt_map[LY_DATA_TYPE_COUNT] = {
    0 /* LY_TYPE_UNKNOWN */,
    LYS_SET_LENGTH /* LY_TYPE_BINARY */,
    LYS_SET_BIT /* LY_TYPE_BITS */,
    0 /* LY_TYPE_BOOL */,
    LYS_SET_FRDIGITS | LYS_SET_RANGE /* LY_TYPE_DEC64 */,
    0 /* LY_TYPE_EMPTY */,
    LYS_SET_ENUM /* LY_TYPE_ENUM */,
    LYS_SET_BASE /* LY_TYPE_IDENT */,
    LYS_SET_REQINST /* LY_TYPE_INST */,
    LYS_SET_REQINST | LYS_SET_PATH /* LY_TYPE_LEAFREF */,
    LYS_SET_LENGTH | LYS_SET_PATTERN /* LY_TYPE_STRING */,
    LYS_SET_TYPE /* LY_TYPE_UNION */,
    LYS_SET_RANGE /* LY_TYPE_INT8 */,
    LYS_SET_RANGE /* LY_TYPE_UINT8 */,
    LYS_SET_RANGE /* LY_TYPE_INT16 */,
    LYS_SET_RANGE /* LY_TYPE_UINT16 */,
    LYS_SET_RANGE /* LY_TYPE_INT32 */,
    LYS_SET_RANGE /* LY_TYPE_UINT32 */,
    LYS_SET_RANGE /* LY_TYPE_INT64 */,
    LYS_SET_RANGE /* LY_TYPE_UINT64 */
};

static LY_ERR
lys_compile_type_enums(struct lysc_ctx *ctx, struct lysp_type_enum *enums_p, LY_DATA_TYPE basetype, int options,
                       struct lysc_type_enum_item *base_enums, struct lysc_type_enum_item **enums)
{
    LY_ERR ret = LY_SUCCESS;
    unsigned int u, v, match;
    int32_t value = 0;
    uint32_t position = 0;
    struct lysc_type_enum_item *e, storage;

    if (base_enums && ctx->mod->compiled->version < 2) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "%s type can be subtyped only in YANG 1.1 modules.",
               basetype == LY_TYPE_ENUM ? "Enumeration" : "Bits");
        return LY_EVALID;
    }

    LY_ARRAY_FOR(enums_p, u) {
        LY_ARRAY_NEW_RET(ctx->ctx, *enums, e, LY_EMEM);
        DUP_STRING(ctx->ctx, enums_p[u].name, e->name);
        if (base_enums) {
            /* check the enum/bit presence in the base type - the set of enums/bits in the derived type must be a subset */
            LY_ARRAY_FOR(base_enums, v) {
                if (!strcmp(e->name, base_enums[v].name)) {
                    break;
                }
            }
            if (v == LY_ARRAY_SIZE(base_enums)) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid %s - derived type adds new item \"%s\".",
                       basetype == LY_TYPE_ENUM ? "enumeration" : "bits", e->name);
                return LY_EVALID;
            }
            match = v;
        }

        if (basetype == LY_TYPE_ENUM) {
            if (enums_p[u].flags & LYS_SET_VALUE) {
                e->value = (int32_t)enums_p[u].value;
                if (!u || e->value >= value) {
                    value = e->value + 1;
                }
                /* check collision with other values */
                for (v = 0; v < LY_ARRAY_SIZE(*enums) - 1; ++v) {
                    if (e->value == (*enums)[v].value) {
                        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                               "Invalid enumeration - value %d collide in items \"%s\" and \"%s\".",
                               e->value, e->name, (*enums)[v].name);
                        return LY_EVALID;
                    }
                }
            } else if (base_enums) {
                /* inherit the assigned value */
                e->value = base_enums[match].value;
                if (!u || e->value >= value) {
                    value = e->value + 1;
                }
            } else {
                /* assign value automatically */
                if (u && value == INT32_MIN) {
                    /* counter overflow */
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                           "Invalid enumeration - it is not possible to auto-assign enum value for "
                           "\"%s\" since the highest value is already 2147483647.", e->name);
                    return LY_EVALID;
                }
                e->value = value++;
            }
        } else { /* LY_TYPE_BITS */
            if (enums_p[u].flags & LYS_SET_VALUE) {
                e->value = (int32_t)enums_p[u].value;
                if (!u || (uint32_t)e->value >= position) {
                    position = (uint32_t)e->value + 1;
                }
                /* check collision with other values */
                for (v = 0; v < LY_ARRAY_SIZE(*enums) - 1; ++v) {
                    if (e->value == (*enums)[v].value) {
                        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                               "Invalid bits - position %u collide in items \"%s\" and \"%s\".",
                               (uint32_t)e->value, e->name, (*enums)[v].name);
                        return LY_EVALID;
                    }
                }
            } else if (base_enums) {
                /* inherit the assigned value */
                e->value = base_enums[match].value;
                if (!u || (uint32_t)e->value >= position) {
                    position = (uint32_t)e->value + 1;
                }
            } else {
                /* assign value automatically */
                if (u && position == 0) {
                    /* counter overflow */
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                           "Invalid bits - it is not possible to auto-assign bit position for "
                           "\"%s\" since the highest value is already 4294967295.", e->name);
                    return LY_EVALID;
                }
                e->value = position++;
            }
        }

        if (base_enums) {
            /* the assigned values must not change from the derived type */
            if (e->value != base_enums[match].value) {
                if (basetype == LY_TYPE_ENUM) {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid enumeration - value of the item \"%s\" has changed from %d to %d in the derived type.",
                       e->name, base_enums[match].value, e->value);
                } else {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid bits - position of the item \"%s\" has changed from %u to %u in the derived type.",
                       e->name, (uint32_t)base_enums[match].value, (uint32_t)e->value);
                }
                return LY_EVALID;
            }
        }

        COMPILE_ARRAY_GOTO(ctx, enums_p[u].iffeatures, e->iffeatures, options, v, lys_compile_iffeature, ret, done);
        COMPILE_ARRAY_GOTO(ctx, enums_p[u].exts, e->exts, options, v, lys_compile_ext, ret, done);

        if (basetype == LY_TYPE_BITS) {
            /* keep bits ordered by position */
            for (v = u; v && (*enums)[v - 1].value > e->value; --v);
            if (v != u) {
                memcpy(&storage, e, sizeof *e);
                memmove(&(*enums)[v + 1], &(*enums)[v], (u - v) * sizeof **enums);
                memcpy(&(*enums)[v], &storage, sizeof storage);
            }
        }
    }

done:
    return ret;
}

static LY_ERR
lys_compile_type_(struct lysc_ctx *ctx, struct lysp_type *type_p, LY_DATA_TYPE basetype, int options, int builtin, const char *tpdfname,
                  struct lysc_type *base,  struct lysc_type **type)
{
    LY_ERR ret = LY_SUCCESS;
    unsigned int u;
    struct lysc_type_bin *bin;
    struct lysc_type_num *num;
    struct lysc_type_str *str;
    struct lysc_type_bits *bits;
    struct lysc_type_enum *enumeration;
    struct lysc_type_dec *dec;

    switch (basetype) {
    case LY_TYPE_BINARY:
        /* RFC 7950 9.8.1, 9.4.4 - length, number of octets it contains */
        bin = (struct lysc_type_bin*)(*type);
        if (type_p->length) {
            ret = lys_compile_type_range(ctx, type_p->length, basetype, 1, 0,
                                         base ? ((struct lysc_type_bin*)base)->length : NULL, &bin->length);
            LY_CHECK_RET(ret);
            if (!tpdfname) {
                COMPILE_ARRAY_GOTO(ctx, type_p->length->exts, bin->length->exts,
                                   options, u, lys_compile_ext, ret, done);
            }
        }

        if (tpdfname) {
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_bin));
        }
        break;
    case LY_TYPE_BITS:
        /* RFC 7950 9.7 - bits */
        bits = (struct lysc_type_bits*)(*type);
        if (type_p->bits) {
            ret = lys_compile_type_enums(ctx, type_p->bits, basetype, options,
                                         base ? (struct lysc_type_enum_item*)((struct lysc_type_bits*)base)->bits : NULL,
                                         (struct lysc_type_enum_item**)&bits->bits);
            LY_CHECK_RET(ret);
        }

        if (builtin && !type_p->flags) {
            /* type derived from bits built-in type must contain at least one bit */
            if (tpdfname) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Missing bit substatement for bits type \"%s\".",
                       tpdfname);
            } else {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Missing bit substatement for bits type.");
                free(*type);
                *type = NULL;
            }
            return LY_EVALID;
        }

        if (tpdfname) {
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_bits));
        }
        break;
    case LY_TYPE_DEC64:
        dec = (struct lysc_type_dec*)(*type);

        /* RFC 7950 9.3.4 - fraction-digits */
        if (builtin) {
            if (!type_p->fraction_digits) {
                if (tpdfname) {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Missing fraction-digits substatement for decimal64 type \"%s\".",
                           tpdfname);
                } else {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Missing fraction-digits substatement for decimal64 type.");
                    free(*type);
                    *type = NULL;
                }
                return LY_EVALID;
            }
        } else if (type_p->fraction_digits) {
            /* fraction digits is prohibited in types not directly derived from built-in decimal64 */
            if (tpdfname) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid fraction-digits substatement for type \"%s\" not directly derived from decimal64 built-in type.",
                       tpdfname);
            } else {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Invalid fraction-digits substatement for type not directly derived from decimal64 built-in type.");
                free(*type);
                *type = NULL;
            }
            return LY_EVALID;
        }
        dec->fraction_digits = type_p->fraction_digits;

        /* RFC 7950 9.2.4 - range */
        if (type_p->range) {
            ret = lys_compile_type_range(ctx, type_p->range, basetype, 0, dec->fraction_digits,
                                         base ? ((struct lysc_type_dec*)base)->range : NULL, &dec->range);
            LY_CHECK_RET(ret);
            if (!tpdfname) {
                COMPILE_ARRAY_GOTO(ctx, type_p->range->exts, dec->range->exts,
                                   options, u, lys_compile_ext, ret, done);
            }
        }

        if (tpdfname) {
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_dec));
        }
        break;
    case LY_TYPE_STRING:
        /* RFC 7950 9.4.4 - length */
        str = (struct lysc_type_str*)(*type);
        if (type_p->length) {
            ret = lys_compile_type_range(ctx, type_p->length, basetype, 1, 0,
                                         base ? ((struct lysc_type_str*)base)->length : NULL, &str->length);
            LY_CHECK_RET(ret);
            if (!tpdfname) {
                COMPILE_ARRAY_GOTO(ctx, type_p->length->exts, str->length->exts,
                                   options, u, lys_compile_ext, ret, done);
            }
        } else if (base && ((struct lysc_type_str*)base)->length) {
            str->length = lysc_range_dup(ctx->ctx, ((struct lysc_type_str*)base)->length);
        }

        /* RFC 7950 9.4.5 - pattern */
        if (type_p->patterns) {
            ret = lys_compile_type_patterns(ctx, type_p->patterns, options,
                                            base ? ((struct lysc_type_str*)base)->patterns : NULL, &str->patterns);
            LY_CHECK_RET(ret);
        } else if (base && ((struct lysc_type_str*)base)->patterns) {
            str->patterns = lysc_patterns_dup(ctx->ctx, ((struct lysc_type_str*)base)->patterns);
        }

        if (tpdfname) {
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_str));
        }
        break;
    case LY_TYPE_ENUM:
        /* RFC 7950 9.6 - enum */
        enumeration = (struct lysc_type_enum*)(*type);
        if (type_p->enums) {
            ret = lys_compile_type_enums(ctx, type_p->enums, basetype, options,
                                         base ? ((struct lysc_type_enum*)base)->enums : NULL, &enumeration->enums);
            LY_CHECK_RET(ret);
        }

        if (builtin && !type_p->flags) {
            /* type derived from enumerations built-in type must contain at least one enum */
            if (tpdfname) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                       "Missing enum substatement for enumeration type \"%s\".", tpdfname);
            } else {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Missing enum substatement for enumeration type.");
                free(*type);
                *type = NULL;
            }
            return LY_EVALID;
        }

        if (tpdfname) {
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_enum));
        }
        break;
    case LY_TYPE_INT8:
    case LY_TYPE_UINT8:
    case LY_TYPE_INT16:
    case LY_TYPE_UINT16:
    case LY_TYPE_INT32:
    case LY_TYPE_UINT32:
    case LY_TYPE_INT64:
    case LY_TYPE_UINT64:
        /* RFC 6020 9.2.4 - range */
        num = (struct lysc_type_num*)(*type);
        if (type_p->range) {
            ret = lys_compile_type_range(ctx, type_p->range, basetype, 0, 0,
                                         base ? ((struct lysc_type_num*)base)->range : NULL, &num->range);
            LY_CHECK_RET(ret);
            if (!tpdfname) {
                COMPILE_ARRAY_GOTO(ctx, type_p->range->exts, num->range->exts,
                                   options, u, lys_compile_ext, ret, done);
            }
        }

        if (tpdfname) {
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_num));
        }
        break;
    case LY_TYPE_INST:
        /* RFC 7950 9.9.3 - require-instance */
        if (type_p->flags & LYS_SET_REQINST) {
            ((struct lysc_type_instanceid*)(*type))->require_instance = type_p->require_instance;
        } else {
            /* default is true */
            ((struct lysc_type_instanceid*)(*type))->require_instance = 1;
        }

        if (tpdfname) {
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_instanceid));
        }
        break;
    case LY_TYPE_BOOL:
    case LY_TYPE_EMPTY:
    case LY_TYPE_UNKNOWN: /* just to complete switch */
        break;
    }
    LY_CHECK_ERR_RET(!(*type), LOGMEM(ctx->ctx), LY_EMEM);
done:
    return ret;
}

static LY_ERR
lys_compile_type(struct lysc_ctx *ctx, struct lysp_node_leaf *leaf_p, int options, struct lysc_type **type)
{
    LY_ERR ret = LY_SUCCESS;
    unsigned int u;
    struct lysp_type *type_p = &leaf_p->type;
    struct type_context {
        const struct lysp_tpdf *tpdf;
        struct lysp_node *node;
        struct lysp_module *mod;
    } *tctx, *tctx_prev = NULL;
    LY_DATA_TYPE basetype = LY_TYPE_UNKNOWN;
    struct lysc_type *base = NULL, *prev_type;
    struct ly_set tpdf_chain = {0};

    (*type) = NULL;

    tctx = calloc(1, sizeof *tctx);
    LY_CHECK_ERR_RET(!tctx, LOGMEM(ctx->ctx), LY_EMEM);
    for (ret = lysp_type_find(type_p->name, (struct lysp_node*)leaf_p, ctx->mod->parsed,
                             &basetype, &tctx->tpdf, &tctx->node, &tctx->mod);
            ret == LY_SUCCESS;
            ret = lysp_type_find(tctx_prev->tpdf->type.name, tctx_prev->node, tctx_prev->mod,
                                         &basetype, &tctx->tpdf, &tctx->node, &tctx->mod)) {
        if (basetype) {
            break;
        }

        /* check status */
        ret = lysc_check_status(ctx, leaf_p->flags, ctx->mod->parsed, leaf_p->name,
                                tctx->tpdf->flags, tctx->mod, tctx->node ? tctx->node->name : tctx->mod->name);
        LY_CHECK_ERR_GOTO(ret,  free(tctx), cleanup);

        if (tctx->tpdf->type.compiled) {
            /* it is not necessary to continue, the rest of the chain was already compiled */
            basetype = tctx->tpdf->type.compiled->basetype;
            ly_set_add(&tpdf_chain, tctx, LY_SET_OPT_USEASLIST);
            tctx = NULL;
            break;
        }

        /* store information for the following processing */
        ly_set_add(&tpdf_chain, tctx, LY_SET_OPT_USEASLIST);

        /* prepare next loop */
        tctx_prev = tctx;
        tctx = calloc(1, sizeof *tctx);
        LY_CHECK_ERR_RET(!tctx, LOGMEM(ctx->ctx), LY_EMEM);
    }
    free(tctx);

    /* allocate type according to the basetype */
    switch (basetype) {
    case LY_TYPE_BINARY:
        *type = calloc(1, sizeof(struct lysc_type_bin));
        break;
    case LY_TYPE_BITS:
        *type = calloc(1, sizeof(struct lysc_type_bits));
        break;
    case LY_TYPE_BOOL:
    case LY_TYPE_EMPTY:
        *type = calloc(1, sizeof(struct lysc_type));
        break;
    case LY_TYPE_DEC64:
        *type = calloc(1, sizeof(struct lysc_type_dec));
        break;
    case LY_TYPE_ENUM:
        *type = calloc(1, sizeof(struct lysc_type_enum));
        break;
    case LY_TYPE_IDENT:
        *type = calloc(1, sizeof(struct lysc_type_identityref));
        break;
    case LY_TYPE_INST:
        *type = calloc(1, sizeof(struct lysc_type_instanceid));
        break;
    case LY_TYPE_LEAFREF:
        *type = calloc(1, sizeof(struct lysc_type_leafref));
        break;
    case LY_TYPE_STRING:
        *type = calloc(1, sizeof(struct lysc_type_str));
        break;
    case LY_TYPE_UNION:
        *type = calloc(1, sizeof(struct lysc_type_union));
        break;
    case LY_TYPE_INT8:
    case LY_TYPE_UINT8:
    case LY_TYPE_INT16:
    case LY_TYPE_UINT16:
    case LY_TYPE_INT32:
    case LY_TYPE_UINT32:
    case LY_TYPE_INT64:
    case LY_TYPE_UINT64:
        *type = calloc(1, sizeof(struct lysc_type_num));
        break;
    case LY_TYPE_UNKNOWN:
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
               "Referenced type \"%s\" not found.", tctx_prev ? tctx_prev->tpdf->type.name : type_p->name);
        ret = LY_EVALID;
        goto cleanup;
    }
    LY_CHECK_ERR_GOTO(!(*type), LOGMEM(ctx->ctx), cleanup);
    if (~type_substmt_map[basetype] & leaf_p->type.flags) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Invalid type restrictions for %s type.",
               ly_data_type2str[basetype]);
        free(*type);
        (*type) = NULL;
        ret = LY_EVALID;
        goto cleanup;
    }

    /* get restrictions from the referred typedefs */
    for (u = tpdf_chain.count - 1; u + 1 > 0; --u) {
        tctx = (struct type_context*)tpdf_chain.objs[u];
        if (~type_substmt_map[basetype] & tctx->tpdf->type.flags) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Invalid type \"%s\" restriction(s) for %s type.",
                   tctx->tpdf->name, ly_data_type2str[basetype]);
            ret = LY_EVALID;
            goto cleanup;
        } else if (tctx->tpdf->type.compiled) {
            base = tctx->tpdf->type.compiled;
            continue;
        } else if ((u != tpdf_chain.count - 1) && !(tctx->tpdf->type.flags)) {
            /* no change, just use the type information from the base */
            base = ((struct lysp_tpdf*)tctx->tpdf)->type.compiled = ((struct type_context*)tpdf_chain.objs[u + 1])->tpdf->type.compiled;
            ++base->refcount;
            continue;
        }

        ++(*type)->refcount;
        (*type)->basetype = basetype;
        prev_type = *type;
        ret = lys_compile_type_(ctx, &((struct lysp_tpdf*)tctx->tpdf)->type, basetype, options, (u == tpdf_chain.count - 1) ? 1 : 0, tctx->tpdf->name, base, type);
        LY_CHECK_GOTO(ret, cleanup);
        base = prev_type;
    }

    /* process the type definition in leaf */
    if (leaf_p->type.flags || !base) {
        /* get restrictions from the node itself, finalize the type structure */
        (*type)->basetype = basetype;
        ++(*type)->refcount;
        ret = lys_compile_type_(ctx, &leaf_p->type, basetype, options, base ? 0 : 1, NULL, base, type);
        LY_CHECK_GOTO(ret, cleanup);
    } else {
        /* no specific restriction in leaf's type definition, copy from the base */
        free(*type);
        (*type) = base;
        ++(*type)->refcount;
    }

    COMPILE_ARRAY_GOTO(ctx, type_p->exts, (*type)->exts, options, u, lys_compile_ext, ret, cleanup);

cleanup:
    ly_set_erase(&tpdf_chain, free);
    return ret;
}

static LY_ERR lys_compile_node(struct lysc_ctx *ctx, struct lysp_node *node_p, int options, struct lysc_node *parent);

static LY_ERR
lys_compile_node_container(struct lysc_ctx *ctx, struct lysp_node *node_p, int options, struct lysc_node *node)
{
    struct lysp_node_container *cont_p = (struct lysp_node_container*)node_p;
    struct lysc_node_container *cont = (struct lysc_node_container*)node;
    struct lysp_node *child_p;
    unsigned int u;
    LY_ERR ret = LY_SUCCESS;

    COMPILE_MEMBER_GOTO(ctx, cont_p->when, cont->when, options, lys_compile_when, ret, done);
    COMPILE_ARRAY_GOTO(ctx, cont_p->iffeatures, cont->iffeatures, options, u, lys_compile_iffeature, ret, done);

    LY_LIST_FOR(cont_p->child, child_p) {
        LY_CHECK_RET(lys_compile_node(ctx, child_p, options, node));
    }

    COMPILE_ARRAY_GOTO(ctx, cont_p->musts, cont->musts, options, u, lys_compile_must, ret, done);
    //COMPILE_ARRAY_GOTO(ctx, cont_p->actions, cont->actions, options, u, lys_compile_action, ret, done);
    //COMPILE_ARRAY_GOTO(ctx, cont_p->notifs, cont->notifs, options, u, lys_compile_notif, ret, done);

done:
    return ret;
}

static LY_ERR
lys_compile_node_leaf(struct lysc_ctx *ctx, struct lysp_node *node_p, int options, struct lysc_node *node)
{
    struct lysp_node_leaf *leaf_p = (struct lysp_node_leaf*)node_p;
    struct lysc_node_leaf *leaf = (struct lysc_node_leaf*)node;
    unsigned int u;
    LY_ERR ret = LY_SUCCESS;

    COMPILE_MEMBER_GOTO(ctx, leaf_p->when, leaf->when, options, lys_compile_when, ret, done);
    COMPILE_ARRAY_GOTO(ctx, leaf_p->iffeatures, leaf->iffeatures, options, u, lys_compile_iffeature, ret, done);

    COMPILE_ARRAY_GOTO(ctx, leaf_p->musts, leaf->musts, options, u, lys_compile_must, ret, done);
    ret = lys_compile_type(ctx, leaf_p, options, &leaf->type);
    LY_CHECK_GOTO(ret, done);

    DUP_STRING(ctx->ctx, leaf_p->units, leaf->units);
    DUP_STRING(ctx->ctx, leaf_p->dflt, leaf->dflt);
done:
    return ret;
}

static LY_ERR
lys_compile_node(struct lysc_ctx *ctx, struct lysp_node *node_p, int options, struct lysc_node *parent)
{
    LY_ERR ret = LY_EVALID;
    struct lysc_node *node, **children;
    unsigned int u;
    LY_ERR (*node_compile_spec)(struct lysc_ctx*, struct lysp_node*, int, struct lysc_node*);

    switch (node_p->nodetype) {
    case LYS_CONTAINER:
        node = (struct lysc_node*)calloc(1, sizeof(struct lysc_node_container));
        node_compile_spec = lys_compile_node_container;
        break;
    case LYS_LEAF:
        node = (struct lysc_node*)calloc(1, sizeof(struct lysc_node_leaf));
        node_compile_spec = lys_compile_node_leaf;
        break;
    case LYS_LIST:
        node = (struct lysc_node*)calloc(1, sizeof(struct lysc_node_list));
        break;
    case LYS_LEAFLIST:
        node = (struct lysc_node*)calloc(1, sizeof(struct lysc_node_leaflist));
        break;
    case LYS_CASE:
        node = (struct lysc_node*)calloc(1, sizeof(struct lysc_node_case));
        break;
    case LYS_CHOICE:
        node = (struct lysc_node*)calloc(1, sizeof(struct lysc_node_choice));
        break;
    case LYS_USES:
        node = (struct lysc_node*)calloc(1, sizeof(struct lysc_node_uses));
        break;
    case LYS_ANYXML:
    case LYS_ANYDATA:
        node = (struct lysc_node*)calloc(1, sizeof(struct lysc_node_anydata));
        break;
    default:
        LOGINT(ctx->ctx);
        return LY_EINT;
    }
    LY_CHECK_ERR_RET(!node, LOGMEM(ctx->ctx), LY_EMEM);
    node->nodetype = node_p->nodetype;
    node->module = ctx->mod;
    node->prev = node;
    node->flags = node_p->flags;

    /* config */
    if (!(node->flags & LYS_CONFIG_MASK)) {
        /* config not explicitely set, inherit it from parent */
        if (parent) {
            node->flags |= parent->flags & LYS_CONFIG_MASK;
        } else {
            /* default is config true */
            node->flags |= LYS_CONFIG_W;
        }
    }

    /* status - it is not inherited by specification, but it does not make sense to have
     * current in deprecated or deprecated in obsolete, so we do print warning and inherit status */
    if (!(node->flags & LYS_STATUS_MASK)) {
        if (parent && (parent->flags & (LYS_STATUS_DEPRC | LYS_STATUS_OBSLT))) {
            LOGWRN(ctx->ctx, "Missing explicit \"%s\" status that was already specified in parent, inheriting.",
                   (parent->flags & LYS_STATUS_DEPRC) ? "deprecated" : "obsolete");
            node->flags |= parent->flags & LYS_STATUS_MASK;
        } else {
            node->flags |= LYS_STATUS_CURR;
        }
    } else if (parent) {
        /* check status compatibility with the parent */
        if ((parent->flags & LYS_STATUS_MASK) > (node->flags & LYS_STATUS_MASK)) {
            if (node->flags & LYS_STATUS_CURR) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                       "A \"current\" status is in conflict with the parent's \"%s\" status.",
                       (parent->flags & LYS_STATUS_DEPRC) ? "deprecated" : "obsolete");
            } else { /* LYS_STATUS_DEPRC */
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                       "A \"deprecated\" status is in conflict with the parent's \"obsolete\" status.");
            }
            goto error;
        }
    }

    if (!(options & LYSC_OPT_FREE_SP)) {
        node->sp = node_p;
    }
    DUP_STRING(ctx->ctx, node_p->name, node->name);
    COMPILE_ARRAY_GOTO(ctx, node_p->exts, node->exts, options, u, lys_compile_ext, ret, error);

    /* nodetype-specific part */
    LY_CHECK_GOTO(node_compile_spec(ctx, node_p, options, node), error);

    /* insert into parent's children */
    if (parent && (children = lysc_node_children(parent))) {
        if (!(*children)) {
            /* first child */
            *children = node;
        } else {
            /* insert at the end of the parent's children list */
            (*children)->prev->next = node;
            node->prev = (*children)->prev;
            (*children)->prev = node;
        }
    } else {
        /* top-level element */
        if (!ctx->mod->compiled->data) {
            ctx->mod->compiled->data = node;
        } else {
            /* insert at the end of the module's top-level nodes list */
            ctx->mod->compiled->data->prev->next = node;
            node->prev = ctx->mod->compiled->data->prev;
            ctx->mod->compiled->data->prev = node;
        }
    }

    return LY_SUCCESS;

error:
    lysc_node_free(ctx->ctx, node);
    return ret;
}

LY_ERR
lys_compile(struct lys_module *mod, int options)
{
    struct lysc_ctx ctx = {0};
    struct lysc_module *mod_c;
    struct lysp_module *sp;
    struct lysp_node *node_p;
    unsigned int u;
    LY_ERR ret;

    LY_CHECK_ARG_RET(NULL, mod, mod->parsed, mod->parsed->ctx, LY_EINVAL);
    sp = mod->parsed;

    if (sp->submodule) {
        LOGERR(sp->ctx, LY_EINVAL, "Submodules (%s) are not supposed to be compiled, compile only the main modules.", sp->name);
        return LY_EINVAL;
    }

    ctx.ctx = sp->ctx;
    ctx.mod = mod;

    mod->compiled = mod_c = calloc(1, sizeof *mod_c);
    LY_CHECK_ERR_RET(!mod_c, LOGMEM(sp->ctx), LY_EMEM);
    mod_c->ctx = sp->ctx;
    mod_c->implemented = sp->implemented;
    mod_c->latest_revision = sp->latest_revision;
    mod_c->version = sp->version;

    DUP_STRING(sp->ctx, sp->name, mod_c->name);
    DUP_STRING(sp->ctx, sp->ns, mod_c->ns);
    DUP_STRING(sp->ctx, sp->prefix, mod_c->prefix);
    if (sp->revs) {
        DUP_STRING(sp->ctx, sp->revs[0].date, mod_c->revision);
    }
    COMPILE_ARRAY_GOTO(&ctx, sp->imports, mod_c->imports, options, u, lys_compile_import, ret, error);
    COMPILE_ARRAY_GOTO(&ctx, sp->features, mod_c->features, options, u, lys_compile_feature, ret, error);
    COMPILE_ARRAY_GOTO(&ctx, sp->identities, mod_c->identities, options, u, lys_compile_identity, ret, error);
    if (sp->identities) {
        LY_CHECK_RET(lys_compile_identities_derived(&ctx, sp->identities, mod_c->identities));
    }

    LY_LIST_FOR(sp->data, node_p) {
        ret = lys_compile_node(&ctx, node_p, options, NULL);
        LY_CHECK_GOTO(ret, error);
    }
    //COMPILE_ARRAY_GOTO(ctx, sp->rpcs, mod_c->rpcs, options, u, lys_compile_action, ret, error);
    //COMPILE_ARRAY_GOTO(ctx, sp->notifs, mod_c->notifs, options, u, lys_compile_notif, ret, error);

    COMPILE_ARRAY_GOTO(&ctx, sp->exts, mod_c->exts, options, u, lys_compile_ext, ret, error);

    if (options & LYSC_OPT_FREE_SP) {
        lysp_module_free(mod->parsed);
        ((struct lys_module*)mod)->parsed = NULL;
    }

    ((struct lys_module*)mod)->compiled = mod_c;
    return LY_SUCCESS;

error:
    lysc_module_free(mod_c, NULL);
    ((struct lys_module*)mod)->compiled = NULL;
    return ret;
}
