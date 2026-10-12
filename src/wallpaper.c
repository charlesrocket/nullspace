#include "wallpaper.h"

#include "log.h"
#include "manager.h"
#include "output.h"
#include "server.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <scenefx/types/wlr_scene.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-util.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

#define LOG_TOPIC     "wallpaper"
#define MAX_DIMENSION 32768

static struct Wallpaper {
    uint8_t *image_pixels;
    int image_width, image_height;
    bool loaded;
    struct wl_list outputs;
} g_wallpaper;

void wallpaper_global_init(void) {
    memset(&g_wallpaper, 0, sizeof(g_wallpaper));
    wl_list_init(&g_wallpaper.outputs);
}

static void paint_default_pattern(uint8_t *dst, int w, int h) {
    if (w <= 0 || h <= 0) return;

    size_t uw = (size_t)w;
    size_t uh = (size_t)h;

    int32_t spacing = wm.wallpaper.pattern_grid_spacing;
    int32_t dot_radius = wm.wallpaper.pattern_dot_radius;
    if (spacing < 1) spacing = 1;
    if (dot_radius < 0) dot_radius = 0;

    uint8_t bg_r = (uint8_t)wm.wallpaper.pattern_bg_r;
    uint8_t bg_g = (uint8_t)wm.wallpaper.pattern_bg_g;
    uint8_t bg_b = (uint8_t)wm.wallpaper.pattern_bg_b;
    uint8_t dot_r = (uint8_t)wm.wallpaper.pattern_dot_r;
    uint8_t dot_g = (uint8_t)wm.wallpaper.pattern_dot_g;
    uint8_t dot_b = (uint8_t)wm.wallpaper.pattern_dot_b;

    for (size_t y = 0; y < uh; y++) {
        for (size_t x = 0; x < uw; x++) {
            uint8_t *dp = &dst[(y * uw + x) * 4];
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
                if (py < 0 || py >= h) continue;

                for (int dx = -dot_radius; dx <= dot_radius; dx++) {
                    int px = gx + dx;
                    if (px < 0 || px >= w) continue;
                    if (dx * dx + dy * dy > dot_radius * dot_radius + 1)
                        continue;

                    uint8_t *dp = &dst[((size_t)py * uw + (size_t)px) * 4];
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

    while (b->pos < b->size) {
        uint8_t c = b->data[b->pos];
        if (c == '#') {
            b->pos++;
            while (b->pos < b->size && b->data[b->pos] != '\n') b->pos++;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            b->pos++;
            continue;
        }

        break;
    }

    if (b->pos >= b->size) return -1;

    while (b->pos < b->size) {
        uint8_t c = b->data[b->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            b->pos++;
            break;
        }
        if (len + 1 >= out_size) return -1;
        out[len++] = (char)c;
        b->pos++;
    }

    out[len] = '\0';
    return 0;
}

static bool ppm_buf_read_int(struct ppm_buf *b, int *out) {
    char token[64];
    if (ppm_buf_token(b, token, sizeof(token)) != 0) return false;

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
        log_warn("Failed to open '%s': %s", path, strerror(errno));
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
            if (errno == EINTR) continue;
            log_warn("Read('%s') failed: %s", path, strerror(errno));
            free(data);
            close(fd);
            return false;
        }

        if (n == 0) break;
        got += (size_t)n;
    }

    close(fd);

    if (got != size) {
        log_warn("Short read on '%s'", path);
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

    if (!read_whole_file(path, &filebuf, &filebuf_size)) return false;

    struct ppm_buf b = {.data = filebuf, .size = filebuf_size, .pos = 0};
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
        log_warn("Malformed PPM header in '%s'", path);
        free(filebuf);
        return false;
    }

    if (width <= 0 || height <= 0 || maxval != 255) {
        log_warn(
            "Unsupported PPM in '%s' (w=%d h=%d maxval=%d)", path, width,
            height, maxval
        );
        free(filebuf);
        return false;
    }

    if (width > MAX_DIMENSION || height > MAX_DIMENSION) {
        log_warn("'%s' is too large (%dx%d)", path, width, height);
        free(filebuf);
        return false;
    }

    size_t pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / 4) {
        free(filebuf);
        return false;
    }

    size_t needed = pixel_count * 3;
    if (needed > b.size - b.pos) {
        log_warn("Truncated pixel data in '%s'", path);
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

static void
scale_cover(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    size_t sw_s = (size_t)sw;
    size_t dw_s = (size_t)dw;
    size_t dh_s = (size_t)dh;

    double scale_x = (double)dw / sw;
    double scale_y = (double)dh / sh;
    double cover_scale = scale_x > scale_y ? scale_x : scale_y;
    if (cover_scale <= 0.0) return;

    int scaled_w = (int)(sw * cover_scale + 0.5);
    int scaled_h = (int)(sh * cover_scale + 0.5);
    int offset_x = (scaled_w - dw) / 2;
    int offset_y = (scaled_h - dh) / 2;

    for (size_t y = 0; y < dh_s; y++) {
        int sy = (int)(((double)y + offset_y) / cover_scale);
        if (sy < 0) sy = 0;
        if (sy >= sh) sy = sh - 1;

        for (size_t x = 0; x < dw_s; x++) {
            int sx = (int)(((double)x + offset_x) / cover_scale);
            if (sx < 0) sx = 0;
            if (sx >= sw) sx = sw - 1;

            const uint8_t *sp = &src[((size_t)sy * sw_s + (size_t)sx) * 4];
            uint8_t *dp = &dst[(y * dw_s + x) * 4];
            dp[0] = sp[2];
            dp[1] = sp[1];
            dp[2] = sp[0];
            dp[3] = sp[3];
        }
    }
}

static void wpo_destroy_cached_variants(struct WallpaperOutput *wpo) {
    free(wpo->cached_sharp);
    wpo->cached_sharp = NULL;
}

static void wpo_compose_to_cpu(struct WallpaperOutput *wpo, uint8_t *dst) {
    if (!g_wallpaper.loaded) {
        paint_default_pattern(dst, wpo->width, wpo->height);
        return;
    }

    if (wpo->cached_sharp == NULL) {
        size_t image_size = (size_t)wpo->width * (size_t)wpo->height * 4;
        wpo->cached_sharp = calloc(1, image_size);
        if (wpo->cached_sharp == NULL) {
            paint_default_pattern(dst, wpo->width, wpo->height);
            return;
        }

        scale_cover(
            g_wallpaper.image_pixels, g_wallpaper.image_width,
            g_wallpaper.image_height, wpo->cached_sharp, wpo->width, wpo->height
        );
    }

    size_t image_size = (size_t)wpo->width * (size_t)wpo->height * 4;
    memcpy(dst, wpo->cached_sharp, image_size);
}

struct wallpaper_buffer {
    struct wlr_buffer base;
    uint8_t *data;
    size_t stride;
};

static void wallpaper_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct wallpaper_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    free(buf->data);
    free(buf);
}

static bool wallpaper_buffer_begin_data_ptr_access(
    struct wlr_buffer *wlr_buffer, uint32_t flags, void **data,
    uint32_t *format, size_t *stride
) {
    struct wallpaper_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    (void)flags;

    *data = buf->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = buf->stride;
    return true;
}

static void wallpaper_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer
) {
    (void)wlr_buffer;
}

