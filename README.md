# cxyml

A compile-time server-side rendering (SSR) library for Cxy. Write HTML components using familiar XML-like markup in `.cxyml` files or inline strings — the compiler transforms them into direct stream writes at build time. Zero runtime parsing, zero allocations per render.

## How It Works

Components are plain Cxy classes that extend `View` and implement a `render` method. The `cxyml` compiler plugin processes your markup at compile time and generates efficient code that writes HTML directly to an `OutputStream`.

```cxy
import plugin "cxyml" as cxyml
import { View } from "@cxyml"

pub class Greeting: View {
    - _name:    String;
    - _message: String;

    func `init`(name: String, message: String) {
        _name    = &&name
        _message = &&message
    }

    @override
    func render(os: &OutputStream): void {
        cxyml::render("""
            <div class="card">
                <h1>Hello, {{ _name }}!</h1>
                <p>{{ _message }}</p>
            </div>
        """)
    }
}

func main(): i32 {
    var card = Greeting("Alice".S, "Welcome to cxyml.".S)
    card.render(&stdout)
    return 0
}
```

Output:

```html
<div class="card">
    <h1>Hello, Alice!</h1>
    <p>Welcome to cxyml.</p>
</div>
```

## Installation

Add `cxyml` to your `Cxyfile.yaml`:

```yaml
dependencies:
  - name: cxyml
    repository: https://github.com/cxy-lang/cxyml.git
    version: "*"
    tag: v0.1.0
```

Then import it in your source file:

```cxy
import plugin "cxyml" as cxyml
import { View, ViewBase, htmlEscape } from "@cxyml"
```

## Markup Syntax

### Interpolation

Use `{{ expr }}` to embed any Cxy expression. Values are automatically HTML-escaped.

```cxyml
<p>{{ _title }}</p>
<span class="{{ _cssClass }}">{{ _count }}</span>
```

### Conditionals

```cxyml
{{ if _isLoggedIn }}
    <a href="/logout">Sign out</a>
{{ /if }}
```

`{{ else }}` is also supported:

```cxyml
{{ if _posts.empty() }}
    <div class="empty-state">
        <p>No posts yet. Check back soon!</p>
    </div>
{{ else }}
    <div class="post-grid">
        {{ for post in _posts }}
            <PostCard post={{ __copy!(post.0) }} />
        {{ /for }}
    </div>
{{ /if }}
```

### Loops

Use `{{ for x in collection }}` / `{{ /for }}` to iterate over any enumerable value. The loop variable is a tuple where `.0` is the element and `.1` is the index (matching Cxy's standard `for` semantics).

```cxyml
{{ for post in _posts }}
    <PostCard post={{ __copy!(post.0) }} />
{{ /for }}
```

### Attribute Values

Attributes accept three forms:

**Interpolated expression** — any Cxy expression inside `{{ }}`:

```cxyml
<a href={{ f"/post/{_post.slug}" }}>Read more</a>
<PostCard post={{ __copy!(post.0) }} />
```

**String literal** — a bare quoted string (no `{{ }}` needed):

```cxyml
<Navbar brand="DevBlog" />
<input type="text" name="title" required />
```

**Inline string** — same as interpolation but simpler for plain field values:

```cxyml
<h1 class="{{ _cssClass }}">{{ _title }}</h1>
```

### Self-Closing Tags

Void HTML elements and components with no children use `/>`:

```cxyml
<meta charset="UTF-8" />
<link rel="stylesheet" href="/style.css" />
<input type="email" name="email" placeholder="you@example.com" required />
<Navbar brand="DevBlog" />
```

### HTML Entities

Standard HTML entities pass through untouched:

```cxyml
<p>By {{ _author }} &middot; {{ _date }}</p>
<a href="/">&larr; Back to posts</a>
<title>{{ _title }} &mdash; DevBlog</title>
```

### Script and Style Blocks

`<script>` and `<style>` block content is passed through as raw text — no escaping or interpolation occurs inside them:

```cxyml
<script>
    document.getElementById('my-form').addEventListener('submit', async function(e) {
        e.preventDefault();
        // plain JavaScript — not processed by cxyml
    });
</script>
```

### External Template Files

Put your markup in a `.cxyml` file and reference it by path. The file is resolved at compile time — no runtime file I/O.

```cxy
@override
func render(os: &OutputStream): void {
    cxyml::render("greeting.cxyml")
}
```

`greeting.cxyml`:

```html
<div class="greeting-card">
    <h1>Hello, {{ _name }}!</h1>
    <p class="greeting-message">{{ _message }}</p>
    {{ if _showFooter }}
    <footer>
        <small>Rendered with cxyml</small>
    </footer>
    {{ /if }}
</div>
```

### Components

Use other `View` subclasses as tags directly in your markup. Uppercase tags are treated as components; attributes map to constructor arguments.

```cxyml
<div class="layout">
    <Navbar brand={{ _brand }} />
    <section class="content">
        <PostCard title="Getting Started" href="/posts/getting-started" />
        <PostCard title="Composing Components" href="/posts/composing" />
    </section>
    <Footer copyright={{ _year }} />
</div>
```

## Built-in Elements

cxyml ships a set of ready-to-use element classes for building UI programmatically when you prefer code over markup:

| Class | HTML tag | Notes |
|-------|----------|-------|
| `Text` | _(none)_ | Plain text node |
| `Div` | `<div>` | Block container |
| `Span` | `<span>` | Inline container |
| `P` | `<p>` | Paragraph |
| `H1` `H2` `H3` | `<h1>` `<h2>` `<h3>` | Headings |
| `A` | `<a>` | Link — takes `href` and text |
| `Button` | `<button>` | Button with optional label |
| `Input` | `<input>` | Self-closing, takes `type` |
| `Label` | `<label>` | Form label |
| `Img` | `<img>` | Self-closing, takes `src` and `alt` |

All elements extend `View` and support `setAttribute`, `addClass`, `setId`, `addChild`, and `render`.

```cxy
import { Div, H1, P, A } from "@cxyml"

var container = Div()
container.setId("main".S)
container.addClass("wrapper".S)
container.addChild(H1("Welcome".S))
container.addChild(P("Get started below.".S))
container.addChild(A("/docs".S, "Read the docs".S))

container.render(&stdout)
```

## API Reference

### `ViewBase`

Base class for all renderable components.

| Method | Description |
|--------|-------------|
| `setAttribute(key, value)` | Set an HTML attribute |
| `getAttribute(key)` | Get an attribute value |
| `removeAttribute(key)` | Remove an attribute |
| `setClass(name)` | Set the `class` attribute |
| `addClass(name)` | Append a class name |
| `setId(id)` | Set the `id` attribute |
| `render(os)` | Write HTML to an `OutputStream` |

### `View`

Extends `ViewBase` with child management and tag-based rendering.

| Method | Description |
|--------|-------------|
| `addChild(child)` | Add a child element |
| `removeChild(index)` | Remove child at index |
| `clearChildren()` | Remove all children |
| `childrenCount()` | Number of direct children |
| `renderToString()` | Render to a `String` instead of a stream |

### `htmlEscape`

Utility function used internally by the plugin to safely embed dynamic values in HTML. Available for use in your own rendering code.

```cxy
import { htmlEscape } from "@cxyml"

htmlEscape(&stdout, userInput)
```

## Testing

```bash
cxy package test
```
