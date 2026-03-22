//
// Cxyml plugin - Parser
//
// Builds a CxymlNode tree from the token stream produced by the lexer.
//
// Grammar (simplified EBNF):
//
//   document    ::= node*
//   node        ::= element | text | interp | for_dir | if_dir
//   element     ::= self_closing | open_element
//   self_closing ::= '<' IDENT attr* '/>'
//   open_element ::= '<' IDENT attr* '>' node* '</' IDENT '>'
//   attr         ::= IDENT ('=' (STRING | interp))?
//   text         ::= TEXT
//   interp       ::= INTERP_EXPR
//   for_dir      ::= CTRL_FOR IDENT IN INTERP_EXPR node* CTRL_END_FOR
//   if_dir       ::= CTRL_IF INTERP_EXPR node* else_chain CTRL_END_IF
//   else_chain   ::= (CTRL_ELSE_IF INTERP_EXPR node* else_chain)?
//                  | (CTRL_ELSE node*)?
//                  | ε
//
// Component vs built-in distinction:
//   Tags beginning with an uppercase letter are treated as custom components
//   (Uppercase = call .render(os)), lowercase = emit raw HTML strings.
//
// Expression parsing is NOT done here - raw expression strings captured
// inside {{ }} are passed to cxyParseExpression() in the codegen phase.
// CXY provides its own parser/lexer for that purpose.
//

#include "state.h"

#include <cxy/core/log.h>
#include <cxy/core/mempool.h>
#include <cxy/core/strpool.h>

#include <string.h>

// Internal parser context - stack allocated, not stored in CxymlState
typedef struct {
    CxymlLexer *lexer;
    MemPool    *pool;    // ctx->pool - allocations have AstNode lifetime
    StrPool    *strings;
    struct Log *L;
} CxymlParser;

// ============================================================================
// Parser helpers
// ============================================================================

static CxymlToken parserPeek(CxymlParser *p)
{
    return cxymlLexerPeek(p->lexer);
}

static CxymlToken parserNext(CxymlParser *p)
{
    return cxymlLexerNext(p->lexer);
}

static bool parserCheck(CxymlParser *p, CxymlTokenType type)
{
    return cxymlLexerCheck(p->lexer, type);
}

static bool parserMatch(CxymlParser *p, CxymlTokenType type, CxymlToken *out)
{
    return cxymlLexerMatch(p->lexer, type, out);
}

static bool parserExpect(CxymlParser *p, CxymlTokenType type, CxymlToken *out)
{
    return cxymlLexerExpect(p->lexer, type, out);
}

// Allocate a CxymlNode from the AstNode cache in ctx->pool.
// sizeof(CxymlNode) == sizeof(AstNode) by design (union overlay).
// The tag field encodes the cxyml node kind using CXYML_TAG_* sentinel values.
static CxymlNode *allocNode(CxymlParser *p, CxymlNodeKind kind, FileLoc loc)
{
    CxymlNode *node = allocFromCacheOrPool(p->pool, memAstNode, sizeof(AstNode));
    memset(node, 0, sizeof(AstNode));

    switch (kind) {
        case CXYML_NODE_ELEMENT: node->tag = CXYML_TAG_ELEMENT; break;
        case CXYML_NODE_TEXT:    node->tag = CXYML_TAG_TEXT;    break;
        case CXYML_NODE_INTERP:  node->tag = CXYML_TAG_INTERP;  break;
        case CXYML_NODE_FOR:     node->tag = CXYML_TAG_FOR;     break;
        case CXYML_NODE_IF:      node->tag = CXYML_TAG_IF;      break;
    }

    node->loc = loc;
    return node;
}

// Intern a sized string from a token into the string pool
static const char *internToken(CxymlParser *p, const CxymlToken *tok)
{
    return makeStringSized(p->strings, tok->start, tok->len);
}