static const struct wlr_buffer_impl wallpaper_buffer_impl = {
    .destroy = wallpaper_buffer_destroy,
    .begin_data_ptr_access = wallpaper_buffer_begin_data_ptr_access,
    .end_data_ptr_access = wallpaper_buffer_end_data_ptr_access,
};

static struct wlr_buffer *alloc_wlr_buffer(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;

    size_t stride = (size_t)w * 4;
    if ((size_t)h > SIZE_MAX / stride) return NULL;

    uint8_t *data = calloc((size_t)h, stride);
    if (data == NULL) return NULL;

    struct wallpaper_buffer *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        free(data);
        return NULL;
    }

    wlr_buffer_init(&buf->base, &wallpaper_buffer_impl, w, h);
    buf->data = data;
    buf->stride = stride;
    return &buf->base;
}

static bool wpo_redraw(struct WallpaperOutput *wpo) {
    int w = wpo->width;
    int h = wpo->height;
    if (w <= 0 || h <= 0) return false;

    size_t uw = (size_t)w;
    size_t uh = (size_t)h;

    uint8_t *pixels = malloc(uw * uh * 4);
    if (pixels == NULL) return false;

    wpo_compose_to_cpu(wpo, pixels);

    struct wlr_buffer *buf = alloc_wlr_buffer(w, h);
    if (buf == NULL) {
        free(pixels);
        return false;
    }

    void *data = NULL;
    uint32_t fmt = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(
            buf, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &fmt, &stride
        )) {
        wlr_buffer_drop(buf);
        free(pixels);
        return false;
    }

    for (size_t y = 0; y < uh; y++) {
        memcpy((uint8_t *)data + y * stride, pixels + y * uw * 4, uw * 4);
    }

    wlr_buffer_end_data_ptr_access(buf);
    free(pixels);

    wlr_scene_buffer_set_buffer(wpo->scene_buffer, buf);
    wlr_buffer_drop(buf);

    wpo->drawn_valid = true;
    wpo->drawn_w = w;
    wpo->drawn_h = h;
    wpo->drawn_loaded = g_wallpaper.loaded;

    wpo->drawn_pattern_bg_r = wm.wallpaper.pattern_bg_r;
    wpo->drawn_pattern_bg_g = wm.wallpaper.pattern_bg_g;
    wpo->drawn_pattern_bg_b = wm.wallpaper.pattern_bg_b;
    wpo->drawn_pattern_dot_r = wm.wallpaper.pattern_dot_r;
    wpo->drawn_pattern_dot_g = wm.wallpaper.pattern_dot_g;
    wpo->drawn_pattern_dot_b = wm.wallpaper.pattern_dot_b;
    wpo->drawn_pattern_grid_spacing = wm.wallpaper.pattern_grid_spacing;
    wpo->drawn_pattern_dot_radius = wm.wallpaper.pattern_dot_radius;

    return true;
}

