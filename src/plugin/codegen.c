//
// Cxyml plugin - Code Generator
//
// Transforms a CxymlNode tree into a linked list of CXY AstNode statements
// that write HTML to an OutputStream.
//
// Generation strategy
// -------------------
// Built-in elements  (tag starts with lowercase):
//   Static fragments (tag names, attribute scaffolding, text, closing tags)
//   are accumulated in a fold buffer.  A single  os << "concatenated"  node
//   is emitted only when the run of static content is broken by something
//   dynamic — an interpolation, a component, a for loop, or an if block.
//
// Custom components  (tag starts with uppercase):
//   Flush the fold buffer, then instantiate and call .render(os):
//     {
//         var _cxymlN = MyComponent(props...)
//         _cxymlN.render(os)
//     }
//
// Text nodes:
//   appendStatic() — folded into the surrounding static run.
//
// Interpolation nodes:
//   Flush, then:  cxymlHtmlEscape(expr)   (writes escaped value to os)
//
// For directive  {{ for var in expr }} … {{ /for }}:
//   Flush, then emit a ForStmt whose body is generated into a fresh list.
//
// If directive  {{ if cond }} … {{ else if cond }} … {{ else }} … {{ /if }}:
//   Flush, then emit an IfStmt chain built recursively via buildIfNode().
//
// Fold buffer
// -----------
// CodegenCtx.segments is a DynArray of  const char *  (interned strings).
// appendStatic()   — push one segment onto the array.
// flushStatic()    — join all segments into one interned string, emit
//                    os << "joined", clear the array.
// Any dynamic node calls flushStatic() before inserting its own AST node,
// so the invariant "buffer is empty after any dynamic emit" is maintained.
//

#include "state.h"

#include <cxy/core/array.h>
#include <cxy/core/log.h>
#include <cxy/core/mempool.h>
#include <cxy/core/strpool.h>
#include <cxy/ast.h>
#include <cxy/flag.h>
#include <cxy/operator.h>
#include <cxy/strings.h>
#include <cxy/plugin.h>

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Context passed through every codegen function
// ============================================================================

typedef struct CodegenCtx {
    CxyPluginContext *ctx;       // plugin context (pool, strings, log)
    AstNode          *osRef;     // resolved reference to the `os` parameter
    u32               varCounter; // monotonically increasing suffix for _cxymlN

    // Static-string fold buffer.
    // Segments are interned const char * values accumulated between dynamic
    // nodes.  flushStatic() joins them into one os << write and clears.
    DynArray          segments;  // DynArray<const char *>
    FileLoc           segLoc;    // source location of the first buffered segment
} CodegenCtx;

// ============================================================================
// Fold buffer helpers
// ============================================================================

// Push one static string fragment onto the fold buffer.
// Records the source location of the first fragment so the eventual
// os << node has a useful location for diagnostics.
static void appendStatic(CodegenCtx    *cg,
                          const FileLoc *loc,
                          const char    *str)
{
    if (dynArrayEmpty(&cg->segments))
        cg->segLoc = *loc;
    pushStringOnDynArray(&cg->segments, str);
}

// Concatenate all buffered segments into one interned string, emit a single
// os << "..." statement into stmts, and clear the buffer.
// Does nothing when the buffer is empty.
static void flushStatic(CodegenCtx  *cg,
                         AstNodeList *stmts)
{
    if (dynArrayEmpty(&cg->segments))
        return;

    const char *str;

    if (cg->segments.size == 1) {
        // Single segment — use directly without extra allocation.
        str = dynArrayAt(const char **, &cg->segments, 0);
    }
    else {
        // Multiple segments — concatenate into a temporary heap buffer,
        // intern the result, then free the temp buffer.
        size_t total = 0;
        for (size_t i = 0; i < cg->segments.size; i++)
            total += strlen(dynArrayAt(const char **, &cg->segments, i));

        char *buf = malloc(total + 1);
        char *p   = buf;
        for (size_t i = 0; i < cg->segments.size; i++) {
            const char *seg = dynArrayAt(const char **, &cg->segments, i);
            size_t len = strlen(seg);
            memcpy(p, seg, len);
            p += len;
        }
        *p = '\0';

        str = makeStringSized(cg->ctx->strings, buf, (u32)total);
        free(buf);
    }

    // Emit  os << "str"
    AstNode *lit  = makeStringLiteral(cg->ctx->pool, &cg->segLoc, str, NULL, NULL);
    AstNode *write = makeExprStmt(
        cg->ctx->pool, &cg->segLoc, flgNone,
        makeBinaryExpr(cg->ctx->pool, &cg->segLoc, flgNone,
                       deepCloneAstNode(cg->ctx->pool, cg->osRef),
                       opShl, lit, NULL, NULL),
        NULL, NULL);
    insertAstNode(stmts, write);

    clearDynArray(&cg->segments);
}

