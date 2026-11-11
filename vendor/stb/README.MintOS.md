# stb_image (vendored)

- Source: https://github.com/nothings/stb — `stb_image.h` v2.30 and `LICENSE`, downloaded
  2026-11-10 from the `master` branch (commit 4e9b368bfc071e97206ea5c6adba68dd0cbf8897).
- SHA-256: `stb_image.h` 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3,
  `LICENSE` bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63.
- Licence: public domain (Unlicense) or MIT, at your choice (see `LICENSE`).
- Unmodified. MintOS compiles it once in `kernel/user/web/wimage.c` with PNG, JPEG,
  GIF and BMP only and every floating-point path disabled (`STBI_NO_HDR`,
  `STBI_NO_LINEAR`, `STBI_NO_SIMD`), allocating from the Web engine's bounded heap.
  `tools/test_web_engine.py` checks the MintOS object file contains no SSE/x87
  instructions.
