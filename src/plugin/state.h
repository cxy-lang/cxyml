//
// Created for cxyml plugin
//
// CxymlNode is designed as a union with AstNode so that:
//   1. It has exactly the same size as AstNode (no extra allocations)
//   2. It uses the AstNode header (CXY_AST_NODE_HEAD) so tag/loc/flags
//      are already in the right place for the compiler
//   3. A parsed CxymlNode can be directly cast to AstNode* and handed
//      back to the Cxy compiler without copying
//
// Expression parsing (interpolations) is delegated entirely to CXY via
// cxyParseExpression(ctx, code, origin). The plugin only lexes and parses
// the XML-like markup structure; it has no CXY parser/lexer of its own.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include <cxy/core/htable.h>
#include <cxy/core/mempool.h>
#include <cxy/core/strpool.h>
#include <cxy/core/log.h>
#include <cxy/ast.h>
#include <cxy/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// ASCII-only character classification
//
// The standard ctype functions (isspace, isalpha, …) are locale-dependent.
// On macOS with a UTF-8 locale, isspace() returns true for certain bytes
// > 0x7F that happen to be UTF-8 continuation bytes, corrupting multi-byte
// characters like → (U+2192 = E2 86 92).  All cxyml whitespace and
// identifier characters are ASCII, so plain range checks are correct and
// portable.
// ============================================================================

static inline bool isAsciiSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static inline bool isAsciiAlpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline bool isAsciiAlnum(char c)
{
    return isAsciiAlpha(c) || (c >= '0' && c <= '9');
}

// ============================================================================
// Lexer mode
//
// Replaces the old `bool inTag` flag with a proper enum so that the
// for-header sub-lexer can drive token production across multiple calls
// to cxymlLexerNext().
//
//   CONTENT    – default; between element tags
//   TAG        – inside < … > (was inTag = true)
//   FOR_VAR    – inside {{ for … }}; next token is the loop variable IDENT
//   FOR_IN     – after the loop variable; next token is the 'in' keyword
//   FOR_EXPR   – after 'in'; next token is INTERP_EXPR (the range expression)
// ============================================================================

typedef enum {
    CXYML_MODE_CONTENT  = 0,  // between elements         (was inTag = false)
    CXYML_MODE_TAG,           // inside < … >             (was inTag = true)
    CXYML_MODE_FOR_VAR,       // after {{ for             — emit loop var IDENT
    CXYML_MODE_FOR_IN,        // after loop var IDENT     — emit 'in' token
    CXYML_MODE_FOR_EXPR,      // after 'in'               — emit range INTERP_EXPR
} CxymlLexMode;

// ============================================================================
// Lexer
// ============================================================================

typedef enum {
    CXYML_TOK_EOF,
    CXYML_TOK_OPEN,          // <
    CXYML_TOK_CLOSE,         // >
    CXYML_TOK_SLASH,         // /
    CXYML_TOK_BANG,          // !
    CXYML_TOK_EQUALS,        // =
    CXYML_TOK_IDENT,         // tag name or attribute name
    CXYML_TOK_STRING,        // "..." or '...'
    CXYML_TOK_TEXT,          // plain text content between tags
    CXYML_TOK_INTERP_OPEN,   // {{
    CXYML_TOK_INTERP_EXPR,   // raw expression captured between {{ and }}
    CXYML_TOK_INTERP_CLOSE,  // }}
    CXYML_TOK_COMMENT,       // <!-- ... --> (will be discarded)

    // ---- Structural token emitted inside {{ for … }} headers ----
    CXYML_TOK_IN,            // the keyword 'in' between loop var and range

    // ---- Control-flow directive tokens ----
    // {{ for <var> in <expr> }}  →  CTRL_FOR, IDENT, IN, INTERP_EXPR
    // {{ /for }}                 →  CTRL_END_FOR
    // {{ if <expr> }}            →  CTRL_IF   (payload = condition expr)
    // {{ else if <expr> }}       →  CTRL_ELSE_IF (payload = condition expr)
    // {{ else }}                 →  CTRL_ELSE
    // {{ /if }}                  →  CTRL_END_IF
    CXYML_TOK_CTRL_FOR,
    CXYML_TOK_CTRL_END_FOR,
    CXYML_TOK_CTRL_IF,       // start/len carry the raw condition expression
    CXYML_TOK_CTRL_ELSE_IF,  // start/len carry the raw condition expression
    CXYML_TOK_CTRL_ELSE,
    CXYML_TOK_CTRL_END_IF,

    CXYML_TOK_ERROR,
} CxymlTokenType;