// Trim whitespace from both ends of a string, collapse internal runs to one
// space, and intern the result into the string pool.
// When trimTrailing is false the single trailing space (if any) is kept —
// this preserves the space between text and a following interpolation.
static const char *normaliseText(CxymlParser *p,
                                  const char  *src,
                                  u32          len,
                                  bool         trimTrailing)
{
    // Build a temporary buffer on the stack (text nodes are typically short).
    // For very long text the pool is used as scratch space.
    char  *buf  = allocFromMemPool(p->pool, len + 1);
    u32    out  = 0;
    bool   inWs = true; // start as true to trim leading whitespace

    for (u32 i = 0; i < len; i++) {
        char c = src[i];
        if (isAsciiSpace(c)) {
            if (!inWs) {
                buf[out++] = ' ';
                inWs       = true;
            }
        }
        else {
            buf[out++] = c;
            inWs       = false;
        }
    }

    // Trim trailing space only when the caller requests it.
    // When text is followed by an interpolation the trailing space is
    // meaningful content and must be preserved.
    if (trimTrailing && out > 0 && buf[out - 1] == ' ')
        out--;

    buf[out] = '\0';
    return makeStringSized(p->strings, buf, out);
}

// ============================================================================
// Attribute parsing
// ============================================================================

//  attr  ::= IDENT ('=' (STRING | interp))?
static CxymlAttr *parseAttr(CxymlParser *p)
{
    CxymlToken nameTok;
    if (!parserExpect(p, CXYML_TOK_IDENT, &nameTok))
        return NULL;

    CxymlAttr *attr  = callocFromMemPool(p->pool, 1, sizeof(CxymlAttr));
    attr->name       = internToken(p, &nameTok);
    attr->nameLoc    = nameTok.loc;

    if (!parserMatch(p, CXYML_TOK_EQUALS, NULL)) {
        // Boolean flag attribute - no value
        return attr;
    }

    // Value is either a quoted string or an interpolation
    CxymlToken valTok = parserPeek(p);

    if (valTok.type == CXYML_TOK_STRING) {
        parserNext(p); // consume
        attr->value    = internToken(p, &valTok);
        attr->valueLoc = valTok.loc;
    }
    else if (valTok.type == CXYML_TOK_INTERP_EXPR) {
        parserNext(p); // consume
        attr->interpExpr = internToken(p, &valTok);
        attr->valueLoc   = valTok.loc;
    }
    else {
        logError(p->L, &valTok.loc,
                 "cxyml: expected string or '{{ expr }}' as attribute value",
                 NULL);
        return NULL;
    }

    return attr;
}

// ============================================================================
// Node parsing  (forward declaration for recursion)
// ============================================================================

static CxymlNode *parseNode(CxymlParser *p);

// ============================================================================
// Element parsing
// ============================================================================