// ============================================================================
// Internal helpers  (unchanged from original)
// ============================================================================

static cstring nextVarName(CodegenCtx *cg)
{
    return makeStringf(cg->ctx->strings, "_cxyml%u", cg->varCounter++);
}

// Build:   os << <rhs>
static AstNode *makeStreamWrite(CodegenCtx    *cg,
                                const FileLoc *loc,
                                AstNode       *rhs)
{
    return makeExprStmt(
        cg->ctx->pool, loc, flgNone,
        makeBinaryExpr(cg->ctx->pool, loc, flgNone,
                       deepCloneAstNode(cg->ctx->pool, cg->osRef),
                       opShl, rhs, NULL, NULL),
        NULL, NULL);
}

// Build:   cxymlHtmlEscape(os, expr)
static AstNode *makeEscapedWrite(CodegenCtx    *cg,
                                 const FileLoc *loc,
                                 AstNode       *expr)
{
    AstNode *escapeFn = makeIdentifier(cg->ctx->pool, loc,
                                       makeString(cg->ctx->strings,
                                                  "cxymlHtmlEscape"),
                                       0, NULL, NULL);
    // Pass the output stream as the first argument, expression as second.
    // Macro arguments are a singly-linked list via ->next.
    AstNode *osArg = deepCloneAstNode(cg->ctx->pool, cg->osRef);
    osArg->next = expr;
    AstNode *call = makeMacroCallAstNode(cg->ctx->pool, loc, flgNone,
                                         escapeFn, osArg, NULL);
    return makeExprStmt(cg->ctx->pool, loc, flgNone, call, NULL, NULL);
}

// ============================================================================
// HTML void elements
// ============================================================================

static bool isVoidElement(const char *tag)
{
    static const char *voidTags[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
    };
    for (u32 i = 0; i < sizeof(voidTags) / sizeof(voidTags[0]); i++) {
        if (strcmp(tag, voidTags[i]) == 0)
            return true;
    }
    return false;
}

// ============================================================================
// Forward declaration  (elements and if/for recurse into children)
// ============================================================================

static bool generateNode(CodegenCtx *cg, const CxymlNode *node, AstNodeList *stmts);

// ============================================================================
// Text node  —  pure static, just append to fold buffer
// ============================================================================

static bool generateText(CodegenCtx      *cg,
                          const CxymlNode *node,
                          AstNodeList     *stmts)
{
    (void)stmts; // text never flushes on its own
    appendStatic(cg, &node->loc, node->text.value);
    return true;
}

// ============================================================================
// Interpolation node  —  dynamic boundary: flush then emit escaped write
// ============================================================================

static bool generateInterp(CodegenCtx      *cg,
                            const CxymlNode *node,
                            AstNodeList     *stmts)
{
    AstNode *expr = cxyParseExpression(cg->ctx,
                                       node->interp.expr,
                                       strlen(node->interp.expr),
                                       &node->interp.exprLoc);
    if (expr == NULL)
        return false;

    flushStatic(cg, stmts);
    insertAstNode(stmts, makeEscapedWrite(cg, &node->loc, expr));
    return true;
}

// ============================================================================
// Open-tag  —  appends static parts to fold buffer;
//              dynamic attribute values cause a localised flush
// ============================================================================

