//
// Cxyml plugin - Lexer
//
// Tokenises the XML-like cxyml markup.
//
// Interpolation expressions ({{ ... }}) are captured as raw strings and
// forwarded to Cxy's own parser — the lexer never tokenises their content.
//
// Control-flow directives are distinguished from plain expressions by peeking
// at the first word inside {{ ... }}:
//
//   {{ for var in expr }}  →  CTRL_FOR, IDENT("var"), IN, INTERP_EXPR("expr")
//   {{ /for }}             →  CTRL_END_FOR
//   {{ if expr }}          →  CTRL_IF   (token payload = raw condition)
//   {{ else if expr }}     →  CTRL_ELSE_IF (payload = raw condition)
//   {{ else }}             →  CTRL_ELSE
//   {{ /if }}              →  CTRL_END_IF
//   {{ anything else }}    →  INTERP_EXPR  (unchanged)
//
// The for-header spans multiple cxymlLexerNext() calls driven by CxymlLexMode:
//
//   lexInterp() detects "for", sets mode = FOR_VAR, returns CTRL_FOR.
//   Next call (FOR_VAR):  lex the loop-variable IDENT, set mode = FOR_IN.
//   Next call (FOR_IN):   match "in" keyword, set mode = FOR_EXPR, return IN.
//   Next call (FOR_EXPR): capture until }}, set mode = CONTENT, return INTERP_EXPR.
//
// Location tracking
// -----------------
// Every token records a FileLoc.  The origin of that location depends on
// how the markup was supplied:
//   • Inline string  – origin is the FileLoc of the cxyml.render!() call site.
//   • External file  – origin is {fileName: path, line:1, col:1}.
//

#include "state.h"

#include <cxy/core/log.h>

#include <ctype.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline bool lexerAtEnd(const CxymlLexer *l)
{
    return l->cur >= l->end;
}

static inline char lexerPeekChar(const CxymlLexer *l)
{
    return lexerAtEnd(l) ? '\0' : *l->cur;
}

static inline char lexerAdvanceChar(CxymlLexer *l)
{
    char c = *l->cur++;
    if (c == '\n') {
        l->row++;
        l->col = 1;
    }
    else {
        l->col++;
    }
    return c;
}

// Snapshot the current position as a single-character FileLoc
static inline FileLoc lexerCurrentLoc(const CxymlLexer *l)
{
    FileLoc loc      = l->origin;
    loc.begin.row    = l->row;
    loc.begin.col    = l->col;
    loc.end          = loc.begin;
    return loc;
}

// Extend loc.end to the current position
static inline void lexerEndLoc(const CxymlLexer *l, FileLoc *loc)
{
    loc->end.row = l->row;
    loc->end.col = l->col;
}

static inline bool isIdentStart(char c)
{
    return isAsciiAlpha(c) || c == '_';
}

static inline bool isIdentContinue(char c)
{
    return isAsciiAlnum(c) || c == '_' || c == '-' || c == '.';
}

// Build a token from already-advanced source positions
static CxymlToken makeToken(CxymlTokenType type,
                            const char    *start,
                            u32            len,
                            FileLoc        loc)
{
    return (CxymlToken){.type = type, .start = start, .len = len, .loc = loc};
}

static CxymlToken errorToken(CxymlLexer *l, const char *msg)
{
    FileLoc loc = lexerCurrentLoc(l);
    logError(l->L, &loc, "{s}", (FormatArg[]){{.s = msg}});
    return makeToken(CXYML_TOK_ERROR, l->cur, 0, loc);
}

// ---------------------------------------------------------------------------
// Skip whitespace-only text between tags
// ---------------------------------------------------------------------------

static void skipWhitespace(CxymlLexer *l)
{
    while (!lexerAtEnd(l) && isAsciiSpace(lexerPeekChar(l)))
        lexerAdvanceChar(l);
}

// ---------------------------------------------------------------------------
// matchKeyword
//
// Returns true when l->cur starts with `kw` (of `kwlen` bytes) AND is
// followed immediately by a word boundary — a whitespace character or `}`.
// Prevents "for" matching "format", "if" matching "index", etc.
// Does NOT advance the cursor.
// ---------------------------------------------------------------------------

static bool matchKeyword(const CxymlLexer *l, const char *kw, u32 kwlen)
{
    if ((u32)(l->end - l->cur) < kwlen)
        return false;
    if (memcmp(l->cur, kw, kwlen) != 0)
        return false;
    if (l->cur + kwlen >= l->end)
        return true; // keyword fills the rest of the buffer — valid boundary
    char after = l->cur[kwlen];
    return isAsciiSpace(after) || after == '}';
}

