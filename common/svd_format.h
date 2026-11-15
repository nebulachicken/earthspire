/* SVD1: Sentinel Viewer Data container, version 1.
 *
 * Portable C (no OS dependencies) shared by the desktop downloader and the
 * calculator client. All multi-byte fields are little-endian and are read and
 * written byte by byte, so struct layout and host endianness never matter.
 *
 * File layout:
 *   [128-byte header]
 *   [payload]: R plane, G plane, B plane (uint16 LE, width*height each),
 *              then optional SCL plane (uint8, width*height).
 *
 * Reflectance = (DN + dn_offset) / dn_div.
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
#define SVD_UNKNOWN_PERMILLE 0xFFFFu

enum {
    SVD_OK = 0,
    SVD_ERR_MAGIC,
    SVD_ERR_VERSION,
    SVD_ERR_HEADER_CRC,
    SVD_ERR_GEOMETRY,
    SVD_ERR_BANDS,
    SVD_ERR_PAYLOAD_SIZE,
    SVD_ERR_PAYLOAD_CRC
};

typedef struct svd_info {
    uint32_t width;
    uint32_t height;
    uint8_t  has_scl;           /* 1 if an SCL byte plane follows the RGB planes */
    int32_t  dn_offset;         /* added to raw DN before dividing */
    uint32_t dn_div;            /* usually 10000 */
    uint16_t cloud_permille;    /* cloudy+shadow share of crop, or SVD_UNKNOWN_PERMILLE */
    uint16_t nodata_permille;   /* SCL no-data share of crop, or SVD_UNKNOWN_PERMILLE */
    char     acquired[24];      /* "YYYY-MM-DDTHH:MM:SSZ", NUL-terminated */
    char     scene_id[56];      /* STAC item id, truncated, NUL-terminated */
    uint32_t payload_size;
    uint32_t payload_crc32;
} svd_info;

uint32_t svd_crc32(uint32_t crc, const void *data, size_t len); /* start with crc = 0 */

/* Payload size implied by geometry; 0 on invalid dimensions. */
size_t svd_expected_payload_size(uint32_t width, uint32_t height, int has_scl);

/* Serialise info into out[128]. payload_size / payload_crc32 must already be set. */
void svd_header_write(const svd_info *info, uint8_t out[SVD_HEADER_SIZE]);

/* Parse and validate a header. Returns SVD_OK or an SVD_ERR_* code. */
int svd_header_parse(const uint8_t in[SVD_HEADER_SIZE], svd_info *info);

/* Verify payload length and CRC against the header. */
int svd_payload_verify(const svd_info *info, const uint8_t *payload, size_t len);

/* Little-endian helpers. */
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