static bool generateOpenTag(CodegenCtx      *cg,
                             const CxymlNode *node,
                             AstNodeList     *stmts)
{
    const char *openStr = makeStringf(cg->ctx->strings,
                                      "<%s", node->element.tag);
    appendStatic(cg, &node->loc, openStr);

    for (const CxymlAttr *attr = node->element.attrs;
         attr;
         attr = attr->next)
    {
        if (attr->interpExpr != NULL) {
            // Dynamic attribute value:
            //   fold " name=\"", flush, emit escaped expr, fold "\""
            const char *prefix = makeStringf(cg->ctx->strings,
                                             " %s=\"", attr->name);
            appendStatic(cg, &attr->nameLoc, prefix);
            flushStatic(cg, stmts);

            AstNode *expr = cxyParseExpression(cg->ctx,
                                               attr->interpExpr,
                                               strlen(attr->interpExpr),
                                               &attr->valueLoc);
            if (expr == NULL)
                return false;

            insertAstNode(stmts, makeEscapedWrite(cg, &attr->valueLoc, expr));
            appendStatic(cg, &attr->valueLoc, "\"");
        }
        else if (attr->value != NULL) {
            // Static attribute:  fold  " name=\"value\""
            const char *attrStr = makeStringf(cg->ctx->strings,
                                              " %s=\"%s\"",
                                              attr->name, attr->value);
            appendStatic(cg, &attr->nameLoc, attrStr);
        }
        else {
            // Boolean flag attribute:  fold  " name"
            const char *flagStr = makeStringf(cg->ctx->strings,
                                              " %s", attr->name);
            appendStatic(cg, &attr->nameLoc, flagStr);
        }
    }

    return true;
}

// ============================================================================
// Built-in element  (lowercase tag)
// All static fragments fold into surrounding runs; children are recursed.
// ============================================================================

static bool generateBuiltin(CodegenCtx      *cg,
                             const CxymlNode *node,
                             AstNodeList     *stmts)
{
    if (!generateOpenTag(cg, node, stmts))
        return false;

    if (node->element.selfClosing || isVoidElement(node->element.tag)) {
        appendStatic(cg, &node->loc, " />");
        return true;
    }

    appendStatic(cg, &node->loc, ">");

    for (const CxymlNode *child = node->element.children;
         child;
         child = (const CxymlNode *)child->next)
    {
        if (!generateNode(cg, child, stmts))
            return false;
    }

    const char *closeStr = makeStringf(cg->ctx->strings,
                                       "</%s>", node->element.tag);
    appendStatic(cg, &node->loc, closeStr);
    return true;
}

// ============================================================================
// Custom component  (uppercase tag)  —  dynamic boundary
//
// Generates:
//   {
//       var _cxymlN = TagName(attr_expr_or_string, ...)
//       _cxymlN.render(os)
//   }
// ============================================================================