// ---------------------------------------------------------------------------
// captureUntilClose
//
// Captures raw characters from the current position until `}}`, tracking
// nested `{}` pairs so that e.g. `{{ if obj.fn({}) }}` works correctly.
// Consumes the closing `}}` before returning.
// Returns an error token when the input ends before `}}` is found.
// ---------------------------------------------------------------------------

static CxymlToken captureUntilClose(CxymlLexer     *l,
                                    CxymlTokenType  type,
                                    const char     *start,
                                    FileLoc         loc)
{
    int depth = 0;

    while (!lexerAtEnd(l)) {
        char c = lexerPeekChar(l);

        if (c == '{') {
            depth++;
            lexerAdvanceChar(l);
            continue;
        }
        if (c == '}') {
            if (depth > 0) {
                depth--;
                lexerAdvanceChar(l);
                continue;
            }
            // Check for '}}'
            if ((l->cur + 1) < l->end && l->cur[1] == '}') {
                u32 len = (u32)(l->cur - start);
                lexerEndLoc(l, &loc);
                lexerAdvanceChar(l); // first '}'
                lexerAdvanceChar(l); // second '}'
                return makeToken(type, start, len, loc);
            }
        }
        lexerAdvanceChar(l);
    }
    return errorToken(l, "cxyml: unterminated template directive '{{ ... }}'");
}

// ---------------------------------------------------------------------------
// skipUntilClose
//
// Discards all characters up to and including the closing `}}`.
// Returns false when the input ends without `}}`.
// ---------------------------------------------------------------------------

static bool skipUntilClose(CxymlLexer *l)
{
    while (!lexerAtEnd(l)) {
        char c = lexerPeekChar(l);
        if (c == '}' && (l->cur + 1) < l->end && l->cur[1] == '}') {
            lexerAdvanceChar(l); // '}'
            lexerAdvanceChar(l); // '}'
            return true;
        }
        lexerAdvanceChar(l);
    }
    return false; // unterminated
}

// ---------------------------------------------------------------------------
// Comment:  <!-- ... -->
// The opening '<' and '!' have already been consumed.
// ---------------------------------------------------------------------------

static CxymlToken lexComment(CxymlLexer *l, const char *start, FileLoc loc)
{
    // We already consumed '<' and '!', expect '--'
    if (lexerAtEnd(l) || *l->cur != '-') return errorToken(l, "Expected '--' after '<!'");
    lexerAdvanceChar(l);
    if (lexerAtEnd(l) || *l->cur != '-') return errorToken(l, "Expected '--' after '<!-'");
    lexerAdvanceChar(l);

    // Scan until '-->'
    while (!lexerAtEnd(l)) {
        if (l->cur[0] == '-' && (l->cur + 1) < l->end && l->cur[1] == '-') {
            lexerAdvanceChar(l); // first '-'
            lexerAdvanceChar(l); // second '-'
            if (lexerAtEnd(l) || *l->cur != '>')
                return errorToken(l, "Malformed comment closing sequence");
            lexerAdvanceChar(l); // '>'
            FileLoc endLoc = loc;
            lexerEndLoc(l, &endLoc);
            return makeToken(CXYML_TOK_COMMENT, start,
                             (u32)(l->cur - start), endLoc);
        }
        lexerAdvanceChar(l);
    }
    return errorToken(l, "Unterminated comment");
}

// ---------------------------------------------------------------------------
// Quoted string attribute value:  "..." or '...'
// ---------------------------------------------------------------------------

static CxymlToken lexString(CxymlLexer *l)
{
    FileLoc loc   = lexerCurrentLoc(l);
    char    quote = lexerAdvanceChar(l); // consume opening quote
    const char *start = l->cur;

    while (!lexerAtEnd(l)) {
        char c = lexerPeekChar(l);
        if (c == quote) {
            u32 len = (u32)(l->cur - start);
            lexerAdvanceChar(l); // consume closing quote
            lexerEndLoc(l, &loc);
            return makeToken(CXYML_TOK_STRING, start, len, loc);
        }
        if (c == '\0')
            break;
        lexerAdvanceChar(l);
    }
    return errorToken(l, "Unterminated string literal");
}

// ---------------------------------------------------------------------------
// Identifier:  tag name or attribute name
// ---------------------------------------------------------------------------

