#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif

#include "wallpaper.h"

#include "log.h"
#include "manager.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <river-window-management-v1-client-protocol.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-util.h>

#define LOG_TOPIC     "wallpaper"

#define MAX_DIMENSION 32768
#define PPM_TOKEN_MAX 12

#define SHM_ANON      ((char *)1)

static void wpo_destroy_cached_variants(struct WallpaperOutput *wpo);

static void paint_default_pattern(uint8_t *dst, int w, int h) {
    if (w <= 0 || h <= 0) { return; }

    int32_t spacing = wm.wallpaper.pattern_grid_spacing;
    int32_t dot_radius = wm.wallpaper.pattern_dot_radius;
    if (spacing < 1) { spacing = 1; }
    if (dot_radius < 0) { dot_radius = 0; }

    uint8_t bg_r = (uint8_t)wm.wallpaper.pattern_bg_r;
    uint8_t bg_g = (uint8_t)wm.wallpaper.pattern_bg_g;
    uint8_t bg_b = (uint8_t)wm.wallpaper.pattern_bg_b;
    uint8_t dot_r = (uint8_t)wm.wallpaper.pattern_dot_r;
    uint8_t dot_g = (uint8_t)wm.wallpaper.pattern_dot_g;
    uint8_t dot_b = (uint8_t)wm.wallpaper.pattern_dot_b;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *dp = &dst[((size_t)y * (size_t)w + (size_t)x) * 4];

            dp[0] = bg_b;
            dp[1] = bg_g;
            dp[2] = bg_r;
            dp[3] = 255;
        }
    }

    int cols = w / spacing;
    int rows = h / spacing;

    int used_w = cols * spacing;
    int used_h = rows * spacing;

    int offset_x = (w - used_w) / 2 + spacing / 2;
    int offset_y = (h - used_h) / 2 + spacing / 2;

    for (int row = 0; row < rows; row++) {
        int gy = offset_y + row * spacing;

        for (int col = 0; col < cols; col++) {
            int gx = offset_x + col * spacing;

            for (int dy = -dot_radius; dy <= dot_radius; dy++) {
                int py = gy + dy;
                if (py < 0 || py >= h) { continue; }

                for (int dx = -dot_radius; dx <= dot_radius; dx++) {
                    int px = gx + dx;
                    if (px < 0 || px >= w) { continue; }

                    if (dx * dx + dy * dy > dot_radius * dot_radius + 1) {
                        continue;
                    }

                    uint8_t *dp =
                        &dst[((size_t)py * (size_t)w + (size_t)px) * 4];

                    dp[0] = dot_b;
                    dp[1] = dot_g;
                    dp[2] = dot_r;
                    dp[3] = 255;
                }
            }
        }
    }
}

struct ppm_buf {
    const uint8_t *data;
    size_t size;
    size_t pos;
};

static int ppm_buf_token(struct ppm_buf *b, char *out, size_t out_size) {
    size_t len = 0;

    // Skip whitespace and comments.
    while (b->pos < b->size) {
        uint8_t c = b->data[b->pos];
        if (c == '#') {
            b->pos++;
            while (b->pos < b->size && b->data[b->pos] != '\n') { b->pos++; }
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            b->pos++;
            continue;
        }

        break;
    }

    if (b->pos >= b->size) { return -1; }

    while (b->pos < b->size) {
        uint8_t c = b->data[b->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            b->pos++;
            break;
        }

        if (len + 1 >= out_size) { return -1; }

        out[len++] = (char)c;
        b->pos++;
    }

    out[len] = '\0';
    return 0;
}

static bool ppm_buf_read_int(struct ppm_buf *b, int *out) {
    char token[64];

    if (ppm_buf_token(b, token, sizeof(token)) != 0) { return false; }

    char *end;
    errno = 0;
    long v = strtol(token, &end, 10);

    if (errno != 0 || end == token || *end != '\0' || v < INT_MIN
        || v > INT_MAX) {
        return false;
    }

    *out = (int)v;
    return true;
}

