# Third-party sources (26.5.014.0 app platform)

| Directory | Project | Version | Source | Licence |
|---|---|---|---|---|
| `musl/` | musl libc | 1.2.5 | https://musl.libc.org/releases/musl-1.2.5.tar.gz (SHA-256 a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4) | MIT, see `musl/COPYRIGHT` |
| `lua/` | Lua | 5.4.9 | https://www.lua.org/ftp/lua-5.4.9.tar.gz (SHA-256 2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6) | MIT, see `lua/LICENSE` |

Both trees are unmodified upstream releases. musl is built out of tree by
`sdk/sdk.mk` with the hand-written `sdk/musl-config.mak`; Lua is compiled from
`lua/src` with `LUA_USE_POSIX`.
