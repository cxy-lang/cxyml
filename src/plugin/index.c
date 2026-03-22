//
// Cxyml plugin - Main entry point
//
// This is the only file compiled by CXY. It includes all implementation
// files so that the plugin is built as a single translation unit.
//
// Plugin actions registered:
//   cxyml::render("markup")
//   cxyml::render("path/to/file.cxyml")
//
// Because the action runs at parse time, `os` is emitted as an unresolved
// identifier that CXY's semantic pass resolves from the enclosing render()
// method scope.  No environment tuple is needed at the call site.
//

#include "state.h"

#include <cxy/core/hash.h>
#include <cxy/core/log.h>
#include <cxy/core/mempool.h>
#include <cxy/core/strpool.h>
#include <cxy/core/utils.h>
#include <cxy/ast.h>
#include <cxy/flag.h>
#include <cxy/plugin.h>

#include <string.h>
#include <sys/stat.h>

// Pull in all implementation files as a single translation unit
#include "lexer.c"
#include "parser.c"
#include "codegen.c"

// ============================================================================
// Plugin state access
// ============================================================================

// ============================================================================
// File helpers
// ============================================================================

// Returns true if the string looks like a file path (ends in .cxyml)
static bool isFilePath(const char *markup)
{
    size_t len = strlen(markup);
    return len > 6 && strcmp(markup + len - 6, ".cxyml") == 0;
}

// Returns the modification time of a file, or 0 on error.
static time_t fileModTime(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}



// ============================================================================
// File cache
// ============================================================================

static CxymlNode *getCachedFile(CxyPluginContext *ctx,
                                CxymlState       *state,
                                const char       *path,
                                const FileLoc    *callSiteLoc)
{
    const char *internedPath = makeString(ctx->strings, path);
    time_t      mtime        = fileModTime(path);

    CxymlCachedFile lookup = {.path = internedPath};
    HashCode        hash   = hashStr(hashInit(), internedPath);

    CxymlCachedFile *entry = findInHashTable(
        &state->fileCache,
        &lookup,
        hash,
        sizeof(CxymlCachedFile),
        cxymlCacheCompare);

    if (entry != NULL) {
        if (entry->mtime == mtime)
            return entry->tree;  // cache hit - file unchanged

        // File changed - re-parse and update in place
        entry->mtime = mtime;
        goto parse_file;
    }

    {
        // Cache miss - insert a new entry then fall through to parse
        CxymlCachedFile newEntry = {.path = internedPath, .mtime = mtime, .tree = NULL};
        insertInHashTable(&state->fileCache,
                          &newEntry,
                          hash,
                          sizeof(CxymlCachedFile),
                          cxymlCacheCompare);
        entry = findInHashTable(&state->fileCache,
                                &newEntry,
                                hash,
                                sizeof(CxymlCachedFile),
                                cxymlCacheCompare);
    }

parse_file:;
    // Build FileLoc origin for the .cxyml file - locations start at {1,1}
    FileLoc fileOrigin = {
        .fileName  = internedPath,
        .begin     = {.row = 1, .col = 1, .byteOffset = 0},
        .end       = {.row = 1, .col = 1, .byteOffset = 0},
    };

    size_t srcLen = 0;
    // Use CXY's own readFile from utils.h: char *readFile(const char *fileName, size_t *file_size)
    const char *src = readFile(path, &srcLen);
    if (src == NULL) {
        logError(ctx->L, callSiteLoc,
                 "cxyml: cannot open or read file '{s}'",
                 (FormatArg[]){{.s = path}});
        return NULL;
    }

    cxymlLexerInit(&state->lexer, src, srcLen, &fileOrigin, ctx->L);
    entry->tree = cxymlParse(&state->lexer, ctx->pool, ctx->strings, ctx->L);
    return entry->tree;
}

// ============================================================================
// render! action
//
// Signature:   cxyml.render!((this, os), markup)
//
// Environment:
//   this  – the current component instance
//   os    – the OutputStream to write into
//
// Markup arg:
//   A string literal - either raw markup or a path ending in ".cxyml"
// ============================================================================