//  element ::= '<' IDENT attr* ( '/>' | '>' node* '</' IDENT '>' )
static CxymlNode *parseElement(CxymlParser *p)
{
    CxymlToken openTok;
    if (!parserExpect(p, CXYML_TOK_OPEN, &openTok))
        return NULL;

    // Skip comment tokens that slipped through - they have no effect
    while (parserCheck(p, CXYML_TOK_COMMENT))
        parserNext(p);

    CxymlToken tagTok;
    if (!parserExpect(p, CXYML_TOK_IDENT, &tagTok))
        return NULL;

    const char *tag = internToken(p, &tagTok);

    CxymlNode *node              = allocNode(p, CXYML_NODE_ELEMENT, openTok.loc);
    node->element.tag            = tag;
    node->element.isComponent    = (tag[0] >= 'A' && tag[0] <= 'Z');

    // Parse attributes
    CxymlAttr *attrHead = NULL;
    CxymlAttr *attrTail = NULL;

    while (!parserCheck(p, CXYML_TOK_CLOSE) &&
           !parserCheck(p, CXYML_TOK_SLASH) &&
           !parserCheck(p, CXYML_TOK_EOF) &&
           !parserCheck(p, CXYML_TOK_ERROR))
    {
        CxymlAttr *attr = parseAttr(p);
        if (attr == NULL)
            return NULL;

        if (attrTail) {
            attrTail->next = attr;
            attrTail       = attr;
        }
        else {
            attrHead = attrTail = attr;
        }
    }
    node->element.attrs = attrHead;

    // Self-closing:  '/>'
    if (parserMatch(p, CXYML_TOK_SLASH, NULL)) {
        if (!parserExpect(p, CXYML_TOK_CLOSE, NULL))
            return NULL;
        node->element.selfClosing = true;
        return node;
    }

    // Opening tag closed with '>'
    if (!parserExpect(p, CXYML_TOK_CLOSE, NULL))
        return NULL;

    // Parse children until we hit '</'
    CxymlNode *childHead = NULL;
    CxymlNode *childTail = NULL;

    while (!parserCheck(p, CXYML_TOK_EOF) &&
           !parserCheck(p, CXYML_TOK_ERROR))
    {
        CxymlToken peek = parserPeek(p);
        if (peek.type == CXYML_TOK_OPEN) {
            // After peeking OPEN, l->cur has advanced past '<'.
            // Check the raw next character to distinguish closing tag from child.
            if (cxymlLexerPeekChar(p->lexer) == '/') {
                // This is '</tag>' - consume OPEN, SLASH, IDENT, CLOSE
                parserNext(p); // consume OPEN
                parserNext(p); // consume '/'

                CxymlToken closingTag;
                if (!parserExpect(p, CXYML_TOK_IDENT, &closingTag))
                    return NULL;

                const char *closingName = internToken(p, &closingTag);
                if (closingName != tag) {
                    logError(p->L, &closingTag.loc,
                             "cxyml: mismatched closing tag: expected '</{s}>' but found '</{s}>'",
                             (FormatArg[]){{.s = tag}, {.s = closingName}});
                    return NULL;
                }

                if (!parserExpect(p, CXYML_TOK_CLOSE, NULL))
                    return NULL;

                break; // closing tag consumed - done with children
            }

            // Child element
            CxymlNode *child = parseElement(p);
            if (child == NULL)
                return NULL;

            if (childTail) {
                childTail->next = (AstNode *)child;
                childTail       = child;
            }
            else {
                childHead = childTail = child;
            }
            continue;
        }

        CxymlNode *child = parseNode(p);
        if (child == NULL)
            continue; // discarded node (whitespace-only text, comment, EOF) - keep scanning

        if (childTail) {
            childTail->next = (AstNode *)child;
            childTail       = child;
        }
        else {
            childHead = childTail = child;
        }
    }

    node->element.children = childHead;
    return node;
}

// ============================================================================
// Text / interpolation node parsing
// ============================================================================

static CxymlNode *parseTextNode(CxymlParser *p)
{
    CxymlToken  tok  = parserNext(p);
    // Preserve trailing space when the next token is an interpolation so that
    // "Hello {{ name }}" renders as "Hello World" and not "HelloWorld".
    bool trimTrailing = !parserCheck(p, CXYML_TOK_INTERP_EXPR);
    const char *normalised = normaliseText(p, tok.start, tok.len, trimTrailing);

    // Whitespace-only text: keep as a single space when sitting between inline
    // content (next token is an interpolation), e.g. "{{ a }} {{ b }}" must
    // emit a space between the two expressions.  Otherwise discard it.
    if (normalised[0] == '\0') {
        if (parserCheck(p, CXYML_TOK_INTERP_EXPR))
            normalised = makeStringSized(p->strings, " ", 1);
        else
            return NULL;
    }

    CxymlNode *node  = allocNode(p, CXYML_NODE_TEXT, tok.loc);
    node->text.value = normalised;
    return node;
}

static CxymlNode *parseInterpNode(CxymlParser *p)
{
    CxymlToken tok = parserNext(p); // CXYML_TOK_INTERP_EXPR
    CxymlNode *node = allocNode(p, CXYML_NODE_INTERP, tok.loc);
    node->interp.expr    = internToken(p, &tok);
    node->interp.exprLoc = tok.loc;
    return node;
}

// ============================================================================
// Helper: collect sibling nodes into a linked list until a stop condition.
//
// Reads nodes via parseNode() and links them via ->next.
// Returns the head of the list (or NULL if no nodes were collected).
// Does NOT consume the stopping token — the caller must do that.
// ============================================================================

// Stop-condition callback: returns true when the current peek token should
// terminate the body collection loop.
typedef bool (*BodyStopFn)(CxymlParser *p);

static CxymlNode *parseBody(CxymlParser *p, BodyStopFn shouldStop)
{
    CxymlNode *head = NULL;
    CxymlNode *tail = NULL;

    while (!shouldStop(p) &&
           !parserCheck(p, CXYML_TOK_EOF) &&
           !parserCheck(p, CXYML_TOK_ERROR))
    {
        CxymlNode *child = parseNode(p);
        if (child == NULL)
            continue;

        if (tail) {
            tail->next = (AstNode *)child;
            tail       = child;
        }
        else {
            head = tail = child;
        }
    }

    return head;
}

