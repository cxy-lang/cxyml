//
// Cxyml plugin - Lexer unit tests
//

#include "test_harness.h"

#include "../../src/plugin/state.h"

#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

static Log     *g_lexer_log  = NULL;
static FileLoc  g_origin = {
    .fileName = "test.cxy",
    .begin    = {.row = 1, .col = 1, .byteOffset = 0},
    .end      = {.row = 1, .col = 1, .byteOffset = 0},
};

static void lexerFromStr(CxymlLexer *l, const char *src)
{
    cxymlLexerInit(l, src, strlen(src), &g_origin, g_lexer_log);
    RESET_ERRORS();
}

// Initialize lexer in tag mode - for testing tokens that only appear inside tags
static void lexerFromTagStr(CxymlLexer *l, const char *src)
{
    cxymlLexerInit(l, src, strlen(src), &g_origin, g_lexer_log);
    l->mode = CXYML_MODE_TAG;
    RESET_ERRORS();
}

// Consume the next non-comment token
static CxymlToken nextMeaningful(CxymlLexer *l)
{
    CxymlToken t;
    do { t = cxymlLexerNext(l); } while (t.type == CXYML_TOK_COMMENT);
    return t;
}

// ============================================================================
// Lexer - EOF
// ============================================================================

static void test_empty_input(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

static void test_whitespace_only(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "   \t\n  ");
    // whitespace not inside tags is returned as TEXT (then normalised to empty)
    // The lexer itself returns it as TEXT; the parser discards it.
    // At minimum we should NOT crash and should eventually reach EOF.
    CxymlToken t;
    int loops = 0;
    do {
        t = cxymlLexerNext(&l);
        loops++;
        ASSERT_MSG(loops < 64, "infinite loop in whitespace-only input");
    } while (t.type != CXYML_TOK_EOF && t.type != CXYML_TOK_ERROR);
    ASSERT_EQ(t.type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

// ============================================================================
// Lexer - Single-character tokens
// ============================================================================

static void test_open_tag(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "<");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_OPEN);
    ASSERT_EQ(t.len, 1);
    ASSERT_NO_ERRORS();
}

static void test_close_tag(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, ">");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_CLOSE);
    ASSERT_EQ(t.len, 1);
    ASSERT_NO_ERRORS();
}

static void test_slash(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "/");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_SLASH);
    ASSERT_EQ(t.len, 1);
    ASSERT_NO_ERRORS();
}

static void test_equals(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "=");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_EQUALS);
    ASSERT_EQ(t.len, 1);
    ASSERT_NO_ERRORS();
}

// ============================================================================
// Lexer - Identifiers
// ============================================================================

static void test_ident_simple(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "div");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_IDENT);
    ASSERT_STRN_EQ(t.start, t.len, "div");
    ASSERT_NO_ERRORS();
}

static void test_ident_uppercase(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "MyComponent");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_IDENT);
    ASSERT_STRN_EQ(t.start, t.len, "MyComponent");
    ASSERT_NO_ERRORS();
}

static void test_ident_with_hyphen(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "data-value");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_IDENT);
    ASSERT_STRN_EQ(t.start, t.len, "data-value");
    ASSERT_NO_ERRORS();
}

static void test_ident_with_dot(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "my.attr");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_IDENT);
    ASSERT_STRN_EQ(t.start, t.len, "my.attr");
    ASSERT_NO_ERRORS();
}

static void test_ident_underscore(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "_private");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_IDENT);
    ASSERT_STRN_EQ(t.start, t.len, "_private");
    ASSERT_NO_ERRORS();
}

// ============================================================================
// Lexer - Strings
// ============================================================================

static void test_string_double_quote(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "\"hello world\"");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_STRING);
    ASSERT_STRN_EQ(t.start, t.len, "hello world");
    ASSERT_NO_ERRORS();
}

static void test_string_single_quote(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "'hello world'");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_STRING);
    ASSERT_STRN_EQ(t.start, t.len, "hello world");
    ASSERT_NO_ERRORS();
}

