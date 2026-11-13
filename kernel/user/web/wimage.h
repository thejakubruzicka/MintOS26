#ifndef MINT_WEB_WIMAGE_H
#define MINT_WEB_WIMAGE_H
/* Image decoding for the Web engine: PNG, JPEG (baseline and progressive),
 * GIF (first frame) and BMP through the vendored stb_image, built with every
 * floating-point path disabled and allocating from the bounded wmem heap.
 * Plus integer-only resampling into premultiplied 0xAARRGGBB pixels. */
#include <stddef.h>
#include <stdint.h>

enum { WIMG_UNKNOWN = 0, WIMG_PNG, WIMG_JPEG, WIMG_GIF, WIMG_BMP, WIMG_SVG, WIMG_WEBP, WIMG_ICO, WIMG_AVIF };
int wimage_sniff(const uint8_t *data, size_t len);
const char *wimage_format_name(int format);
/* Dimensions without decoding; 1 on success. */
int wimage_info(const uint8_t *data, size_t len, int *w, int *h);
/* Decodes to straight-alpha RGBA (wmem buffer, caller frees). Refuses images
 * with more than max_pixels pixels. NULL on failure (see wimage_failure). */
uint8_t *wimage_decode(const uint8_t *data, size_t len, int *w, int *h, size_t max_pixels);
const char *wimage_failure(void);
/* Resamples straight RGBA sw x sh to dw x dh premultiplied ARGB (area average
 * when shrinking, bilinear when enlarging). wmem buffer, caller frees. */
uint32_t *wimage_scale(const uint8_t *rgba, int sw, int sh, int dw, int dh);
#endif
