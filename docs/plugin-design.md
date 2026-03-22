# Cxyml Plugin Design Document

## Overview

Cxyml is a compile-time plugin for the Cxy language that enables **React-like server-side rendering (SSR)**. 
Components are Cxy classes that extend `View` and implement a `render` method. The `cxyml.render!` plugin 
action transforms XML-like markup declared inside `render` into efficient Cxy code that writes HTML directly 
to an `OutputStream`.

**Key Principles:**
- Components are classes, not templates
- Rendering writes HTML strings to a stream (SSR)
- Markup is compiled away at compile time - zero runtime parsing
- Composable: components can use other components in their markup
- Interpolations use Cxy's own parser - no custom expression engine needed

---

## The Component Model

### Defining a Component

```cxy
import plugin "cxyml" as cxyml
import { View } from "cxyml/view.cxy"

pub class MyPage: View {
    - _title: String;
    - _subtitle: String;

    func `init`(title: String, subtitle: String) {
        _title = &&title
        _subtitle = &&subtitle
    }

    @override
    func render(os: &OutputStream): void {
        cxyml::render("""
            <div class="page">
                <h1>{{ _title }}</h1>
                <p>{{ _subtitle }}</p>
            </div>
        """)
    }
}
```

### Using a Component Inside Another

```cxy
import plugin "cxyml" as cxyml
import { View } from "cxyml/view.cxy"
import { MyPage } from "./mypage.cxy"
import { Header, Footer } from "./layout.cxy"

pub class AppShell: View {
    @override
    func render(os: &OutputStream): void {
        cxyml::render("""
            <div class="app">
                <Header />
                <MyPage title="Hello" subtitle="Welcome to Cxyml" />
                <Footer />
            </div>
        """)
    }
}
```

### Passing Props

Props are passed as attributes on the component tag. The plugin maps them to constructor arguments:

```xml
<MyPage title={{ pageTitle }} subtitle="A static subtitle" />
```

Generates:

```cxy
var _cxyml1 = MyPage(pageTitle, "A static subtitle".S)
_cxyml1.render(os)
```

---

## Plugin Architecture

### Plugin State

The plugin maintains a custom state object across all action invocations. This is initialised
once in `pluginInit` and retrieved in every action via `pluginState()`.

All allocations use `ctx->pool` - the same `MemPool *` CXY uses for AST nodes - so the
plugin never manages its own heap memory.

```c
#include "core/mempool.h"
#include "core/htable.h"
#include "core/array.h"

// Key used to look up cached files in the HashTable
typedef struct CxymlCacheKey {
    const char *path;    // interned path string (pointer equality is sufficient)
    time_t      mtime;   // last modified time at parse time
} CxymlCacheKey;

// Value stored in the file cache
typedef struct CxymlCachedFile {
    CxymlCacheKey key;
    CxymlNode    *tree;  // parsed node tree, allocated from ctx->pool
} CxymlCachedFile;

// Plugin-wide state - lives for the entire compilation
typedef struct CxymlState {
    CxymlLexer  lexer;      // reusable lexer instance
    CxymlParser parser;     // reusable parser instance
    HashTable   fileCache;  // HashTable<CxymlCachedFile> keyed by path+mtime
} CxymlState;
```

### Registration

```c
bool pluginInit(CxyPluginContext *ctx, const FileLoc *loc)
{
    // Allocate and zero-initialise state from CXY's own pool
    CxymlState *state = callocFromMemPool(ctx->pool, 1, sizeof(CxymlState));

    // File cache: stores CxymlCachedFile entries, backed by CXY's pool
    state->fileCache = newHashTable(sizeof(CxymlCachedFile), ctx->pool);

    // Register state so it is accessible in every action call
    pluginSetState(ctx, state);

    cxyPluginRegisterAction(
        ctx, loc,
        (CxyPluginAction[]){
            {.name = "render", .fn = cxymlRender},
        },
        1
    );
    return true;
}

void pluginDeInit(CxyPluginContext *ctx)
{
    // The HashTable buckets are heap-allocated by the HashTable implementation
    // and must be released explicitly even though entries live in ctx->pool
    CxymlState *state = (CxymlState *)pluginState(ctx);
    if (state)
        freeHashTable(&state->fileCache);

    // All other allocations (state struct, nodes, strings) belong to ctx->pool
    // which CXY owns and frees at the end of compilation - nothing else to do
}
```