static bool
read_whole_file(const char *path, uint8_t **out_data, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_warn("failed to open '%s': %s", path, strerror(errno));
        return false;
    }

    struct stat st;

    if (fstat(fd, &st) < 0) {
        log_warn("fstat('%s') failed: %s", path, strerror(errno));
        close(fd);
        return false;
    }

    if (!S_ISREG(st.st_mode)) {
        log_warn("'%s' is not a regular file", path);
        close(fd);
        return false;
    }

    if (st.st_size <= 0) {
        log_warn("'%s' is empty", path);
        close(fd);
        return false;
    }

    size_t size = (size_t)st.st_size;
    uint8_t *data = malloc(size);

    if (data == NULL) {
        close(fd);
        return false;
    }

    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, data + got, size - got);

        if (n < 0) {
            if (errno == EINTR) { continue; }
            log_warn("read('%s') failed: %s", path, strerror(errno));
            free(data);
            close(fd);
            return false;
        }

        if (n == 0) { break; }
        got += (size_t)n;
    }

    close(fd);

    if (got != size) {
        log_warn("short read on '%s'", path);
        free(data);
        return false;
    }

    *out_data = data;
    *out_size = size;

    return true;
}

static bool
ppm_load(const char *path, uint8_t **out_rgba, int *out_w, int *out_h) {
    uint8_t *filebuf = NULL;
    size_t filebuf_size = 0;

    if (!read_whole_file(path, &filebuf, &filebuf_size)) { return false; }

    struct ppm_buf b = {
        .data = filebuf,
        .size = filebuf_size,
        .pos = 0,
    };

    char token[64];

    if (ppm_buf_token(&b, token, sizeof(token)) != 0
        || strcmp(token, "P6") != 0) {
        log_warn("'%s' is not a binary PPM (P6)", path);
        free(filebuf);
        return false;
    }

    int width, height, maxval;
    if (!ppm_buf_read_int(&b, &width) || !ppm_buf_read_int(&b, &height)
        || !ppm_buf_read_int(&b, &maxval)) {
        log_warn("malformed PPM header in '%s'", path);
        free(filebuf);
        return false;
    }

    if (width <= 0 || height <= 0 || maxval != 255) {
        log_warn(
            "unsupported PPM in '%s' (w=%d h=%d maxval=%d, "
            "only maxval=255 supported)",
            path, width, height, maxval
        );

        free(filebuf);
        return false;
    }

    if (width > MAX_DIMENSION || height > MAX_DIMENSION) {
        log_warn(
            "'%s' is too large (%dx%d, max %d)", path, width, height,
            MAX_DIMENSION
        );

        free(filebuf);
        return false;
    }

    size_t pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / 4) {
        log_warn("'%s' dimensions overflow", path);
        free(filebuf);
        return false;
    }

    size_t needed = pixel_count * 3;
    if (needed > b.size - b.pos) {
        log_warn("truncated pixel data in '%s'", path);
        free(filebuf);
        return false;
    }

    const uint8_t *rgb = b.data + b.pos;

    uint8_t *rgba = malloc(pixel_count * 4);
    if (rgba == NULL) {
        free(filebuf);
        return false;
    }

    for (size_t i = 0; i < pixel_count; i++) {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }

    free(filebuf);

    *out_rgba = rgba;
    *out_w = width;
    *out_h = height;

    return true;
}

bool wallpaper_load_ppm(struct Wallpaper *wlp, const char *path) {
    uint8_t *pixels;
    int w, h;

    if (!ppm_load(path, &pixels, &w, &h)) {
        log_warn("using default pattern");
        return false;
    }

    log_info("loaded '%s'", path);

    free(wlp->image_pixels);

    wlp->image_pixels = pixels;
    wlp->image_width = w;
    wlp->image_height = h;
    wlp->loaded = true;

    wallpaper_invalidate(wlp);

    return true;
}