static bool generateComponent(CodegenCtx      *cg,
                               const CxymlNode *node,
                               AstNodeList     *stmts)
{
    MemPool       *pool    = cg->ctx->pool;
    StrPool       *strings = cg->ctx->strings;
    const FileLoc *loc     = &node->loc;

    // 1. Build constructor argument list from attributes
    AstNodeList ctorArgs = {};

    for (const CxymlAttr *attr = node->element.attrs; attr; attr = attr->next) {
        AstNode *argExpr = NULL;

        if (attr->interpExpr != NULL) {
            argExpr = cxyParseExpression(cg->ctx,
                                         attr->interpExpr,
                                         strlen(attr->interpExpr),
                                         &attr->valueLoc);
            if (argExpr == NULL)
                return false;
        }
        else if (attr->value != NULL) {
            argExpr = makeStringLiteral(pool, &attr->valueLoc,
                                        attr->value, NULL, NULL);
        }
        // Boolean flag attributes have no value — skip for constructor.

        if (argExpr != NULL)
            insertAstNode(&ctorArgs, argExpr);
    }

    // 2.  var _cxymlN = TagName(args...)
    cstring  varName  = nextVarName(cg);
    AstNode *typePath = makePath(pool, loc,
                                 makeString(strings, node->element.tag),
                                 flgNone, NULL);
    AstNode *ctorCall = makeCallExpr(pool, loc, typePath,
                                     ctorArgs.first, flgNone, NULL, NULL);
    AstNode *varDecl  = makeVarDecl(pool, loc, flgNone, varName,
                                    NULL, ctorCall, NULL, NULL);

    // 3.  _cxymlN.render(os)
    AstNode *varRef      = makeIdentifier(pool, loc, varName, 0, NULL, NULL);
    AstNode *renderIdent = makeIdentifier(pool, loc,
                                          makeString(strings, "render"),
                                          0, NULL, NULL);
    AstNode *memberExpr  = makeMemberExpr(pool, loc, flgNone,
                                          varRef, renderIdent, NULL, NULL);
    AstNode *renderCall  = makeCallExpr(pool, loc, memberExpr,
                                        deepCloneAstNode(pool, cg->osRef),
                                        flgNone, NULL, NULL);
    AstNode *renderStmt  = makeExprStmt(pool, loc, flgNone,
                                        renderCall, NULL, NULL);

    varDecl->next = renderStmt;

    // 4. Component is a dynamic boundary — flush static, then insert block
    flushStatic(cg, stmts);
    insertAstNode(stmts, makeBlockStmt(pool, loc, varDecl, NULL, NULL));
    return true;
}

// ============================================================================
// If directive  —  dynamic boundary
//
// buildIfNode() is recursive (handles else-if / else chains) and returns
// an AstNode* so it can be passed as the `otherwise` argument to makeIfStmt.
// generateIf() is the public wrapper: flushes static then inserts the node.
//
// Branch kinds:
//   condition != NULL  →  if / else-if  →  makeIfStmt(cond, then, otherwise)
//   condition == NULL  →  bare else     →  makeBlockStmt(body)
// ============================================================================

static AstNode *buildIfNode(CodegenCtx *cg, const CxymlNode *node)
{
    MemPool       *pool = cg->ctx->pool;
    const FileLoc *loc  = &node->loc;

    // Generate the then-body into a fresh statement list.
    // The shared fold buffer is empty on entry (invariant maintained by
    // flushStatic calls at every dynamic boundary).
    AstNodeList bodyStmts = {};

    for (const CxymlNode *child = node->ifNode.thenBody;
         child;
         child = (const CxymlNode *)child->next)
    {
        if (!generateNode(cg, child, &bodyStmts))
            return NULL;
    }
    flushStatic(cg, &bodyStmts); // flush any trailing static inside the body

    AstNode *thenBlock = makeBlockStmt(pool, loc, bodyStmts.first, NULL, NULL);

    // Bare else branch: no condition — return the body block directly.
    // The parent makeIfStmt receives it as its `otherwise` argument.
    if (node->ifNode.condition == NULL)
        return thenBlock;

    // if / else-if branch: parse condition
    AstNode *condExpr = cxyParseExpression(cg->ctx,
                                           node->ifNode.condition,
                                           strlen(node->ifNode.condition),
                                           &node->ifNode.condLoc);
    if (condExpr == NULL)
        return NULL;

    // Recurse into the else chain
    AstNode *otherwise = NULL;
    if (node->ifNode.elseNode != NULL) {
        otherwise = buildIfNode(cg, node->ifNode.elseNode);
        if (otherwise == NULL)
            return NULL;
    }

    return makeIfStmt(pool, loc, flgNone, condExpr, thenBlock, otherwise, NULL);
}

static bool generateIf(CodegenCtx      *cg,
                        const CxymlNode *node,
                        AstNodeList     *stmts)
{
    flushStatic(cg, stmts); // if is a dynamic boundary

    AstNode *ifStmt = buildIfNode(cg, node);
    if (ifStmt == NULL)
        return false;

    insertAstNode(stmts, ifStmt);
    return true;
}

