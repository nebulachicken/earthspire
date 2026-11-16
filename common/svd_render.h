/* Converts legacy SVD1 reflectance data into an RGB565 image.
 * Integer-only (no FPU, no libm) so it is cheap on the calculator's ARM926.
 * The desktop app reuses this code for its optional BMP preview, so the
 * preview matches what the calculator shows. */
#ifndef SVD_RENDER_H
#define SVD_RENDER_H

#include "svd_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SVD_GAIN_MIN 128   /* 0.5x */
#define SVD_GAIN_MAX 1024  /* 4.0x */
#define SVD_GAIN_DEFAULT 256

typedef struct svd_render_opts {
    int gain_q8;        /* 256 = 1.0. Clamped to [SVD_GAIN_MIN, SVD_GAIN_MAX]. */
    int show_clouds;    /* tint SCL cloud / shadow pixels */
} svd_render_opts;

void svd_render_defaults(svd_render_opts *opts);

/* payload must already be verified. out holds width*height uint16 (RGB565).
 * Returns SVD_OK or SVD_ERR_*. */
int svd_render_rgb565(const svd_info *info, const uint8_t *payload,
                      const svd_render_opts *opts, uint16_t *out);

/* Render a decoded SVD2 level. raw_payload contains RGB565 pixels followed by
 * the packed 2-bit classification mask described in svd_format.h. */
int svd_pyramid_render_rgb565(const svd_pyramid_info *info,
                              const uint8_t *raw_payload, unsigned level,
                              const svd_render_opts *opts, uint16_t *out);

/* Tone curve, exposed for tests: DN with offset already applied -> 0..255. */
uint8_t svd_tone(int32_t dn_plus_offset, uint32_t dn_div, int gain_q8);

#ifdef __cplusplus
}
#endif
#endif