static void test_string_empty(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "\"\"");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_STRING);
    ASSERT_EQ(t.len, 0);
    ASSERT_NO_ERRORS();
}

static void test_string_unterminated(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "\"unterminated");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_ERROR);
    ASSERT_HAS_ERRORS();
}

// ============================================================================
// Lexer - Text content
// ============================================================================

static void test_text_simple(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "Hello World");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_TEXT);
    ASSERT_STRN_EQ(t.start, t.len, "Hello World");
    ASSERT_NO_ERRORS();
}

static void test_text_stops_at_open_tag(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "Hello<div");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_TEXT);
    ASSERT_STRN_EQ(t.start, t.len, "Hello");

    t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_OPEN);
    ASSERT_NO_ERRORS();
}

static void test_text_stops_at_interp(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "Hello {{ name }}");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_TEXT);
    ASSERT_STRN_EQ(t.start, t.len, "Hello ");

    t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_NO_ERRORS();
}

// ============================================================================
// Lexer - Interpolations
// ============================================================================

static void test_interp_simple(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ name }}");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_INTERP_EXPR);
    // Trimmed: ' name ' - the lexer captures raw content between {{ }}
    ASSERT_STRN_EQ(t.start, t.len, " name ");
    ASSERT_NO_ERRORS();
}

static void test_interp_expression(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ a + b }}");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_STRN_EQ(t.start, t.len, " a + b ");
    ASSERT_NO_ERRORS();
}

static void test_interp_nested_braces(void)
{
    CxymlLexer l;
    // Expression contains a map literal with braces
    lexerFromStr(&l, "{{ obj.method() }}");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_NO_ERRORS();
}

static void test_interp_multiple(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ a }}{{ b }}");

    CxymlToken t1 = cxymlLexerNext(&l);
    ASSERT_EQ(t1.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_STRN_EQ(t1.start, t1.len, " a ");

    CxymlToken t2 = cxymlLexerNext(&l);
    ASSERT_EQ(t2.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_STRN_EQ(t2.start, t2.len, " b ");

    ASSERT_NO_ERRORS();
}

static void test_interp_unterminated(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ name");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_ERROR);
    ASSERT_HAS_ERRORS();
}

// ============================================================================
// Lexer - Comments
// ============================================================================

static void test_comment_basic(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "<!-- this is a comment -->");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_COMMENT);
    ASSERT_NO_ERRORS();
}

static void test_comment_followed_by_tag(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "<!-- comment --><div");
    CxymlToken t = nextMeaningful(&l);
    ASSERT_EQ(t.type, CXYML_TOK_OPEN);
    ASSERT_NO_ERRORS();
}

static void test_comment_unterminated(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "<!-- unterminated");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_ERROR);
    ASSERT_HAS_ERRORS();
}

static void test_comment_malformed_close(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "<!-- bad --x>");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_ERROR);
    ASSERT_HAS_ERRORS();
}

// ============================================================================
// Lexer - Token sequences  (simulate real markup fragments)
// ============================================================================

static void test_seq_open_ident_close(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "<div>");

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_OPEN);
    CxymlToken ident = cxymlLexerNext(&l);
    ASSERT_EQ(ident.type, CXYML_TOK_IDENT);
    ASSERT_STRN_EQ(ident.start, ident.len, "div");
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_CLOSE);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

static void test_seq_self_closing(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "<br />");

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_OPEN);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_IDENT);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_SLASH);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_CLOSE);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

static void test_seq_attribute(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "class=\"btn\"");

    CxymlToken name = cxymlLexerNext(&l);
    ASSERT_EQ(name.type, CXYML_TOK_IDENT);
    ASSERT_STRN_EQ(name.start, name.len, "class");

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EQUALS);

    CxymlToken val = cxymlLexerNext(&l);
    ASSERT_EQ(val.type, CXYML_TOK_STRING);
    ASSERT_STRN_EQ(val.start, val.len, "btn");
    ASSERT_NO_ERRORS();
}

