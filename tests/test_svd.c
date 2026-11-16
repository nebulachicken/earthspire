/* Host tests for the shared SVD container/render code. Build: see tests/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svd_format.h"
#include "svd_render.h"

static int g_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

#define W 320
#define H 240

static void write_ppm(const char *path, const uint16_t *px) {
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; ++i) {
        unsigned v = px[i];
        unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        fputc((int)((r << 3) | (r >> 2)), f);
        fputc((int)((g << 2) | (g >> 4)), f);
        fputc((int)((b << 3) | (b >> 2)), f);
    }
    fclose(f);
}

/* Synthetic scene: sea (left), land (right), a cloud block, a no-data corner. */
static uint8_t *make_payload(size_t *len_out) {
    size_t px = (size_t)W * H, len = px * 7, i;
    uint8_t *p = (uint8_t *)calloc(1, len);
    uint8_t *scl = p + px * 6;
    int x, y;
    for (y = 0; y < H; ++y) {
        for (x = 0; x < W; ++x) {
            i = (size_t)y * W + x;
            /* DN already includes the +1000 processing-baseline shift. */
            unsigned r = x < 160 ? 1000 + 100 : 1000 + 900 + (unsigned)y * 3;
            unsigned g = x < 160 ? 1000 + 250 : 1000 + 800 + (unsigned)y * 2;
            unsigned b = x < 160 ? 1000 + 600 : 1000 + 500;
            scl[i] = x < 160 ? 6 : 4;                  /* water / vegetation-ish */
            if (x >= 200 && x < 260 && y >= 40 && y < 100) {   /* cloud */
                r = g = b = 1000 + 3500;   /* reflectance 0.35: above white point */
                scl[i] = 9;
            }
            if (x >= 200 && x < 260 && y >= 100 && y < 130) {  /* shadow */
                r = 1000 + 80; g = 1000 + 100; b = 1000 + 120;
                scl[i] = 3;
            }
            if (x < 20 && y < 20) { r = g = b = 0; scl[i] = 0; }  /* no data */
            svd_put_u16(p + i * 2, (uint16_t)r);
            svd_put_u16(p + px * 2 + i * 2, (uint16_t)g);
            svd_put_u16(p + px * 4 + i * 2, (uint16_t)b);
        }
    }
    *len_out = len;
    return p;
}