### Accessing State in Actions

```c
static AstNode *cxymlRender(CxyPluginContext *ctx,
                            const AstNode    *node,
                            AstNode          *args)
{
    // Retrieve plugin state registered during pluginInit
    CxymlState *state = (CxymlState *)pluginState(ctx);

    // ... use state->lexer, state->parser, state->fileCache ...
}
```

### File Cache Lookup

```c
// Compare function required by HashTable for CxymlCachedFile lookup
static bool cacheEntryCompare(const void *lhs, const void *rhs)
{
    const CxymlCachedFile *a = lhs;
    const CxymlCachedFile *b = rhs;
    // Path pointers are interned so pointer equality is sufficient
    return a->key.path == b->key.path;
}

// Returns a cached tree if path has been parsed and file has not changed,
// otherwise parses the file, caches the result and returns the new tree.
static CxymlNode *getCachedFile(CxyPluginContext *ctx,
                                CxymlState       *state,
                                const char       *path,
                                const FileLoc    *callSiteLoc)
{
    time_t mtime = fileModTime(path);

    // Intern the path so pointer equality works in cacheEntryCompare
    const char *internedPath = makeString(ctx->strings, path);

    CxymlCachedFile lookup = { .key = { .path = internedPath } };
    HashCode        hash   = hashStr(0, internedPath);

    CxymlCachedFile *entry = findInHashTable(
        &state->fileCache, &lookup, hash,
        sizeof(CxymlCachedFile), cacheEntryCompare);

    if (entry != NULL) {
        if (entry->key.mtime == mtime)
            return entry->tree;   // cache hit - file unchanged

        // File changed - re-parse and update in place
        entry->key.mtime = mtime;
        entry->tree      = parseMarkupFile(ctx, state, path, callSiteLoc);
        return entry->tree;
    }

    // Cache miss - parse and insert a new entry
    // Entry struct is stack-initialised here; HashTable copies it into
    // storage that was allocated from ctx->pool via newHashTable()
    CxymlCachedFile newEntry = {
        .key  = { .path = internedPath, .mtime = mtime },
        .tree = parseMarkupFile(ctx, state, path, callSiteLoc),
    };

    insertInHashTable(&state->fileCache, &newEntry, hash,
                      sizeof(CxymlCachedFile), cacheEntryCompare);

    return newEntry.tree;
}
```

### Action: `render`

**Signature:** `cxyml::render(markup)`

**Purpose:** Parse markup and generate code that writes HTML to the output stream,
inlined into the enclosing `render` method.

**Why no environment tuple?**
The action runs at **parse time** — before CXY's semantic analysis resolves any
references. There is therefore no point asking for resolved `this` or `os` nodes;
they do not exist yet. Instead the plugin emits an unresolved `os` identifier and
CXY's semantic pass resolves it from the enclosing `render(os: &OutputStream)` scope,
exactly as it would for any hand-written reference to `os`.

**Markup Argument:**
- A string literal containing inline markup, **or**
- A string literal whose value ends in `.cxyml` — loaded from disk and cached

**Returns:** A linked list of `AstNode` statements that write to `os`.

**Full Implementation:**

```c
static AstNode *cxymlRender(CxyPluginContext *ctx,
                            const AstNode    *node,
                            AstNode          *args)
{
    // 1. Retrieve plugin state
    CxymlState *state = (CxymlState *)cxyPluginState(ctx);

    // 2. Build an unresolved `os` identifier.
    //    CXY's semantic pass resolves it from the enclosing render() scope.
    AstNode *osRef = makeIdentifier(ctx->pool, &node->loc,
                                    makeString(ctx->strings, "os"),
                                    0, NULL, NULL);

    // 3. Get the markup argument
    CXY_REQUIRED_ARG(ctx->L, markupArg, args, &node->loc);

    if (!nodeIs(markupArg, StringLit)) {
        logError(ctx->L, &markupArg->loc,
                 "cxyml::render: expected a string literal or file path", NULL);
        return NULL;
    }

    const char *markup = markupArg->stringLiteral.value;
    CxymlNode  *tree   = NULL;

    // 4. Dispatch: file path or inline string?
    if (isFilePath(markup)) {
        // Resolve path relative to the current source file and use file cache
        const char *resolved = joinPath(ctx->strings, node->loc.fileName, markup);
        tree = getCachedFile(ctx, state, resolved, &node->loc);
    } else {
        // Inline string — origin tracks back to the call-site FileLoc so
        // error locations point at the correct line/col in the .cxy source
        FileLoc origin = node->loc;
        cxymlLexerInit(&state->lexer, markup, strlen(markup), &origin, ctx->L);
        tree = cxymlParse(&state->lexer, ctx->pool, ctx->strings, ctx->L);
    }

    if (tree == NULL)
        return NULL;   // errors already logged

    // 5. Generate AST nodes from the parse tree
    return cxymlGenerate(ctx, tree, osRef);
}
```

