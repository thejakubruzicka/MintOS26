# Hand-written musl 1.2.5 config for the MintOS SDK (replaces ./configure).
# Static libc only, x86-64, built with clang for the x86_64-linux-musl target.
# MintOS runs these binaries through its Linux x86-64 syscall-compat subset.
ARCH = x86_64
SUBARCH =
ASMSUBARCH =
srcdir = $(MUSL_SRC)
prefix = /
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin
libdir = $(prefix)/lib
includedir = $(prefix)/include
syslibdir = /lib
CC = $(SDK_CC) -target x86_64-unknown-linux-musl
CFLAGS = -O2 -fPIE
CFLAGS_C99FSE = -std=c99 -nostdinc -ffreestanding -fexcess-precision=standard -frounding-math -fno-strict-aliasing
CFLAGS_AUTO = $(CFLAGS_C99FSE) -D_XOPEN_SOURCE=700 -fomit-frame-pointer -fno-unwind-tables \
	-fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -w
CFLAGS_MEMOPS =
CFLAGS_NOSSP = -fno-stack-protector
CPPFLAGS =
LDFLAGS =
LDFLAGS_AUTO =
CROSS_COMPILE =
AR = $(SDK_AR)
RANLIB = $(SDK_RANLIB)
LIBCC =
OPTIMIZE_GLOBS = internal/*.c malloc/*.c string/*.c
ALL_TOOLS =
TOOL_LIBS =
ADD_CFI = no
MALLOC_DIR = mallocng
SHARED_LIBS =
WRAPCC_GCC = $(CC)
WRAPCC_CLANG = $(CC)
