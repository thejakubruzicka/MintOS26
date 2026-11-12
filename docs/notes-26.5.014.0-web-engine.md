# 26.5.014.0 — web engine (topic/web-engine)

MintOS Web now draws real pages with its own engine instead of a text-only reader: HTML is parsed into a DOM,
the page's CSS is applied, a layout engine places boxes, text, images and form controls, and MintDE paints the
result. JavaScript is not run (by design for this release).

## What changed

**New engine, `kernel/user/web/` (portable C, host-tested, built for ring 3 with `-mno-sse -mgeneral-regs-only`):**
- `wmem.c` — bounded heap (boundary tags, coalescing, NULL on exhaustion) and per-document arenas.
- `inflate.c` — DEFLATE/zlib/gzip decoder (own implementation, output-bounded, checksums verified).
- `whttp.c` — URL parsing/resolution (RFC 3986, IDN-less), HTTP/1.1 request builder (realistic WebKit-style
  User-Agent, `Accept-Encoding: gzip, deflate`, Referer policy strict-origin-when-cross-origin, Origin on POST),
  incremental response parser (Content-Length, chunked with extensions/trailers, until-close, 1xx), Content-Encoding,
  Content-Disposition, RFC 6265 cookie jar (domain/path/expiry/Max-Age/Secure/HttpOnly/`__Host-`/`__Secure-`, 300
  cookies, persisted), `application/x-www-form-urlencoded`.
- `whtml.c` — HTML tree builder with HTML5-style recovery (implied html/head/body, implied end tags for p/li/dt/dd/
  option/rows/cells, void and raw-text elements, foster parenting, scoped end tags), ~250 named entities, numeric
  references, charset sniffing and windows-1252/ISO-8859-1/ISO-8859-2/windows-1250 conversion, invalid UTF-8 repair.
  Unused attribute families (`on*`, `data-*` except lazy-image ones, RDFa, most ARIA) are dropped to save memory.
- `wcss.c` — CSS parser and cascade: type/class/id/universal/attribute selectors, descendant/child/sibling
  combinators, `:first-child`, `:last-child`, `:nth-child()`, `:not()`, `:is()/:where()`, `:root`, `:link`, …;
  specificity, origin and `!important`; `@media` (width/height incl. range syntax, prefers-color-scheme, print),
  `@supports`, `@layer`; document-wide custom properties with `var()`; `calc()/min()/max()/clamp()`; shorthands;
  presentational HTML attributes (bgcolor, width, align, cellpadding, `<font>`…); quirks-mode table rule; rules are
  bucketed by id/class/tag and identical computed styles are shared. Hidden-content heuristics (visually-hidden,
  off-screen, image replacement, mask-image icons). Unknown CSS is ignored.
- `wlayout.c` — block and inline formatting with line breaking, text-align (incl. justify), collapsing sibling margins,
  floats with line wrapping and `clear`, lists (disc/circle/square/decimal/alpha/roman, `start`, `value`), tables
  (colspan/rowspan, intrinsic min/max column widths, percentages, captions, cell vertical-align, collapsed borders),
  inline-block, flex rows (approximated as wrapping lines; `justify-content`, `gap`, `flex-grow` = full row), images
  with intrinsic/attribute/CSS sizes and `max-width`, form controls, `<pre>`/monospace, sub/sup, text decorations,
  `text-transform`. CSS sizes map to the nearest Inter face; italic is drawn by shearing. Recursion and table nesting
  are bounded for the 64 KiB ring-3 stack.
- `wimage.c` — vendored stb_image 2.30 (`vendor/stb/`, public domain/MIT, unmodified) with PNG, JPEG (baseline and
  progressive), GIF (first frame) and BMP only, every floating-point path disabled; integer area/bilinear resampling to
  premultiplied ARGB. WebP/AVIF/SVG are recognised and left as placeholders.

**Web app (`kernel/user/mintde/web.c`, rewritten):** up to 4 parallel keep-alive connections (TLS or plain) with
request priority (document, style sheets, downloads, images), redirects (10, 303/POST→GET), cookies, style sheets fetched
before first layout (6 s cap), images fetched lazily near the viewport and decoded one per frame within a memory budget,
`<meta refresh>`, fragment links, image documents, plain text documents. Forms: text/search/password/hidden/checkbox/
radio/submit/reset/button/image inputs, `<textarea>`, `<select>` (popup list), `<button>`; focus, caret, typing (UTF-8),
Tab/Shift-Tab, implicit submission on Return, GET and POST. Downloads: non-displayable or `attachment` responses
stream to `~/Downloads/<name>` (unique names, Downloads panel with progress, "Show in Files"). Favorites:
`~/Library/Web/Bookmarks`, ⌘D toggles, book button popover, start page grid, File menu items (Add to Favorites,
Show Favorites, Show Downloads ⇧⌘J). Cookies persist in `~/Library/Web/Cookies`. Without a MintOS HD everything works
except persistence and downloads (clear message).