static void test_seq_interpolated_attribute(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, "id=\"{{ myId }}\"");

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_IDENT);  // id
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EQUALS);
    // The quoted value contains interpolation - but the lexer sees it as a
    // STRING that contains '{{'. Actually '{{ }}' starts before the quote ends
    // only if the user writes id={{ myId }} without quotes.
    // With quotes, the whole "{{ myId }}" is returned as a STRING token.
    CxymlToken val = cxymlLexerNext(&l);
    ASSERT_EQ(val.type, CXYML_TOK_STRING);
    ASSERT_NO_ERRORS();
}

static void test_seq_closing_tag(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "</div>");

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_OPEN);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_SLASH);
    CxymlToken ident = cxymlLexerNext(&l);
    ASSERT_EQ(ident.type, CXYML_TOK_IDENT);
    ASSERT_STRN_EQ(ident.start, ident.len, "div");
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_CLOSE);
    ASSERT_NO_ERRORS();
}

// ============================================================================
// Lexer - Peek / Match / Expect
// ============================================================================

static void test_peek_does_not_consume(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "<div");

    CxymlToken p1 = cxymlLexerPeek(&l);
    CxymlToken p2 = cxymlLexerPeek(&l);
    ASSERT_EQ(p1.type, p2.type);
    ASSERT_EQ(p1.start, p2.start);

    // next should return the same token
    CxymlToken n = cxymlLexerNext(&l);
    ASSERT_EQ(n.type, CXYML_TOK_OPEN);
    ASSERT_NO_ERRORS();
}

static void test_match_success(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "<");

    CxymlToken out;
    bool matched = cxymlLexerMatch(&l, CXYML_TOK_OPEN, &out);
    ASSERT_TRUE(matched);
    ASSERT_EQ(out.type, CXYML_TOK_OPEN);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

static void test_match_failure_does_not_consume(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "<");

    bool matched = cxymlLexerMatch(&l, CXYML_TOK_CLOSE, NULL);
    ASSERT_FALSE(matched);

    // Token should still be available
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_OPEN);
    ASSERT_NO_ERRORS();
}

static void test_expect_success(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, ">");

    CxymlToken out;
    bool ok = cxymlLexerExpect(&l, CXYML_TOK_CLOSE, &out);
    ASSERT_TRUE(ok);
    ASSERT_EQ(out.type, CXYML_TOK_CLOSE);
    ASSERT_NO_ERRORS();
}

static void test_expect_failure_logs_error(void)
{
    CxymlLexer l;
    lexerFromTagStr(&l, ">");

    bool ok = cxymlLexerExpect(&l, CXYML_TOK_OPEN, NULL);
    ASSERT_FALSE(ok);
    ASSERT_HAS_ERRORS();
}

// ============================================================================
// Lexer - Location tracking
// ============================================================================

static void test_location_first_token(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "<");
    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.loc.begin.row, 1);
    ASSERT_EQ(t.loc.begin.col, 1);
    ASSERT_NO_ERRORS();
}

static void test_location_after_newline(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "\n<");
    // '\n' is text, skip it; '<' is on row 2
    CxymlToken t1 = cxymlLexerNext(&l); // text '\n'
    (void)t1;
    CxymlToken t2 = cxymlLexerNext(&l); // '<'
    ASSERT_EQ(t2.type, CXYML_TOK_OPEN);
    ASSERT_EQ(t2.loc.begin.row, 2);
    ASSERT_NO_ERRORS();
}

static void test_location_origin_offset(void)
{
    // Simulate an inline string that starts at row 5, col 10
    FileLoc origin = {
        .fileName = "source.cxy",
        .begin    = {.row = 5, .col = 10, .byteOffset = 0},
        .end      = {.row = 5, .col = 10, .byteOffset = 0},
    };
    CxymlLexer l;
    cxymlLexerInit(&l, "<", 1, &origin, g_lexer_log);
    RESET_ERRORS();

    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_OPEN);
    ASSERT_EQ(t.loc.begin.row, 5);
    ASSERT_EQ(t.loc.begin.col, 10);
    ASSERT_NO_ERRORS();
}

