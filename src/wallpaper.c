#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif

#include "wallpaper.h"

#include <errno.h>
#include <fcntl.h>
#include <river-window-management-v1-client-protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#define NO_WALLPAPER_BG_R         0x12
#define NO_WALLPAPER_BG_G         0x12
#define NO_WALLPAPER_BG_B         0x12

#define NO_WALLPAPER_DOT_R        0x3a
#define NO_WALLPAPER_DOT_G        0xb5
#define NO_WALLPAPER_DOT_B        0x5e

#define NO_WALLPAPER_GRID_SPACING 24
#define NO_WALLPAPER_DOT_RADIUS   1

static void paint_no_wallpaper_pattern(uint8_t *dst, int w, int h) {
    if (w <= 0 || h <= 0) { return; }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *dp = &dst[((size_t)y * (size_t)w + (size_t)x) * 4];

            dp[0] = NO_WALLPAPER_BG_B;
            dp[1] = NO_WALLPAPER_BG_G;
            dp[2] = NO_WALLPAPER_BG_R;
            dp[3] = 255;
        }
    }

    int cols = w / NO_WALLPAPER_GRID_SPACING;
    int rows = h / NO_WALLPAPER_GRID_SPACING;

    int used_w = cols * NO_WALLPAPER_GRID_SPACING;
    int used_h = rows * NO_WALLPAPER_GRID_SPACING;

    int offset_x = (w - used_w) / 2 + NO_WALLPAPER_GRID_SPACING / 2;
    int offset_y = (h - used_h) / 2 + NO_WALLPAPER_GRID_SPACING / 2;

    for (int row = 0; row < rows; row++) {
        int gy = offset_y + row * NO_WALLPAPER_GRID_SPACING;

        for (int col = 0; col < cols; col++) {
            int gx = offset_x + col * NO_WALLPAPER_GRID_SPACING;

            for (int dy = -NO_WALLPAPER_DOT_RADIUS;
                 dy <= NO_WALLPAPER_DOT_RADIUS; dy++) {
                int py = gy + dy;
                if (py < 0 || py >= h) { continue; }

                for (int dx = -NO_WALLPAPER_DOT_RADIUS;
                     dx <= NO_WALLPAPER_DOT_RADIUS; dx++) {
                    int px = gx + dx;
                    if (px < 0 || px >= w) { continue; }

                    if (dx * dx + dy * dy
                        > NO_WALLPAPER_DOT_RADIUS * NO_WALLPAPER_DOT_RADIUS
                              + 1) {
                        continue;
                    }

                    uint8_t *dp =
                        &dst[((size_t)py * (size_t)w + (size_t)px) * 4];

                    dp[0] = NO_WALLPAPER_DOT_B;
                    dp[1] = NO_WALLPAPER_DOT_G;
                    dp[2] = NO_WALLPAPER_DOT_R;
                    dp[3] = 255;
                }
            }
        }
    }
}

static int ppm_read_token(FILE *f, char *buf, size_t buf_size) {
    int c;
    size_t len = 0;

    // Skip whitespace and comments.
    while ((c = fgetc(f)) != EOF) {
        if (c == '#') {
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { continue; }
        break;
    }

    if (c == EOF) { return -1; }

    while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        if (len + 1 >= buf_size) { return -1; }
        buf[len++] = (char)c;
        c = fgetc(f);
    }

    buf[len] = '\0';
    return 0;
}

