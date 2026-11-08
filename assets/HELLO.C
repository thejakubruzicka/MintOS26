/* MintOS26 26.3.004.4 - Code Studio starter file */
#include <stdint.h>

/* User-space execution is not implemented yet. This file is a starter source
 * for the future MintOS UAPI/runtime. It currently builds as documentation only. */
int main(void) {
    volatile uint64_t welcome = 26003;
    return (int)(welcome & 0u);
}