// ============================================================================
// Control flow: for directive
// ============================================================================

// {{ for user in users }}  →  CTRL_FOR, IDENT("user"), IN, INTERP_EXPR("users ")
static void test_ctrl_for_token_sequence(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ for user in users }}");

    CxymlToken t1 = cxymlLexerNext(&l);
    ASSERT_EQ(t1.type, CXYML_TOK_CTRL_FOR);

    CxymlToken t2 = cxymlLexerNext(&l);
    ASSERT_EQ(t2.type, CXYML_TOK_IDENT);
    ASSERT_EQ(t2.len, 4u);
    ASSERT_TRUE(strncmp(t2.start, "user", 4) == 0);

    CxymlToken t3 = cxymlLexerNext(&l);
    ASSERT_EQ(t3.type, CXYML_TOK_IN);

    CxymlToken t4 = cxymlLexerNext(&l);
    ASSERT_EQ(t4.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_TRUE(t4.len >= 5u);
    ASSERT_TRUE(strncmp(t4.start, "users", 5) == 0);

    CxymlToken t5 = cxymlLexerNext(&l);
    ASSERT_EQ(t5.type, CXYML_TOK_EOF);

    ASSERT_NO_ERRORS();
}

// Loop variable with underscores, range is a function call
static void test_ctrl_for_complex_expr(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ for _item in getItems() }}");

    CxymlToken t1 = cxymlLexerNext(&l);
    ASSERT_EQ(t1.type, CXYML_TOK_CTRL_FOR);

    CxymlToken t2 = cxymlLexerNext(&l);
    ASSERT_EQ(t2.type, CXYML_TOK_IDENT);
    ASSERT_EQ(t2.len, 5u);
    ASSERT_TRUE(strncmp(t2.start, "_item", 5) == 0);

    CxymlToken t3 = cxymlLexerNext(&l);
    ASSERT_EQ(t3.type, CXYML_TOK_IN);

    CxymlToken t4 = cxymlLexerNext(&l);
    ASSERT_EQ(t4.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_TRUE(t4.len >= 10u);
    ASSERT_TRUE(strncmp(t4.start, "getItems()", 10) == 0);

    ASSERT_NO_ERRORS();
}

// {{ /for }}  →  CTRL_END_FOR
static void test_ctrl_end_for(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ /for }}");

    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_CTRL_END_FOR);

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

// Missing 'in' keyword → CTRL_FOR + IDENT + error at FOR_IN stage
static void test_ctrl_for_missing_in_error(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ for user users }}");

    CxymlToken t1 = cxymlLexerNext(&l);
    ASSERT_EQ(t1.type, CXYML_TOK_CTRL_FOR);

    CxymlToken t2 = cxymlLexerNext(&l);
    ASSERT_EQ(t2.type, CXYML_TOK_IDENT); // "user"

    // FOR_IN mode sees "users" — not the 'in' keyword → error
    CxymlToken t3 = cxymlLexerNext(&l);
    ASSERT_EQ(t3.type, CXYML_TOK_ERROR);
    ASSERT_HAS_ERRORS();
}

// Extra whitespace inside {{ for  x   in   xs }}
static void test_ctrl_for_extra_whitespace(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{  for  x   in   xs  }}");

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_CTRL_FOR);

    CxymlToken var = cxymlLexerNext(&l);
    ASSERT_EQ(var.type, CXYML_TOK_IDENT);
    ASSERT_EQ(var.len, 1u);
    ASSERT_TRUE(strncmp(var.start, "x", 1) == 0);

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_IN);

    CxymlToken range = cxymlLexerNext(&l);
    ASSERT_EQ(range.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_TRUE(strncmp(range.start, "xs", 2) == 0);

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

// 'for' in identifier name must NOT be treated as a directive (e.g. "format")
static void test_ctrl_for_not_matched_in_ident(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ format }}");

    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_TRUE(strncmp(t.start, " format", 7) == 0);
    ASSERT_NO_ERRORS();
}

