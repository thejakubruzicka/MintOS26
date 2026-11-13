# MintOS SDK (26.5.014.0): static musl 1.2.5 + clang/lld, for command-line
# programs that run on MintOS through its Linux x86-64 syscall-compat subset.
# Included from mk/app-platform.mk. See docs/SDK.md.
SDK_DIR := build/sdk
SDK_SYSROOT := $(SDK_DIR)/sysroot
SDK_BIN := $(SDK_DIR)/bin
MUSL_SRC := vendor/musl
LUA_SRC := vendor/lua/src
LLVM_BIN ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /usr/lib/llvm-19/bin /usr/lib/llvm-18/bin))
SDK_AR ?= $(if $(LLVM_BIN),$(LLVM_BIN)/llvm-ar,llvm-ar)
SDK_RANLIB ?= $(if $(LLVM_BIN),$(LLVM_BIN)/llvm-ranlib,llvm-ranlib)
SDK_CLANG ?= $(shell command -v /usr/bin/clang 2>/dev/null || command -v clang)
SDK_CFLAGS := -target x86_64-unknown-linux-musl -nostdinc -isystem $(SDK_SYSROOT)/include \
	-fPIE -O2 -ffunction-sections -fdata-sections -Wall -Wextra
# Programs are static ET_EXEC images at 64 GiB (the start of MintOS's user
# window). crt1.o's weak _DYNAMIC reference is unused in static programs.
SDK_LDFLAGS := -static -nostdlib --image-base=0x1000000000 --defsym=_DYNAMIC=__ehdr_start \
	-z max-page-size=0x1000 --gc-sections -s
SDK_CRT_BEGIN := $(SDK_SYSROOT)/lib/crt1.o $(SDK_SYSROOT)/lib/crti.o
SDK_CRT_END := $(SDK_SYSROOT)/lib/libc.a $(SDK_SYSROOT)/lib/crtn.o
SDK_LIBC := $(SDK_SYSROOT)/lib/libc.a

$(SDK_LIBC): sdk/musl-config.mak
	mkdir -p $(BUILD)/musl-obj $(SDK_SYSROOT)
	cp sdk/musl-config.mak $(BUILD)/musl-obj/config.mak
	$(MAKE) -C $(BUILD)/musl-obj -f $(CURDIR)/$(MUSL_SRC)/Makefile MUSL_SRC=$(CURDIR)/$(MUSL_SRC) \
		SDK_CC=$(SDK_CLANG) SDK_AR=$(SDK_AR) SDK_RANLIB=$(SDK_RANLIB) DESTDIR=$(CURDIR)/$(SDK_SYSROOT) \
		install-headers install-libs >/dev/null

# Native command-line tools in sdk/programs/<name>.c.
SDK_TOOLS := hello wc grep sysinfo spin
SDK_TOOL_BINS := $(SDK_TOOLS:%=$(SDK_BIN)/%)
$(SDK_DIR)/obj/%.o: sdk/programs/%.c $(SDK_LIBC) $(wildcard kernel/include/uapi/mintos/*.h)
	mkdir -p $(dir $@)
	$(SDK_CLANG) $(SDK_CFLAGS) -Ikernel/include -c $< -o $@
$(SDK_BIN)/%: $(SDK_DIR)/obj/%.o $(SDK_LIBC)
	mkdir -p $(SDK_BIN)
	ld.lld $(SDK_LDFLAGS) -o $@ $(SDK_CRT_BEGIN) $< $(SDK_CRT_END)

# Lua 5.4 (interpreter + standard libraries), LUA_USE_POSIX.
LUA_CORE := lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate \
	lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib ldblib liolib lmathlib loadlib \
	loslib lstrlib ltablib lutf8lib linit lua
LUA_OBJS := $(LUA_CORE:%=$(SDK_DIR)/lua/%.o)
$(SDK_DIR)/lua/%.o: $(LUA_SRC)/%.c $(wildcard $(LUA_SRC)/*.h) $(SDK_LIBC)
	mkdir -p $(dir $@)
	$(SDK_CLANG) $(SDK_CFLAGS) -std=gnu99 -DLUA_USE_POSIX -Wno-unused-parameter -c $< -o $@
$(SDK_BIN)/lua: $(LUA_OBJS) $(SDK_LIBC)
	mkdir -p $(SDK_BIN)
	ld.lld $(SDK_LDFLAGS) -o $@ $(SDK_CRT_BEGIN) $(LUA_OBJS) $(SDK_CRT_END)

SDK_EXAMPLES := $(wildcard sdk/examples/*.lua)
PROGRAM_BUNDLE_FILES := $(SDK_TOOL_BINS) $(SDK_BIN)/lua $(SDK_EXAMPLES)
PROGRAM_BUNDLE_ARGS := $(foreach t,$(SDK_TOOLS) lua,/System/Programs/$(t)=$(SDK_BIN)/$(t)) \
	$(foreach e,$(SDK_EXAMPLES),/Library/Examples/Lua/$(notdir $(e))=$(e))

.PHONY: sdk
sdk: $(SDK_TOOL_BINS) $(SDK_BIN)/lua