bool wallpaper_load_ppm(const char *path) {
    uint8_t *pixels = NULL;
    int w = 0, h = 0;

    if (!ppm_load(path, &pixels, &w, &h)) {
        log_warn("Using default pattern");
        return false;
    }

    log_info("Loaded '%s' (%dx%d)", path, w, h);
    free(g_wallpaper.image_pixels);

    g_wallpaper.image_pixels = pixels;
    g_wallpaper.image_width = w;
    g_wallpaper.image_height = h;
    g_wallpaper.loaded = true;

    wallpaper_invalidate_all();
    return true;
}

void wallpaper_invalidate_all(void) {
    struct WallpaperOutput *wpo;
    wl_list_for_each(wpo, &g_wallpaper.outputs, link) {
        wpo_destroy_cached_variants(wpo);
        wpo->drawn_valid = false;
    }
}

static bool wallpaper_blur_wanted(void) {
    if (wm.wallpaper.blur_radius <= 0) return false;
    if (wm.wallpaper.blur_passes <= 0) return false;

    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed) continue;
        if (w->space_hidden) continue;
        if (w->space != wm.current_space) continue;

        return true;
    }

    return false;
}

static void wpo_sync_blur(struct WallpaperOutput *wpo, struct Output *output) {
    bool want_blur = wallpaper_blur_wanted();

    if (!want_blur) {
        if (wpo->blur != NULL) {
            wlr_scene_node_destroy(&wpo->blur->node);
            wpo->blur = NULL;
        }
        return;
    }

    if (output->width <= 0 || output->height <= 0) return;

    if (wpo->blur == NULL) {
        wpo->blur = wlr_scene_blur_create(
            server.wallpaper_blur_tree, output->width, output->height
        );

        if (wpo->blur == NULL) return;

        wlr_scene_blur_set_strength(wpo->blur, 1.0f);
        wlr_scene_blur_set_alpha(wpo->blur, 1.0f);
    }

    wlr_scene_node_set_position(&wpo->blur->node, output->pos_x, output->pos_y);
    wlr_scene_blur_set_size(wpo->blur, output->width, output->height);
}

struct WallpaperOutput *wallpaper_output_create(struct Output *output) {
    struct WallpaperOutput *wpo = calloc(1, sizeof(*wpo));
    if (wpo == NULL) return NULL;

    wpo->output = output;
    wpo->width = output->width;
    wpo->height = output->height;

    wpo->scene_buffer = wlr_scene_buffer_create(server.background_tree, NULL);
    if (wpo->scene_buffer == NULL) {
        free(wpo);
        return NULL;
    }

    wlr_scene_node_set_position(
        &wpo->scene_buffer->node, output->pos_x, output->pos_y
    );

    wl_list_insert(&g_wallpaper.outputs, &wpo->link);
    wpo_sync_blur(wpo, output);

    return wpo;
}

void wallpaper_output_destroy(struct WallpaperOutput *wpo) {
    if (wpo == NULL) return;

    wpo_destroy_cached_variants(wpo);

    if (wpo->blur != NULL) {
        wlr_scene_node_destroy(&wpo->blur->node);
        wpo->blur = NULL;
    }

    if (wpo->scene_buffer != NULL) {
        wlr_scene_node_destroy(&wpo->scene_buffer->node);
    }

    wl_list_remove(&wpo->link);
    free(wpo);
}

void wallpaper_frame(struct Output *output) {
    struct WallpaperOutput *wpo;

    wl_list_for_each(wpo, &g_wallpaper.outputs, link) {
        if (wpo->output != output) continue;
        if (output->width <= 0 || output->height <= 0) return;

        if (wpo->width != output->width || wpo->height != output->height) {
            wpo->width = output->width;
            wpo->height = output->height;
            wpo_destroy_cached_variants(wpo);
            wpo->drawn_valid = false;
        }

        wlr_scene_node_set_position(
            &wpo->scene_buffer->node, output->pos_x, output->pos_y
        );

        wpo_sync_blur(wpo, output);

        if (wpo->drawn_valid && wpo->drawn_loaded == g_wallpaper.loaded) {
            if (g_wallpaper.loaded) { return; }

            if (wpo->drawn_pattern_bg_r == wm.wallpaper.pattern_bg_r
                && wpo->drawn_pattern_bg_g == wm.wallpaper.pattern_bg_g
                && wpo->drawn_pattern_bg_b == wm.wallpaper.pattern_bg_b
                && wpo->drawn_pattern_dot_r == wm.wallpaper.pattern_dot_r
                && wpo->drawn_pattern_dot_g == wm.wallpaper.pattern_dot_g
                && wpo->drawn_pattern_dot_b == wm.wallpaper.pattern_dot_b
                && wpo->drawn_pattern_grid_spacing
                       == wm.wallpaper.pattern_grid_spacing
                && wpo->drawn_pattern_dot_radius
                       == wm.wallpaper.pattern_dot_radius) {
                return;
            }
        }

        wpo_redraw(wpo);
        return;
    }
}
