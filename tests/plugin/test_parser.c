//
// Cxyml plugin - Parser unit tests
//

#include "test_harness.h"

#include "../../src/plugin/state.h"

#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

static Log    *g_parser_log = NULL;
static MemPool g_pool;
static StrPool g_strings;

// Called before each test suite to get a fresh pool + strpool
static void setupParser(void)
{
    g_pool    = newMemPool();
    g_strings = newStrPool(&g_pool);
    RESET_ERRORS();
}

static void teardownParser(void)
{
    freeStrPool(&g_strings);
    freeMemPool(&g_pool);
}

static CxymlNode *parse(const char *src)
{
    FileLoc origin = {
        .fileName = "test.cxy",
        .begin    = {.row = 1, .col = 1, .byteOffset = 0},
        .end      = {.row = 1, .col = 1, .byteOffset = 0},
    };
    CxymlLexer lexer;
    cxymlLexerInit(&lexer, src, strlen(src), &origin, g_parser_log);
    return cxymlParse(&lexer, &g_pool, &g_strings, g_parser_log);
}

// ============================================================================
// Helpers to inspect nodes
// ============================================================================

static CxymlNode *firstChild(CxymlNode *n)
{
    if (!n || cxymlNodeKind(n) != CXYML_NODE_ELEMENT) return NULL;
    return n->element.children;
}

static CxymlNode *nextSibling(CxymlNode *n)
{
    if (!n) return NULL;
    return (CxymlNode *)n->next;
}

static CxymlAttr *firstAttr(CxymlNode *n)
{
    if (!n || cxymlNodeKind(n) != CXYML_NODE_ELEMENT) return NULL;
    return n->element.attrs;
}

// ============================================================================
// Parser - empty / trivial
// ============================================================================

