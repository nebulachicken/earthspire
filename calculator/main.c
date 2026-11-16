/* Cape Canaveral Sentinel viewer - TI-Nspire CX II (Ndless) client.
 *
 * SVD2 files contain a compressed image pyramid. Multiply zooms in; divide
 * zooms out. Plus/minus adjust brightness, C toggles the cloud overlay,
 * I shows scene/zoom information, Esc quits. Legacy SVD1 files still load.
 */
#include <os.h>
#include <libndls.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svd_format.h"
#include "svd_render.h"

#define IMG_W 320
#define IMG_H 240

typedef struct viewer_data {
    int version;                    /* 1 = legacy SVD1, 2 = SVD2 pyramid */
    FILE *file;                     /* kept open for on-demand SVD2 level reads */
    svd_info legacy_info;
    uint8_t *legacy_payload;
    svd_pyramid_info pyramid;
    long level_offsets[SVD_MAX_LEVELS];
    uint8_t *stored_level;
    size_t stored_capacity;
    uint8_t *raw_level;
    size_t raw_capacity;
} viewer_data;

/* Ndless fopen() takes the on-calculator filename including the .tns suffix. */
static const char *const kPaths[] = {
    "/documents/capecanaveral.svd.tns",
    "/documents/ndless/capecanaveral.svd.tns",
    "/ndless/capecanaveral.svd.tns",
    "/capecanaveral.svd.tns",
    NULL
};

static void fail(const char *msg) {
    show_msgbox("Cape Canaveral viewer", msg);
}

static FILE *open_data(const char **used) {
    int i;
    for (i = 0; kPaths[i]; ++i) {
        FILE *f = fopen(kPaths[i], "rb");
        if (f) {
            *used = kPaths[i];
            return f;
        }
    }
    return NULL;
}

static void close_data(viewer_data *data) {
    if (data->file) fclose(data->file);
    free(data->legacy_payload);
    free(data->stored_level);
    free(data->raw_level);
    memset(data, 0, sizeof *data);
}

/* Load metadata. For SVD2 the compressed level data is read only when selected. */
static int load(viewer_data *data) {
    uint8_t magic[4];
    const char *used = NULL;
    FILE *f = open_data(&used);
    unsigned i;
    memset(data, 0, sizeof *data);
    if (!f) {
        fail("capecanaveral.svd.tns not found.\nSend it to My Documents first.");
        return 0;
    }
    if (fread(magic, 1, sizeof magic, f) != sizeof magic || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fail("Data file is too short.");
        return 0;
    }

    if (memcmp(magic, "SVD1", 4) == 0) {
        uint8_t header[SVD_HEADER_SIZE];
        int rc;
        if (fread(header, 1, sizeof header, f) != sizeof header) {
            fclose(f);
            fail("Legacy data header is truncated.");
            return 0;
        }
        rc = svd_header_parse(header, &data->legacy_info);
        if (rc != SVD_OK) {
            fclose(f);
            fail(svd_strerror(rc));
            return 0;
        }
        if (data->legacy_info.width != IMG_W || data->legacy_info.height != IMG_H) {
            fclose(f);
            fail("Unsupported image size (need 320x240).");
            return 0;
        }
        data->legacy_payload = (uint8_t *)malloc(data->legacy_info.payload_size);
        if (!data->legacy_payload) {
            fclose(f);
            fail("Not enough memory for image data.");
            return 0;
        }
        if (fread(data->legacy_payload, 1, data->legacy_info.payload_size, f) !=
            data->legacy_info.payload_size) {
            fclose(f);
            free(data->legacy_payload);
            data->legacy_payload = NULL;
            fail("Data file is truncated - resend it.");
            return 0;
        }
        fclose(f);
        rc = svd_payload_verify(&data->legacy_info, data->legacy_payload,
                                data->legacy_info.payload_size);
        if (rc != SVD_OK) {
            free(data->legacy_payload);
            data->legacy_payload = NULL;
            fail(svd_strerror(rc));
            return 0;
        }
        data->version = 1;
        (void)used;
        return 1;
    }

    if (memcmp(magic, "SVD2", 4) == 0) {
        uint8_t header[SVD2_HEADER_SIZE];
        int rc;
        long file_size;
        unsigned long end_offset = SVD2_HEADER_SIZE;
        if (fread(header, 1, sizeof header, f) != sizeof header) {
            fclose(f);
            fail("Pyramid header is truncated.");
            return 0;
        }
        rc = svd_pyramid_header_parse(header, &data->pyramid);
        if (rc != SVD_OK) {
            fclose(f);
            fail(svd_strerror(rc));
            return 0;
        }
        if (data->pyramid.width != IMG_W || data->pyramid.height != IMG_H) {
            fclose(f);
            fail("Unsupported image size (need 320x240).");
            return 0;
        }
        for (i = 0; i < data->pyramid.level_count; ++i) {
            data->level_offsets[i] = (long)end_offset;
            end_offset += data->pyramid.levels[i].stored_size;
            if (end_offset > 0x7FFFFFFFul) {
                fclose(f);
                fail("Image pyramid is too large for this calculator.");
                return 0;
            }
        }
        if (fseek(f, 0, SEEK_END) != 0 || (file_size = ftell(f)) < 0 ||
            (unsigned long)file_size < end_offset) {
            fclose(f);
            fail("Image pyramid is truncated - resend it.");
            return 0;
        }
        data->file = f;
        data->version = 2;
        (void)used;
        return 1;
    }

    fclose(f);
    fail("Not a supported Sentinel Viewer file.");
    return 0;
}