static void
scale_cover(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) { return; }

    double scale_x = (double)dw / sw;
    double scale_y = (double)dh / sh;
    double cover_scale = scale_x > scale_y ? scale_x : scale_y;

    if (cover_scale <= 0.0) { return; }

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

            dp[0] = sp[2]; // B
            dp[1] = sp[1]; // G
            dp[2] = sp[0]; // R
            dp[3] = sp[3]; // A
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
                int denom = count > 0 ? count : 1;
                out_row[x * 4 + c] = (uint8_t)(sum / denom);

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
                int denom = count > 0 ? count : 1;
                pixels[((size_t)y * (size_t)w + (size_t)x) * 4 + (size_t)c] =
                    (uint8_t)(sum / denom);

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

static int create_shm_fd(size_t size) {
    int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);

    if (fd < 0) {
        log_warn("shm_open(SHM_ANON) failed: %s", strerror(errno));
        return -1;
    }

    if (ftruncate(fd, (off_t)size) < 0) {
        log_warn("ftruncate failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void wpo_destroy_cached_variants(struct WallpaperOutput *wpo) {
    free(wpo->cached_sharp);
    wpo->cached_sharp = NULL;

    free(wpo->cached_blurred);
    wpo->cached_blurred = NULL;
    wpo->drawn_valid = false;
}

void wallpaper_invalidate(struct Wallpaper *wlp) {
    if (wlp == NULL) { return; }

    struct WallpaperOutput *wpo;
    wl_list_for_each(wpo, &wlp->outputs, link) {
        wpo_destroy_cached_variants(wpo);
    }
}

static void buffer_handle_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;

    struct WallpaperOutput *wpo = data;
    wpo->busy = false;

    if (wpo->deferred) {
        wpo->deferred = false;
        river_window_manager_v1_manage_dirty(wpo->wp->wm);
    }
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_handle_release,
};

static void wpo_destroy_buffer(struct WallpaperOutput *wpo) {
    if (wpo->buffer != NULL) {
        wl_buffer_destroy(wpo->buffer);
        wpo->buffer = NULL;
    }

    if (wpo->data != NULL) {
        munmap(wpo->data, wpo->size);
        wpo->data = NULL;
    }

    wpo->size = 0;
    wpo->busy = false;
    wpo->deferred = false;
    wpo->drawn_valid = false;
}

static bool
wpo_alloc_buffer(struct Wallpaper *wlp, struct WallpaperOutput *wpo) {
    wpo_destroy_buffer(wpo);

    if (wpo->width <= 0 || wpo->height <= 0) { return false; }

    size_t stride = (size_t)wpo->width * 4;
    if (stride == 0 || (size_t)wpo->height > SIZE_MAX / stride) {
        return false;
    }

    size_t size = stride * (size_t)wpo->height;
    if (size > INT32_MAX) { return false; }

    int fd = create_shm_fd(size);
    if (fd < 0) { return false; }

    uint8_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        log_warn("mmap failed: %s", strerror(errno));
        close(fd);
        return false;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(wlp->shm, fd, (int32_t)size);
    if (pool == NULL) {
        log_warn("wl_shm_create_pool failed");
        munmap(data, size);
        close(fd);
        return false;
    }

    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, wpo->width, wpo->height, (int32_t)stride,
        WL_SHM_FORMAT_ARGB8888
    );

    wl_shm_pool_destroy(pool);
    close(fd);

    if (buffer == NULL) {
        log_warn("wl_shm_pool_create_buffer failed");
        munmap(data, size);
        return false;
    }

    wl_buffer_add_listener(buffer, &buffer_listener, wpo);

    wpo->buffer = buffer;
    wpo->data = data;
    wpo->size = size;

    return true;
}