static bool
ppm_load(const char *path, uint8_t **out_rgba, int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(
            stderr, "Wallpaper: failed to open '%s': %s\n", path,
            strerror(errno)
        );

        return false;
    }

    char token[64];

    if (ppm_read_token(f, token, sizeof(token)) != 0
        || strcmp(token, "P6") != 0) {
        fprintf(stderr, "Wallpaper: '%s' is not a binary PPM (P6)\n", path);
        fclose(f);
        return false;
    }

    int width, height, maxval;

    if (ppm_read_token(f, token, sizeof(token)) != 0) { goto parse_error; }
    width = atoi(token);

    if (ppm_read_token(f, token, sizeof(token)) != 0) { goto parse_error; }
    height = atoi(token);

    if (ppm_read_token(f, token, sizeof(token)) != 0) { goto parse_error; }
    maxval = atoi(token);

    if (width <= 0 || height <= 0 || maxval != 255) {
        fprintf(
            stderr,
            "Wallpaper: unsupported PPM in '%s' (w=%d h=%d maxval=%d, "
            "only maxval=255 supported)\n",
            path, width, height, maxval
        );
        fclose(f);
        return false;
    }

    size_t pixel_count = (size_t)width * (size_t)height;
    uint8_t *rgb = malloc(pixel_count * 3);

    if (rgb == NULL) {
        fclose(f);
        return false;
    }

    if (fread(rgb, 1, pixel_count * 3, f) != pixel_count * 3) {
        fprintf(stderr, "Wallpaper: truncated pixel data in '%s'\n", path);
        free(rgb);
        fclose(f);
        return false;
    }

    fclose(f);

    uint8_t *rgba = malloc(pixel_count * 4);

    if (rgba == NULL) {
        free(rgb);
        return false;
    }

    for (size_t i = 0; i < pixel_count; i++) {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }

    free(rgb);

    *out_rgba = rgba;
    *out_w = width;
    *out_h = height;

    return true;

parse_error:
    fprintf(stderr, "Wallpaper: malformed PPM header in '%s'\n", path);
    fclose(f);
    return false;
}

bool wallpaper_load_ppm(struct Wallpaper *wp, const char *path) {
    uint8_t *pixels;
    int w, h;

    if (!ppm_load(path, &pixels, &w, &h)) { return false; }

    free(wp->image_pixels);
    wp->image_pixels = pixels;
    wp->image_width = w;
    wp->image_height = h;
    wp->loaded = true;

    return true;
}

static void
scale_cover(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) { return; }

    double scale_x = (double)dw / sw;
    double scale_y = (double)dh / sh;
    double cover_scale = scale_x > scale_y ? scale_x : scale_y;

    int scaled_w = (int)(sw * cover_scale + 0.5);
    int scaled_h = (int)(sh * cover_scale + 0.5);
    int offset_x = (scaled_w - dw) / 2;
    int offset_y = (scaled_h - dh) / 2;

    for (int y = 0; y < dh; y++) {
        int sy = (int)((y + offset_y) / cover_scale);

        if (sy < 0) { sy = 0; }
        if (sy >= sh) { sy = sh - 1; }

        for (int x = 0; x < dw; x++) {
            int sx = (int)((x + offset_x) / cover_scale);

            if (sx < 0) { sx = 0; }
            if (sx >= sw) { sx = sw - 1; }

            const uint8_t *sp =
                &src[((size_t)sy * (size_t)sw + (size_t)sx) * 4];

            uint8_t *dp = &dst[((size_t)y * (size_t)dw + (size_t)x) * 4];

            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];
        }
    }
}

static void box_blur(uint8_t *pixels, int w, int h, int radius) {
    if (radius <= 0 || w <= 0 || h <= 0) { return; }

    uint8_t *scratch = malloc((size_t)w * (size_t)h * 4);
    if (scratch == NULL) { return; }

    for (int y = 0; y < h; y++) {
        uint8_t *row = &pixels[(size_t)y * (size_t)w * 4];
        uint8_t *out_row = &scratch[(size_t)y * (size_t)w * 4];

        for (int c = 0; c < 3; c++) {
            long sum = 0;
            int count = 0;

            for (int x = -radius; x <= radius; x++) {
                int cx = x < 0 ? 0 : (x >= w ? w - 1 : x);
                sum += row[cx * 4 + c];
                count++;
            }

            for (int x = 0; x < w; x++) {
                out_row[x * 4 + c] = (uint8_t)(sum / count);

                int add_x = x + radius + 1;
                int rem_x = x - radius;

                if (add_x >= w) { add_x = w - 1; }
                if (rem_x < 0) { rem_x = 0; }

                sum += row[add_x * 4 + c];
                sum -= row[rem_x * 4 + c];
            }
        }
    }

    for (int x = 0; x < w; x++) {
        for (int c = 0; c < 3; c++) {
            long sum = 0;
            int count = 0;

            for (int y = -radius; y <= radius; y++) {
                int cy = y < 0 ? 0 : (y >= h ? h - 1 : y);

                sum += scratch
                    [((size_t)cy * (size_t)w + (size_t)x) * 4 + (size_t)c];

                count++;
            }

            for (int y = 0; y < h; y++) {
                pixels[((size_t)y * (size_t)w + (size_t)x) * 4 + (size_t)c] =
                    (uint8_t)(sum / count);

                int add_y = y + radius + 1;
                int rem_y = y - radius;

                if (add_y >= h) { add_y = h - 1; }
                if (rem_y < 0) { rem_y = 0; }

                sum += scratch
                    [((size_t)add_y * (size_t)w + (size_t)x) * 4 + (size_t)c];

                sum -= scratch
                    [((size_t)rem_y * (size_t)w + (size_t)x) * 4 + (size_t)c];
            }
        }
    }

    free(scratch);
}

