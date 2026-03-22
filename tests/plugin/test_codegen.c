//
// Cxyml plugin - Code Generator unit tests
//
// Verifies that cxymlGenerate() produces the correct CXY AstNode tree.
//
// With the fold-buffer optimisation, every contiguous run of static content
// (tag names, attributes, text, closing tags) is collapsed into a SINGLE
// os << "..." statement.  Dynamic nodes (interpolations, components, for/if
// blocks) act as flush boundaries, splitting the static run at that point.
//
// Consequence for these tests
// ---------------------------
// Fully-static markup always produces exactly ONE stream-write statement
// containing the complete HTML string.
//
// Mixed markup produces one stream-write per contiguous static run, separated
// by the dynamic nodes:
//
//   <div><Comp /></div>
//     → os<<"<div>"   BlockStmt(Comp)   os<<"</div>"     (3 stmts)
//
// Interpolation ({{ expr }}) is intentionally NOT tested for the dynamic path.
// cxyParseExpression() requires the full CXY parser infrastructure which is
// only available when the plugin is loaded by CXY itself via
// cxyPluginInitialize(ctx, state, pipParser).  Calling it with only a bare
// pool/strings/log context crashes the process.  Interpolation correctness
// must be verified through integration tests against the real compiler.
//
// Build (add codegen.c to the compile command):
//   cc -D_DARWIN_C_SOURCE -I $CXY_ROOT/include -L $CXY_ROOT/lib -lcxy-plugin \
//      tests/plugin/main.c                                                    \
//      src/plugin/lexer.c src/plugin/parser.c src/plugin/codegen.c            \
//      -o run_tests
//

#include "test_harness.h"

#include "../../src/plugin/state.h"

#include <cxy/operator.h>
#include <string.h>

// ============================================================================
// Test state
// ============================================================================

static Log             *g_codegen_log = NULL;
static MemPool          g_cg_pool;
static StrPool          g_cg_strings;
static CxyPluginContext  g_ctx;
static AstNode         *g_osRef = NULL;

static FileLoc g_cg_origin = {
    .fileName = "test.cxy",
    .begin    = {.row = 1, .col = 1, .byteOffset = 0},
    .end      = {.row = 1, .col = 1, .byteOffset = 0},
};

static void setupCodegen(void)
{
    g_cg_pool    = newMemPool();
    g_cg_strings = newStrPool(&g_cg_pool);
    g_ctx.pool    = &g_cg_pool;
    g_ctx.strings = &g_cg_strings;
    g_ctx.L       = g_codegen_log;

    // Build a simple Identifier node for `os` (the OutputStream parameter).
    g_osRef = makeIdentifier(g_ctx.pool, &g_cg_origin,
                             makeString(g_ctx.strings, "os"),
                             0, NULL, NULL);
    RESET_ERRORS();
}

static void teardownCodegen(void)
{
    freeStrPool(&g_cg_strings);
    freeMemPool(&g_cg_pool);
    g_osRef = NULL;
}

// ============================================================================
// Helpers
// ============================================================================

// Parse markup then run cxymlGenerate(); returns the head AstNode statement.
static AstNode *parseAndGenerate(const char *markup)
{
    CxymlLexer lexer;
    cxymlLexerInit(&lexer, markup, strlen(markup), &g_cg_origin, g_ctx.L);
    CxymlNode *tree = cxymlParse(&lexer, g_ctx.pool, g_ctx.strings, g_ctx.L);
    if (tree == NULL)
        return NULL;
    return cxymlGenerate(&g_ctx, tree, g_osRef);
}

// Count siblings in a linked AstNode list.
static int countStmts(const AstNode *n)
{
    int c = 0;
    for (; n != NULL; n = n->next)
        c++;
    return c;
}

// Return the n-th node (0-based) in a linked list, or NULL.
static const AstNode *nthStmt(const AstNode *n, int idx)
{
    for (int i = 0; n != NULL && i < idx; i++, n = n->next)
        ;
    return n;
}

