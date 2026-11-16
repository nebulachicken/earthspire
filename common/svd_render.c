#include "svd_render.h"

#include <stdlib.h>

/* White point: 30% surface reflectance maps to full scale at gain 1.0.
 * Gamma is approximated by a square root (gamma 2.0). */
#define LUT_SIZE 8192

static uint32_t isqrt32(uint32_t v) {
    uint32_t res = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

static int clamp_gain(int g) {
    return g < SVD_GAIN_MIN ? SVD_GAIN_MIN : (g > SVD_GAIN_MAX ? SVD_GAIN_MAX : g);
}

uint8_t svd_tone(int32_t v, uint32_t dn_div, int gain_q8) {
    uint64_t lin;
    uint64_t white = (uint64_t)dn_div * 3u / 10u;
    if (v <= 0) return 0;
    if (white == 0) white = 1;
    gain_q8 = clamp_gain(gain_q8);
    lin = (uint64_t)v * 65535u * (uint64_t)gain_q8 / (256u * white);
    if (lin > 65535u) lin = 65535u;
    return (uint8_t)isqrt32((uint32_t)(lin * 65025u / 65535u));
}

void svd_render_defaults(svd_render_opts *o) {
    o->gain_q8 = SVD_GAIN_DEFAULT;
    o->show_clouds = 0;
}

static uint16_t pack565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static unsigned map_channel(const uint8_t *lut, int32_t v, uint32_t div, int gain) {
    if (v <= 0) return 0;
    if (v < LUT_SIZE) return lut[v];
    return svd_tone(v, div, gain);   /* rare: very bright pixel */
}

int svd_render_rgb565(const svd_info *info, const uint8_t *payload,
                      const svd_render_opts *opts, uint16_t *out) {
    size_t px, i;
    const uint8_t *pr, *pg, *pb, *scl;
    uint8_t *lut;
    int gain, clouds;
    int32_t off;

    if (!info || !payload || !out) return SVD_ERR_GEOMETRY;

    px = (size_t)info->width * info->height;
    pr = payload;
    pg = payload + px * 2;
    pb = payload + px * 4;
    scl = info->has_scl ? payload + px * 6 : NULL;
    gain = opts ? clamp_gain(opts->gain_q8) : SVD_GAIN_DEFAULT;
    clouds = opts ? opts->show_clouds : 0;
    off = info->dn_offset;

    lut = (uint8_t *)malloc(LUT_SIZE);
    if (!lut) return SVD_ERR_GEOMETRY;
    for (i = 0; i < LUT_SIZE; ++i)
        lut[i] = svd_tone((int32_t)i, info->dn_div, gain);

    for (i = 0; i < px; ++i) {
        unsigned R = map_channel(lut, (int32_t)svd_get_u16(pr + i * 2) + off, info->dn_div, gain);
        unsigned G = map_channel(lut, (int32_t)svd_get_u16(pg + i * 2) + off, info->dn_div, gain);
        unsigned B = map_channel(lut, (int32_t)svd_get_u16(pb + i * 2) + off, info->dn_div, gain);

        if (scl) {
            uint8_t c = scl[i];
            if (c == 0) {                                   /* no data */
                R = G = B = 0;
            } else if (clouds) {
                if (c == 8 || c == 9 || c == 10) {          /* clouds, cirrus */
                    R = (R + 255) / 2; G = (G + 64) / 2; B = (B + 255) / 2;
                } else if (c == 3) {                        /* cloud shadow */
                    R = R / 2; G = (G + 160) / 2; B = (B + 255) / 2;
                }
            }
        }
        out[i] = pack565(R, G, B);
    }
    free(lut);
    return SVD_OK;
}


int svd_pyramid_render_rgb565(const svd_pyramid_info *info,
                              const uint8_t *raw_payload, unsigned level,
                              const svd_render_opts *opts, uint16_t *out) {
    size_t px, i;
    const uint8_t *mask;
    int gain, clouds;
    uint32_t scale_q8;
    if (!info || !raw_payload || !out || level >= info->level_count ||
        info->width == 0 || info->height == 0) return SVD_ERR_GEOMETRY;

    px = (size_t)info->width * info->height;
    mask = raw_payload + px * 2u;
    gain = opts ? clamp_gain(opts->gain_q8) : SVD_GAIN_DEFAULT;
    clouds = opts ? opts->show_clouds : 0;
    /* Original renderer applies gain before its square-root tone curve. The
     * pre-rendered image therefore uses sqrt(gain) to preserve that feel. */
    scale_q8 = isqrt32((uint32_t)gain * 256u);

    for (i = 0; i < px; ++i) {
        const unsigned packed = (unsigned)mask[i >> 2];
        const unsigned cls = (packed >> ((i & 3u) * 2u)) & 3u;
        const unsigned v = svd_get_u16(raw_payload + i * 2u);
        unsigned r = (((v >> 11) & 31u) << 3) | (((v >> 11) & 31u) >> 2);
        unsigned g = (((v >> 5) & 63u) << 2) | (((v >> 5) & 63u) >> 4);
        unsigned b = ((v & 31u) << 3) | ((v & 31u) >> 2);

        if (cls == 1u) {
            r = g = b = 0;
        } else {
            r = (r * scale_q8 + 128u) >> 8;
            g = (g * scale_q8 + 128u) >> 8;
            b = (b * scale_q8 + 128u) >> 8;
            if (r > 255u) r = 255u;
            if (g > 255u) g = 255u;
            if (b > 255u) b = 255u;
            if (clouds && cls == 3u) {
                r = (r + 255u) / 2u; g = (g + 64u) / 2u; b = (b + 255u) / 2u;
            } else if (clouds && cls == 2u) {
                r = r / 2u; g = (g + 160u) / 2u; b = (b + 255u) / 2u;
            }
        }
        out[i] = pack565(r, g, b);
    }
    return SVD_OK;
}