---

## Code Generation Strategy

The plugin generates two kinds of output depending on whether a tag is a **built-in HTML element** 
or a **custom component**:

### Convention: Uppercase vs Lowercase

| Tag | Example | Treatment |
|-----|---------|-----------|
| Lowercase | `<div>`, `<h1>` | Write raw HTML strings to `os` |
| Uppercase | `<MyComponent />` | Instantiate class, call `.render(os)` |

This mirrors React's JSX convention exactly.

### Built-in Elements (Lowercase)

Lowercase tags generate direct string writes to `os` - no object allocation:

**Input:**
```xml
<div class="container">
    <h1>Hello</h1>
</div>
```

**Generated:**
```cxy
os << "<div class=\"container\">"
os << "<h1>"
os << "Hello"
os << "</h1>"
os << "</div>"
```

### Custom Components (Uppercase)

Uppercase tags instantiate the component class and call its `render` method:

**Input:**
```xml
<MyComponent title="Hello" />
```

**Generated:**
```cxy
{
    var _cxyml0 = MyComponent("Hello".S)
    _cxyml0.render(os)
}
```

### Mixed Example

**Input:**
```xml
<Div class="page">
    <H1>{{ _title }}</H1>
    <ImportedComponent props={{ someValue }} />
</Div>
```

**Generated:**
```cxy
{
    var _cxyml0 = Div()
    _cxyml0.setClass("page".S)
    _cxyml0.render(os)
}
// Wait - see note below on render ordering
```

> **Note on render ordering:** Because custom components control their own rendering, 
> the plugin must generate an **inline rendering order** rather than a tree. See the 
> Inline Rendering section below.

---

## Inline Rendering

Unlike a build-then-render approach, Cxyml generates code that writes to the stream 
**in document order**. This avoids building a full in-memory tree before output.

**Input:**
```xml
<div class="page">
    <h1>{{ _title }}</h1>
    <MyWidget value={{ count }} />
    <p>Footer text</p>
</div>
```

**Generated (inline, stream order):**
```cxy
os << "<div class=\"page\">"
os << "<h1>"
os << _title
os << "</h1>"
{
    var _cxyml0 = MyWidget(count)
    _cxyml0.render(os)
}
os << "<p>"
os << "Footer text"
os << "</p>"
os << "</div>"
```

### Why Inline Rendering?

- ✅ No intermediate tree allocation for built-in elements
- ✅ Custom components render themselves (they own their subtree)
- ✅ Output order matches document order
- ✅ Memory efficient - write and forget
- ✅ Natural fit for HTTP response streaming

---

## Interpolation

Interpolations use `{{ expression }}` syntax. The **plugin does not parse expressions** - 
it captures the raw text between `{{` and `}}` and passes it to Cxy's `parseExpression()` function.

### How It Works

```
Markup text:  "Hello {{ user.getName() }}, you have {{ count + 1 }} messages"
                       ^^^^^^^^^^^^^^^^               ^^^^^^^^^
                       raw string captured            raw string captured
                       → parseExpression()            → parseExpression()
                       → AstNode                      → AstNode
```

### Text Interpolation

**Input:**
```xml
<p>Hello {{ name }}, you are {{ age }} years old!</p>
```

**Generated:**
```cxy
os << "<p>"
os << "Hello "
os << name
os << ", you are "
os << age
os << " years old!"
os << "</p>"
```

### Attribute Interpolation

**Input:**
```xml
<div id={{ elementId }} class={{ baseClass }}>
```

