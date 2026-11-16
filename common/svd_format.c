#include "svd_format.h"

#include <stdlib.h>
#include <string.h>

#define MAX_DIM 4096u
#define LZ_WINDOW 4096u
#define LZ_MIN_MATCH 3u
#define LZ_MAX_MATCH 18u
#define LZ_HASH_SIZE 65536u
#define LZ_CHAIN_LIMIT 64u

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
    size_t px;
    if (width == 0 || height == 0 || width > MAX_DIM || height > MAX_DIM) return 0;
    px = (size_t)width * height;
    return px * 6u + (has_scl ? px : 0u);
}

size_t svd_pyramid_raw_size(uint32_t width, uint32_t height) {
    size_t px;
    if (width == 0 || height == 0 || width > MAX_DIM || height > MAX_DIM) return 0;
    px = (size_t)width * height;
    return px * 2u + (px + 3u) / 4u;
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
    memcpy(out + 32, info->acquired, 23);
    memcpy(out + 56, info->scene_id, 55);
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

void svd_pyramid_header_write(const svd_pyramid_info *info,
                              uint8_t out[SVD2_HEADER_SIZE]) {
    unsigned i;
    memset(out, 0, SVD2_HEADER_SIZE);
    memcpy(out, "SVD2", 4);
    svd_put_u16(out + 4, SVD2_VERSION);
    svd_put_u16(out + 6, SVD2_HEADER_SIZE);
    put_u32(out + 8, info->width);
    put_u32(out + 12, info->height);
    out[16] = info->level_count;
    out[17] = SVD2_PIXEL_FORMAT_RGB565_MASK2;
    svd_put_u16(out + 18, info->cloud_permille);
    svd_put_u16(out + 20, info->nodata_permille);
    memcpy(out + 24, info->acquired, 23);
    memcpy(out + 48, info->scene_id, 55);
    for (i = 0; i < info->level_count && i < SVD_MAX_LEVELS; ++i) {
        uint8_t *p = out + 112u + i * 16u;
        svd_put_u16(p, info->levels[i].resolution_m);
        p[2] = info->levels[i].codec;
        put_u32(p + 4, info->levels[i].raw_size);
        put_u32(p + 8, info->levels[i].stored_size);
        put_u32(p + 12, info->levels[i].raw_crc32);
    }
    put_u32(out + 252, svd_crc32(0, out, 252));
}

int svd_pyramid_header_parse(const uint8_t in[SVD2_HEADER_SIZE],
                             svd_pyramid_info *info) {
    uint32_t w, h;
    unsigned i;
    if (memcmp(in, "SVD2", 4) != 0) return SVD_ERR_MAGIC;
    if (svd_get_u16(in + 4) != SVD2_VERSION || svd_get_u16(in + 6) != SVD2_HEADER_SIZE)
        return SVD_ERR_VERSION;
    if (svd_crc32(0, in, 252) != get_u32(in + 252)) return SVD_ERR_HEADER_CRC;
    w = get_u32(in + 8);
    h = get_u32(in + 12);
    if (w == 0 || h == 0 || w > MAX_DIM || h > MAX_DIM) return SVD_ERR_GEOMETRY;
    if (in[16] == 0 || in[16] > SVD_MAX_LEVELS) return SVD_ERR_LEVEL;
    if (in[17] != SVD2_PIXEL_FORMAT_RGB565_MASK2) return SVD_ERR_BANDS;

    memset(info, 0, sizeof *info);
    info->width = w;
    info->height = h;
    info->level_count = in[16];
    info->cloud_permille = svd_get_u16(in + 18);
    info->nodata_permille = svd_get_u16(in + 20);
    memcpy(info->acquired, in + 24, 23);
    memcpy(info->scene_id, in + 48, 55);

    for (i = 0; i < info->level_count; ++i) {
        const uint8_t *p = in + 112u + i * 16u;
        svd_pyramid_level *level = &info->levels[i];
        const uint32_t expected = (uint32_t)svd_pyramid_raw_size(w, h);
        level->resolution_m = svd_get_u16(p);
        level->codec = p[2];
        level->raw_size = get_u32(p + 4);
        level->stored_size = get_u32(p + 8);
        level->raw_crc32 = get_u32(p + 12);
        if (level->resolution_m != (uint16_t)(10u << i)) return SVD_ERR_LEVEL;
        if (level->raw_size != expected || level->stored_size == 0) return SVD_ERR_PAYLOAD_SIZE;
        if (level->codec == SVD_CODEC_RAW) {
            if (level->stored_size != level->raw_size) return SVD_ERR_PAYLOAD_SIZE;
        } else if (level->codec == SVD_CODEC_LZSS || level->codec == SVD_CODEC_LZSS_XOR) {
            if ((size_t)level->stored_size > svd_lzss_compress_bound(level->raw_size))
                return SVD_ERR_PAYLOAD_SIZE;
        } else {
            return SVD_ERR_COMPRESSION;
        }
    }
    return SVD_OK;
}

size_t svd_lzss_compress_bound(size_t input_len) {
    const size_t groups = input_len / 8u + ((input_len % 8u) ? 1u : 0u);
    if (input_len > (size_t)-1 - groups) return 0;
    return input_len + groups;
}

static uint32_t hash3(const uint8_t *p) {
    uint32_t h = (uint32_t)p[0] * 251u + (uint32_t)p[1] * 31u + p[2];
    return h & (LZ_HASH_SIZE - 1u);
}

int svd_lzss_compress(const uint8_t *src, size_t src_len,
                      uint8_t *dst, size_t dst_cap, size_t *written) {
    int32_t *head = NULL, *prev = NULL;
    size_t pos = 0, out = 0;
    if (!written || (!src && src_len) || (!dst && dst_cap)) return SVD_ERR_COMPRESSION;
    *written = 0;
    if (src_len == 0) return SVD_OK;
    if (src_len > (size_t)0x7FFFFFFF) return SVD_ERR_COMPRESSION;
    head = (int32_t *)malloc(LZ_HASH_SIZE * sizeof(int32_t));
    prev = (int32_t *)malloc(src_len * sizeof(int32_t));
    if (!head || !prev) { free(head); free(prev); return SVD_ERR_COMPRESSION; }
    for (size_t i = 0; i < LZ_HASH_SIZE; ++i) head[i] = -1;
    for (size_t i = 0; i < src_len; ++i) prev[i] = -1;

    while (pos < src_len) {
        size_t control_pos;
        uint8_t control = 0;
        if (out >= dst_cap) goto fail;
        control_pos = out++;
        for (unsigned bit = 0; bit < 8 && pos < src_len; ++bit) {
            size_t best_len = 0, best_dist = 0;
            int32_t candidate = -1;
            if (pos + 2 < src_len) {
                const uint32_t h = hash3(src + pos);
                candidate = head[h];
                unsigned searched = 0;
                const size_t min_pos = pos > LZ_WINDOW ? pos - LZ_WINDOW : 0;
                while (candidate >= 0 && (size_t)candidate >= min_pos && searched < LZ_CHAIN_LIMIT) {
                    const size_t c = (size_t)candidate;
                    size_t n = 0;
                    ++searched;
                    if (src[c] == src[pos] && src[c + 1] == src[pos + 1] && src[c + 2] == src[pos + 2]) {
                        const size_t max_len = (src_len - pos < LZ_MAX_MATCH) ? src_len - pos : LZ_MAX_MATCH;
                        while (n < max_len && src[c + n] == src[pos + n]) ++n;
                        if (n > best_len) {
                            best_len = n;
                            best_dist = pos - c;
                            if (n == LZ_MAX_MATCH) break;
                        }
                    }
                    candidate = prev[c];
                }
            }

            if (best_len >= LZ_MIN_MATCH) {
                const size_t dm1 = best_dist - 1u;
                const uint8_t a = (uint8_t)(dm1 & 0xFFu);
                const uint8_t b = (uint8_t)(((dm1 >> 4) & 0xF0u) | (best_len - LZ_MIN_MATCH));
                if (out + 2 > dst_cap) goto fail;
                dst[out++] = a;
                dst[out++] = b;
                for (size_t j = 0; j < best_len; ++j) {
                    const size_t at = pos + j;
                    if (at + 2 < src_len) {
                        const uint32_t h = hash3(src + at);
                        prev[at] = head[h];
                        head[h] = (int32_t)at;
                    }
                }
                pos += best_len;
            } else {
                control |= (uint8_t)(1u << bit);
                if (out >= dst_cap) goto fail;
                dst[out++] = src[pos];
                if (pos + 2 < src_len) {
                    const uint32_t h = hash3(src + pos);
                    prev[pos] = head[h];
                    head[h] = (int32_t)pos;
                }
                ++pos;
            }
        }
        dst[control_pos] = control;
    }
    *written = out;
    free(head);
    free(prev);
    return SVD_OK;

fail:
    free(head);
    free(prev);
    return SVD_ERR_COMPRESSION;
}

int svd_lzss_decompress(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_len) {
    size_t in = 0, out = 0;
    if ((!src && src_len) || (!dst && dst_len)) return SVD_ERR_COMPRESSION;
    while (out < dst_len) {
        uint8_t control;
        if (in >= src_len) return SVD_ERR_COMPRESSION;
        control = src[in++];
        for (unsigned bit = 0; bit < 8 && out < dst_len; ++bit) {
            if (control & (uint8_t)(1u << bit)) {
                if (in >= src_len) return SVD_ERR_COMPRESSION;
                dst[out++] = src[in++];
            } else {
                size_t dist, len;
                uint8_t a, b;
                if (in + 2 > src_len) return SVD_ERR_COMPRESSION;
                a = src[in++];
                b = src[in++];
                dist = ((size_t)(b & 0xF0u) << 4) | a;
                ++dist;
                len = (size_t)(b & 0x0Fu) + LZ_MIN_MATCH;
                if (dist > out || len > dst_len - out) return SVD_ERR_COMPRESSION;
                while (len--) {
                    dst[out] = dst[out - dist];
                    ++out;
                }
            }
        }
    }
    return in == src_len ? SVD_OK : SVD_ERR_COMPRESSION;
}

int svd_pyramid_level_decode(const svd_pyramid_info *info, unsigned level,
                             const uint8_t *stored, size_t stored_len,
                             uint8_t *raw, size_t raw_len) {
    int rc;
    const svd_pyramid_level *meta;
    if (!info || level >= info->level_count || level >= SVD_MAX_LEVELS) return SVD_ERR_LEVEL;
    meta = &info->levels[level];
    if (!stored || !raw || stored_len != meta->stored_size || raw_len != meta->raw_size)
        return SVD_ERR_PAYLOAD_SIZE;
    if (meta->codec == SVD_CODEC_RAW) {
        memcpy(raw, stored, raw_len);
        rc = SVD_OK;
    } else if (meta->codec == SVD_CODEC_LZSS || meta->codec == SVD_CODEC_LZSS_XOR) {
        rc = svd_lzss_decompress(stored, stored_len, raw, raw_len);
        if (rc == SVD_OK && meta->codec == SVD_CODEC_LZSS_XOR) {
            uint32_t y;
            /* The first pixel of each row is literal. Other pixels contain
             * XOR against their left neighbour; decode left-to-right. */
            for (y = 0; y < info->height; ++y) {
                uint32_t x;
                const size_t row = (size_t)y * info->width;
                for (x = 1; x < info->width; ++x) {
                    const size_t at = row + x;
                    const uint16_t delta = svd_get_u16(raw + at * 2u);
                    const uint16_t left = svd_get_u16(raw + (at - 1u) * 2u);
                    svd_put_u16(raw + at * 2u, (uint16_t)(delta ^ left));
                }
            }
        }
    } else {
        return SVD_ERR_COMPRESSION;
    }
    if (rc != SVD_OK) return rc;
    return svd_crc32(0, raw, raw_len) == meta->raw_crc32 ? SVD_OK : SVD_ERR_PAYLOAD_CRC;
}

const char *svd_strerror(int code) {
    switch (code) {
    case SVD_OK:               return "ok";
    case SVD_ERR_MAGIC:        return "not an SVD file";
    case SVD_ERR_VERSION:      return "unsupported SVD version";
    case SVD_ERR_HEADER_CRC:   return "header checksum mismatch";
    case SVD_ERR_GEOMETRY:     return "invalid image geometry";
    case SVD_ERR_BANDS:        return "unsupported band or pixel layout";
    case SVD_ERR_PAYLOAD_SIZE: return "payload size mismatch (truncated transfer?)";
    case SVD_ERR_PAYLOAD_CRC:  return "payload checksum mismatch (corrupt transfer?)";
    case SVD_ERR_COMPRESSION:  return "compressed image data is invalid";
    case SVD_ERR_LEVEL:        return "invalid zoom-level table";
    default:                   return "unknown error";
    }
}