#define BLUR_RADIUS 12
#define BLUR_PASSES 2

#ifndef SHM_ANON
#define SHM_ANON ((char *)1)
#endif

static int create_shm_fd(size_t size) {
    int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);

    if (fd < 0) {
        fprintf(
            stderr, "Wallpaper: shm_open(SHM_ANON) failed: %s\n",
            strerror(errno)
        );

        return -1;
    }

    if (ftruncate(fd, (off_t)size) < 0) {
        fprintf(stderr, "Wallpaper: ftruncate failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void wpo_destroy_buffers(struct WallpaperOutput *wpo) {
    for (int i = 0; i < 2; i++) {
        if (wpo->buffers[i] != NULL) {
            wl_buffer_destroy(wpo->buffers[i]);
            wpo->buffers[i] = NULL;
        }
    }

    if (wpo->pool != NULL) {
        wl_shm_pool_destroy(wpo->pool);
        wpo->pool = NULL;
    }

    if (wpo->pool_data != NULL) {
        munmap(wpo->pool_data, wpo->pool_size);
        wpo->pool_data = NULL;
    }

    if (wpo->pool_fd >= 0) {
        close(wpo->pool_fd);
        wpo->pool_fd = -1;
    }

    wpo->pool_size = 0;
}

static bool
wpo_alloc_buffers(struct Wallpaper *wp, struct WallpaperOutput *wpo) {
    wpo_destroy_buffers(wpo);

    if (wpo->width <= 0 || wpo->height <= 0) { return false; }

    size_t stride = (size_t)wpo->width * 4;
    size_t image_size = stride * (size_t)wpo->height;
    size_t pool_size = image_size * 2; // two buffers, double-buffered

    int fd = create_shm_fd(pool_size);
    if (fd < 0) { return false; }

    uint8_t *data =
        mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (data == MAP_FAILED) {
        fprintf(stderr, "Wallpaper: mmap failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    struct wl_shm_pool *pool =
        wl_shm_create_pool(wp->shm, fd, (int32_t)pool_size);

    wpo->pool = pool;
    wpo->pool_fd = fd;
    wpo->pool_data = data;
    wpo->pool_size = pool_size;
    wpo->next_buffer = 0;

    for (int i = 0; i < 2; i++) {
        wpo->buffers[i] = wl_shm_pool_create_buffer(
            pool, (int32_t)(image_size * (size_t)i), wpo->width, wpo->height,
            (int32_t)stride, WL_SHM_FORMAT_ARGB8888
        );
    }

    return true;
}

static void
wpo_prepare_variants(struct Wallpaper *wp, struct WallpaperOutput *wpo) {
    size_t image_size = (size_t)wpo->width * (size_t)wpo->height * 4;

    free(wpo->cached_sharp);
    free(wpo->cached_blurred);
    wpo->cached_sharp = malloc(image_size);
    wpo->cached_blurred = malloc(image_size);

    scale_cover(
        wp->image_pixels, wp->image_width, wp->image_height, wpo->cached_sharp,
        wpo->width, wpo->height
    );

    memcpy(wpo->cached_blurred, wpo->cached_sharp, image_size);

    for (int p = 0; p < BLUR_PASSES; p++) {
        box_blur(wpo->cached_blurred, wpo->width, wpo->height, BLUR_RADIUS);
    }

    uint8_t *variants[2] = {wpo->cached_sharp, wpo->cached_blurred};

    for (int v = 0; v < 2; v++) {
        uint8_t *buf = variants[v];
        for (size_t i = 0; i < (size_t)wpo->width * (size_t)wpo->height; i++) {
            uint8_t *px = &buf[i * 4];
            uint8_t r = px[0];
            px[0] = px[2];
            px[2] = r;
        }
    }
}

static void
wpo_redraw(struct Wallpaper *wp, struct WallpaperOutput *wpo, bool blurred) {
    if (wpo->pool_data == NULL) { return; }

    size_t image_size = (size_t)wpo->width * (size_t)wpo->height * 4;
    int idx = wpo->next_buffer;
    uint8_t *dst = wpo->pool_data + image_size * (size_t)idx;

    if (!wp->loaded) {
        paint_no_wallpaper_pattern(dst, wpo->width, wpo->height);
    } else {
        if (wpo->cached_sharp == NULL) { wpo_prepare_variants(wp, wpo); }
        memcpy(
            dst, blurred ? wpo->cached_blurred : wpo->cached_sharp, image_size
        );
    }

    wpo->next_buffer = 1 - idx;
    river_shell_surface_v1_sync_next_commit(wpo->shell_surface);
    wl_surface_attach(wpo->surface, wpo->buffers[idx], 0, 0);
    wl_surface_damage_buffer(wpo->surface, 0, 0, wpo->width, wpo->height);
    wl_surface_commit(wpo->surface);
    wpo->needs_redraw = false;
}
void wallpaper_init(
    struct Wallpaper *wp, struct wl_compositor *compositor, struct wl_shm *shm,
    struct river_window_manager_v1 *wm
) {
    memset(wp, 0, sizeof(*wp));
    wp->compositor = compositor;
    wp->shm = shm;
    wp->wm = wm;
    wl_list_init(&wp->outputs);
}

struct WallpaperOutput *
wallpaper_output_create(struct Wallpaper *wp, struct river_output_v1 *output) {
    struct WallpaperOutput *wpo = calloc(1, sizeof(struct WallpaperOutput));

    wpo->output = output;
    wpo->pool_fd = -1;

    wpo->surface = wl_compositor_create_surface(wp->compositor);
    wpo->shell_surface =
        river_window_manager_v1_get_shell_surface(wp->wm, wpo->surface);

    wpo->node = river_shell_surface_v1_get_node(wpo->shell_surface);
    wpo->needs_redraw = true;

    wl_list_insert(wp->outputs.prev, &wpo->link);

    return wpo;
}

void wallpaper_output_set_dimensions(
    struct WallpaperOutput *wpo, int32_t width, int32_t height
) {
    if (wpo->width == width && wpo->height == height
        && wpo->pool_data != NULL) {
        return;
    }

    wpo->width = width;
    wpo->height = height;
    wpo->needs_redraw = true;
}

void wallpaper_output_destroy(struct WallpaperOutput *wpo) {
    wpo_destroy_buffers(wpo);

    if (wpo->node != NULL) { river_node_v1_destroy(wpo->node); }
    if (wpo->shell_surface != NULL) {
        river_shell_surface_v1_destroy(wpo->shell_surface);
    }

    if (wpo->surface != NULL) { wl_surface_destroy(wpo->surface); }

    wl_list_remove(&wpo->link);
    free(wpo);
}

void wallpaper_manage(struct Wallpaper *wp, bool any_windows_open) {
    bool want_blur = any_windows_open;
    bool blur_state_changed = want_blur != wp->blurred;
    wp->blurred = want_blur;

    struct WallpaperOutput *wpo;

    wl_list_for_each(wpo, &wp->outputs, link) {
        if (wpo->width <= 0 || wpo->height <= 0) { continue; }
        if (wpo->pool_data == NULL) {
            if (!wpo_alloc_buffers(wp, wpo)) { continue; }
            wpo->needs_redraw = true;
        }

        if (!wpo->configured) {
            // Always keep the wallpaper below every window/layer surface.
            river_node_v1_place_bottom(wpo->node);
            wpo->configured = true;
        }

        if (wpo->needs_redraw || (wp->loaded && blur_state_changed)) {
            wpo_redraw(wp, wpo, wp->blurred);
        }
    }
}