**Generated:**
```cxy
os << "<div id=\""
os << elementId
os << "\" class=\""
os << baseClass
os << "\">"
```

### Expression Parsing via Cxy's Parser

```c
AstNode *parseInterpolation(CxyPluginContext *ctx,
                            const char *exprText,
                            const FileLoc *loc)
{
    // Create a lexer over the raw expression string.
    // Pass loc so that any errors reported inside the expression point back
    // to the correct position in the original source (inline string or file).
    Lexer lexer;
    initLexer(&lexer, ctx->strings, exprText, strlen(exprText), loc);

    // Use Cxy's own parser - no custom expression handling needed
    Parser parser;
    initParser(&parser, &lexer, ctx->L, ctx->pool);

    AstNode *expr = parseExpression(&parser);

    if (expr == NULL || parser.ahead.tag != tokEof) {
        logError(ctx->L, loc, "Invalid expression in interpolation", NULL);
        return NULL;
    }

    return expr;
}
```

**Benefits:**
- ✅ No duplicate parsing logic
- ✅ All Cxy expressions work: `{{ f"Value: {x}" }}`, `{{ a + b }}`, `{{ obj.method() }}`
- ✅ Errors reported with Cxy's standard error format
- ✅ Future language features automatically available in interpolations

---

## Markup Syntax Specification

### Elements

**Standard:**
```xml
<TagName attribute="value">children</TagName>
```

**Self-closing:**
```xml
<TagName attribute="value" />
```

### Attributes

| Syntax | Example | Generated |
|--------|---------|-----------|
| Static string | `class="btn"` | `os << " class=\"btn\""` |
| Interpolated | `class={{ cls }}` | `os << cls` |
| Boolean flag | `disabled` | `os << " disabled"` |

### Special Attributes: `id` and `class`

Since these are the most common, the plugin generates optimized code:

For **built-in elements** (lowercase): written directly into the HTML string.

For **custom components** (uppercase): mapped to `setId()` / `setClass()` before `render()` is called.

### Text Content

```xml
<p>Static text</p>                    → os << "Static text"
<p>Hello {{ name }}</p>               → os << "Hello " then os << name
<p>{{ greeting }}, {{ name }}!</p>    → os << greeting, os << ", ", os << name, os << "!"
```

### Comments

```xml
<!-- This is ignored at compile time -->
```

Comments are stripped during lexing. They produce no generated code.

### Whitespace Rules

1. Trim leading/trailing whitespace from text nodes
2. Collapse internal whitespace sequences to a single space
3. Discard whitespace-only text nodes between elements

---

## Source Location Tracking

Accurate source locations (`FileLoc`) must be attached to every AST node the plugin generates.
This is what allows the Cxy compiler to point diagnostics at the right line and column - whether
the markup came from an inline string or an external file.

### Inline String

When the markup is an inline string literal inside a `.cxy` source file:

```cxy
cxyml.render!((this, os), """
    <div class="page">
        <h1>{{ _title }}</h1>
    </div>
""")
```

The plugin receives a `FileLoc` for the call site (the `node->loc` passed to the action).
All token locations must be computed **relative to `node->loc.begin`**:

```c
// node->loc.begin points at the opening """ in the .cxy source file
// The lexer advances line/column from that origin as it scans markup

typedef struct {
    const char  *src;       // pointer into the markup string
    FileLoc      origin;    // node->loc copied here at lexer init
    u32          line;      // current line, starts at origin.begin.line
    u32          column;    // current column, starts at origin.begin.column
} CxymlLexer;

static void lexerInit(CxymlLexer *l, const char *src, const FileLoc *origin)
{
    l->src    = src;
    l->origin = *origin;
    l->line   = origin->begin.line;
    l->column = origin->begin.column;
}

// Build a FileLoc snapshot for the current token
static FileLoc currentLoc(const CxymlLexer *l)
{
    FileLoc loc  = l->origin;       // inherit fileName from origin
    loc.begin.line   = l->line;
    loc.begin.column = l->column;
    loc.end          = loc.begin;   // will be widened as token is consumed
    return loc;
}
```

Every token stores its `FileLoc` so that nodes generated from it inherit
the exact position inside the originating `.cxy` source file.

### External File (`.cxyml`)