static void
wpo_prepare_variants(struct Wallpaper *wlp, struct WallpaperOutput *wpo) {
    if (wpo->width <= 0 || wpo->height <= 0) { return; }
    if (wlp->image_pixels == NULL) { return; }
    if (wlp->image_width <= 0 || wlp->image_height <= 0) { return; }

    size_t image_size = (size_t)wpo->width * (size_t)wpo->height * 4;

    wpo_destroy_cached_variants(wpo);

    wpo->cached_sharp = calloc(1, image_size);
    wpo->cached_blurred = calloc(1, image_size);

    if (wpo->cached_sharp == NULL || wpo->cached_blurred == NULL) {
        wpo_destroy_cached_variants(wpo);
        return;
    }

    scale_cover(
        wlp->image_pixels, wlp->image_width, wlp->image_height,
        wpo->cached_sharp, wpo->width, wpo->height
    );

    memcpy(wpo->cached_blurred, wpo->cached_sharp, image_size);

    for (int p = 0; p < wm.wallpaper.blur_passes; p++) {
        box_blur(
            wpo->cached_blurred, wpo->width, wpo->height,
            wm.wallpaper.blur_radius
        );
    }
}

static uint32_t
blur_weight(int32_t y, int32_t top, int32_t top_fade, int32_t fade) {
    int32_t inset = wm.wallpaper.blur_top_inset;
    if (inset > top) { inset = top; }
    if (top_fade > inset) { top_fade = inset; }
    if (y < inset - top_fade) { return 0; }
    if (top_fade > 0 && y < inset) {
        return (uint32_t)(255 * (y - (inset - top_fade)) / top_fade);
    }

    if (y < top) { return 255; }
    if (fade > 0 && y < top + fade) {
        return (uint32_t)(255 * (top + fade - y) / fade);
    }

    return 0;
}

static void wpo_composite(
    struct WallpaperOutput *wpo, uint8_t *dst, int32_t top, int32_t top_fade,
    int32_t fade
) {
    size_t row_bytes = (size_t)wpo->width * 4;

    if (top < 0) { top = 0; }
    if (top > wpo->height) { top = wpo->height; }
    if (top_fade < 0) { top_fade = 0; }
    if (fade < 0) { fade = 0; }
    if (top + fade > wpo->height) { fade = wpo->height - top; }

    for (int32_t y = 0; y < wpo->height; y++) {
        uint32_t alpha = blur_weight(y, top, top_fade, fade);

        uint8_t *sp = wpo->cached_sharp + (size_t)y * row_bytes;
        uint8_t *bp = wpo->cached_blurred + (size_t)y * row_bytes;
        uint8_t *dp = dst + (size_t)y * row_bytes;

        if (alpha == 0) {
            memcpy(dp, sp, row_bytes);
            continue;
        }

        if (alpha == 255) {
            memcpy(dp, bp, row_bytes);
            continue;
        }

        uint32_t inv = 255 - alpha;

        for (int32_t x = 0; x < wpo->width; x++) {
            for (int c = 0; c < 3; c++) {
                uint32_t s = sp[x * 4 + c];
                uint32_t b = bp[x * 4 + c];
                dp[x * 4 + c] = (uint8_t)((s * inv + b * alpha) / 255);
            }

            dp[x * 4 + 3] = 255;
        }
    }
}

static bool
wpo_needs_draw(const struct Wallpaper *wlp, const struct WallpaperOutput *wpo) {
    if (!wpo->drawn_valid) { return true; }
    if (wpo->drawn_w != wpo->width || wpo->drawn_h != wpo->height) {
        return true;
    }

    if (wpo->drawn_loaded != wlp->loaded) { return true; }

    if (!wlp->loaded) {
        if (wpo->drawn_pattern_bg_r != wm.wallpaper.pattern_bg_r
            || wpo->drawn_pattern_bg_g != wm.wallpaper.pattern_bg_g
            || wpo->drawn_pattern_bg_b != wm.wallpaper.pattern_bg_b
            || wpo->drawn_pattern_dot_r != wm.wallpaper.pattern_dot_r
            || wpo->drawn_pattern_dot_g != wm.wallpaper.pattern_dot_g
            || wpo->drawn_pattern_dot_b != wm.wallpaper.pattern_dot_b
            || wpo->drawn_pattern_grid_spacing
                   != wm.wallpaper.pattern_grid_spacing
            || wpo->drawn_pattern_dot_radius
                   != wm.wallpaper.pattern_dot_radius) {
            return true;
        }

        return false;
    }

    if (wpo->drawn_blur_radius != wm.wallpaper.blur_radius
        || wpo->drawn_blur_passes != wm.wallpaper.blur_passes
        || wpo->drawn_blur_top_inset != wm.wallpaper.blur_top_inset) {
        return true;
    }

    return wpo->drawn_blur_all != wlp->blur_all
        || wpo->drawn_top_h != wlp->blur_top_h
        || wpo->drawn_top_fade_h != wlp->blur_top_fade_h
        || wpo->drawn_fade_h != wlp->blur_fade_h;
}