static CxymlToken lexIdent(CxymlLexer *l)
{
    FileLoc    loc   = lexerCurrentLoc(l);
    const char *start = l->cur;
    lexerAdvanceChar(l); // consume first char (already checked isIdentStart)

    while (!lexerAtEnd(l) && isIdentContinue(lexerPeekChar(l)))
        lexerAdvanceChar(l);

    lexerEndLoc(l, &loc);
    return makeToken(CXYML_TOK_IDENT, start, (u32)(l->cur - start), loc);
}

// ---------------------------------------------------------------------------
// Text content between tags.
// Stops at '<' or '{{'.
// ---------------------------------------------------------------------------

static CxymlToken lexText(CxymlLexer *l)
{
    FileLoc    loc   = lexerCurrentLoc(l);
    const char *start = l->cur;

    while (!lexerAtEnd(l)) {
        char c = lexerPeekChar(l);
        if (c == '<')
            break;
        if (c == '{' && (l->cur + 1) < l->end && l->cur[1] == '{')
            break;
        lexerAdvanceChar(l);
    }

    u32 len = (u32)(l->cur - start);
    if (len == 0)
        return makeToken(CXYML_TOK_EOF, l->cur, 0, loc);

    lexerEndLoc(l, &loc);
    return makeToken(CXYML_TOK_TEXT, start, len, loc);
}

// ---------------------------------------------------------------------------
// Interpolation / directive:  {{ ... }}
//
// Peeks at the first word inside {{ ... }} to distinguish control-flow
// directives from plain expression interpolations.
//
// Directive detection (in priority order):
//   /for        →  CTRL_END_FOR  (no payload)
//   /if         →  CTRL_END_IF   (no payload)
//   for         →  CTRL_FOR      (no payload); sets mode = FOR_VAR so that
//                  subsequent cxymlLexerNext() calls produce IDENT, IN, INTERP_EXPR
//   else if     →  CTRL_ELSE_IF  (payload = raw condition)
//   else        →  CTRL_ELSE     (no payload)
//   if          →  CTRL_IF       (payload = raw condition)
//   <anything>  →  INTERP_EXPR   (payload = full raw expression, unchanged)
// ---------------------------------------------------------------------------