static int load_level(viewer_data *data, unsigned level, const char **error) {
    const svd_pyramid_level *meta;
    uint8_t *p;
    int rc;
    if (data->version != 2 || level >= data->pyramid.level_count) {
        *error = "Zoom level is not available.";
        return 0;
    }
    meta = &data->pyramid.levels[level];

    if (meta->stored_size > data->stored_capacity) {
        p = (uint8_t *)realloc(data->stored_level, meta->stored_size);
        if (!p) { *error = "Not enough memory for compressed image data."; return 0; }
        data->stored_level = p;
        data->stored_capacity = meta->stored_size;
    }
    if (meta->raw_size > data->raw_capacity) {
        p = (uint8_t *)realloc(data->raw_level, meta->raw_size);
        if (!p) { *error = "Not enough memory for decoded image data."; return 0; }
        data->raw_level = p;
        data->raw_capacity = meta->raw_size;
    }
    if (fseek(data->file, data->level_offsets[level], SEEK_SET) != 0 ||
        fread(data->stored_level, 1, meta->stored_size, data->file) != meta->stored_size) {
        *error = "Could not read this zoom level from the data file.";
        return 0;
    }
    rc = svd_pyramid_level_decode(&data->pyramid, level,
                                  data->stored_level, meta->stored_size,
                                  data->raw_level, meta->raw_size);
    if (rc != SVD_OK) {
        *error = svd_strerror(rc);
        return 0;
    }
    return 1;
}

static int render_current(viewer_data *data, unsigned level,
                          const svd_render_opts *opts, uint16_t *img) {
    if (data->version == 1)
        return svd_render_rgb565(&data->legacy_info, data->legacy_payload, opts, img);
    return svd_pyramid_render_rgb565(&data->pyramid, data->raw_level, level, opts, img);
}

static void show_info(const viewer_data *data, unsigned level) {
    char msg[240];
    uint16_t resolution = 10;
    uint32_t width = IMG_W, height = IMG_H;
    uint16_t cloud = SVD_UNKNOWN_PERMILLE;
    const char *scene;
    const char *date;
    if (data->version == 1) {
        scene = data->legacy_info.scene_id;
        date = data->legacy_info.acquired;
        cloud = data->legacy_info.cloud_permille;
    } else {
        resolution = data->pyramid.levels[level].resolution_m;
        scene = data->pyramid.scene_id;
        date = data->pyramid.acquired;
        cloud = data->pyramid.cloud_permille;
    }
    if (cloud == SVD_UNKNOWN_PERMILLE) {
        snprintf(msg, sizeof msg,
                 "%s\n%s\nCloud over centre: unknown\n%u m/pixel  %.1f x %.1f km",
                 scene, date, (unsigned)resolution,
                 width * (double)resolution / 1000.0,
                 height * (double)resolution / 1000.0);
    } else {
        snprintf(msg, sizeof msg,
                 "%s\n%s\nCloud over centre: %u.%u%%\n%u m/pixel  %.1f x %.1f km",
                 scene, date, (unsigned)(cloud / 10), (unsigned)(cloud % 10),
                 (unsigned)resolution,
                 width * (double)resolution / 1000.0,
                 height * (double)resolution / 1000.0);
    }
    show_msgbox("Scene / zoom", msg);
}