// ============================================================================
// For directive   {{ for var in expr }} … {{ /for }}
// ============================================================================

static bool stopAtEndFor(CxymlParser *p)
{
    return parserCheck(p, CXYML_TOK_CTRL_END_FOR);
}

//  for_dir ::= CTRL_FOR IDENT IN INTERP_EXPR body CTRL_END_FOR
static CxymlNode *parseForDirective(CxymlParser *p)
{
    CxymlToken forTok;
    if (!parserExpect(p, CXYML_TOK_CTRL_FOR, &forTok))
        return NULL;

    // Loop variable identifier  (produced by the lexer's FOR_VAR mode)
    CxymlToken varTok;
    if (!parserExpect(p, CXYML_TOK_IDENT, &varTok))
        return NULL;

    // Structural 'in' keyword  (produced by the lexer's FOR_IN mode)
    if (!parserExpect(p, CXYML_TOK_IN, NULL))
        return NULL;

    // Range expression  (produced by the lexer's FOR_EXPR mode)
    CxymlToken exprTok;
    if (!parserExpect(p, CXYML_TOK_INTERP_EXPR, &exprTok))
        return NULL;

    // Body nodes until {{ /for }}
    CxymlNode *body = parseBody(p, stopAtEndFor);

    if (!parserExpect(p, CXYML_TOK_CTRL_END_FOR, NULL))
        return NULL;

    CxymlNode *node          = allocNode(p, CXYML_NODE_FOR, forTok.loc);
    node->forNode.iterVar    = internToken(p, &varTok);
    node->forNode.iterExpr   = internToken(p, &exprTok);
    node->forNode.exprLoc    = exprTok.loc;
    node->forNode.body       = body;
    return node;
}

// ============================================================================
// If directive   {{ if cond }} … {{ else if cond }} … {{ else }} … {{ /if }}
//
// parseIfDirective() handles all three branch kinds:
//
//   CTRL_IF       → condition from token payload, recurse for else chain
//   CTRL_ELSE_IF  → same
//   CTRL_ELSE     → condition = NULL, consume body until CTRL_END_IF
//
// The function is called recursively to build the else-if / else chain as a
// linked list of CXYML_NODE_IF nodes via the elseNode pointer.
// ============================================================================

// Returns true when the current token should end an if-branch body.
static bool stopAtIfBranch(CxymlParser *p)
{
    return parserCheck(p, CXYML_TOK_CTRL_ELSE_IF) ||
           parserCheck(p, CXYML_TOK_CTRL_ELSE)    ||
           parserCheck(p, CXYML_TOK_CTRL_END_IF);
}

// Returns true when the current token should end an else body.
static bool stopAtEndIf(CxymlParser *p)
{
    return parserCheck(p, CXYML_TOK_CTRL_END_IF);
}

// Forward declaration so parseIfBranchDirective can call itself recursively.
static CxymlNode *parseIfBranchDirective(CxymlParser *p);

// Parse a single if/else-if/else branch plus any following branches.
// On entry, the next token is CTRL_IF, CTRL_ELSE_IF, or CTRL_ELSE.
static CxymlNode *parseIfBranchDirective(CxymlParser *p)
{
    CxymlToken tok = parserNext(p); // consume CTRL_IF / CTRL_ELSE_IF / CTRL_ELSE

    // Extract condition from token payload (CTRL_ELSE has none)
    const char *condition = NULL;
    FileLoc     condLoc   = tok.loc;

    if (tok.type == CXYML_TOK_CTRL_IF || tok.type == CXYML_TOK_CTRL_ELSE_IF) {
        if (tok.len == 0) {
            logError(p->L, &tok.loc,
                     "cxyml: empty condition in '{{ if }}' / '{{ else if }}'",
                     NULL);
            return NULL;
        }
        condition = internToken(p, &tok);
        condLoc   = tok.loc;
    }

    // Parse body nodes — stop condition depends on which branch kind this is.
    BodyStopFn stopFn = (tok.type == CXYML_TOK_CTRL_ELSE) ? stopAtEndIf
                                                           : stopAtIfBranch;
    CxymlNode *body = parseBody(p, stopFn);

    // Build the node before recursing so its address is stable.
    CxymlNode *node         = allocNode(p, CXYML_NODE_IF, tok.loc);
    node->ifNode.condition  = condition;
    node->ifNode.condLoc    = condLoc;
    node->ifNode.thenBody   = body;
    node->ifNode.elseNode   = NULL;

    // Recurse into the next branch or consume the closing {{ /if }}.
    if (tok.type != CXYML_TOK_CTRL_ELSE) {
        if (parserCheck(p, CXYML_TOK_CTRL_ELSE_IF)) {
            node->ifNode.elseNode = parseIfBranchDirective(p);
        }
        else if (parserCheck(p, CXYML_TOK_CTRL_ELSE)) {
            node->ifNode.elseNode = parseIfBranchDirective(p);
        }
        else {
            // No else branch — consume {{ /if }}
            if (!parserExpect(p, CXYML_TOK_CTRL_END_IF, NULL))
                return NULL;
        }
    }
    else {
        // This IS the else branch — consume {{ /if }}
        if (!parserExpect(p, CXYML_TOK_CTRL_END_IF, NULL))
            return NULL;
    }

    return node;
}

