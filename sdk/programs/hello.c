/* hello: the first MintOS SDK program (musl libc, Linux-compat syscalls). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

int main(int argc, char **argv) {
    struct utsname u;
    printf("Hello from a MintOS program! (pid %d)\n", (int)getpid());
    if (uname(&u) == 0) printf("Running on %s %s (%s)\n", u.sysname, u.release, u.machine);
    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; ++i) printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    const char *home = getenv("HOME");
    printf("HOME = %s\n", home ? home : "(unset)");
    double x = 2.0;
    for (int i = 0; i < 5; ++i) x = x / 2.0 + 1.0 / x;   /* Newton steps toward sqrt(2) */
    printf("sqrt(2) ~= %.12f\n", x);
    char *buffer = malloc(1 << 20);
    if (buffer) { memset(buffer, 'm', 1 << 20); printf("malloc(1 MiB) ok, last byte '%c'\n", buffer[(1 << 20) - 1]); free(buffer); }
    return argc > 1 && strcmp(argv[1], "fail") == 0 ? 3 : 0;
}