typedef struct {
    CxymlTokenType type;
    const char    *start;   // pointer into source buffer (not NUL-terminated)
    u32            len;
    FileLoc        loc;     // exact location in original source
} CxymlToken;

typedef struct {
    // Source
    const char *src;        // full source buffer
    const char *cur;        // current read position
    const char *end;        // one past last byte

    // Location tracking - advanced as characters are consumed
    FileLoc     origin;     // base location (call-site for inline, {file,1,1} for .cxyml)
    u32         row;        // current row  (mirrors FilePos.row)
    u32         col;        // current col  (mirrors FilePos.col)

    // Lexer mode - controls what cxymlLexerNext() produces.
    // Replaces the old `bool inTag` flag.
    CxymlLexMode mode;

    // One token of lookahead
    CxymlToken  ahead;
    bool        hasPeek;

    // Diagnostics
    struct Log *L;
} CxymlLexer;

// ============================================================================
// Parse tree
// ============================================================================

typedef enum {
    CXYML_NODE_ELEMENT,
    CXYML_NODE_TEXT,
    CXYML_NODE_INTERP,
    CXYML_NODE_FOR,   // {{ for var in expr }} … {{ /for }}
    CXYML_NODE_IF,    // {{ if expr }} … {{ else if }} … {{ else }} … {{ /if }}
} CxymlNodeKind;

// Attribute on an element.
// Attributes are lightweight and only live during the parse→codegen phase,
// so they are allocated from ctx->pool but are not AstNodes themselves.
typedef struct CxymlAttr {
    const char       *name;
    FileLoc           nameLoc;

    // Exactly one of value/interpExpr is set
    const char       *value;       // static string value (NUL-terminated, trimmed)
    const char       *interpExpr;  // raw expression text captured from {{ ... }}
    FileLoc           valueLoc;    // location of the value / interpolation

    struct CxymlAttr *next;
} CxymlAttr;

// CxymlNode is a union with AstNode so that:
//   - It occupies exactly sizeof(AstNode) bytes (allocated from ctx->pool
//     via allocFromCacheOrPool just like a real AstNode)
//   - CXY_AST_NODE_HEAD fields (tag, loc, flags, next, …) are accessible
//     directly as node->tag, node->loc, node->next etc.
//   - When codegen is done the same memory is handed to the Cxy compiler
//     via a plain (AstNode *) cast - no copy, no extra allocation
//
// IMPORTANT: the `_` member must NEVER be accessed directly.
//            It exists solely to make sizeof(CxymlNode) == sizeof(AstNode).
//            To obtain an AstNode* always cast:  (AstNode *)node
typedef union CxymlNode {
    // Sizing anchor only - DO NOT access this field directly
    AstNode _;

    struct {
        // ---- AstNode header (same layout as CXY_AST_NODE_HEAD) ----
        CXY_AST_NODE_HEAD

        // ---- Cxyml-specific payload (active during parse phase) ----
        union {
            // CXYML_NODE_ELEMENT
            struct {
                const char       *tag;         // interned tag name
                bool              selfClosing;
                bool              isComponent; // true when tag[0] is uppercase
                CxymlAttr        *attrs;
                union CxymlNode  *children;    // first child (siblings via ->next)
            } element;

            // CXYML_NODE_TEXT
            struct {
                const char *value;  // trimmed, whitespace-collapsed text
            } text;

            // CXYML_NODE_INTERP
            struct {
                const char *expr;    // raw expression string → cxyParseExpression()
                FileLoc     exprLoc; // location of expression inside {{ }}
            } interp;

            // CXYML_NODE_FOR
            // Represents:  {{ for iterVar in iterExpr }} body {{ /for }}
            // iterVar  is a plain identifier (interned name string).
            // iterExpr is the raw CXY range expression forwarded to
            //          cxyParseExpression() in the codegen phase.
            struct {
                const char       *iterVar;   // loop variable name (interned)
                const char       *iterExpr;  // raw range expression
                FileLoc           exprLoc;   // source location of iterExpr
                union CxymlNode  *body;      // first body node (siblings via ->next)
            } forNode;

            // CXYML_NODE_IF
            // Represents one branch of an if / else-if / else chain.
            //   condition == NULL  →  bare else branch (no expression)
            //   condition != NULL  →  if or else-if branch
            //   elseNode  != NULL  →  another CXYML_NODE_IF for the next branch
            struct {
                const char       *condition;  // raw CXY condition (NULL for else)
                FileLoc           condLoc;    // source location of condition
                union CxymlNode  *thenBody;   // first body node for this branch
                union CxymlNode  *elseNode;   // next branch node, or NULL
            } ifNode;
        };
    };
} CxymlNode;