When the markup argument is a file path:

```cxy
cxyml.render!((this, os), "templates/dashboard.cxyml")
```

The plugin opens and reads the file, then initialises the lexer with a fresh
`FileLoc` whose `fileName` is the resolved path of the `.cxyml` file and whose
`begin` starts at `{line: 1, column: 1}`:

```c
static bool loadFile(CxyPluginContext *ctx, const char *path,
                     const FileLoc *callSiteLoc,
                     const char **srcOut, FileLoc *originOut)
{
    char *src = readFileContents(path);   // platform file read
    if (src == NULL) {
        logError(ctx->L, callSiteLoc,
                 "cxyml: cannot open file '%s'", path);
        return false;
    }

    // Origin is the .cxyml file itself, not the call site
    originOut->fileName     = makeString(ctx->strings, path);
    originOut->begin.line   = 1;
    originOut->begin.column = 1;
    originOut->end          = originOut->begin;

    *srcOut = src;
    return true;
}
```

Diagnostics for markup errors in an external file will then reference the
`.cxyml` file and line, not the `.cxy` call site.

### Summary

| Source | `FileLoc` origin | Diagnostic points to |
|--------|-----------------|----------------------|
| Inline string | `node->loc.begin` from call site | Line/col in the `.cxy` file |
| External `.cxyml` | `{fileName: path, line:1, col:1}` | Line/col in the `.cxyml` file |

Every `CxymlNode` and `CxymlToken` stores its own `FileLoc` snapshot taken
at the moment it was lexed, so all generated `AstNode`s carry accurate
location information through to the compiler's diagnostic engine.

---

## Parser Implementation

### Lexer

The Cxyml lexer only handles **markup structure**. Interpolation content is captured as raw strings.
Each token records a `FileLoc` using the tracking rules described above.

**Token Types:**
```c
typedef enum {
    CXYML_TOK_OPEN,         // <
    CXYML_TOK_CLOSE,        // >
    CXYML_TOK_SLASH,        // /
    CXYML_TOK_IDENT,        // TagName or attributeName
    CXYML_TOK_EQUALS,       // =
    CXYML_TOK_STRING,       // "value" or 'value'
    CXYML_TOK_TEXT,         // plain text between tags
    CXYML_TOK_INTERP_OPEN,  // {{
    CXYML_TOK_INTERP_EXPR,  // raw expression string (everything until }})
    CXYML_TOK_INTERP_CLOSE, // }}
    CXYML_TOK_COMMENT,      // <!-- ... --> (discarded)
    CXYML_TOK_EOF
} CxymlTokenType;

typedef struct {
    CxymlTokenType type;
    const char    *start;   // pointer into source buffer
    u32            length;
    FileLoc        loc;     // exact location, inline-relative or file-based
} CxymlToken;
```

**State machine:**
- Default state: scan for `<`, `{{`, or text
- Tag state: scan identifier, attributes, `>` or `/>`
- Attribute value state: handle `"..."` or `{{ ... }}`
- Interpolation state: capture raw bytes until `}}`, recording start `FileLoc`
  so that `parseInterpolation()` receives the correct origin for the expression

### Parse Tree

Every node in the parse tree stores a `FileLoc` so that generated `AstNode`s
inherit correct source positions for the compiler's diagnostic engine.

```c
typedef enum {
    CXYML_NODE_ELEMENT,
    CXYML_NODE_TEXT,
    CXYML_NODE_INTERP,
} CxymlNodeKind;

typedef struct CxymlNode {
    CxymlNodeKind kind;
    FileLoc        loc;             // source location of this node
    struct CxymlNode *next;         // next sibling

    // CXYML_NODE_ELEMENT
    const char *tagName;
    bool selfClosing;
    bool isCustom;                  // uppercase = true
    CxymlAttr *attrs;
    struct CxymlNode *children;

    // CXYML_NODE_TEXT
    const char *text;

    // CXYML_NODE_INTERP
    const char *expr;               // raw expression string
    FileLoc     exprLoc;            // location of the expression inside {{ }}
                                    // passed as origin to parseInterpolation()
} CxymlNode;

typedef struct CxymlAttr {
    const char *name;
    FileLoc     nameLoc;            // location of the attribute name
    const char *value;              // NULL if interpolated
    FileLoc     valueLoc;           // location of the value (static or interp)
    const char *interpExpr;         // raw expression, NULL if static
    struct CxymlAttr *next;
} CxymlAttr;
```