static void wpo_redraw(struct Wallpaper *wlp, struct WallpaperOutput *wpo) {
    size_t image_size = (size_t)wpo->width * (size_t)wpo->height * 4;

    if (!wlp->loaded) {
        paint_default_pattern(wpo->data, wpo->width, wpo->height);
    } else {
        if (wpo->cached_sharp == NULL || wpo->cached_blurred == NULL) {
            wpo_prepare_variants(wlp, wpo);
            if (wpo->cached_sharp == NULL || wpo->cached_blurred == NULL) {
                return;
            }
        }

        if (wlp->blur_all) {
            memcpy(wpo->data, wpo->cached_blurred, image_size);
        } else if (wlp->blur_top_h > 0) {
            wpo_composite(
                wpo, wpo->data, wlp->blur_top_h, wlp->blur_top_fade_h,
                wlp->blur_fade_h
            );
        } else {
            memcpy(wpo->data, wpo->cached_sharp, image_size);
        }
    }

    wpo->busy = true;

    river_shell_surface_v1_sync_next_commit(wpo->shell_surface);
    wl_surface_attach(wpo->surface, wpo->buffer, 0, 0);
    wl_surface_damage_buffer(wpo->surface, 0, 0, wpo->width, wpo->height);
    wl_surface_commit(wpo->surface);

    wpo->drawn_w = wpo->width;
    wpo->drawn_h = wpo->height;
    wpo->drawn_loaded = wlp->loaded;
    wpo->drawn_blur_all = wlp->blur_all;
    wpo->drawn_top_h = wlp->blur_top_h;
    wpo->drawn_top_fade_h = wlp->blur_top_fade_h;
    wpo->drawn_fade_h = wlp->blur_fade_h;

    wpo->drawn_pattern_bg_r = wm.wallpaper.pattern_bg_r;
    wpo->drawn_pattern_bg_g = wm.wallpaper.pattern_bg_g;
    wpo->drawn_pattern_bg_b = wm.wallpaper.pattern_bg_b;
    wpo->drawn_pattern_dot_r = wm.wallpaper.pattern_dot_r;
    wpo->drawn_pattern_dot_g = wm.wallpaper.pattern_dot_g;
    wpo->drawn_pattern_dot_b = wm.wallpaper.pattern_dot_b;
    wpo->drawn_pattern_grid_spacing = wm.wallpaper.pattern_grid_spacing;
    wpo->drawn_pattern_dot_radius = wm.wallpaper.pattern_dot_radius;
    wpo->drawn_blur_radius = wm.wallpaper.blur_radius;
    wpo->drawn_blur_passes = wm.wallpaper.blur_passes;
    wpo->drawn_blur_top_inset = wm.wallpaper.blur_top_inset;

    wpo->drawn_valid = true;
}

void wallpaper_init(
    struct Wallpaper *wlp, struct wl_compositor *cmp, struct wl_shm *wshm,
    struct river_window_manager_v1 *manager
) {
    memset(wlp, 0, sizeof(*wlp));
    wlp->compositor = cmp;
    wlp->shm = wshm;
    wlp->wm = manager;

    wl_list_init(&wlp->outputs);
}

static void wpo_destroy_unlinked(struct WallpaperOutput *wpo) {
    if (wpo->node != NULL) { river_node_v1_destroy(wpo->node); }
    if (wpo->shell_surface != NULL) {
        river_shell_surface_v1_destroy(wpo->shell_surface);
    }

    if (wpo->surface != NULL) { wl_surface_destroy(wpo->surface); }

    free(wpo);
}