// ============================================================================
// Control flow: if directive
// ============================================================================

// {{ if user.isAdmin }}  →  CTRL_IF with payload = "user.isAdmin"
static void test_ctrl_if_simple(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ if user.isAdmin }}");

    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_CTRL_IF);
    ASSERT_TRUE(t.len >= 12u);
    ASSERT_TRUE(strncmp(t.start, "user.isAdmin", 12) == 0);

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

// {{ else if user.active }}  →  CTRL_ELSE_IF with payload = "user.active"
static void test_ctrl_else_if(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ else if user.active }}");

    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_CTRL_ELSE_IF);
    ASSERT_TRUE(t.len >= 11u);
    ASSERT_TRUE(strncmp(t.start, "user.active", 11) == 0);

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

// {{ else }}  →  CTRL_ELSE (no payload)
static void test_ctrl_else(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ else }}");

    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_CTRL_ELSE);

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

// {{ /if }}  →  CTRL_END_IF
static void test_ctrl_end_if(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ /if }}");

    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_CTRL_END_IF);

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

// 'if' inside an identifier must NOT be treated as a directive (e.g. "ifdef")
static void test_ctrl_if_not_matched_in_ident(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ ifdef }}");

    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_TRUE(strncmp(t.start, " ifdef", 6) == 0);
    ASSERT_NO_ERRORS();
}

// 'else' inside an identifier (e.g. "elsewhere") must NOT match
static void test_ctrl_else_not_matched_in_ident(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ elsewhere }}");

    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_TRUE(strncmp(t.start, " elsewhere", 10) == 0);
    ASSERT_NO_ERRORS();
}

// ============================================================================
// Control flow: non-interference with existing behaviour
// ============================================================================

// Plain {{ expr }} interpolation must still produce INTERP_EXPR unchanged
static void test_regular_interp_unaffected_by_directives(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ user.name }}");

    CxymlToken t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_INTERP_EXPR);
    ASSERT_TRUE(strncmp(t.start, " user.name", 10) == 0);

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

// Full for-loop token sequence interleaved with HTML content
static void test_ctrl_for_in_html_context(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ for row in rows }}<tr/>{{ /for }}");

    // for header
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_CTRL_FOR);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_IDENT);  // row
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_IN);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_INTERP_EXPR); // rows

    // body: <tr/>
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_OPEN);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_IDENT);  // tr
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_SLASH);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_CLOSE);

    // closing directive
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_CTRL_END_FOR);

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

// Full if / else-if / else token sequence
static void test_ctrl_if_else_chain_tokens(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ if a }}{{ else if b }}{{ else }}{{ /if }}");

    CxymlToken t;

    t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_CTRL_IF);
    ASSERT_TRUE(t.len >= 1u);
    ASSERT_TRUE(strncmp(t.start, "a", 1) == 0);

    t = cxymlLexerNext(&l);
    ASSERT_EQ(t.type, CXYML_TOK_CTRL_ELSE_IF);
    ASSERT_TRUE(t.len >= 1u);
    ASSERT_TRUE(strncmp(t.start, "b", 1) == 0);

    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_CTRL_ELSE);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_CTRL_END_IF);
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

// Peek / match / expect work correctly on control-flow tokens
static void test_ctrl_peek_and_match(void)
{
    CxymlLexer l;
    lexerFromStr(&l, "{{ /for }}{{ /if }}");

    // peek does not consume
    CxymlToken peeked = cxymlLexerPeek(&l);
    ASSERT_EQ(peeked.type, CXYML_TOK_CTRL_END_FOR);

    CxymlToken matched;
    ASSERT_TRUE(cxymlLexerMatch(&l, CXYML_TOK_CTRL_END_FOR, &matched));
    ASSERT_EQ(matched.type, CXYML_TOK_CTRL_END_FOR);

    ASSERT_TRUE(cxymlLexerMatch(&l, CXYML_TOK_CTRL_END_IF, NULL));
    ASSERT_EQ(cxymlLexerNext(&l).type, CXYML_TOK_EOF);
    ASSERT_NO_ERRORS();
}

