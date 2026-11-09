# MintOS26 26.5.012.0 / MintDE 2.2.0 worklog

Previous worklog: `docs/WORKLOG-26.5.011.0.md`.

## Goal

Make the Web app reach the modern, HTTPS-only web, with real certificate verification rather than "encrypted but unauthenticated".

## Design decisions

- **TLS lives in user space.** The kernel already provides TCP. The TLS client and its crypto are a library linked into MintDE, so a parser bug cannot corrupt the kernel. The same C files are compiled on the build host and tested there against OpenSSL and real servers before running in MintOS.
- **TLS 1.3 only, X25519 only.** This keeps the state machine small, and most of the web supports it. TLS 1.2 and P-256 key exchange are left for later; affected servers get a specific error message instead of a generic failure.
- **Verify, never bypass.**
  - Chains are validated to Mozilla's root list, the same list Firefox and curl use. It was downloaded from curl.se with the user's approval.
  - The generator checks every certificate's SHA-256 fingerprint and keeps only subject names and public keys (54 KB).
  - Certificate errors stop the load, and the UI offers no "proceed anyway".
- **Verification-only public-key code.** RSA and ECDSA only verify public data, so a simple, readable Montgomery big-number implementation is enough. Heavy temporaries are static to stay well within the user stack.
- **Randomness.** RDSEED/RDRAND, where present (Skylake, QEMU `-cpu max`), feeds a SHA-512 DRBG, always mixed with TSC jitter. A kernel entropy pool is a later step.

## Problems found during the work

- **ECDSA failed while RSA passed.** `bn_add`/`bn_sub` wrote only the active limbs, so stack temporaries such as `p − 2` kept garbage in their upper limbs. Exponentiation scanned all 130 limbs, so inversion produced nonsense. A white-box test located it: the curve constants and Montgomery round trip were correct, but 2G was not on the curve and the inverse was wrong. Both functions now clear the unused limbs.
- **Relative links were broken in 26.5.011.0.** `page_url` was never assigned, so relative links and relative redirects resolved against an empty host. It is now set when a response arrives, before redirect handling.
- **badssl.com does not speak TLS 1.3,** so it cannot be used for certificate-error tests of this client. The local `openssl s_server` tests cover wrong host, untrusted root and every signature type instead.
- **3 MB of zeros in the desktop ELF.** With `-mcmodel=large`, clang emits `.ldata`/`.lbss`. A small `.ldata` (two structs initialised to -1) made lld turn the whole merged `.data` output section into file bytes, including all zero-fill buffers. Zero-fill now has its own `NOLOAD` `.bss` output section: the desktop ELF went from 5.1 MB to 2.1 MB and the kernel from 17.8 MB to 14.8 MB.
- **A flaky test of my own.** The non-CA test sometimes saw "not yet valid", because it captured `now` before minting certificates. The chain checks now read the clock each time.
- **Small user stack.** MintDE had a 16 KiB stack; the default is now the loader's 64 KiB limit.

## Validation

- **Host:**
  - `test_crypto.py`: 19 checks (hashlib/hmac, RFC 8439, RFC 7748 including the 1,000-iteration test, FIPS-197, NIST GCM, OpenSSL X25519).
  - `test_x509.py`: 23 checks, plus live chains from six sites.
  - `test_tls.py`: 17 local `s_server` checks, plus live sites under every forced suite.
  - `test_tls_contracts.py`: 16 checks.
- **QEMU:**
  - `curl -I https://example.com` and `curl -s https://www.google.com/` (88 KB in 560 ms).
  - Web with Wikipedia over HTTPS (padlock, opens at the article title) and DuckDuckGo search results.
- **Not validated:** VirtualBox and the physical OptiPlex 7040.