// True when the node is   ExprStmt( BinaryExpr( os, <<, rhs ) )
static bool isStreamWrite(const AstNode *n)
{
    if (!nodeIs(n, ExprStmt))
        return false;
    const AstNode *expr = n->exprStmt.expr;
    return nodeIs(expr, BinaryExpr) && expr->binaryExpr.op == opShl;
}

// Return the string literal value from a stream write  os << "text".
// Returns NULL when the node is not a stream write or the rhs is not a StringLit.
static const char *streamWriteStr(const AstNode *n)
{
    if (!isStreamWrite(n))
        return NULL;
    const AstNode *rhs = n->exprStmt.expr->binaryExpr.rhs;
    if (!nodeIs(rhs, StringLit))
        return NULL;
    return rhs->stringLiteral.value;
}

// Return the stmts head inside a BlockStmt, or NULL.
static const AstNode *blockBody(const AstNode *n)
{
    if (!nodeIs(n, BlockStmt))
        return NULL;
    return n->blockStmt.stmts;
}

// ============================================================================
// Codegen - null / empty inputs
// ============================================================================

static void test_codegen_null_tree(void)
{
    setupCodegen();
    AstNode *result = cxymlGenerate(&g_ctx, NULL, g_osRef);
    ASSERT_NULL(result);
    ASSERT_HAS_ERRORS();
    teardownCodegen();
}

