/* SVD: Sentinel Viewer Data container.
 *
 * SVD1 stores raw reflectance planes (legacy compatibility).
 * SVD2 stores a compressed RGB565 + 2-bit classification-mask pyramid.
 * All multi-byte fields are little-endian and are serialised byte-by-byte.
 */
#ifndef SVD_FORMAT_H
#define SVD_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SVD_HEADER_SIZE 128u
#define SVD_VERSION 1u
#define SVD2_HEADER_SIZE 256u
#define SVD2_VERSION 2u
#define SVD_MAX_LEVELS 5u
#define SVD_UNKNOWN_PERMILLE 0xFFFFu
#define SVD_CODEC_RAW 0u
#define SVD_CODEC_LZSS 1u
#define SVD_CODEC_LZSS_XOR 2u
#define SVD2_PIXEL_FORMAT_RGB565_MASK2 1u

enum {
    SVD_OK = 0,
    SVD_ERR_MAGIC,
    SVD_ERR_VERSION,
    SVD_ERR_HEADER_CRC,
    SVD_ERR_GEOMETRY,
    SVD_ERR_BANDS,
    SVD_ERR_PAYLOAD_SIZE,
    SVD_ERR_PAYLOAD_CRC,
    SVD_ERR_COMPRESSION,
    SVD_ERR_LEVEL
};

typedef struct svd_info {
    uint32_t width;
    uint32_t height;
    uint8_t  has_scl;
    int32_t  dn_offset;
    uint32_t dn_div;
    uint16_t cloud_permille;
    uint16_t nodata_permille;
    char     acquired[24];
    char     scene_id[56];
    uint32_t payload_size;
    uint32_t payload_crc32;
} svd_info;

typedef struct svd_pyramid_level {
    uint16_t resolution_m;       /* ground metres per pixel: 10, 20, 40... */
    uint8_t  codec;              /* RAW, LZSS, or LZSS with horizontal XOR */
    uint32_t raw_size;           /* RGB565 image + packed 2-bit mask */
    uint32_t stored_size;
    uint32_t raw_crc32;
} svd_pyramid_level;

typedef struct svd_pyramid_info {
    uint32_t width;
    uint32_t height;
    uint8_t  level_count;
    uint16_t cloud_permille;
    uint16_t nodata_permille;
    char     acquired[24];
    char     scene_id[56];
    svd_pyramid_level levels[SVD_MAX_LEVELS];
} svd_pyramid_info;

uint32_t svd_crc32(uint32_t crc, const void *data, size_t len);
size_t svd_expected_payload_size(uint32_t width, uint32_t height, int has_scl);
void svd_header_write(const svd_info *info, uint8_t out[SVD_HEADER_SIZE]);
int svd_header_parse(const uint8_t in[SVD_HEADER_SIZE], svd_info *info);
int svd_payload_verify(const svd_info *info, const uint8_t *payload, size_t len);

/* SVD2 header and payload helpers. A level's decoded bytes are:
 *   width*height uint16 RGB565 pixels, little-endian;
 *   ceil(width*height/4) bytes containing 2-bit classes:
 *     0 normal, 1 no-data, 2 cloud shadow, 3 cloud/cirrus.
 */
size_t svd_pyramid_raw_size(uint32_t width, uint32_t height);
void svd_pyramid_header_write(const svd_pyramid_info *info,
                              uint8_t out[SVD2_HEADER_SIZE]);
int svd_pyramid_header_parse(const uint8_t in[SVD2_HEADER_SIZE],
                             svd_pyramid_info *info);
int svd_pyramid_level_decode(const svd_pyramid_info *info, unsigned level,
                             const uint8_t *stored, size_t stored_len,
                             uint8_t *raw, size_t raw_len);

/* Small LZSS codec used by SVD2. The decoder is intentionally tiny and
 * portable for the calculator; the encoder is used by the desktop exporter. */
size_t svd_lzss_compress_bound(size_t input_len);
int svd_lzss_compress(const uint8_t *src, size_t src_len,
                      uint8_t *dst, size_t dst_cap, size_t *written);
int svd_lzss_decompress(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_len);

static inline uint16_t svd_get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline void svd_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

const char *svd_strerror(int code);

#ifdef __cplusplus
}
#endif
#endif