static void draw(const uint16_t *img) {
    lcd_blit((void *)img, SCR_320x240_565);
}

int main(void) {
    viewer_data data;
    svd_render_opts opts;
    uint16_t *img;
    unsigned current_level = 0;
    int running = 1;
    const char *error = NULL;

    assert_ndless_rev(2016); /* CX II support */
    if (!load(&data)) return 0;

    img = (uint16_t *)malloc(IMG_W * IMG_H * sizeof(uint16_t));
    if (!img) {
        close_data(&data);
        fail("Not enough memory for the display buffer.");
        return 0;
    }
    svd_render_defaults(&opts);

    if (data.version == 2) {
        if (!load_level(&data, current_level, &error)) {
            free(img);
            close_data(&data);
            fail(error ? error : "Could not load the first zoom level.");
            return 0;
        }
    }
    if (render_current(&data, current_level, &opts, img) != SVD_OK) {
        free(img);
        close_data(&data);
        fail("Could not convert the image data.");
        return 0;
    }

    if (!lcd_init(SCR_320x240_565)) {
        free(img);
        close_data(&data);
        fail("Could not switch the display to colour mode.");
        return 0;
    }
    draw(img);

    while (running) {
        int dirty = 0;
        int zoom_changed = 0;
        const unsigned previous_level = current_level;
        wait_key_pressed();

        if (isKeyPressed(KEY_NSPIRE_ESC)) {
            running = 0;
        } else if (isKeyPressed(KEY_NSPIRE_PLUS)) {
            if (opts.gain_q8 < SVD_GAIN_MAX) { opts.gain_q8 += 64; dirty = 1; }
        } else if (isKeyPressed(KEY_NSPIRE_MINUS)) {
            if (opts.gain_q8 > SVD_GAIN_MIN) { opts.gain_q8 -= 64; dirty = 1; }
        } else if (isKeyPressed(KEY_NSPIRE_MULTIPLY)) {
            if (data.version == 2 && current_level > 0) {
                --current_level;
                zoom_changed = 1;
            }
        } else if (isKeyPressed(KEY_NSPIRE_DIVIDE)) {
            if (data.version == 2 && current_level + 1u < data.pyramid.level_count) {
                ++current_level;
                zoom_changed = 1;
            }
        } else if (isKeyPressed(KEY_NSPIRE_C)) {
            opts.show_clouds = !opts.show_clouds;
            dirty = 1;
        } else if (isKeyPressed(KEY_NSPIRE_I)) {
            lcd_init(SCR_TYPE_INVALID);
            show_info(&data, current_level);
            if (!lcd_init(SCR_320x240_565)) break;
            draw(img);
        }

        if (zoom_changed) {
            error = NULL;
            if (load_level(&data, current_level, &error)) {
                dirty = 1;
            } else {
                /* Keep the old image and restore the previous level if a read fails. */
                current_level = previous_level;
                {
                    const char *restore_error = NULL;
                    (void)load_level(&data, current_level, &restore_error);
                }
                lcd_init(SCR_TYPE_INVALID);
                fail(error ? error : "Could not load zoom level.");
                if (!lcd_init(SCR_320x240_565)) break;
                draw(img);
            }
        }
        if (dirty) {
            if (render_current(&data, current_level, &opts, img) == SVD_OK) draw(img);
        }
        wait_no_key_pressed();
    }

    lcd_init(SCR_TYPE_INVALID);
    free(img);
    close_data(&data);
    return 0;
}