static void test_codegen_null_ctx(void)
{
    setupCodegen();
    CxymlLexer lexer;
    cxymlLexerInit(&lexer, "<br />", 6, &g_cg_origin, g_ctx.L);
    CxymlNode *tree = cxymlParse(&lexer, g_ctx.pool, g_ctx.strings, g_ctx.L);
    ASSERT_NOT_NULL(tree);

    // ctx is NULL so logError can't be called — no errors expected
    AstNode *result = cxymlGenerate(NULL, tree, g_osRef);
    ASSERT_NULL(result);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_null_osref(void)
{
    setupCodegen();
    CxymlLexer lexer;
    cxymlLexerInit(&lexer, "<br />", 6, &g_cg_origin, g_ctx.L);
    CxymlNode *tree = cxymlParse(&lexer, g_ctx.pool, g_ctx.strings, g_ctx.L);
    ASSERT_NOT_NULL(tree);

    AstNode *result = cxymlGenerate(&g_ctx, tree, NULL);
    ASSERT_NULL(result);
    ASSERT_HAS_ERRORS();
    teardownCodegen();
}

// ============================================================================
// Codegen - self-closing elements
//
// All fragments (<tag, attrs, " />") are static — folded into ONE write.
// ============================================================================

static void test_codegen_self_closing_stmt_count(void)
{
    setupCodegen();
    // <br />  →  os << "<br />"  (single folded write)
    AstNode *stmts = parseAndGenerate("<br />");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_self_closing_full_str(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<br />");
    ASSERT_NOT_NULL(stmts);
    ASSERT_TRUE(isStreamWrite(stmts));
    ASSERT_STR_EQ(streamWriteStr(stmts), "<br />");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_self_closing_is_stream_write(void)
{
    setupCodegen();
    // Confirm the single statement is indeed a stream write
    AstNode *stmts = parseAndGenerate("<br />");
    ASSERT_NOT_NULL(stmts);
    ASSERT_TRUE(isStreamWrite(stmts));
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_self_closing_tag_name_in_str(void)
{
    setupCodegen();
    // Different tag name — confirm tag string appears in the folded write
    AstNode *stmts = parseAndGenerate("<hr />");
    ASSERT_NOT_NULL(stmts);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<hr />");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

// ============================================================================
// Codegen - open / close elements
//
// <tag>, children (none here), </tag> all fold into ONE write.
// ============================================================================

static void test_codegen_open_close_stmt_count(void)
{
    setupCodegen();
    // <div></div>  →  os << "<div></div>"  (1 folded write)
    AstNode *stmts = parseAndGenerate("<div></div>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_open_close_full_str(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<div></div>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<div></div>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_all_open_close_stmts_are_stream_writes(void)
{
    setupCodegen();
    // Every statement for a plain element (no children) must be a stream write
    AstNode *stmts = parseAndGenerate("<p></p>");
    ASSERT_NOT_NULL(stmts);
    for (const AstNode *n = stmts; n != NULL; n = n->next)
        ASSERT_TRUE(isStreamWrite(n));
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

// ============================================================================
// Codegen - attributes
//
// Static attributes fold with the tag into one write.
// Dynamic (interpolated) attributes break the fold; tested via integration.
// ============================================================================

static void test_codegen_static_attr_stmt_count(void)
{
    setupCodegen();
    // <div class="btn"></div>  →  1 folded write
    AstNode *stmts = parseAndGenerate("<div class=\"btn\"></div>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_static_attr_full_str(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<div class=\"btn\"></div>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<div class=\"btn\"></div>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_boolean_attr_stmt_count(void)
{
    setupCodegen();
    // <input disabled />  →  1 folded write
    AstNode *stmts = parseAndGenerate("<input disabled />");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_boolean_attr_full_str(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<input disabled />");
    ASSERT_NOT_NULL(stmts);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<input disabled />");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_multiple_attrs_stmt_count(void)
{
    setupCodegen();
    // <a href="/" class="nav"></a>  →  1 folded write
    AstNode *stmts = parseAndGenerate("<a href=\"/\" class=\"nav\"></a>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_multiple_attrs_full_str(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<a href=\"/\" class=\"nav\"></a>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<a href=\"/\" class=\"nav\"></a>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

// ============================================================================
// Codegen - text children
//
// Text folds with surrounding tag fragments into one write.
// ============================================================================

static void test_codegen_text_child_stmt_count(void)
{
    setupCodegen();
    // <p>Hello</p>  →  os << "<p>Hello</p>"  (1 folded write)
    AstNode *stmts = parseAndGenerate("<p>Hello</p>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_text_child_full_str(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<p>Hello</p>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<p>Hello</p>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_text_whitespace_collapsed(void)
{
    setupCodegen();
    // Parser normalises whitespace before codegen sees the text node
    AstNode *stmts = parseAndGenerate("<p>  Hello   World  </p>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<p>Hello World</p>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_text_with_attr(void)
{
    setupCodegen();
    // <span class="x">Hi</span>  →  1 folded write
    AstNode *stmts = parseAndGenerate("<span class=\"x\">Hi</span>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<span class=\"x\">Hi</span>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

// ============================================================================
// Codegen - nested elements
//
// Fully-static nesting folds the entire tree into one write.
// ============================================================================

static void test_codegen_nested_stmt_count(void)
{
    setupCodegen();
    // <div><span></span></div>  →  1 folded write
    AstNode *stmts = parseAndGenerate("<div><span></span></div>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_nested_full_str(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<div><span></span></div>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<div><span></span></div>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_nested_outer_tag_in_str(void)
{
    setupCodegen();
    // Confirm the outer closing tag is present in the single folded string
    AstNode *stmts = parseAndGenerate("<div><span></span></div>");
    ASSERT_NOT_NULL(stmts);
    const char *s = streamWriteStr(stmts);
    ASSERT_NOT_NULL(s);
    ASSERT_TRUE(strstr(s, "</div>") != NULL);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_two_children_stmt_count(void)
{
    setupCodegen();
    // <ul><li>A</li><li>B</li></ul>  →  1 folded write
    AstNode *stmts = parseAndGenerate("<ul><li>A</li><li>B</li></ul>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_two_children_full_str(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<ul><li>A</li><li>B</li></ul>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<ul><li>A</li><li>B</li></ul>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

// ============================================================================
// Codegen - custom components
//
// A component is a dynamic boundary: static runs before and after are each
// flushed as their own write, and the component itself is a BlockStmt.
// ============================================================================

static void test_codegen_component_is_block_stmt(void)
{
    setupCodegen();
    // <MyComp /> generates a single BlockStmt
    AstNode *stmts = parseAndGenerate("<MyComp />");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_TRUE(nodeIs(stmts, BlockStmt));
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_inner_stmt_count(void)
{
    setupCodegen();
    // Block body: var _cxyml0 = MyComp()   +   _cxyml0.render(os)  = 2
    AstNode *stmts = parseAndGenerate("<MyComp />");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(blockBody(stmts)), 2);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_var_is_vardecl(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<MyComp />");
    ASSERT_NOT_NULL(stmts);
    const AstNode *inner = blockBody(stmts);
    ASSERT_NOT_NULL(inner);
    ASSERT_TRUE(nodeIs(inner, VarDecl));
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_var_name(void)
{
    setupCodegen();
    // First component always receives the name _cxyml0
    AstNode *stmts = parseAndGenerate("<MyComp />");
    ASSERT_NOT_NULL(stmts);
    const AstNode *inner = blockBody(stmts);
    ASSERT_NOT_NULL(inner);
    ASSERT_TRUE(nodeIs(inner, VarDecl));
    ASSERT_STR_EQ(inner->varDecl.name, "_cxyml0");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_ctor_is_call(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<MyComp />");
    ASSERT_NOT_NULL(stmts);
    const AstNode *inner = blockBody(stmts);
    ASSERT_NOT_NULL(inner);
    const AstNode *init = inner->varDecl.init;
    ASSERT_NOT_NULL(init);
    ASSERT_TRUE(nodeIs(init, CallExpr));
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_ctor_path_name(void)
{
    setupCodegen();
    // The constructor callee is a Path whose first element is the tag name
    AstNode *stmts = parseAndGenerate("<MyComp />");
    ASSERT_NOT_NULL(stmts);
    const AstNode *init   = blockBody(stmts)->varDecl.init;
    const AstNode *callee = init->callExpr.callee;
    ASSERT_TRUE(nodeIs(callee, Path));
    const AstNode *elem = callee->path.elements;
    ASSERT_NOT_NULL(elem);
    ASSERT_STR_EQ(elem->pathElement.name, "MyComp");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_render_is_expr_stmt(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<MyComp />");
    ASSERT_NOT_NULL(stmts);
    const AstNode *renderStmt = blockBody(stmts)->next;
    ASSERT_NOT_NULL(renderStmt);
    ASSERT_TRUE(nodeIs(renderStmt, ExprStmt));
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_render_is_member_call(void)
{
    setupCodegen();
    AstNode *stmts     = parseAndGenerate("<MyComp />");
    ASSERT_NOT_NULL(stmts);
    const AstNode *call = blockBody(stmts)->next->exprStmt.expr;
    ASSERT_TRUE(nodeIs(call, CallExpr));

    // Callee is a MemberExpr  _cxyml0.render
    const AstNode *callee = call->callExpr.callee;
    ASSERT_TRUE(nodeIs(callee, MemberExpr));

    // Member identifier is "render"
    const AstNode *member = callee->memberExpr.member;
    ASSERT_TRUE(nodeIs(member, Identifier));
    ASSERT_STR_EQ(member->ident.value, "render");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_counter_increments(void)
{
    setupCodegen();
    // Two consecutive components: first _cxyml0, second _cxyml1
    AstNode *stmts = parseAndGenerate("<A /><B />");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 2);

    const AstNode *block0 = nthStmt(stmts, 0);
    const AstNode *block1 = nthStmt(stmts, 1);
    ASSERT_TRUE(nodeIs(block0, BlockStmt));
    ASSERT_TRUE(nodeIs(block1, BlockStmt));
    ASSERT_STR_EQ(blockBody(block0)->varDecl.name, "_cxyml0");
    ASSERT_STR_EQ(blockBody(block1)->varDecl.name, "_cxyml1");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_string_attr_becomes_ctor_arg(void)
{
    setupCodegen();
    // <Card title="Hello" />  ->  Card("Hello")
    AstNode *stmts = parseAndGenerate("<Card title=\"Hello\" />");
    ASSERT_NOT_NULL(stmts);
    const AstNode *init = blockBody(stmts)->varDecl.init;
    ASSERT_TRUE(nodeIs(init, CallExpr));
    const AstNode *arg = init->callExpr.args;
    ASSERT_NOT_NULL(arg);
    ASSERT_TRUE(nodeIs(arg, StringLit));
    ASSERT_STR_EQ(arg->stringLiteral.value, "Hello");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_bool_attr_skipped_as_ctor_arg(void)
{
    setupCodegen();
    // Boolean flag attributes produce no constructor argument
    AstNode *stmts = parseAndGenerate("<Card disabled />");
    ASSERT_NOT_NULL(stmts);
    const AstNode *init = blockBody(stmts)->varDecl.init;
    ASSERT_TRUE(nodeIs(init, CallExpr));
    ASSERT_NULL(init->callExpr.args);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_no_attrs_no_ctor_args(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<Bare />");
    ASSERT_NOT_NULL(stmts);
    const AstNode *init = blockBody(stmts)->varDecl.init;
    ASSERT_TRUE(nodeIs(init, CallExpr));
    ASSERT_NULL(init->callExpr.args);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

// ============================================================================
// Codegen - mixed builtin and component
//
// A component flushes the static buffer before it.  Static content after the
// component starts a new run, flushed at the end.
//
//   <div><Comp /></div>
//     stmt[0]  os << "<div>"        (static run before Comp)
//     stmt[1]  BlockStmt(Comp)      (component — dynamic boundary)
//     stmt[2]  os << "</div>"       (static run after Comp)
//     total = 3
// ============================================================================

static void test_codegen_builtin_wraps_component(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<div><Comp /></div>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 3);
    ASSERT_TRUE(isStreamWrite(nthStmt(stmts, 0)));
    ASSERT_STR_EQ(streamWriteStr(nthStmt(stmts, 0)), "<div>");
    ASSERT_TRUE(nodeIs(nthStmt(stmts, 1), BlockStmt));
    ASSERT_TRUE(isStreamWrite(nthStmt(stmts, 2)));
    ASSERT_STR_EQ(streamWriteStr(nthStmt(stmts, 2)), "</div>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_component_between_builtins(void)
{
    setupCodegen();
    // <p></p><Comp /><p></p>
    //   stmt[0]  os << "<p></p>"     (static run: both p tags fold together)
    //   stmt[1]  BlockStmt(Comp)
    //   stmt[2]  os << "<p></p>"
    //   total = 3
    AstNode *stmts = parseAndGenerate("<p></p><Comp /><p></p>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 3);
    ASSERT_STR_EQ(streamWriteStr(nthStmt(stmts, 0)), "<p></p>");
    ASSERT_TRUE(nodeIs(nthStmt(stmts, 1), BlockStmt));
    ASSERT_STR_EQ(streamWriteStr(nthStmt(stmts, 2)), "<p></p>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

// ============================================================================
// Codegen - void elements
//
// Void elements always emit " />" regardless of whether the source had />
// or an explicit close tag.  All fragments fold into one write.
// ============================================================================

static void test_codegen_void_br_without_slash(void)
{
    setupCodegen();
    // <br> (no explicit />) — void element still emits self-closing output
    AstNode *stmts = parseAndGenerate("<br></br>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<br />");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_void_br_with_slash(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<br />");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<br />");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_void_img_with_attrs(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<img src=\"pic.jpg\" alt=\"photo\"></img>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_STR_EQ(streamWriteStr(stmts),
                  "<img src=\"pic.jpg\" alt=\"photo\" />");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_void_input(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<input type=\"text\"></input>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<input type=\"text\" />");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_void_meta(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<meta charset=\"UTF-8\"></meta>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<meta charset=\"UTF-8\" />");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_void_hr(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<hr></hr>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<hr />");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_non_void_not_self_closed(void)
{
    setupCodegen();
    // <div></div> is NOT void — must emit open+close (not " />")
    AstNode *stmts = parseAndGenerate("<div></div>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<div></div>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

// ============================================================================
// Codegen - multiple roots
//
// Multiple root nodes with no dynamic content fold into a single write.
// ============================================================================

static void test_codegen_multiple_roots_count(void)
{
    setupCodegen();
    // <h1></h1><p></p>  →  1 folded write
    AstNode *stmts = parseAndGenerate("<h1></h1><p></p>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_EQ(countStmts(stmts), 1);
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

static void test_codegen_multiple_roots_full_str(void)
{
    setupCodegen();
    AstNode *stmts = parseAndGenerate("<h1></h1><p></p>");
    ASSERT_NOT_NULL(stmts);
    ASSERT_STR_EQ(streamWriteStr(stmts), "<h1></h1><p></p>");
    ASSERT_NO_ERRORS();
    teardownCodegen();
}

// ============================================================================
// Codegen - FOR / IF directives
//
// generateFor() and generateIf() (when condition != NULL) both call
// cxyParseExpression(), which requires the full CXY compiler infrastructure
// available only after cxyPluginInitialize().  Those paths are therefore
// covered by integration tests (build the plugin, compile a .cxy file that
// uses cxyml.render!(), inspect the generated CXY AST).
//
// EXCEPTION: the bare {{ else }} branch (condition == NULL) generates a plain
// BlockStmt wrapping the body writes WITHOUT calling cxyParseExpression.
// We can unit-test that path here by constructing CxymlNode trees directly.
// ============================================================================

// ---- Manual node builders (called from within a test, after setupCodegen) --

static CxymlNode *makeTestTextNode(const char *text)
{
    CxymlNode *n  = callocFromMemPool(g_ctx.pool, 1, sizeof(AstNode));
    n->tag        = CXYML_TAG_TEXT;
    n->loc        = g_cg_origin;
    n->text.value = makeString(g_ctx.strings, text);
    return n;
}

static CxymlNode *makeTestIfNode(const char      *condition,
                                  CxymlNode       *body,
                                  CxymlNode       *elseBranch)
{
    CxymlNode *n        = callocFromMemPool(g_ctx.pool, 1, sizeof(AstNode));
    n->tag               = CXYML_TAG_IF;
    n->loc               = g_cg_origin;
    n->ifNode.condition  = condition;   // NULL → bare else branch
    n->ifNode.condLoc    = g_cg_origin;
    n->ifNode.thenBody   = body;
    n->ifNode.elseNode   = elseBranch;
    return n;
}

// ---- Tests ------------------------------------------------------------------

// A bare else branch (condition == NULL) must produce a BlockStmt — no
// call to cxyParseExpression, so this is fully unit-testable.
static void test_codegen_else_branch_is_block(void)
{
    setupCodegen();

    CxymlNode *text = makeTestTextNode("fallback");
    CxymlNode *node = makeTestIfNode(NULL, text, NULL);

    AstNode *result = cxymlGenerate(&g_ctx, node, g_osRef);
    ASSERT_NOT_NULL(result);
    ASSERT_NO_ERRORS();
    ASSERT_TRUE(nodeIs(result, BlockStmt));

    teardownCodegen();
}

// The BlockStmt produced by the else branch must contain the body writes.
static void test_codegen_else_branch_body_write(void)
{
    setupCodegen();

    CxymlNode *text = makeTestTextNode("fallback");
    CxymlNode *node = makeTestIfNode(NULL, text, NULL);

    AstNode *result = cxymlGenerate(&g_ctx, node, g_osRef);
    ASSERT_NOT_NULL(result);
    ASSERT_NO_ERRORS();

    const AstNode *stmts = blockBody(result);
    ASSERT_NOT_NULL(stmts);
    ASSERT_TRUE(isStreamWrite(stmts));
    ASSERT_STR_EQ(streamWriteStr(stmts), "fallback");

    teardownCodegen();
}

// An else branch with no body produces a BlockStmt with a NULL stmts chain.
static void test_codegen_else_branch_empty_body(void)
{
    setupCodegen();

    CxymlNode *node = makeTestIfNode(NULL, NULL, NULL);

    AstNode *result = cxymlGenerate(&g_ctx, node, g_osRef);
    ASSERT_NOT_NULL(result);
    ASSERT_NO_ERRORS();
    ASSERT_TRUE(nodeIs(result, BlockStmt));
    ASSERT_NULL(blockBody(result));

    teardownCodegen();
}

void run_codegen_tests(Log *log)
{
    g_codegen_log = log;

    SUITE("Codegen - null / empty inputs");
    RUN_TEST(test_codegen_null_tree);
    RUN_TEST(test_codegen_null_ctx);
    RUN_TEST(test_codegen_null_osref);

    SUITE("Codegen - self-closing elements (folded to one write)");
    RUN_TEST(test_codegen_self_closing_stmt_count);
    RUN_TEST(test_codegen_self_closing_full_str);
    RUN_TEST(test_codegen_self_closing_is_stream_write);
    RUN_TEST(test_codegen_self_closing_tag_name_in_str);

    SUITE("Codegen - open/close elements (folded to one write)");
    RUN_TEST(test_codegen_open_close_stmt_count);
    RUN_TEST(test_codegen_open_close_full_str);
    RUN_TEST(test_codegen_all_open_close_stmts_are_stream_writes);

    SUITE("Codegen - attributes (folded to one write)");
    RUN_TEST(test_codegen_static_attr_stmt_count);
    RUN_TEST(test_codegen_static_attr_full_str);
    RUN_TEST(test_codegen_boolean_attr_stmt_count);
    RUN_TEST(test_codegen_boolean_attr_full_str);
    RUN_TEST(test_codegen_multiple_attrs_stmt_count);
    RUN_TEST(test_codegen_multiple_attrs_full_str);

    SUITE("Codegen - text children (folded to one write)");
    RUN_TEST(test_codegen_text_child_stmt_count);
    RUN_TEST(test_codegen_text_child_full_str);
    RUN_TEST(test_codegen_text_whitespace_collapsed);
    RUN_TEST(test_codegen_text_with_attr);

    SUITE("Codegen - nested elements (folded to one write)");
    RUN_TEST(test_codegen_nested_stmt_count);
    RUN_TEST(test_codegen_nested_full_str);
    RUN_TEST(test_codegen_nested_outer_tag_in_str);
    RUN_TEST(test_codegen_two_children_stmt_count);
    RUN_TEST(test_codegen_two_children_full_str);

    SUITE("Codegen - custom components");
    RUN_TEST(test_codegen_component_is_block_stmt);
    RUN_TEST(test_codegen_component_inner_stmt_count);
    RUN_TEST(test_codegen_component_var_is_vardecl);
    RUN_TEST(test_codegen_component_var_name);
    RUN_TEST(test_codegen_component_ctor_is_call);
    RUN_TEST(test_codegen_component_ctor_path_name);
    RUN_TEST(test_codegen_component_render_is_expr_stmt);
    RUN_TEST(test_codegen_component_render_is_member_call);
    RUN_TEST(test_codegen_component_counter_increments);
    RUN_TEST(test_codegen_component_string_attr_becomes_ctor_arg);
    RUN_TEST(test_codegen_component_bool_attr_skipped_as_ctor_arg);
    RUN_TEST(test_codegen_component_no_attrs_no_ctor_args);

    SUITE("Codegen - mixed builtin and component (fold boundary)");
    RUN_TEST(test_codegen_builtin_wraps_component);
    RUN_TEST(test_codegen_component_between_builtins);

    SUITE("Codegen - void elements (folded to one write)");
    RUN_TEST(test_codegen_void_br_without_slash);
    RUN_TEST(test_codegen_void_br_with_slash);
    RUN_TEST(test_codegen_void_img_with_attrs);
    RUN_TEST(test_codegen_void_input);
    RUN_TEST(test_codegen_void_meta);
    RUN_TEST(test_codegen_void_hr);
    RUN_TEST(test_codegen_non_void_not_self_closed);

    SUITE("Codegen - multiple roots (folded to one write)");
    RUN_TEST(test_codegen_multiple_roots_count);
    RUN_TEST(test_codegen_multiple_roots_full_str);

    SUITE("Codegen - for/if directives (else branch, no cxyParseExpression)");
    RUN_TEST(test_codegen_else_branch_is_block);
    RUN_TEST(test_codegen_else_branch_body_write);
    RUN_TEST(test_codegen_else_branch_empty_body);
}