// ============================================================================
// For directive  —  dynamic boundary
//
// Generates:
//   for (var iterVar in rangeExpr) {
//       … body statements …
//   }
// ============================================================================

static bool generateFor(CodegenCtx      *cg,
                         const CxymlNode *node,
                         AstNodeList     *stmts)
{
    MemPool       *pool = cg->ctx->pool;
    const FileLoc *loc  = &node->loc;

    // Loop variable declaration (no initialiser — bound by the for-range)
    AstNode *varDecl = makeVarDecl(pool, loc, flgNone,
                                   node->forNode.iterVar,
                                   NULL, NULL, NULL, NULL);

    // Range expression
    AstNode *rangeExpr = cxyParseExpression(cg->ctx,
                                            node->forNode.iterExpr,
                                            strlen(node->forNode.iterExpr),
                                            &node->forNode.exprLoc);
    if (rangeExpr == NULL)
        return false;

    // Flush any outer static content accumulated before this for-loop FIRST.
    // The fold buffer is shared — if we build the body before flushing, outer
    // fragments (e.g. <table>, <thead>, <tbody>) would be appended to by the
    // body's own static content and then drained into the body block instead
    // of the outer statement list where they belong.
    flushStatic(cg, stmts);

    // Generate the loop body into a fresh statement list.
    // The fold buffer is now empty — body content starts a clean run.
    AstNodeList bodyStmts = {};

    for (const CxymlNode *child = node->forNode.body;
         child;
         child = (const CxymlNode *)child->next)
    {
        if (!generateNode(cg, child, &bodyStmts))
            return false;
    }
    flushStatic(cg, &bodyStmts); // flush trailing static inside body

    AstNode *bodyBlock = makeBlockStmt(pool, loc, bodyStmts.first, NULL, NULL);

    // Outer static was already flushed above — just insert the ForStmt.
    insertAstNode(stmts,
                  makeForStmt(pool, loc, flgNone,
                              varDecl, rangeExpr,
                              NULL,        // no filter condition
                              bodyBlock,
                              NULL));
    return true;
}

// ============================================================================
// Node dispatch
// ============================================================================

static bool generateNode(CodegenCtx      *cg,
                          const CxymlNode *node,
                          AstNodeList     *stmts)
{
    switch (cxymlNodeKind(node)) {
        case CXYML_NODE_TEXT:
            return generateText(cg, node, stmts);

        case CXYML_NODE_INTERP:
            return generateInterp(cg, node, stmts);

        case CXYML_NODE_ELEMENT:
            if (node->element.isComponent)
                return generateComponent(cg, node, stmts);
            else
                return generateBuiltin(cg, node, stmts);

        case CXYML_NODE_FOR:
            return generateFor(cg, node, stmts);

        case CXYML_NODE_IF:
            return generateIf(cg, node, stmts);
    }

    // unreachable
    assert(0 && "unknown CxymlNodeKind");
    return false;
}

// ============================================================================
// Public entry point
// ============================================================================

AstNode *cxymlGenerate(CxyPluginContext *ctx,
                       const CxymlNode *tree,
                       AstNode         *osRef)
{
    if (tree == NULL || ctx == NULL || osRef == NULL) {
        if (ctx != NULL && ctx->L != NULL) {
            logError(ctx->L, builtinLoc(),
                     "cxyml: cxymlGenerate called with NULL argument(s)",
                     NULL);
        }
        return NULL;
    }

    CodegenCtx cg = {
        .ctx        = ctx,
        .osRef      = osRef,
        .varCounter = 0,
        .segments   = newDynArray(sizeof(const char *)),
    };

    AstNodeList stmts = {};

    for (const CxymlNode *node = tree;
         node;
         node = (const CxymlNode *)node->next)
    {
        if (!generateNode(&cg, node, &stmts)) {
            freeDynArray(&cg.segments);
            return NULL;
        }
    }

    // Flush any trailing static content accumulated after the last dynamic node
    flushStatic(&cg, &stmts);
    freeDynArray(&cg.segments);

    return stmts.first;
}