struct WallpaperOutput *wallpaper_output_create(struct Wallpaper *wlp) {
    if (wlp->compositor == NULL || wlp->shm == NULL || wlp->wm == NULL) {
        return NULL;
    }

    struct WallpaperOutput *wpo = calloc(1, sizeof(struct WallpaperOutput));
    if (wpo == NULL) { return NULL; }

    wpo->wp = wlp;
    wpo->surface = wl_compositor_create_surface(wlp->compositor);

    if (wpo->surface != NULL) {
        wpo->shell_surface =
            river_window_manager_v1_get_shell_surface(wlp->wm, wpo->surface);
    }

    if (wpo->shell_surface != NULL) {
        wpo->node = river_shell_surface_v1_get_node(wpo->shell_surface);
    }

    if (wpo->node == NULL) {
        wpo_destroy_unlinked(wpo);
        return NULL;
    }

    wl_list_insert(wlp->outputs.prev, &wpo->link);
    return wpo;
}

void wallpaper_output_set_dimensions(
    struct WallpaperOutput *wpo, int32_t width, int32_t height
) {
    if (wpo->width == width && wpo->height == height) { return; }

    wpo->width = width;
    wpo->height = height;

    wpo_destroy_cached_variants(wpo);
}

void wallpaper_output_set_position(
    struct WallpaperOutput *wpo, int32_t x, int32_t y
) {
    if (wpo->pos_valid && wpo->pos_x == x && wpo->pos_y == y) { return; }

    wpo->pos_x = x;
    wpo->pos_y = y;
    wpo->pos_valid = true;
    wpo->position_sent = false;
}

void wallpaper_output_destroy(struct WallpaperOutput *wpo) {
    wpo_destroy_buffer(wpo);
    wpo_destroy_cached_variants(wpo);

    if (wpo->node != NULL) { river_node_v1_destroy(wpo->node); }
    if (wpo->shell_surface != NULL) {
        river_shell_surface_v1_destroy(wpo->shell_surface);
    }

    if (wpo->surface != NULL) { wl_surface_destroy(wpo->surface); }

    wl_list_remove(&wpo->link);
    free(wpo);
}

void wallpaper_manage(
    struct Wallpaper *wlp, bool blur_all, int32_t blur_top_h,
    int32_t blur_top_fade_h, int32_t blur_fade_h
) {
    if (blur_all) {
        blur_top_h = 0;
        blur_top_fade_h = 0;
        blur_fade_h = 0;
    }

    if (blur_top_h < 0) { blur_top_h = 0; }
    if (blur_top_fade_h < 0) { blur_top_fade_h = 0; }
    if (blur_fade_h < 0) { blur_fade_h = 0; }

    wlp->blur_all = blur_all;
    wlp->blur_top_h = blur_top_h;
    wlp->blur_top_fade_h = blur_top_fade_h;
    wlp->blur_fade_h = blur_fade_h;

    struct WallpaperOutput *wpo;

    wl_list_for_each(wpo, &wlp->outputs, link) {
        if (wpo->width <= 0 || wpo->height <= 0) { continue; }
        if (wpo->buffer != NULL
            && (wpo->drawn_w != wpo->width || wpo->drawn_h != wpo->height)) {
            wpo_destroy_buffer(wpo);
        }

        if (wpo->buffer == NULL && !wpo_alloc_buffer(wlp, wpo)) { continue; }
        if (!wpo->placed) {
            river_node_v1_place_bottom(wpo->node);
            wpo->placed = true;
        }

        if (!wpo->position_sent && wpo->pos_valid) {
            river_node_v1_set_position(wpo->node, wpo->pos_x, wpo->pos_y);
            wpo->position_sent = true;
        }

        if (!wpo_needs_draw(wlp, wpo)) { continue; }
        if (wpo->busy) {
            wpo->deferred = true;
            continue;
        }

        wpo_redraw(wlp, wpo);
    }
}
