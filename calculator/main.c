/* Cape Canaveral Sentinel viewer - TI-Nspire CX II (Ndless) client.
 *
 * Loads capecanaveral.svd.tns (raw Sentinel-2 band data produced by the desktop
 * app), verifies it, converts it to an RGB565 image on the calculator and
 * displays it.
 *
 * Keys:  + / -  brightness     C  cloud overlay     I  scene info     Esc  quit
 *
 * Build with the Ndless SDK (see Makefile). NOT yet tested on hardware.
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

/* Ndless fopen() takes the real on-calculator filename, which includes the
 * trailing ".tns" that the OS requires for transferred files. */
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

/* Returns a malloc'd, CRC-verified payload, or NULL after showing an error. */
static uint8_t *load(svd_info *info) {
    uint8_t header[SVD_HEADER_SIZE];
    const char *used = NULL;
    uint8_t *payload;
    char msg[160];
    int rc;
    FILE *f = open_data(&used);

    if (!f) {
        fail("capecanaveral.svd.tns not found.\n"
             "Send it to the calculator's My Documents folder first.");
        return NULL;
    }
    if (fread(header, 1, sizeof header, f) != sizeof header) {
        fclose(f);
        fail("Data file is too short.");
        return NULL;
    }
    rc = svd_header_parse(header, info);
    if (rc != SVD_OK) {
        fclose(f);
        fail(svd_strerror(rc));
        return NULL;
    }
    if (info->width != IMG_W || info->height != IMG_H) {
        fclose(f);
        fail("Unsupported image size (need 320x240).");
        return NULL;
    }
    payload = (uint8_t *)malloc(info->payload_size);
    if (!payload) {
        fclose(f);
        fail("Not enough memory for the image data.");
        return NULL;
    }
    if (fread(payload, 1, info->payload_size, f) != info->payload_size) {
        fclose(f);
        free(payload);
        fail("Data file is truncated - resend it.");
        return NULL;
    }
    fclose(f);

    rc = svd_payload_verify(info, payload, info->payload_size);
    if (rc != SVD_OK) {
        snprintf(msg, sizeof msg, "%s", svd_strerror(rc));
        free(payload);
        fail(msg);
        return NULL;
    }
    return payload;
}

static void show_info(const svd_info *info) {
    char msg[200];
    if (info->cloud_permille == SVD_UNKNOWN_PERMILLE)
        snprintf(msg, sizeof msg, "%s\n%s\nCloud over area: unknown",
                 info->scene_id, info->acquired);
    else
        snprintf(msg, sizeof msg, "%s\n%s\nCloud over area: %u.%u%%",
                 info->scene_id, info->acquired,
                 (unsigned)(info->cloud_permille / 10),
                 (unsigned)(info->cloud_permille % 10));
    show_msgbox("Scene", msg);
}

static void draw(const uint16_t *img) {
    lcd_blit((void *)img, SCR_320x240_565);
}

int main(void) {
    svd_info info;
    svd_render_opts opts;
    uint8_t *payload;
    uint16_t *img;
    int running = 1;

    assert_ndless_rev(2016); /* CX II support */

    payload = load(&info);
    if (!payload) return 0;

    img = (uint16_t *)malloc(IMG_W * IMG_H * sizeof(uint16_t));
    if (!img) {
        free(payload);
        fail("Not enough memory for the display buffer.");
        return 0;
    }

    svd_render_defaults(&opts);
    if (svd_render_rgb565(&info, payload, &opts, img) != SVD_OK) {
        free(img);
        free(payload);
        fail("Could not convert the image data.");
        return 0;
    }

    if (!lcd_init(SCR_320x240_565)) {
        free(img);
        free(payload);
        fail("Could not switch the display to colour mode.");
        return 0;
    }
    draw(img);

    while (running) {
        int dirty = 0;
        wait_key_pressed();

        if (isKeyPressed(KEY_NSPIRE_ESC)) {
            running = 0;
        } else if (isKeyPressed(KEY_NSPIRE_PLUS)) {
            if (opts.gain_q8 < SVD_GAIN_MAX) { opts.gain_q8 += 64; dirty = 1; }
        } else if (isKeyPressed(KEY_NSPIRE_MINUS)) {
            if (opts.gain_q8 > SVD_GAIN_MIN) { opts.gain_q8 -= 64; dirty = 1; }
        } else if (isKeyPressed(KEY_NSPIRE_C)) {
            opts.show_clouds = !opts.show_clouds;
            dirty = 1;
        } else if (isKeyPressed(KEY_NSPIRE_I)) {
            lcd_init(SCR_TYPE_INVALID);   /* back to the OS screen for the dialog */
            show_info(&info);
            if (!lcd_init(SCR_320x240_565)) break;
            draw(img);
        }

        if (dirty) {
            svd_render_rgb565(&info, payload, &opts, img);
            draw(img);
        }
        wait_no_key_pressed();
    }

    lcd_init(SCR_TYPE_INVALID);   /* restore the OS display mode */
    free(img);
    free(payload);
    return 0;
}
