/* Host tool: render the MintOS boot splash (kernel/ui/splash_art.c) to a
 * binary PPM so the boot animation can be reviewed off-target.
 *   render_splash_preview out.ppm [width height leaf_alpha progress shimmer] */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../kernel/include/boot_splash.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s out.ppm [w h alpha progress shimmer]\n", argv[0]); return 2; }
    int w = argc > 3 ? atoi(argv[2]) : 1920, h = argc > 3 ? atoi(argv[3]) : 1080;
    unsigned alpha = argc > 4 ? (unsigned)atoi(argv[4]) : 255u, progress = argc > 5 ? (unsigned)atoi(argv[5]) : 620u;
    unsigned shimmer = argc > 6 ? (unsigned)atoi(argv[6]) : 120u;
    if (w < 64 || h < 64 || w > 8192 || h > 8192) return 2;
    uint32_t *px = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!px) return 1;
    MINT_SPLASH_GEOMETRY g;
    mint_splash_geometry(&g, w, h);
    mint_splash_render(px, w, &g, 0, 0, w, h, alpha, progress, shimmer);
    FILE *f = fopen(argv[1], "wb");
    if (!f) { free(px); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        unsigned char c[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i]};
        fwrite(c, 1, 3, f);
    }
    fclose(f);
    free(px);
    return 0;
}
