#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/include/desktop_abi.h"

#define CHECK(c,m) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",m); return 1; } else printf("PASS: %s\n",m); } while (0)
int main(void) {
    uint32_t pixels[8u*6u];
    for (size_t i=0;i<8u*6u;i++) pixels[i]=0xAA00AAu;
    MINT_DISPLAY_SURFACE surface={0};
    MINT_FRAMEBUFFER_INFO fb={0};
    fb.base=(uint64_t)(uintptr_t)pixels;fb.size=sizeof(pixels);fb.width=8;fb.height=6;
    fb.pixels_per_scanline=8;fb.pixel_format=1;
    CHECK(mint_display_surface_init(&surface,&fb),"bitmap test framebuffer initializes");
    const uint8_t row[8]={255,0,0,255, 0,0,255,128};
    CHECK(mint_display_current_draw_rgba_scaled_row(2,2,4,2,row),"bounded RGBA row scales and draws");
    CHECK(pixels[2u*8u+2u]==0xFF0000u && pixels[2u*8u+3u]==0xFF0000u,
          "nearest-neighbor scaling preserves opaque icon pixels");
    CHECK(pixels[2u*8u+4u]==0x5500D4u || pixels[2u*8u+4u]==0x5500D5u,
          "semi-transparent icon pixel blends over existing framebuffer");
    CHECK(mint_display_current_draw_rgba_scaled_row(-2,1,4,2,row),"partly offscreen bitmap row is safely clipped");
    CHECK(!mint_display_current_draw_rgba_scaled_row(0,1,129,2,row),"bitmap target width above policy is rejected");
    CHECK(!mint_display_current_draw_rgba_scaled_row(0,1,4,97,row),"bitmap source width above policy is rejected");
    puts("PASS: bounded RGBA bitmap rendering checks complete");
    return 0;
}
