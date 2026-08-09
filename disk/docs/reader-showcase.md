# TinyOS Reader Gallery

This document exercises the Reader's **Markdown**, *emphasis*, ~~strikethrough~~, `inline code`, escaped \*markers\*, and [links](https://example.com). Named entities render too: &lt;tag&gt;, &amp;, and &#65;.

## UTF-8 Typography

Literal typography is normalized for TinyOS's ASCII bitmap font:

- em dash: word—word becomes `word--word`
- en dash: 2024–2026 becomes `2024-2026`
- smart quotes and ellipsis: “Read widely…” becomes `"Read widely..."`
- accented Latin and full-width ASCII: café ＡＢＣ becomes `cafe ABC`
- unsupported scripts remain visible as code points: 中文 becomes `[U+4E2D][U+6587]`

HTML entities use the same path: left&mdash;right, &ldquo;quoted&rdquo;, and wait&hellip;

![A lantern glowing over an open book](reader-cover.ppm "Reader cover")

Setext Heading
==============

## Lists and Tasks

- Unordered item
- [x] Image decoding works
- [ ] Add your own chapter

1. Open the shell
2. Run `reader /docs/reader-showcase.md`
3. Turn pages with Space and B

### Quotations

> A reader lives a thousand lives before he closes the book.
> Nested quote markers are accepted and wrapped as a readable block.

### Code

```c
#include <stdio.h>

int main(void) {
    puts("Hello from a fenced code block");
    return 0;
}
```

    Indented code is supported as well.

### Tables

| Format | Reader behavior |
|---|---|
| TXT | Literal text and wrapping |
| Markdown | Styled blocks and cleaned inline tags |
| PNG | 8-bit grayscale, RGB, palette, gray-alpha, RGBA |
| BMP | Uncompressed 24-bit and 32-bit |
| PPM | ASCII P3 and binary P6 |

---

## Relative Images

The image below is loaded relative to this Markdown file, not the shell's current directory.

![A diagram of Markdown flowing into pages](reader-flow.ppm)

#### HTML and Autolinks

HTML presentation tags are removed while their text remains: <strong>strong text</strong> and <em>emphasized text</em>.

Autolinks remain readable: <https://example.com/docs>.

##### Small Heading

Image failures do not close the document. A labeled placeholder identifies the missing path.

###### End of Gallery

Use **Up/Down** to scroll, **Right/Space** for the next page, **Left/B** for the previous page, `g` and `G` for the ends, and **Q** to close.