### AST Generation

Every generated `AstNode` uses the `FileLoc` stored on the `CxymlNode` that
produced it. This ensures the compiler's diagnostics point at the markup
source - whether that is a line in a `.cxy` file (inline) or a `.cxyml` file
(external) - rather than the plugin's own internal code.

```c
// Dispatch based on node kind and tag case.
// node->loc is already the correct FileLoc (inline-relative or file-based).
AstNode *generateNode(CxyPluginContext *ctx, CxymlNode *node,
                      AstNode *osRef)
{
    switch (node->kind) {
        case CXYML_NODE_TEXT:
            // Use node->loc so the write statement traces back to the
            // text in the source markup.
            return generateTextWrite(ctx, node->text, osRef, &node->loc);

        case CXYML_NODE_INTERP:
            // node->exprLoc points at the expression inside {{ }}
            return generateInterpWrite(ctx, node->expr, osRef, &node->exprLoc);

        case CXYML_NODE_ELEMENT:
            if (node->isCustom)
                return generateCustomComponent(ctx, node, osRef);
            else
                return generateBuiltinElement(ctx, node, osRef);
    }
}

// Built-in: write tag strings and recurse children
AstNode *generateBuiltinElement(CxyPluginContext *ctx, CxymlNode *node,
                                AstNode *osRef, const FileLoc *loc)
{
    AstNodeList stmts = {};

    // Opening tag
    insertAstNode(&stmts, makeStreamWrite(ctx, osRef, openingTag(node), loc));

    // Children in order
    for (CxymlNode *child = node->children; child; child = child->next)
        insertAstNode(&stmts, generateNode(ctx, child, osRef, loc));

    // Closing tag (if not self-closing)
    if (!node->selfClosing)
        insertAstNode(&stmts, makeStreamWrite(ctx, osRef, closingTag(node), loc));

    return stmts.first;
}

// Custom: instantiate class, set props, call render(os)
AstNode *generateCustomComponent(CxyPluginContext *ctx, CxymlNode *node,
                                 AstNode *osRef, const FileLoc *loc)
{
    // var _cxymlN = TagName(props...)
    AstNode *ctor = makeCallExpr(ctx->pool, loc,
                       makeResolvedPath(ctx->pool, loc,
                           makeString(ctx->strings, node->tagName),
                           flgNone, NULL, NULL),
                       buildPropsArgs(ctx, node->attrs, loc),
                       flgNone, NULL);

    AstNode *varDecl = makeVarDecl(ctx->pool, loc, flgNone,
                           genVarName(ctx), NULL, ctor, NULL);

    // _cxymlN.render(os)
    AstNode *renderCall = makeCallExpr(ctx->pool, loc,
                              makeMemberExpr(ctx->pool, loc, varDecl, "render"),
                              osRef, flgNone, NULL);

    // Wrap in block scope
    return makeBlockStmt(ctx->pool, loc,
               linkNodes(varDecl, renderCall));
}
```

---

## Error Handling

### Plugin Reports: Markup Syntax Errors

```
error: Unclosed tag at line 4, column 5
    <Div class="page"
    ^
```

```
error: Mismatched closing tag at line 9
    <Div>...</H1>
               ^
    expected </Div>
```

```
error: Unclosed interpolation at line 6
    {{ user.name
    ^
```

### Cxy Compiler Reports: Semantic Errors

Unknown component types, bad expression types, missing imports - all reported by the Cxy 
compiler using its own diagnostics, from the generated AST's source locations.

```
error: Cannot find value `UnknownWidget` in this scope
    hint: Did you forget to import UnknownWidget?
```

---

## File Loading

When the markup argument ends in `.cxyml`, the plugin reads the file from disk:

```cxy
cxyml.render!((this, os), "templates/homepage.cxyml")
```

**Resolution:** Relative to the current source file's directory.

**Caching:** Parsed files are cached by path and mtime. Recompilation only occurs if the file changes.

---

## Usage Examples

### Simple Page Component