**TLS (`kernel/user/crypto/`):** TLS 1.2 client — ECDHE (X25519 or P-256) with ECDSA or RSA certificates and AEAD
suites only (ECDHE-{ECDSA,RSA}-AES128-GCM-SHA256, -AES256-GCM-SHA384, -CHACHA20-POLY1305), extended master secret
(classic PRF if the server lacks it), secure-renegotiation indication, SNI, ALPN, RFC 8446 downgrade sentinel checked,
ServerKeyExchange signature verified (PKCS#1, PSS, ECDSA), same no-bypass certificate policy. P-256 ECDH (new in
`pubkey.c`, on-curve check, always-add ladder) also serves TLS 1.3 through HelloRetryRequest (with cookie echo).
`secp384r1` is advertised (last) so TLS 1.2 servers can use P-384 certificates. Terminal curl prints the version.

**Shared files touched (small):** `Makefile` (engine objects, `test_web_engine.py` in `make check`),
`kernel/include/address_space.h` (`MINT_ASPACE_MAX_USER_FRAMES 16384u`, app-platform's exact text),
`kernel/user/desktop.c` (Web items in the File menu, `ACT_WEB_BASE`), `kernel/user/mintde/mintde.h` (`web_command`),
`kernel/user/mintde/apps.c` (curl TLS version line), `tools/test_usb_net_contracts.py`, `tools/test_tls_contracts.py`.
No syscalls or log components were added.

## Memory (important for the merge)
`WEB_HEAP_BYTES` in `web.c` is **8 MiB** on this branch, because the base tree's physmem (4096 claim records) caps a
process at ~14 MiB; MintDE is now ~12.5 MB (2.4 MB text + 10 MB BSS). After app-platform's frame pool is merged,
raise it to ~32–40 MiB (one define). With 8 MiB, Wikipedia's "Prague" (1.2 MB HTML, 21k nodes, 1,900 CSS rules)
lays out in ~6.5 MB; images then load only near the viewport and become placeholders when memory is short.

## Tests
- `tools/test_web_engine.py` (new, in `make check`): 139 checks — URL/cookie/form/date/heap unit tests, inflate vs
  Python zlib (560 streams + corrupt/truncated/bomb/fuzz), HTTP parser in random pieces, image decoders (PNG/GIF/BMP
  pixel-exact, JPEG vs libjpeg-turbo fixtures), resampling, layout of `tools/fixtures/web/article.html` at 320–1600 px,
  quirks mode, a deep/malformed stress page, ring-3 build with no FP instructions, source contracts.
- `tools/test_tls.py`: 39 local checks incl. 18 TLS 1.2 suite/curve/certificate combinations, no-EMS, CBC-only
  refusal, TLS 1.3 P-256 HRR; `--live` adds badssl.com TLS-1.2-only/ECC/RSA and expired/wrong-host/untrusted refusals.
- `tools/test_tls_contracts.py`, `tools/test_usb_net_contracts.py` updated for TLS 1.2 and the new web.c.
- Whole `make check` passes on macOS except `tools/test_context_switch.py` (Linux-only, as before).

## QEMU evidence (`test-evidence/`)
- `qemu-26.5.014.0-web-engine-wikipedia.png`, `-wikipedia-scrolled.png` — styled article, infobox photo, lazy images.
- `qemu-26.5.014.0-web-engine-duckduckgo-form.png` — DuckDuckGo Lite search submitted through its POST form.
- `qemu-26.5.014.0-web-engine-tls12-only-site.png` — tls-v1-2.badssl.com:1012 over TLS 1.2.
- `qemu-26.5.014.0-web-engine-download-panel.png`, `-download-in-files.png` — PDF saved to ~/Downloads.
- `qemu-26.5.014.0-web-engine-mintfs.txt` — host fsck of the disk afterwards; Downloads, Bookmarks and Cookies files.

## Sites (tested in QEMU, honestly)
| Site | Result |
|---|---|
| en.wikipedia.org articles | Works well: styled text, links, infobox, tables, images, references; header area has large gaps and the logo (SVG) is a placeholder; some navboxes at the bottom are wider than the window. |
| lite.duckduckgo.com (search, forms) | Works: form POST, results, selects. |
| news.ycombinator.com | Works: table layout and orange header; story titles were centred in the QEMU run, fixed afterwards (quirks-mode tables, covered by a host test, not re-shot). |
| tls-v1-2.badssl.com:1012, badssl ECC/RSA | Work over TLS 1.2; expired/wrong-host/untrusted certificates are refused. |
| www.bbc.com/news | Partly: navigation renders, headlines are laid out, but ~3,000 px of empty space precede them (their image containers/grid), so it looks empty at first. |
| Start page, favorites, downloads | Work; persisted on MintOS HD. |

Expected not to work: anything that needs JavaScript (single-page apps, consent walls rendered by script, Google search
results pages beyond basic HTML), CSS grid (laid out as blocks), positioned overlays (static or hidden), background
images, web fonts, SVG/WebP/AVIF images, `<iframe>`/video, horizontal scrolling of over-wide pages.

## Known limits
- Fonts are the MintDE Inter faces (11–30 px), so CSS sizes are approximated; no real italic or monospace face.
- Flexbox is approximate (no shrink/basis distribution, `flex-grow` makes an item take the row); no CSS grid.
- No HTTP cache, no `@import` (sheets linked from CSS), no `srcset` density selection (first candidate), no
  multipart form uploads, gzip downloads are buffered up to 64 MiB before writing.
- TLS: no session resumption, client certificates, CBC or RSA key exchange; P-384/P-521 ECDHE not offered.
- Text selection, copy, find-in-page and tabs are not implemented yet.