// ============================================================================
// Test suite entry point (called from main.c)
// ============================================================================

void run_lexer_tests(Log *log)
{
    g_lexer_log = log;
    SUITE("Lexer - EOF / empty input");
    RUN_TEST(test_empty_input);
    RUN_TEST(test_whitespace_only);

    SUITE("Lexer - Single-character tokens");
    RUN_TEST(test_open_tag);
    RUN_TEST(test_close_tag);
    RUN_TEST(test_slash);
    RUN_TEST(test_equals);

    SUITE("Lexer - Identifiers");
    RUN_TEST(test_ident_simple);
    RUN_TEST(test_ident_uppercase);
    RUN_TEST(test_ident_with_hyphen);
    RUN_TEST(test_ident_with_dot);
    RUN_TEST(test_ident_underscore);

    SUITE("Lexer - Strings");
    RUN_TEST(test_string_double_quote);
    RUN_TEST(test_string_single_quote);
    RUN_TEST(test_string_empty);
    RUN_TEST(test_string_unterminated);

    SUITE("Lexer - Text content");
    RUN_TEST(test_text_simple);
    RUN_TEST(test_text_stops_at_open_tag);
    RUN_TEST(test_text_stops_at_interp);

    SUITE("Lexer - Interpolations");
    RUN_TEST(test_interp_simple);
    RUN_TEST(test_interp_expression);
    RUN_TEST(test_interp_nested_braces);
    RUN_TEST(test_interp_multiple);
    RUN_TEST(test_interp_unterminated);

    SUITE("Lexer - Comments");
    RUN_TEST(test_comment_basic);
    RUN_TEST(test_comment_followed_by_tag);
    RUN_TEST(test_comment_unterminated);
    RUN_TEST(test_comment_malformed_close);

    SUITE("Lexer - Token sequences");
    RUN_TEST(test_seq_open_ident_close);
    RUN_TEST(test_seq_self_closing);
    RUN_TEST(test_seq_attribute);
    RUN_TEST(test_seq_interpolated_attribute);
    RUN_TEST(test_seq_closing_tag);

    SUITE("Lexer - Peek / Match / Expect");
    RUN_TEST(test_peek_does_not_consume);
    RUN_TEST(test_match_success);
    RUN_TEST(test_match_failure_does_not_consume);
    RUN_TEST(test_expect_success);
    RUN_TEST(test_expect_failure_logs_error);

    SUITE("Lexer - Location tracking");
    RUN_TEST(test_location_first_token);
    RUN_TEST(test_location_after_newline);
    RUN_TEST(test_location_origin_offset);

    SUITE("Lexer - Control flow: for directive");
    RUN_TEST(test_ctrl_for_token_sequence);
    RUN_TEST(test_ctrl_for_complex_expr);
    RUN_TEST(test_ctrl_end_for);
    RUN_TEST(test_ctrl_for_missing_in_error);
    RUN_TEST(test_ctrl_for_extra_whitespace);
    RUN_TEST(test_ctrl_for_not_matched_in_ident);

    SUITE("Lexer - Control flow: if directive");
    RUN_TEST(test_ctrl_if_simple);
    RUN_TEST(test_ctrl_else_if);
    RUN_TEST(test_ctrl_else);
    RUN_TEST(test_ctrl_end_if);
    RUN_TEST(test_ctrl_if_not_matched_in_ident);
    RUN_TEST(test_ctrl_else_not_matched_in_ident);

    SUITE("Lexer - Control flow: non-interference");
    RUN_TEST(test_regular_interp_unaffected_by_directives);
    RUN_TEST(test_ctrl_for_in_html_context);
    RUN_TEST(test_ctrl_if_else_chain_tokens);
    RUN_TEST(test_ctrl_peek_and_match);
}