```cxy
import plugin "cxyml" as cxyml
import { View } from "cxyml/view.cxy"

pub class HomePage: View {
    - _username: String;

    func `init`(username: String) {
        _username = &&username
    }

    @override
    func render(os: &OutputStream): void {
        cxyml::render("""
            <div class="home">
                <h1>Welcome, {{ _username }}!</h1>
                <p>You are now logged in.</p>
            </div>
        """)
    }
}
```

### Composed Components

```cxy
import plugin "cxyml" as cxyml
import { View } from "cxyml/view.cxy"
import { Navbar } from "./navbar.cxy"
import { Footer } from "./footer.cxy"
import { UserCard } from "./usercard.cxy"

pub class Dashboard: View {
    - _user: User;

    func `init`(user: User) {
        _user = &&user
    }

    @override
    func render(os: &OutputStream): void {
        cxyml::render("""
            <div class="dashboard">
                <Navbar title="Dashboard" />
                <div class="content">
                    <UserCard user={{ _user }} />
                </div>
                <Footer />
            </div>
        """)
    }
}
```

### From File

```cxy
@override
func render(os: &OutputStream): void {
    cxyml::render("templates/dashboard.cxyml")
}
```

### Server-Side Route Handler

```cxy
import { HomePage } from "./pages/home.cxy"
import { Request, Response } from "stdlib/http.cxy"

func handleHome(req: Request, res: Response): void {
    var page = HomePage(req.session.username)
    res.setHeader("Content-Type".S, "text/html".S)
    page.render(res.stream())
}
```

---

## Implementation Phases

### Phase 1: MVP - Static Markup
- Lexer for tags, attributes, text
- Parser into CxymlNode tree
- Code generation for built-in (lowercase) elements
- Code generation for custom (uppercase) components
- Basic error messages

**Done when:**
```cxy
cxyml.render!((this, os), "<div><h1>Hello</h1></div>")
```
works and outputs `<div><h1>Hello</h1></div>` to `os`.

### Phase 2: Interpolation
- Lexer captures raw `{{ ... }}` expressions
- Integration with Cxy's `parseExpression()`
- Text interpolation
- Attribute interpolation
- Error forwarding from Cxy parser

**Done when:**
```cxy
cxyml.render!((this, os), "<h1>Hello {{ _name }}</h1>")
```
writes `<h1>Hello Alice</h1>` at runtime.

### Phase 3: File Loading
- Detect file path vs inline string
- File reading relative to source file
- Parsed file caching by path + mtime

### Phase 4: Optimization
- Merge consecutive static string writes into one
- Eliminate single-use block scopes
- Better variable names in debug mode
- **In-place `CxymlNode` → `AstNode` transformation**:
  Currently codegen allocates fresh `AstNode`s (via `makeStringLiteral`,
  `makeBinaryExpr`, etc.) and discards the parsed `CxymlNode`s. Because
  `CxymlNode` is a union with `AstNode` and shares the same size and
  `CXY_AST_NODE_HEAD` layout, simple nodes (text literals, interpolations)
  could be transformed **in place** - overwriting the cxyml-specific fields
  with real `AstTag` values and AST payloads - and returned directly as
  `(AstNode *)node` with zero extra allocation. This is safe because the
  `CxymlNode` memory is already owned by `ctx->pool` at `sizeof(AstNode)`
  and will not be needed again after codegen. Complex nodes (elements,
  components) that expand into multiple statements still require fresh
  allocations, but leaf nodes are the majority and stand to benefit most.

### Phase 5: Advanced Features (Future)
- Conditional rendering: `<div if={{ isLoggedIn }}>`
- Loop rendering: `<li for={{ item in items }}>{{ item }}</li>`
- Slot/children composition

---

## Appendix: Grammar (EBNF)

```ebnf
document    ::= node*
node        ::= element | text | interpolation
element     ::= self_closing | open_element
self_closing ::= '<' IDENT attr* '/>'
open_element ::= '<' IDENT attr* '>' node* '</' IDENT '>'
attr        ::= IDENT '=' (STRING | interpolation) | IDENT
text        ::= TEXT+
interpolation ::= '{{' RAW_EXPR '}}'
```

---

**Status:** Design Draft v2  
**Model:** React-like SSR with compile-time markup  
**Rendering:** Stream-based, inline, document-order  
**Expressions:** Delegated to Cxy's `parseExpression()`  