// Public entry called from parseNode() when CTRL_IF is seen.
static CxymlNode *parseIfDirective(CxymlParser *p)
{
    return parseIfBranchDirective(p);
}

// ============================================================================
// Top-level node dispatch
// ============================================================================

static CxymlNode *parseNode(CxymlParser *p)
{
    CxymlToken peek = parserPeek(p);

    switch (peek.type) {
        case CXYML_TOK_OPEN:
            return parseElement(p);

        case CXYML_TOK_TEXT:
            return parseTextNode(p);

        case CXYML_TOK_INTERP_EXPR:
            return parseInterpNode(p);

        case CXYML_TOK_COMMENT:
            // Discard comments silently
            parserNext(p);
            return NULL;

        // ---- Control-flow directives ----
        case CXYML_TOK_CTRL_FOR:
            return parseForDirective(p);

        case CXYML_TOK_CTRL_IF:
            return parseIfDirective(p);

        // These close their parent directive's body — never consumed here.
        // They would only reach parseNode() if they appear at the document
        // root or inside an element where they don't belong.  Log an error,
        // consume the token to avoid an infinite loop, and return NULL.
        case CXYML_TOK_CTRL_ELSE_IF:
        case CXYML_TOK_CTRL_ELSE:
        case CXYML_TOK_CTRL_END_IF:
        case CXYML_TOK_CTRL_END_FOR:
            logError(p->L, &peek.loc,
                     "cxyml: unexpected '{s}' directive outside a directive body",
                     (FormatArg[]){{.s = peek.type == CXYML_TOK_CTRL_ELSE_IF  ? "else if"
                                       : peek.type == CXYML_TOK_CTRL_ELSE     ? "else"
                                       : peek.type == CXYML_TOK_CTRL_END_IF   ? "/if"
                                                                               : "/for"}});
            parserNext(p); // consume to avoid infinite loop
            return NULL;

        case CXYML_TOK_EOF:
            return NULL;

        default:
            logError(p->L, &peek.loc,
                     "cxyml: unexpected token in content",
                     NULL);
            parserNext(p); // consume so we don't loop forever
            return NULL;
    }
}

// ============================================================================
// Public API
// ============================================================================

// Parse a complete markup document and return the first root node.
// Multiple root nodes are linked via ->next.
// Returns NULL only on a hard error (error token with no progress possible).
CxymlNode *cxymlParse(CxymlLexer *lexer,
                      MemPool    *pool,
                      StrPool    *strings,
                      struct Log *L)
{
    CxymlParser p_ = {.lexer = lexer, .pool = pool, .strings = strings, .L = L};
    CxymlParser *p = &p_;
    CxymlNode *head = NULL;
    CxymlNode *tail = NULL;

    while (!parserCheck(p, CXYML_TOK_EOF) &&
           !parserCheck(p, CXYML_TOK_ERROR))
    {
        CxymlNode *node = parseNode(p);
        if (node == NULL)
            continue; // whitespace-only text or comment - skip

        if (tail) {
            tail->next = (AstNode *)node;
            tail       = node;
        }
        else {
            head = tail = node;
        }
    }

    return head;
}