static void test_crc(void) {
    /* Standard CRC-32 check value. */
    CHECK(svd_crc32(0, "123456789", 9) == 0xCBF43926u);
    /* Incremental == one-shot. */
    CHECK(svd_crc32(svd_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
}

static void test_tone(void) {
    CHECK(svd_tone(0, 10000, 256) == 0);
    CHECK(svd_tone(-5, 10000, 256) == 0);
    CHECK(svd_tone(3000, 10000, 256) == 255);   /* white point */
    CHECK(svd_tone(9000, 10000, 256) == 255);   /* saturates */
    {
        int v = svd_tone(750, 10000, 256);      /* quarter white -> ~127 with sqrt */
        CHECK(v >= 126 && v <= 128);
    }
    CHECK(svd_tone(1500, 10000, 1024) == 255);  /* 4x gain saturates earlier */
    CHECK(svd_tone(1500, 10000, 256) < svd_tone(1500, 10000, 512));
    /* Gain is clamped, not wrapped. */
    CHECK(svd_tone(1000, 10000, 100000) == svd_tone(1000, 10000, SVD_GAIN_MAX));
    CHECK(svd_tone(1000, 10000, -4) == svd_tone(1000, 10000, SVD_GAIN_MIN));
}

static void test_roundtrip_and_corruption(void) {
    size_t len;
    uint8_t *payload = make_payload(&len);
    svd_info in, out;
    uint8_t hdr[SVD_HEADER_SIZE];
    uint16_t *img = (uint16_t *)malloc(W * H * 2);
    svd_render_opts opts;
    int rc;

    memset(&in, 0, sizeof in);
    in.width = W; in.height = H; in.has_scl = 1;
    in.dn_offset = -1000; in.dn_div = 10000;
    in.cloud_permille = 125; in.nodata_permille = 5;
    strcpy(in.acquired, "2026-11-10T16:00:12Z");
    strcpy(in.scene_id, "S2B_17RMN_20261110_0_L2A");
    in.payload_size = (uint32_t)len;
    in.payload_crc32 = svd_crc32(0, payload, len);
    CHECK(len == svd_expected_payload_size(W, H, 1));

    svd_header_write(&in, hdr);
    rc = svd_header_parse(hdr, &out);
    CHECK(rc == SVD_OK);
    CHECK(out.width == W && out.height == H && out.has_scl == 1);
    CHECK(out.dn_offset == -1000 && out.dn_div == 10000);
    CHECK(out.cloud_permille == 125 && out.nodata_permille == 5);
    CHECK(strcmp(out.acquired, "2026-11-10T16:00:12Z") == 0);
    CHECK(strcmp(out.scene_id, "S2B_17RMN_20261110_0_L2A") == 0);
    CHECK(svd_payload_verify(&out, payload, len) == SVD_OK);

    /* Truncated payload. */
    CHECK(svd_payload_verify(&out, payload, len - 1) == SVD_ERR_PAYLOAD_SIZE);
    /* Flipped payload bit. */
    payload[12345] ^= 0x10;
    CHECK(svd_payload_verify(&out, payload, len) == SVD_ERR_PAYLOAD_CRC);
    payload[12345] ^= 0x10;
    /* Header damage: magic, version, any byte covered by header CRC. */
    {
        uint8_t bad[SVD_HEADER_SIZE];
        memcpy(bad, hdr, sizeof bad); bad[0] = 'X';
        CHECK(svd_header_parse(bad, &out) == SVD_ERR_MAGIC);
        memcpy(bad, hdr, sizeof bad); bad[40] ^= 1;
        CHECK(svd_header_parse(bad, &out) == SVD_ERR_HEADER_CRC);
        memcpy(bad, hdr, sizeof bad); bad[8] ^= 1;
        CHECK(svd_header_parse(bad, &out) == SVD_ERR_HEADER_CRC);
    }
    /* Header with inconsistent payload size but valid CRC must be rejected. */
    {
        svd_info lie = in; uint8_t h2[SVD_HEADER_SIZE];
        lie.payload_size = (uint32_t)len - 10;
        svd_header_write(&lie, h2);
        CHECK(svd_header_parse(h2, &out) == SVD_ERR_PAYLOAD_SIZE);
    }
    /* Absurd geometry. */
    {
        svd_info big = in; uint8_t h3[SVD_HEADER_SIZE];
        big.width = 100000;
        svd_header_write(&big, h3);
        CHECK(svd_header_parse(h3, &out) == SVD_ERR_GEOMETRY);
    }

    /* Render, with and without cloud tint. */
    svd_header_parse(hdr, &out);
    svd_render_defaults(&opts);
    CHECK(svd_render_rgb565(&out, payload, &opts, img) == SVD_OK);
    CHECK(img[0] == 0);                                  /* no-data corner is black */
    {
        unsigned v = img[100 * W + 100];                 /* sea pixel */
        unsigned r = (v >> 11) & 31, b = v & 31;
        CHECK(b > r);                                    /* sea is bluer than red */
    }
    {
        unsigned v = img[60 * W + 220];                  /* cloud pixel: ~white */
        CHECK(v == 0xFFFF);
    }
    write_ppm("test_plain.ppm", img);

    opts.show_clouds = 1;
    CHECK(svd_render_rgb565(&out, payload, &opts, img) == SVD_OK);
    {
        unsigned v = img[60 * W + 220];                  /* cloud now tinted magenta */
        unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        CHECK(r > 28 && b > 28 && g < 45);
    }
    write_ppm("test_clouds.ppm", img);

    /* Higher gain brightens the land. */
    {
        uint16_t a, c;
        opts.show_clouds = 0; opts.gain_q8 = 256;
        svd_render_rgb565(&out, payload, &opts, img); a = img[100 * W + 300];
        opts.gain_q8 = 512;
        svd_render_rgb565(&out, payload, &opts, img); c = img[100 * W + 300];
        CHECK(((c >> 11) & 31) >= ((a >> 11) & 31));
        CHECK(c != a);
    }
    free(payload);
    free(img);
}

static void test_no_scl(void) {
    size_t px = 16, len = px * 6, i;
    uint8_t *p = (uint8_t *)calloc(1, len);
    svd_info info;
    uint16_t out[16];
    for (i = 0; i < px; ++i) svd_put_u16(p + i * 2, 4000);
    memset(&info, 0, sizeof info);
    info.width = 4; info.height = 4; info.has_scl = 0; info.dn_div = 10000;
    info.payload_size = (uint32_t)len;
    info.payload_crc32 = svd_crc32(0, p, len);
    CHECK(svd_render_rgb565(&info, p, NULL, out) == SVD_OK);   /* NULL opts ok */
    CHECK(((out[0] >> 11) & 31) == 31);                        /* red saturated */
    CHECK((out[0] & 31) == 0);                                 /* blue plane is zero */
    free(p);
}


static void test_lzss_and_pyramid(void) {
    const size_t n = 12000;
    uint8_t *source = (uint8_t *)malloc(n);
    uint8_t *compressed = (uint8_t *)malloc(svd_lzss_compress_bound(n));
    uint8_t *decoded = (uint8_t *)malloc(n);
    size_t written = 0;
    svd_pyramid_info info, parsed;
    uint8_t header[SVD2_HEADER_SIZE];
    const size_t px = W * H;
    const size_t raw_len = svd_pyramid_raw_size(W, H);
    uint8_t *raw = (uint8_t *)calloc(1, raw_len);
    uint8_t *packed = (uint8_t *)malloc(svd_lzss_compress_bound(raw_len));
    uint8_t *roundtrip = (uint8_t *)malloc(raw_len);
    uint16_t img[W * H];
    svd_render_opts opts;
    unsigned i;
    int rc;

    CHECK(source && compressed && decoded && raw && packed && roundtrip);
    for (i = 0; i < n; ++i) source[i] = (uint8_t)((i % 97u) < 80u ? (i % 7u) : (i * 13u));
    rc = svd_lzss_compress(source, n, compressed, svd_lzss_compress_bound(n), &written);
    CHECK(rc == SVD_OK && written > 0 && written <= svd_lzss_compress_bound(n));
    CHECK(svd_lzss_decompress(compressed, written, decoded, n) == SVD_OK);
    CHECK(memcmp(source, decoded, n) == 0);
    CHECK(svd_lzss_decompress(compressed, written - 1, decoded, n) != SVD_OK);

    /* A small RGB565 image payload with a packed classification mask. */
    for (i = 0; i < px; ++i) {
        const uint16_t color = (uint16_t)(0x1000u + (i % 256u));
        svd_put_u16(raw + i * 2u, color);
    }
    /* Pixel 0 is no-data, pixel 1 shadow, pixel 2 cloud, pixel 3 normal. */
    raw[px * 2u] = (uint8_t)(1u | (2u << 2) | (3u << 4));
    memset(&info, 0, sizeof info);
    info.width = W; info.height = H; info.level_count = SVD_MAX_LEVELS;
    info.cloud_permille = 20; info.nodata_permille = 0;
    strcpy(info.acquired, "2026-11-15T16:00:00Z");
    strcpy(info.scene_id, "SVD2_TEST");
    for (i = 0; i < SVD_MAX_LEVELS; ++i) {
        info.levels[i].resolution_m = (uint16_t)(10u << i);
        info.levels[i].raw_size = (uint32_t)raw_len;
        info.levels[i].raw_crc32 = svd_crc32(0, raw, raw_len);
        rc = svd_lzss_compress(raw, raw_len, packed, svd_lzss_compress_bound(raw_len), &written);
        CHECK(rc == SVD_OK);
        if (written < raw_len) {
            info.levels[i].codec = SVD_CODEC_LZSS;
            info.levels[i].stored_size = (uint32_t)written;
        } else {
            info.levels[i].codec = SVD_CODEC_RAW;
            info.levels[i].stored_size = (uint32_t)raw_len;
        }
    }
    svd_pyramid_header_write(&info, header);
    CHECK(svd_pyramid_header_parse(header, &parsed) == SVD_OK);
    CHECK(parsed.level_count == SVD_MAX_LEVELS);
    CHECK(parsed.levels[4].resolution_m == 160);

    /* Exercise the lossless horizontal XOR filter used for image compression. */
    {
        uint8_t *filtered = (uint8_t *)malloc(raw_len);
        CHECK(filtered != NULL);
        memcpy(filtered, raw, raw_len);
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = W; x-- > 1;) {
                const size_t at = (size_t)y * W + x;
                const uint16_t current = svd_get_u16(raw + at * 2u);
                const uint16_t left = svd_get_u16(raw + (at - 1u) * 2u);
                svd_put_u16(filtered + at * 2u, (uint16_t)(current ^ left));
            }
        }
        CHECK(svd_lzss_compress(filtered, raw_len, packed,
                                svd_lzss_compress_bound(raw_len), &written) == SVD_OK);
        info.levels[0].codec = SVD_CODEC_LZSS_XOR;
        info.levels[0].stored_size = (uint32_t)written;
        svd_pyramid_header_write(&info, header);
        CHECK(svd_pyramid_header_parse(header, &parsed) == SVD_OK);
        CHECK(svd_pyramid_level_decode(&parsed, 0, packed, written, roundtrip, raw_len) == SVD_OK);
        CHECK(memcmp(raw, roundtrip, raw_len) == 0);
        free(filtered);
    }

    for (i = 1; i < SVD_MAX_LEVELS; ++i) {
        size_t stored_len;
        if (info.levels[i].codec == SVD_CODEC_LZSS) {
            stored_len = info.levels[i].stored_size;
            CHECK(svd_lzss_compress(raw, raw_len, packed, svd_lzss_compress_bound(raw_len), &written) == SVD_OK);
            CHECK(written == stored_len);
        } else {
            stored_len = raw_len;
            memcpy(packed, raw, raw_len);
        }
        CHECK(svd_pyramid_level_decode(&parsed, i, packed, stored_len, roundtrip, raw_len) == SVD_OK);
        CHECK(memcmp(raw, roundtrip, raw_len) == 0);
    }

    svd_render_defaults(&opts);
    /* Restore a direct image with primary colours to validate mask rendering. */
    svd_put_u16(raw + 0, 0xF800); /* red, no-data -> black */
    svd_put_u16(raw + 2, 0x07E0); /* green, shadow */
    svd_put_u16(raw + 4, 0x001F); /* blue, cloud */
    svd_put_u16(raw + 6, 0xFFFF); /* white, normal */
    info.levels[0].raw_crc32 = svd_crc32(0, raw, raw_len);
    CHECK(svd_pyramid_render_rgb565(&info, raw, 0, &opts, img) == SVD_OK);
    CHECK(img[0] == 0);
    CHECK(img[3] == 0xFFFF);
    opts.show_clouds = 1;
    CHECK(svd_pyramid_render_rgb565(&info, raw, 0, &opts, img) == SVD_OK);
    CHECK(img[1] != 0x07E0);
    CHECK(img[2] != 0x001F);

    /* Header checksum and level metadata are covered by the header CRC. */
    header[112] ^= 1u;
    CHECK(svd_pyramid_header_parse(header, &parsed) == SVD_ERR_HEADER_CRC);

    free(source); free(compressed); free(decoded); free(raw); free(packed); free(roundtrip);
}

int main(void) {
    test_crc();
    test_tone();
    test_roundtrip_and_corruption();
    test_no_scl();
    test_lzss_and_pyramid();
    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("all checks passed\n");
    return 0;
}