static AstNode *cxymlRender(CxyPluginContext *ctx,
                            const AstNode    *node,
                            AstNode          *args)
{
    // 1. Retrieve plugin state
    CxymlState *state = (CxymlState *)cxyPluginState(ctx);

    // 2. Resolve the stream reference and the markup argument.
    //
    // Two calling conventions are supported:
    //
    //   cxyml::render("markup")
    //     — one argument, stream defaults to the identifier `os`
    //
    //   cxyml::render(streamVar, "markup")
    //     — two arguments, first must be an Identifier or Path
    //
    // We deliberately restrict the stream argument to Identifier / Path.
    // Any expression that is cloned per-write (e.g. a CallExpr) would be
    // re-evaluated for every os << statement the template emits.  If a
    // complex expression is needed the caller should capture it first:
    //   var s = callMe();  cxyml::render(s, "...")
    AstNode *osRef;
    AstNode *markupArg;

    if (args != NULL && args->next != NULL) {
        // ---- Two-argument form:  cxyml::render(streamVar, "markup") ----
        AstNode *streamArg = args;
        markupArg          = args->next;

        if (args->next->next != NULL) {
            logError(ctx->L, &args->next->next->loc,
                     "cxyml::render!: too many arguments — expected "
                     "(\"markup\") or (stream, \"markup\")",
                     NULL);
            return NULL;
        }

        if (!nodeIs(streamArg, Identifier) && !nodeIs(streamArg, Path)) {
            logError(ctx->L, &streamArg->loc,
                     "cxyml::render!: stream argument must be a plain "
                     "variable or path (e.g. 'ss' or 'pkg.out').\n"
                     "         For expressions with side-effects, capture "
                     "first:  var s = expr;  cxyml::render(s, \"...\")",
                     NULL);
            return NULL;
        }

        // Use the already-parsed node directly as osRef.  deepCloneAstNode
        // is called on it for every write, so a plain identifier is safe.
        osRef = streamArg;
    }
    else {
        // ---- One-argument form:  cxyml::render("markup") ----
        // Default to `os` — resolved by CXY's semantic pass from the
        // enclosing render() method parameter, just like any identifier.
        CXY_REQUIRED_ARG(ctx->L, firstArg, args, &node->loc);
        markupArg = firstArg;
        osRef     = makeIdentifier(ctx->pool, &node->loc,
                                   makeString(ctx->strings, "os"),
                                   0, NULL, NULL);
    }

    // 3. Validate the markup argument
    if (!nodeIs(markupArg, StringLit)) {
        logError(ctx->L, &markupArg->loc,
                 "cxyml::render!: markup argument must be a string literal "
                 "or a .cxyml file path",
                 NULL);
        return NULL;
    }

    const char *markup = markupArg->stringLiteral.value;
    CxymlNode  *tree   = NULL;

    // 4. Inline string or external file?
    if (isFilePath(markup)) {
        // Resolve path relative to the directory of the source file that
        // contains the call site.  node->loc.fileName is the full path to
        // the .cxy file, so we strip the filename to get the directory.
        const char *srcFile = node->loc.fileName;
        const char *lastSlash = strrchr(srcFile, '/');
        cstring dir;
        if (lastSlash != NULL)
            dir = makeStringSized(ctx->strings, srcFile, (u32)(lastSlash - srcFile));
        else
            dir = makeString(ctx->strings, ".");

        const char *resolved = joinPath(ctx->strings, dir, markup);

        tree = getCachedFile(ctx, state, resolved, &node->loc);
    }
    else {
        // Inline markup - origin tracks back to the call-site FileLoc
        FileLoc origin = node->loc;

        cxymlLexerInit(&state->lexer, markup, strlen(markup), &origin, ctx->L);
        tree = cxymlParse(&state->lexer, ctx->pool, ctx->strings, ctx->L);
    }

    if (tree == NULL) {
        // Either a parse error (already logged) or empty markup.
        // Return a noop so the compiler does not see NULL.
        return makeAstNode(ctx->pool, &node->loc, &(AstNode){.tag = astNoop});
    }

    // 5. Generate CXY AST statements from the parse tree
    AstNode *generated = cxymlGenerate(ctx, tree, osRef);

    if (generated == NULL) {
        // Code generation error - already logged
        return makeAstNode(ctx->pool, &node->loc, &(AstNode){.tag = astNoop});
    }

    // 6. Wrap in a BlockStmt so CXY receives a single node.
    // The action call is a single expression; CXY replaces it with whatever
    // the action returns.  If we return a raw sibling chain CXY only takes
    // the first node — the rest are lost.  A BlockStmt is one node whose
    // stmts field holds the full chain.
    return makeBlockStmt(ctx->pool, &node->loc, generated, NULL, NULL);
}

// ============================================================================
// Plugin lifecycle
// ============================================================================

bool pluginInit(CxyPluginContext *ctx, const FileLoc *loc)
{
    // Allocate and zero-initialise state using CXY's own memory pool.
    // The state lives for the entire compilation; no manual free needed.
    CxymlState *state = callocFromMemPool(ctx->pool, 1, sizeof(CxymlState));

    // File cache backed by CXY's pool
    state->fileCache = newHashTable(sizeof(CxymlCachedFile), ctx->pool);

    // Register state and declare this plugin operates at the parser level.
    // CXY will create a parser/lexer for us when needed (e.g. cxyParseExpression).
    cxyPluginInitialize(ctx, state, pipParser);

    // Register the render! action
    return cxyPluginRegisterAction(
        ctx,
        loc,
        (CxyPluginAction[]){
            {.name = "render", .fn = cxymlRender},
        },
        1);
}

void pluginDeInit(CxyPluginContext *ctx)
{
    CxymlState *state = (CxymlState *)cxyPluginState(ctx);
    if (state == NULL)
        return;

    // The HashTable's internal bucket array is heap-allocated (not from the
    // pool) so it must be released explicitly.
    freeHashTable(&state->fileCache);

    // Everything else (state struct, all nodes, strings) belongs to ctx->pool
    // which CXY owns and will free at the end of compilation.
}
