# MintDE 2.2.0 — Secure web browsing

- **Web opens HTTPS sites.**
  - Connections use MintOS's own TLS 1.3, and every certificate is verified against the Mozilla list of trusted authorities.
  - The address bar shows a padlock and the site's host name, like Safari.
  - If verification fails, the page "This connection is not private" explains why (expired, wrong site, untrusted issuer) and nothing is sent. There is no bypass.
  - Sites that only support TLS 1.2 get an explanation.
- **Search** in the address bar now uses DuckDuckGo Lite over HTTPS.
- **Start page** links to Wikipedia, NPR, CNN Lite, Hacker News, 68k.news and the first website.
- **Pages open at their main content** when the site marks it (`<main>`, `<article>`), skipping long navigation menus. Larger pages (up to 1 MiB of HTML) are supported.
- **Fix:** relative links and relative redirects resolved against an empty host in MintDE 2.1.0. They now use the current page's address.
- **Terminal:** `curl https://…` works. It shows the negotiated cipher suite and reports certificate problems the way curl does (`curl: (60) SSL certificate problem: …`).