static CxymlToken lexInterp(CxymlLexer *l)
{
    // Consume '{{'
    lexerAdvanceChar(l);
    lexerAdvanceChar(l);

    FileLoc     loc       = lexerCurrentLoc(l);
    // Save position right after '{{' — used as the start of plain INTERP_EXPR
    // tokens so that leading whitespace is preserved, e.g. {{ name }} yields
    // the token content " name " just as the original lexer did.
    const char *origStart = l->cur;

    // Skip leading whitespace only to identify the first keyword character.
    // We do NOT move origStart — it stays at the character after '{{'.
    while (!lexerAtEnd(l) && isAsciiSpace(lexerPeekChar(l)))
        lexerAdvanceChar(l);

    if (lexerAtEnd(l))
        return errorToken(l, "cxyml: unterminated interpolation '{{ ... }}'");

    char c = lexerPeekChar(l);

    // ---- Closing directives:  {{ /for }}  and  {{ /if }} ----
    if (c == '/') {
        lexerAdvanceChar(l); // consume '/'

        if (matchKeyword(l, "for", 3)) {
            for (int i = 0; i < 3; i++) lexerAdvanceChar(l);
            if (!skipUntilClose(l))
                return errorToken(l, "cxyml: unterminated '{{ /for }}'");
            lexerEndLoc(l, &loc);
            return makeToken(CXYML_TOK_CTRL_END_FOR, l->cur, 0, loc);
        }
        if (matchKeyword(l, "if", 2)) {
            for (int i = 0; i < 2; i++) lexerAdvanceChar(l);
            if (!skipUntilClose(l))
                return errorToken(l, "cxyml: unterminated '{{ /if }}'");
            lexerEndLoc(l, &loc);
            return makeToken(CXYML_TOK_CTRL_END_IF, l->cur, 0, loc);
        }
        return errorToken(l, "cxyml: unknown closing directive — expected '/for' or '/if'");
    }

    // ---- {{ for <var> in <expr> }} ----
    // Emit CTRL_FOR now; the var/in/expr tokens are produced on the next
    // three calls via the FOR_VAR → FOR_IN → FOR_EXPR mode sequence.
    if (matchKeyword(l, "for", 3)) {
        for (int i = 0; i < 3; i++) lexerAdvanceChar(l);
        // l->cur now points at the space before the loop variable.
        l->mode = CXYML_MODE_FOR_VAR;
        lexerEndLoc(l, &loc);
        return makeToken(CXYML_TOK_CTRL_FOR, l->cur, 0, loc);
    }

    // ---- {{ else if <cond> }}  or  {{ else }} ----
    if (matchKeyword(l, "else", 4)) {
        for (int i = 0; i < 4; i++) lexerAdvanceChar(l);

        // Skip whitespace between 'else' and an optional 'if'
        while (!lexerAtEnd(l) && isAsciiSpace(lexerPeekChar(l)))
            lexerAdvanceChar(l);

        if (matchKeyword(l, "if", 2)) {
            // {{ else if <cond> }} — advance past 'if', capture condition
            for (int i = 0; i < 2; i++) lexerAdvanceChar(l);
            while (!lexerAtEnd(l) && isAsciiSpace(lexerPeekChar(l)))
                lexerAdvanceChar(l);
            FileLoc condLoc = lexerCurrentLoc(l);
            return captureUntilClose(l, CXYML_TOK_CTRL_ELSE_IF, l->cur, condLoc);
        }

        // {{ else }} — discard everything until '}}'
        if (!skipUntilClose(l))
            return errorToken(l, "cxyml: unterminated '{{ else }}'");
        lexerEndLoc(l, &loc);
        return makeToken(CXYML_TOK_CTRL_ELSE, l->cur, 0, loc);
    }

    // ---- {{ if <cond> }} ----
    if (matchKeyword(l, "if", 2)) {
        for (int i = 0; i < 2; i++) lexerAdvanceChar(l);
        while (!lexerAtEnd(l) && isAsciiSpace(lexerPeekChar(l)))
            lexerAdvanceChar(l);
        FileLoc condLoc = lexerCurrentLoc(l);
        return captureUntilClose(l, CXYML_TOK_CTRL_IF, l->cur, condLoc);
    }

    // ---- Regular {{ expr }} ----
    // Pass origStart (position right after '{{') so that leading whitespace is
    // included in the token, preserving the original INTERP_EXPR behaviour.
    return captureUntilClose(l, CXYML_TOK_INTERP_EXPR, origStart, loc);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void cxymlLexerInit(CxymlLexer    *l,
                    const char    *src,
                    size_t         len,
                    const FileLoc *origin,
                    struct Log    *L)
{
    l->src     = src;
    l->cur     = src;
    l->end     = src + len;
    l->origin  = *origin;
    l->row     = origin->begin.row;
    l->col     = origin->begin.col;
    l->hasPeek = false;
    l->mode    = CXYML_MODE_CONTENT;
    l->L       = L;
}

// Advance and return the next token.
CxymlToken cxymlLexerNext(CxymlLexer *l)
{
    if (l->hasPeek) {
        l->hasPeek = false;
        return l->ahead;
    }

    // Skip whitespace in all modes except CONTENT (where whitespace is
    // significant as part of text nodes).
    if (l->mode != CXYML_MODE_CONTENT)
        skipWhitespace(l);

    if (lexerAtEnd(l))
        return makeToken(CXYML_TOK_EOF, l->cur, 0, lexerCurrentLoc(l));

    char c = lexerPeekChar(l);

    // Interpolation / directives:  {{ ... }}
    // Valid in CONTENT mode (text body) and TAG mode (attribute values).
    // NOT reachable in FOR_* modes — those are mid-directive continuations.
    if ((l->mode == CXYML_MODE_CONTENT || l->mode == CXYML_MODE_TAG) &&
         c == '{' && (l->cur + 1) < l->end && l->cur[1] == '{')
    {
        return lexInterp(l);
    }

    switch (l->mode) {

        // --------------------------------------------------------------------
        // CONTENT mode: '<' starts a tag; everything else is text.
        // --------------------------------------------------------------------
        case CXYML_MODE_CONTENT: {
            if (c == '<') {
                FileLoc loc = lexerCurrentLoc(l);
                lexerAdvanceChar(l); // consume '<'

                char next = lexerPeekChar(l);
                if (next == '!') {
                    lexerAdvanceChar(l); // consume '!'
                    return lexComment(l, l->cur - 2, loc);
                }

                l->mode = CXYML_MODE_TAG;
                lexerEndLoc(l, &loc);
                return makeToken(CXYML_TOK_OPEN, l->cur - 1, 1, loc);
            }
            return lexText(l);
        }

        // --------------------------------------------------------------------
        // TAG mode: tokenise tag syntax (names, attributes, delimiters).
        // --------------------------------------------------------------------
        case CXYML_MODE_TAG: {
            if (c == '>') {
                FileLoc loc = lexerCurrentLoc(l);
                lexerAdvanceChar(l);
                l->mode = CXYML_MODE_CONTENT;
                lexerEndLoc(l, &loc);
                return makeToken(CXYML_TOK_CLOSE, l->cur - 1, 1, loc);
            }
            if (c == '/') {
                FileLoc loc = lexerCurrentLoc(l);
                lexerAdvanceChar(l);
                lexerEndLoc(l, &loc);
                return makeToken(CXYML_TOK_SLASH, l->cur - 1, 1, loc);
            }
            if (c == '=') {
                FileLoc loc = lexerCurrentLoc(l);
                lexerAdvanceChar(l);
                lexerEndLoc(l, &loc);
                return makeToken(CXYML_TOK_EQUALS, l->cur - 1, 1, loc);
            }
            if (c == '"' || c == '\'')
                return lexString(l);
            if (isIdentStart(c))
                return lexIdent(l);
            return errorToken(l, "cxyml: unexpected character inside tag");
        }

        // --------------------------------------------------------------------
        // FOR_VAR mode: lex the loop variable identifier.
        // Called immediately after CTRL_FOR is returned.
        // --------------------------------------------------------------------
        case CXYML_MODE_FOR_VAR: {
            if (!isIdentStart(c))
                return errorToken(l, "cxyml: expected loop variable name after 'for'");
            CxymlToken ident = lexIdent(l);
            l->mode = CXYML_MODE_FOR_IN;
            return ident;
        }

        // --------------------------------------------------------------------
        // FOR_IN mode: match the 'in' keyword.
        // --------------------------------------------------------------------
        case CXYML_MODE_FOR_IN: {
            if (!matchKeyword(l, "in", 2))
                return errorToken(l, "cxyml: expected 'in' after loop variable");
            FileLoc loc = lexerCurrentLoc(l);
            lexerAdvanceChar(l); // 'i'
            lexerAdvanceChar(l); // 'n'
            l->mode = CXYML_MODE_FOR_EXPR;
            lexerEndLoc(l, &loc);
            return makeToken(CXYML_TOK_IN, l->cur - 2, 2, loc);
        }

        // --------------------------------------------------------------------
        // FOR_EXPR mode: capture the range expression until '}}'.
        // Consumes '}}' and returns to CONTENT mode.
        // --------------------------------------------------------------------
        case CXYML_MODE_FOR_EXPR: {
            FileLoc     loc   = lexerCurrentLoc(l);
            const char *start = l->cur;
            CxymlToken tok = captureUntilClose(l, CXYML_TOK_INTERP_EXPR, start, loc);
            if (tok.type != CXYML_TOK_ERROR)
                l->mode = CXYML_MODE_CONTENT;
            return tok;
        }

    } // switch

    // Unreachable — all enum values are handled above.
    return errorToken(l, "cxyml: internal lexer error (unknown mode)");
}

// Peek at the next token without consuming it.
CxymlToken cxymlLexerPeek(CxymlLexer *l)
{
    if (!l->hasPeek) {
        l->ahead   = cxymlLexerNext(l);
        l->hasPeek = true;
    }
    return l->ahead;
}

// Check whether the next token matches the given type.
bool cxymlLexerCheck(CxymlLexer *l, CxymlTokenType type)
{
    return cxymlLexerPeek(l).type == type;
}

// Consume the next token and return true only if its type matches.
bool cxymlLexerMatch(CxymlLexer *l, CxymlTokenType expected, CxymlToken *out)
{
    CxymlToken t = cxymlLexerPeek(l);
    if (t.type != expected)
        return false;
    if (out)
        *out = t;
    cxymlLexerNext(l); // consume
    return true;
}

// Consume the next token and emit an error if it does not match.
bool cxymlLexerExpect(CxymlLexer *l, CxymlTokenType expected, CxymlToken *out)
{
    CxymlToken t = cxymlLexerNext(l);
    if (t.type != expected) {
        logError(l->L, &t.loc,
                 "cxyml: unexpected token (got {i32}, expected {i32})",
                 (FormatArg[]){{.i32 = (i32)t.type},
                               {.i32 = (i32)expected}});
        return false;
    }
    if (out)
        *out = t;
    return true;
}