static void test_parse_empty(void)
{
    setupParser();
    CxymlNode *tree = parse("");
    ASSERT_NULL(tree);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_whitespace_only(void)
{
    setupParser();
    CxymlNode *tree = parse("   \n\t  ");
    // Whitespace-only text nodes are discarded by the parser
    ASSERT_NULL(tree);
    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - self-closing elements
// ============================================================================

static void test_parse_self_closing_lowercase(void)
{
    setupParser();
    CxymlNode *tree = parse("<br />");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_ELEMENT);
    ASSERT_STR_EQ(tree->element.tag, "br");
    ASSERT_TRUE(tree->element.selfClosing);
    ASSERT_FALSE(tree->element.isComponent);
    ASSERT_NULL(tree->element.children);
    ASSERT_NULL(firstAttr(tree));
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_self_closing_component(void)
{
    setupParser();
    CxymlNode *tree = parse("<MyWidget />");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_ELEMENT);
    ASSERT_STR_EQ(tree->element.tag, "MyWidget");
    ASSERT_TRUE(tree->element.selfClosing);
    ASSERT_TRUE(tree->element.isComponent);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_self_closing_no_space(void)
{
    setupParser();
    CxymlNode *tree = parse("<img/>");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_ELEMENT);
    ASSERT_STR_EQ(tree->element.tag, "img");
    ASSERT_TRUE(tree->element.selfClosing);
    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - open/close elements
// ============================================================================

static void test_parse_open_close_empty(void)
{
    setupParser();
    CxymlNode *tree = parse("<div></div>");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_ELEMENT);
    ASSERT_STR_EQ(tree->element.tag, "div");
    ASSERT_FALSE(tree->element.selfClosing);
    ASSERT_NULL(tree->element.children);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_uppercase_component(void)
{
    setupParser();
    CxymlNode *tree = parse("<Header></Header>");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_ELEMENT);
    ASSERT_STR_EQ(tree->element.tag, "Header");
    ASSERT_TRUE(tree->element.isComponent);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_mismatched_closing_tag(void)
{
    setupParser();
    CxymlNode *tree = parse("<div></span>");
    // Parser should log an error for the mismatch
    ASSERT_HAS_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - attributes
// ============================================================================

static void test_parse_static_attribute(void)
{
    setupParser();
    CxymlNode *tree = parse("<div class=\"btn\"></div>");
    ASSERT_NOT_NULL(tree);
    CxymlAttr *attr = firstAttr(tree);
    ASSERT_NOT_NULL(attr);
    ASSERT_STR_EQ(attr->name, "class");
    ASSERT_STR_EQ(attr->value, "btn");
    ASSERT_NULL(attr->interpExpr);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_single_quote_attribute(void)
{
    setupParser();
    CxymlNode *tree = parse("<div id='main'></div>");
    ASSERT_NOT_NULL(tree);
    CxymlAttr *attr = firstAttr(tree);
    ASSERT_NOT_NULL(attr);
    ASSERT_STR_EQ(attr->name, "id");
    ASSERT_STR_EQ(attr->value, "main");
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_interpolated_attribute(void)
{
    setupParser();
    CxymlNode *tree = parse("<div class={{ cls }}></div>");
    ASSERT_NOT_NULL(tree);
    CxymlAttr *attr = firstAttr(tree);
    ASSERT_NOT_NULL(attr);
    ASSERT_STR_EQ(attr->name, "class");
    ASSERT_NULL(attr->value);
    ASSERT_NOT_NULL(attr->interpExpr);
    // Raw expression content between {{ and }}
    ASSERT_STR_EQ(attr->interpExpr, " cls ");
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_boolean_flag_attribute(void)
{
    setupParser();
    CxymlNode *tree = parse("<input disabled />");
    ASSERT_NOT_NULL(tree);
    CxymlAttr *attr = firstAttr(tree);
    ASSERT_NOT_NULL(attr);
    ASSERT_STR_EQ(attr->name, "disabled");
    ASSERT_NULL(attr->value);
    ASSERT_NULL(attr->interpExpr);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_multiple_attributes(void)
{
    setupParser();
    CxymlNode *tree = parse("<div id=\"x\" class=\"y\" disabled></div>");
    ASSERT_NOT_NULL(tree);

    CxymlAttr *a1 = firstAttr(tree);
    ASSERT_NOT_NULL(a1);
    ASSERT_STR_EQ(a1->name, "id");

    CxymlAttr *a2 = a1->next;
    ASSERT_NOT_NULL(a2);
    ASSERT_STR_EQ(a2->name, "class");

    CxymlAttr *a3 = a2->next;
    ASSERT_NOT_NULL(a3);
    ASSERT_STR_EQ(a3->name, "disabled");
    ASSERT_NULL(a3->next);
    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - text children
// ============================================================================

static void test_parse_text_child(void)
{
    setupParser();
    CxymlNode *tree = parse("<p>Hello World</p>");
    ASSERT_NOT_NULL(tree);

    CxymlNode *child = firstChild(tree);
    ASSERT_NOT_NULL(child);
    ASSERT_EQ(cxymlNodeKind(child), CXYML_NODE_TEXT);
    ASSERT_STR_EQ(child->text.value, "Hello World");
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_text_whitespace_collapsed(void)
{
    setupParser();
    CxymlNode *tree = parse("<p>  Hello   World  </p>");
    ASSERT_NOT_NULL(tree);

    CxymlNode *child = firstChild(tree);
    ASSERT_NOT_NULL(child);
    ASSERT_EQ(cxymlNodeKind(child), CXYML_NODE_TEXT);
    ASSERT_STR_EQ(child->text.value, "Hello World");
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_text_whitespace_only_discarded(void)
{
    setupParser();
    // Whitespace-only between elements should produce no text node
    CxymlNode *tree = parse("<div>   </div>");
    ASSERT_NOT_NULL(tree);
    ASSERT_NULL(firstChild(tree));
    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - interpolation children
// ============================================================================

static void test_parse_interp_child(void)
{
    setupParser();
    CxymlNode *tree = parse("<p>{{ name }}</p>");
    ASSERT_NOT_NULL(tree);

    CxymlNode *child = firstChild(tree);
    ASSERT_NOT_NULL(child);
    ASSERT_EQ(cxymlNodeKind(child), CXYML_NODE_INTERP);
    ASSERT_STR_EQ(child->interp.expr, " name ");
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_mixed_text_and_interp(void)
{
    setupParser();
    CxymlNode *tree = parse("<p>Hello {{ name }}!</p>");
    ASSERT_NOT_NULL(tree);

    // Should have 3 children: text "Hello ", interp "name", text "!"
    CxymlNode *c1 = firstChild(tree);
    ASSERT_NOT_NULL(c1);
    ASSERT_EQ(cxymlNodeKind(c1), CXYML_NODE_TEXT);
    ASSERT_STR_EQ(c1->text.value, "Hello ");

    CxymlNode *c2 = nextSibling(c1);
    ASSERT_NOT_NULL(c2);
    ASSERT_EQ(cxymlNodeKind(c2), CXYML_NODE_INTERP);
    ASSERT_STR_EQ(c2->interp.expr, " name ");

    CxymlNode *c3 = nextSibling(c2);
    ASSERT_NOT_NULL(c3);
    ASSERT_EQ(cxymlNodeKind(c3), CXYML_NODE_TEXT);
    ASSERT_STR_EQ(c3->text.value, "!");

    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_multiple_interps(void)
{
    setupParser();
    CxymlNode *tree = parse("<p>{{ a }}{{ b }}</p>");
    ASSERT_NOT_NULL(tree);

    CxymlNode *c1 = firstChild(tree);
    ASSERT_NOT_NULL(c1);
    ASSERT_EQ(cxymlNodeKind(c1), CXYML_NODE_INTERP);
    ASSERT_STR_EQ(c1->interp.expr, " a ");

    CxymlNode *c2 = nextSibling(c1);
    ASSERT_NOT_NULL(c2);
    ASSERT_EQ(cxymlNodeKind(c2), CXYML_NODE_INTERP);
    ASSERT_STR_EQ(c2->interp.expr, " b ");

    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_space_between_interps(void)
{
    setupParser();
    // "{{ a }} {{ b }}" - the space between the two interpolations must be
    // preserved as a TEXT node containing " ", not silently discarded.
    CxymlNode *tree = parse("<p>{{ a }} {{ b }}</p>");
    ASSERT_NOT_NULL(tree);

    CxymlNode *c1 = firstChild(tree);
    ASSERT_NOT_NULL(c1);
    ASSERT_EQ(cxymlNodeKind(c1), CXYML_NODE_INTERP);

    CxymlNode *c2 = nextSibling(c1);
    ASSERT_NOT_NULL(c2);
    ASSERT_EQ(cxymlNodeKind(c2), CXYML_NODE_TEXT);
    ASSERT_STR_EQ(c2->text.value, " ");

    CxymlNode *c3 = nextSibling(c2);
    ASSERT_NOT_NULL(c3);
    ASSERT_EQ(cxymlNodeKind(c3), CXYML_NODE_INTERP);

    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - element children (nesting)
// ============================================================================

static void test_parse_single_child_element(void)
{
    setupParser();
    CxymlNode *tree = parse("<div><span></span></div>");
    ASSERT_NOT_NULL(tree);
    ASSERT_STR_EQ(tree->element.tag, "div");

    CxymlNode *child = firstChild(tree);
    ASSERT_NOT_NULL(child);
    ASSERT_EQ(cxymlNodeKind(child), CXYML_NODE_ELEMENT);
    ASSERT_STR_EQ(child->element.tag, "span");
    ASSERT_NULL(nextSibling(child));
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_multiple_sibling_children(void)
{
    setupParser();
    CxymlNode *tree = parse("<div><h1></h1><p></p><footer></footer></div>");
    ASSERT_NOT_NULL(tree);

    CxymlNode *c1 = firstChild(tree);
    ASSERT_NOT_NULL(c1);
    ASSERT_STR_EQ(c1->element.tag, "h1");

    CxymlNode *c2 = nextSibling(c1);
    ASSERT_NOT_NULL(c2);
    ASSERT_STR_EQ(c2->element.tag, "p");

    CxymlNode *c3 = nextSibling(c2);
    ASSERT_NOT_NULL(c3);
    ASSERT_STR_EQ(c3->element.tag, "footer");

    ASSERT_NULL(nextSibling(c3));
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_deeply_nested(void)
{
    setupParser();
    CxymlNode *tree = parse("<div><section><article><p>Deep</p></article></section></div>");
    ASSERT_NOT_NULL(tree);
    ASSERT_STR_EQ(tree->element.tag, "div");

    CxymlNode *section = firstChild(tree);
    ASSERT_NOT_NULL(section);
    ASSERT_STR_EQ(section->element.tag, "section");

    CxymlNode *article = firstChild(section);
    ASSERT_NOT_NULL(article);
    ASSERT_STR_EQ(article->element.tag, "article");

    CxymlNode *p = firstChild(article);
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->element.tag, "p");

    CxymlNode *text = firstChild(p);
    ASSERT_NOT_NULL(text);
    ASSERT_EQ(cxymlNodeKind(text), CXYML_NODE_TEXT);
    ASSERT_STR_EQ(text->text.value, "Deep");

    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_component_with_element_children(void)
{
    setupParser();
    CxymlNode *tree = parse("<Card><h1>Title</h1><p>Body</p></Card>");
    ASSERT_NOT_NULL(tree);
    ASSERT_TRUE(tree->element.isComponent);
    ASSERT_STR_EQ(tree->element.tag, "Card");

    CxymlNode *h1 = firstChild(tree);
    ASSERT_NOT_NULL(h1);
    ASSERT_STR_EQ(h1->element.tag, "h1");

    CxymlNode *p = nextSibling(h1);
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->element.tag, "p");

    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - comments
// ============================================================================

static void test_parse_comment_discarded(void)
{
    setupParser();
    // Comment between elements should be silently discarded
    CxymlNode *tree = parse("<div><!-- ignored --></div>");
    ASSERT_NOT_NULL(tree);
    ASSERT_NULL(firstChild(tree));
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_comment_between_siblings(void)
{
    setupParser();
    CxymlNode *tree = parse("<div><p></p><!-- comment --><span></span></div>");
    ASSERT_NOT_NULL(tree);

    CxymlNode *c1 = firstChild(tree);
    ASSERT_NOT_NULL(c1);
    ASSERT_STR_EQ(c1->element.tag, "p");

    CxymlNode *c2 = nextSibling(c1);
    ASSERT_NOT_NULL(c2);
    // Comment should be gone - next sibling is span
    ASSERT_STR_EQ(c2->element.tag, "span");

    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - multiple root nodes
// ============================================================================

static void test_parse_multiple_roots(void)
{
    setupParser();
    CxymlNode *tree = parse("<h1></h1><p></p>");
    ASSERT_NOT_NULL(tree);
    ASSERT_STR_EQ(tree->element.tag, "h1");

    CxymlNode *second = nextSibling(tree);
    ASSERT_NOT_NULL(second);
    ASSERT_STR_EQ(second->element.tag, "p");

    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - location tracking
// ============================================================================

static void test_parse_element_loc(void)
{
    setupParser();
    CxymlNode *tree = parse("<div></div>");
    ASSERT_NOT_NULL(tree);
    // Root element starts at row 1, col 1
    ASSERT_EQ(tree->loc.begin.row, 1);
    ASSERT_EQ(tree->loc.begin.col, 1);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_child_loc_after_newline(void)
{
    setupParser();
    CxymlNode *tree = parse("<div>\n  <p></p>\n</div>");
    ASSERT_NOT_NULL(tree);

    CxymlNode *child = firstChild(tree);
    ASSERT_NOT_NULL(child);
    // Child <p> is on row 2
    ASSERT_EQ(child->loc.begin.row, 2);
    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - isComponent flag
// ============================================================================

static void test_parse_iscomponent_lowercase(void)
{
    setupParser();
    CxymlNode *tree = parse("<div />");
    ASSERT_NOT_NULL(tree);
    ASSERT_FALSE(tree->element.isComponent);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_iscomponent_uppercase(void)
{
    setupParser();
    CxymlNode *tree = parse("<Div />");
    ASSERT_NOT_NULL(tree);
    ASSERT_TRUE(tree->element.isComponent);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_iscomponent_mixed(void)
{
    setupParser();
    CxymlNode *tree = parse("<div><MyButton /></div>");
    ASSERT_NOT_NULL(tree);
    ASSERT_FALSE(tree->element.isComponent);

    CxymlNode *child = firstChild(tree);
    ASSERT_NOT_NULL(child);
    ASSERT_TRUE(child->element.isComponent);
    ASSERT_STR_EQ(child->element.tag, "MyButton");
    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - node tags
// ============================================================================

static void test_parse_node_tag_element(void)
{
    setupParser();
    CxymlNode *tree = parse("<div />");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ((u32)tree->tag, (u32)CXYML_TAG_ELEMENT);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_node_tag_text(void)
{
    setupParser();
    CxymlNode *tree = parse("<p>Hello</p>");
    ASSERT_NOT_NULL(tree);
    CxymlNode *text = firstChild(tree);
    ASSERT_NOT_NULL(text);
    ASSERT_EQ((u32)text->tag, (u32)CXYML_TAG_TEXT);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_node_tag_interp(void)
{
    setupParser();
    CxymlNode *tree = parse("<p>{{ x }}</p>");
    ASSERT_NOT_NULL(tree);
    CxymlNode *interp = firstChild(tree);
    ASSERT_NOT_NULL(interp);
    ASSERT_EQ((u32)interp->tag, (u32)CXYML_TAG_INTERP);
    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Helpers for for/if node inspection
// ============================================================================

static CxymlNode *forBody(CxymlNode *n)
{
    if (!n || cxymlNodeKind(n) != CXYML_NODE_FOR) return NULL;
    return n->forNode.body;
}

static CxymlNode *ifThenBody(CxymlNode *n)
{
    if (!n || cxymlNodeKind(n) != CXYML_NODE_IF) return NULL;
    return n->ifNode.thenBody;
}

static CxymlNode *ifElseNode(CxymlNode *n)
{
    if (!n || cxymlNodeKind(n) != CXYML_NODE_IF) return NULL;
    return n->ifNode.elseNode;
}

// ============================================================================
// Parser - for directive
// ============================================================================

static void test_parse_for_node_kind(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ for user in users }}{{ /for }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_FOR);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_for_node_tag(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ for user in users }}{{ /for }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ((u32)tree->tag, (u32)CXYML_TAG_FOR);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_for_iter_var(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ for user in users }}{{ /for }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_STR_EQ(tree->forNode.iterVar, "user");
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_for_iter_expr(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ for user in users }}{{ /for }}");
    ASSERT_NOT_NULL(tree);
    // iterExpr is the raw captured string; leading content must match.
    // Trailing whitespace is preserved (consistent with INTERP_EXPR behaviour).
    ASSERT_NOT_NULL(tree->forNode.iterExpr);
    ASSERT_TRUE(strncmp(tree->forNode.iterExpr, "users", 5) == 0);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_for_empty_body(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ for x in xs }}{{ /for }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_FOR);
    ASSERT_STR_EQ(tree->forNode.iterVar, "x");
    ASSERT_NULL(forBody(tree));
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_for_with_element_body(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ for item in items }}<div>text</div>{{ /for }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_FOR);
    CxymlNode *body = forBody(tree);
    ASSERT_NOT_NULL(body);
    ASSERT_EQ(cxymlNodeKind(body), CXYML_NODE_ELEMENT);
    ASSERT_STR_EQ(body->element.tag, "div");
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_for_multiple_body_siblings(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ for u in users }}<h3>name</h3><p>email</p>{{ /for }}");
    ASSERT_NOT_NULL(tree);
    CxymlNode *body = forBody(tree);
    ASSERT_NOT_NULL(body);
    ASSERT_STR_EQ(body->element.tag, "h3");
    CxymlNode *sib = nextSibling(body);
    ASSERT_NOT_NULL(sib);
    ASSERT_STR_EQ(sib->element.tag, "p");
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_for_sibling_after(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ for x in xs }}<p>x</p>{{ /for }}<footer />");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_FOR);
    CxymlNode *sib = nextSibling(tree);
    ASSERT_NOT_NULL(sib);
    ASSERT_EQ(cxymlNodeKind(sib), CXYML_NODE_ELEMENT);
    ASSERT_STR_EQ(sib->element.tag, "footer");
    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - if directive
// ============================================================================

static void test_parse_if_node_kind(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ if isAdmin }}<p>yes</p>{{ /if }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_IF);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_if_node_tag(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ if isAdmin }}<p>yes</p>{{ /if }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ((u32)tree->tag, (u32)CXYML_TAG_IF);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_if_condition(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ if isAdmin }}<p>yes</p>{{ /if }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_NOT_NULL(tree->ifNode.condition);
    ASSERT_TRUE(strncmp(tree->ifNode.condition, "isAdmin", 7) == 0);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_if_then_body(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ if ok }}<span>yes</span>{{ /if }}");
    ASSERT_NOT_NULL(tree);
    CxymlNode *body = ifThenBody(tree);
    ASSERT_NOT_NULL(body);
    ASSERT_EQ(cxymlNodeKind(body), CXYML_NODE_ELEMENT);
    ASSERT_STR_EQ(body->element.tag, "span");
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_if_no_else(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ if ok }}<p>yes</p>{{ /if }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_NULL(ifElseNode(tree));
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_if_empty_body(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ if cond }}{{ /if }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_IF);
    ASSERT_NULL(ifThenBody(tree));
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_if_else(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ if ok }}<p>yes</p>{{ else }}<p>no</p>{{ /if }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_NOT_NULL(tree->ifNode.condition);
    ASSERT_NOT_NULL(ifThenBody(tree));
    // elseNode is a bare else branch — condition must be NULL
    CxymlNode *elseN = ifElseNode(tree);
    ASSERT_NOT_NULL(elseN);
    ASSERT_EQ(cxymlNodeKind(elseN), CXYML_NODE_IF);
    ASSERT_NULL(elseN->ifNode.condition);
    ASSERT_NOT_NULL(elseN->ifNode.thenBody);
    ASSERT_NULL(elseN->ifNode.elseNode);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_if_else_if(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ if a }}<p>a</p>{{ else if b }}<p>b</p>{{ /if }}");
    ASSERT_NOT_NULL(tree);
    ASSERT_NOT_NULL(tree->ifNode.condition);
    ASSERT_TRUE(strncmp(tree->ifNode.condition, "a", 1) == 0);
    CxymlNode *elseIf = ifElseNode(tree);
    ASSERT_NOT_NULL(elseIf);
    ASSERT_EQ(cxymlNodeKind(elseIf), CXYML_NODE_IF);
    ASSERT_NOT_NULL(elseIf->ifNode.condition);
    ASSERT_TRUE(strncmp(elseIf->ifNode.condition, "b", 1) == 0);
    ASSERT_NULL(elseIf->ifNode.elseNode);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_if_else_if_else(void)
{
    setupParser();
    const char *src =
        "{{ if a }}<p>a</p>"
        "{{ else if b }}<p>b</p>"
        "{{ else }}<p>other</p>"
        "{{ /if }}";
    CxymlNode *tree = parse(src);
    ASSERT_NOT_NULL(tree);
    // Root: IF(condition="a")
    ASSERT_TRUE(strncmp(tree->ifNode.condition, "a", 1) == 0);
    // elseNode: IF(condition="b")
    CxymlNode *elseIf = ifElseNode(tree);
    ASSERT_NOT_NULL(elseIf);
    ASSERT_TRUE(strncmp(elseIf->ifNode.condition, "b", 1) == 0);
    // elseIf->elseNode: bare else, condition=NULL
    CxymlNode *elseN = ifElseNode(elseIf);
    ASSERT_NOT_NULL(elseN);
    ASSERT_NULL(elseN->ifNode.condition);
    ASSERT_NOT_NULL(elseN->ifNode.thenBody);
    ASSERT_NULL(elseN->ifNode.elseNode);
    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - control flow nesting
// ============================================================================

static void test_parse_if_nested_in_for(void)
{
    setupParser();
    const char *src =
        "{{ for item in items }}"
        "{{ if item.active }}<span>active</span>{{ /if }}"
        "{{ /for }}";
    CxymlNode *tree = parse(src);
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_FOR);
    CxymlNode *body = forBody(tree);
    ASSERT_NOT_NULL(body);
    ASSERT_EQ(cxymlNodeKind(body), CXYML_NODE_IF);
    ASSERT_NOT_NULL(body->ifNode.condition);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_for_nested_in_if(void)
{
    setupParser();
    const char *src =
        "{{ if show }}"
        "{{ for x in xs }}<p>x</p>{{ /for }}"
        "{{ /if }}";
    CxymlNode *tree = parse(src);
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_IF);
    CxymlNode *body = ifThenBody(tree);
    ASSERT_NOT_NULL(body);
    ASSERT_EQ(cxymlNodeKind(body), CXYML_NODE_FOR);
    ASSERT_NO_ERRORS();
    teardownParser();
}

static void test_parse_for_inside_element(void)
{
    setupParser();
    CxymlNode *tree = parse("<ul>{{ for li in items }}<li>item</li>{{ /for }}</ul>");
    ASSERT_NOT_NULL(tree);
    ASSERT_EQ(cxymlNodeKind(tree), CXYML_NODE_ELEMENT);
    ASSERT_STR_EQ(tree->element.tag, "ul");
    CxymlNode *child = firstChild(tree);
    ASSERT_NOT_NULL(child);
    ASSERT_EQ(cxymlNodeKind(child), CXYML_NODE_FOR);
    ASSERT_NO_ERRORS();
    teardownParser();
}

// ============================================================================
// Parser - orphaned directive error recovery
// ============================================================================

static void test_parse_orphaned_end_for(void)
{
    setupParser();
    // {{ /for }} at the document root with no matching {{ for }} is an error.
    CxymlNode *tree = parse("{{ /for }}");
    ASSERT_NULL(tree);
    ASSERT_HAS_ERRORS();
    teardownParser();
}

static void test_parse_orphaned_else(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ else }}");
    ASSERT_NULL(tree);
    ASSERT_HAS_ERRORS();
    teardownParser();
}

static void test_parse_orphaned_end_if(void)
{
    setupParser();
    CxymlNode *tree = parse("{{ /if }}");
    ASSERT_NULL(tree);
    ASSERT_HAS_ERRORS();
    teardownParser();
}

// ============================================================================
// Test suite entry point (called from main.c)
// ============================================================================

void run_parser_tests(Log *log)
{
    g_parser_log = log;
    SUITE("Parser - empty / trivial");
    RUN_TEST(test_parse_empty);
    RUN_TEST(test_parse_whitespace_only);

    SUITE("Parser - self-closing elements");
    RUN_TEST(test_parse_self_closing_lowercase);
    RUN_TEST(test_parse_self_closing_component);
    RUN_TEST(test_parse_self_closing_no_space);

    SUITE("Parser - open/close elements");
    RUN_TEST(test_parse_open_close_empty);
    RUN_TEST(test_parse_uppercase_component);
    RUN_TEST(test_parse_mismatched_closing_tag);

    SUITE("Parser - attributes");
    RUN_TEST(test_parse_static_attribute);
    RUN_TEST(test_parse_single_quote_attribute);
    RUN_TEST(test_parse_interpolated_attribute);
    RUN_TEST(test_parse_boolean_flag_attribute);
    RUN_TEST(test_parse_multiple_attributes);

    SUITE("Parser - text children");
    RUN_TEST(test_parse_text_child);
    RUN_TEST(test_parse_text_whitespace_collapsed);
    RUN_TEST(test_parse_text_whitespace_only_discarded);

    SUITE("Parser - interpolation children");
    RUN_TEST(test_parse_interp_child);
    RUN_TEST(test_parse_mixed_text_and_interp);
    RUN_TEST(test_parse_multiple_interps);
    RUN_TEST(test_parse_space_between_interps);

    SUITE("Parser - nesting");
    RUN_TEST(test_parse_single_child_element);
    RUN_TEST(test_parse_multiple_sibling_children);
    RUN_TEST(test_parse_deeply_nested);
    RUN_TEST(test_parse_component_with_element_children);

    SUITE("Parser - comments");
    RUN_TEST(test_parse_comment_discarded);
    RUN_TEST(test_parse_comment_between_siblings);

    SUITE("Parser - multiple root nodes");
    RUN_TEST(test_parse_multiple_roots);

    SUITE("Parser - location tracking");
    RUN_TEST(test_parse_element_loc);
    RUN_TEST(test_parse_child_loc_after_newline);

    SUITE("Parser - isComponent flag");
    RUN_TEST(test_parse_iscomponent_lowercase);
    RUN_TEST(test_parse_iscomponent_uppercase);
    RUN_TEST(test_parse_iscomponent_mixed);

    SUITE("Parser - node tags");
    RUN_TEST(test_parse_node_tag_element);
    RUN_TEST(test_parse_node_tag_text);
    RUN_TEST(test_parse_node_tag_interp);

    SUITE("Parser - for directive");
    RUN_TEST(test_parse_for_node_kind);
    RUN_TEST(test_parse_for_node_tag);
    RUN_TEST(test_parse_for_iter_var);
    RUN_TEST(test_parse_for_iter_expr);
    RUN_TEST(test_parse_for_empty_body);
    RUN_TEST(test_parse_for_with_element_body);
    RUN_TEST(test_parse_for_multiple_body_siblings);
    RUN_TEST(test_parse_for_sibling_after);

    SUITE("Parser - if directive");
    RUN_TEST(test_parse_if_node_kind);
    RUN_TEST(test_parse_if_node_tag);
    RUN_TEST(test_parse_if_condition);
    RUN_TEST(test_parse_if_then_body);
    RUN_TEST(test_parse_if_no_else);
    RUN_TEST(test_parse_if_empty_body);
    RUN_TEST(test_parse_if_else);
    RUN_TEST(test_parse_if_else_if);
    RUN_TEST(test_parse_if_else_if_else);

    SUITE("Parser - control flow nesting");
    RUN_TEST(test_parse_if_nested_in_for);
    RUN_TEST(test_parse_for_nested_in_if);
    RUN_TEST(test_parse_for_inside_element);

    SUITE("Parser - orphaned directive error recovery");
    RUN_TEST(test_parse_orphaned_end_for);
    RUN_TEST(test_parse_orphaned_else);
    RUN_TEST(test_parse_orphaned_end_if);
}