// Convenience: the CxymlNodeKind is stored in the AstTag field (tag).
// We reserve a block of values above the existing CXY tags.
// The tag field is used only during the parse phase; after conversion it
// will be overwritten with a real AstTag by the codegen pass.
#define CXYML_TAG_ELEMENT  ((AstTag)0xC001)
#define CXYML_TAG_TEXT     ((AstTag)0xC002)
#define CXYML_TAG_INTERP   ((AstTag)0xC003)
#define CXYML_TAG_FOR      ((AstTag)0xC004)
#define CXYML_TAG_IF       ((AstTag)0xC005)

static inline CxymlNodeKind cxymlNodeKind(const CxymlNode *n)
{
    switch ((u32)n->tag) {
        case 0xC001: return CXYML_NODE_ELEMENT;
        case 0xC002: return CXYML_NODE_TEXT;
        case 0xC003: return CXYML_NODE_INTERP;
        case 0xC004: return CXYML_NODE_FOR;
        case 0xC005: return CXYML_NODE_IF;
        default:     return CXYML_NODE_INTERP; // fallback (should not happen)
    }
}

// ============================================================================
// File cache
// ============================================================================

typedef struct {
    const char *path;    // interned - pointer equality is valid
    time_t      mtime;
    CxymlNode  *tree;    // first root node (siblings via ->next), from ctx->pool
} CxymlCachedFile;

static bool cxymlCacheCompare(const void *lhs, const void *rhs)
{
    // Paths are interned so pointer equality is sufficient
    return ((const CxymlCachedFile *)lhs)->path ==
           ((const CxymlCachedFile *)rhs)->path;
}

// ============================================================================
// Plugin state  (registered via cxyPluginInitialize - owned by CXY)
// ============================================================================

typedef struct {
    CxymlLexer lexer;      // reusable markup lexer - reinitialised per render! call
    HashTable  fileCache;  // HashTable<CxymlCachedFile>, backed by ctx->pool
} CxymlState;

// ============================================================================
// Lexer inline helpers
// ============================================================================

// Peek at the raw next character in the source buffer without advancing.
// Safe to call after cxymlLexerPeek() has returned CXYML_TOK_OPEN - at that
// point l->cur has already advanced past '<' so this returns the character
// immediately following '<', letting the parser distinguish '<tag' from '</'.
static inline char cxymlLexerPeekChar(const CxymlLexer *l)
{
    return (l->cur < l->end) ? *l->cur : '\0';
}

// ============================================================================
// Lexer function declarations
// ============================================================================

void cxymlLexerInit(CxymlLexer    *l,
                    const char    *src,
                    size_t         len,
                    const FileLoc *origin,
                    struct Log    *L);

CxymlToken cxymlLexerNext(CxymlLexer *l);
CxymlToken cxymlLexerPeek(CxymlLexer *l);
bool       cxymlLexerCheck(CxymlLexer *l, CxymlTokenType type);
bool       cxymlLexerMatch(CxymlLexer *l, CxymlTokenType expected, CxymlToken *out);
bool       cxymlLexerExpect(CxymlLexer *l, CxymlTokenType expected, CxymlToken *out);

// ============================================================================
// Parser function declarations
// ============================================================================

CxymlNode *cxymlParse(CxymlLexer *lexer,
                      MemPool    *pool,
                      StrPool    *strings,
                      struct Log *L);

// ============================================================================
// Codegen function declarations
// ============================================================================

// Generate a linked list of CXY AstNode statements from a parsed CxymlNode
// tree.  osRef is an AstNode representing the OutputStream parameter that
// receives the markup writes.  Returns NULL on error (error already logged).
AstNode *cxymlGenerate(CxyPluginContext *ctx,
                       const CxymlNode *tree,
                       AstNode         *osRef);

#ifdef __cplusplus
}
#endif