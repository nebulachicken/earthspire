#include "svd_format.h"

#include <string.h>

#define MAX_DIM 4096u

static uint32_t g_table[256];
static int g_table_ready = 0;

static void build_table(void) {
    uint32_t i, k, c;
    for (i = 0; i < 256; ++i) {
        c = i;
        for (k = 0; k < 8; ++k)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_table[i] = c;
    }
    g_table_ready = 1;
}

uint32_t svd_crc32(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    if (!g_table_ready) build_table();
    crc = ~crc;
    while (len--) crc = g_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

size_t svd_expected_payload_size(uint32_t width, uint32_t height, int has_scl) {
    uint32_t px;
    if (width == 0 || height == 0 || width > MAX_DIM || height > MAX_DIM) return 0;
    px = width * height; /* <= 16.7M, fits in 32 bits */
    return (size_t)px * 6u + (has_scl ? (size_t)px : 0u);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void svd_header_write(const svd_info *info, uint8_t out[SVD_HEADER_SIZE]) {
    memset(out, 0, SVD_HEADER_SIZE);
    memcpy(out, "SVD1", 4);
    svd_put_u16(out + 4, SVD_VERSION);
    svd_put_u16(out + 6, SVD_HEADER_SIZE);
    put_u32(out + 8, info->width);
    put_u32(out + 12, info->height);
    out[16] = 3;
    out[17] = info->has_scl ? 1 : 0;
    put_u32(out + 20, (uint32_t)info->dn_offset);
    put_u32(out + 24, info->dn_div);
    svd_put_u16(out + 28, info->cloud_permille);
    svd_put_u16(out + 30, info->nodata_permille);
    memcpy(out + 32, info->acquired, 23);   /* byte 55 stays NUL */
    memcpy(out + 56, info->scene_id, 55);   /* byte 111 stays NUL */
    put_u32(out + 112, info->payload_crc32);
    put_u32(out + 116, info->payload_size);
    put_u32(out + 124, svd_crc32(0, out, 124));
}

int svd_header_parse(const uint8_t in[SVD_HEADER_SIZE], svd_info *info) {
    uint32_t w, h;
    if (memcmp(in, "SVD1", 4) != 0) return SVD_ERR_MAGIC;
    if (svd_get_u16(in + 4) != SVD_VERSION || svd_get_u16(in + 6) != SVD_HEADER_SIZE)
        return SVD_ERR_VERSION;
    if (svd_crc32(0, in, 124) != get_u32(in + 124)) return SVD_ERR_HEADER_CRC;

    w = get_u32(in + 8);
    h = get_u32(in + 12);
    if (w == 0 || h == 0 || w > MAX_DIM || h > MAX_DIM) return SVD_ERR_GEOMETRY;
    if (in[16] != 3) return SVD_ERR_BANDS;

    memset(info, 0, sizeof *info);
    info->width = w;
    info->height = h;
    info->has_scl = in[17] ? 1 : 0;
    info->dn_offset = (int32_t)get_u32(in + 20);
    info->dn_div = get_u32(in + 24);
    if (info->dn_div == 0) return SVD_ERR_GEOMETRY;
    info->cloud_permille = svd_get_u16(in + 28);
    info->nodata_permille = svd_get_u16(in + 30);
    memcpy(info->acquired, in + 32, 23);
    memcpy(info->scene_id, in + 56, 55);
    info->payload_crc32 = get_u32(in + 112);
    info->payload_size = get_u32(in + 116);
    if (info->payload_size != svd_expected_payload_size(w, h, info->has_scl))
        return SVD_ERR_PAYLOAD_SIZE;
    return SVD_OK;
}

int svd_payload_verify(const svd_info *info, const uint8_t *payload, size_t len) {
    if (len != info->payload_size) return SVD_ERR_PAYLOAD_SIZE;
    if (svd_crc32(0, payload, len) != info->payload_crc32) return SVD_ERR_PAYLOAD_CRC;
    return SVD_OK;
}

const char *svd_strerror(int code) {
    switch (code) {
    case SVD_OK:               return "ok";
    case SVD_ERR_MAGIC:        return "not an SVD1 file";
    case SVD_ERR_VERSION:      return "unsupported SVD version";
    case SVD_ERR_HEADER_CRC:   return "header checksum mismatch";
    case SVD_ERR_GEOMETRY:     return "invalid image geometry";
    case SVD_ERR_BANDS:        return "unsupported band layout";
    case SVD_ERR_PAYLOAD_SIZE: return "payload size mismatch (truncated transfer?)";
    case SVD_ERR_PAYLOAD_CRC:  return "payload checksum mismatch (corrupt transfer?)";
    default:                   return "unknown error";
    }
}
