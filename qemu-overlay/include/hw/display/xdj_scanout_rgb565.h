/* RGB565 conversion shared by the XDJ-XZ scanout and its focused unit test. */
#ifndef HW_DISPLAY_XDJ_SCANOUT_RGB565_H
#define HW_DISPLAY_XDJ_SCANOUT_RGB565_H

#include <stdint.h>

/* The physical fb0 format is little-endian RGB565 with floor expansion. */
static inline uint32_t xdj_scanout_rgb565_to_xrgb8888(uint16_t pixel)
{
    uint32_t r = ((pixel >> 11) & 0x1f) * 255 / 31;
    uint32_t g = ((pixel >> 5) & 0x3f) * 255 / 63;
    uint32_t b = (pixel & 0x1f) * 255 / 31;

    return 0xff000000u | (r << 16) | (g << 8) | b;
}

#endif /* HW_DISPLAY_XDJ_SCANOUT_RGB565_H */
