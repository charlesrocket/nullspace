#include "wallpaper.h"

#include "log.h"
#include "manager.h"
#include "output.h"
#include "server.h"
#include "window.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pixman.h>
#include <pthread.h>
#include <scenefx/types/wlr_scene.h>
#include <spng.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_pthreadtypes.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>

#define LOG_TOPIC       "wallpaper"

#define MAX_DIMENSION   32768u
#define MAX_PIXELS      ((uint64_t)64 * 1024 * 1024) // 256MB (32bit)
#define MAX_FILE_BYTES  ((uint64_t)256 * 1024 * 1024)
#define PNG_CHUNK_LIMIT ((size_t)4 * 1024 * 1024)
#define PNG_CACHE_LIMIT ((size_t)8 * 1024 * 1024)

static struct Wallpaper {
    uint8_t *image_pixels;
    int image_width, image_height;
    bool loaded;
    struct wl_list outputs;
} g_wallpaper = {
    .outputs = {.prev = &g_wallpaper.outputs, .next = &g_wallpaper.outputs},
};

struct WpJob {
    struct wl_list link;
    struct WallpaperOutput *wpo;

    uint32_t gen;

    int32_t width, height;
    int32_t src_w, src_h;
    int32_t pattern_bg_r, pattern_bg_g, pattern_bg_b;
    int32_t pattern_dot_r, pattern_dot_g, pattern_dot_b;
    int32_t pattern_grid_spacing, pattern_dot_radius;
    int32_t blur_radius, blur_passes;

    uint8_t *src, *sharp, *blurred;

    bool blur_enabled, src_loaded;
};

static struct {
    struct wl_event_source *wake_source;
    struct wl_list pending, completed;

    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    uint32_t gen;
    int wake_pipe[2];

    bool quit, started;
} g_worker = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .wake_pipe = {-1, -1},
};

static void paint_default_pattern(
    uint8_t *dst, int w, int h, int32_t bg_r, int32_t bg_g, int32_t bg_b,
    int32_t dot_r, int32_t dot_g, int32_t dot_b, int32_t spacing,
    int32_t dot_radius
) {
    if (w <= 0 || h <= 0) return;
    if (spacing < 1) spacing = 1;
    if (dot_radius < 0) dot_radius = 0;
    if (dot_radius > spacing) dot_radius = spacing;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *dp = &dst[((size_t)y * (size_t)w + (size_t)x) * 4];
            dp[0] = (uint8_t)bg_b;
            dp[1] = (uint8_t)bg_g;
            dp[2] = (uint8_t)bg_r;
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

                    uint8_t *dp =
                        &dst[((size_t)py * (size_t)w + (size_t)px) * 4];

                    dp[0] = (uint8_t)dot_b;
                    dp[1] = (uint8_t)dot_g;
                    dp[2] = (uint8_t)dot_r;
                    dp[3] = 255;
                }
            }
        }
    }
}

static void put_px(uint8_t *p, uint8_t r, uint8_t g, uint8_t b) {
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 255;
}

static bool image_size_ok(const char *path, uint64_t w, uint64_t h) {
    if (w == 0 || h == 0 || w > MAX_DIMENSION || h > MAX_DIMENSION) {
        log_warn(
            "'%s' has unsupported dimensions (%" PRIu64 "x%" PRIu64 ")", path,
            w, h
        );

        return false;
    }

    if (w * h > MAX_PIXELS) {
        log_warn(
            "'%s' is too large (%" PRIu64 " pixels, max %" PRIu64 ")", path,
            w * h, MAX_PIXELS
        );

        return false;
    }

    return true;
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
        || v > INT_MAX)
        return false;

    *out = (int)v;
    return true;
}

static bool
read_whole_file(const char *path, uint8_t **out_data, size_t *out_size) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
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

    if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
        log_warn("'%s' is not a non-empty regular file", path);
        close(fd);
        return false;
    }

    if ((uint64_t)st.st_size > MAX_FILE_BYTES) {
        log_warn("'%s' is too large", path);
        close(fd);
        return false;
    }

    size_t size = (size_t)st.st_size;
    uint8_t *data = malloc(size);
    if (data == NULL) {
        log_warn("OOM reading '%s'", path);
        close(fd);
        return false;
    }

    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, data + got, size - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_warn("Read ('%s') failed: %s", path, strerror(errno));
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

static bool ppm_decode(
    const char *path, const uint8_t *filebuf, size_t filebuf_size,
    uint8_t **out_px, int *out_w, int *out_h
) {
    struct ppm_buf b = {.data = filebuf, .size = filebuf_size, .pos = 0};
    char token[64];

    if (ppm_buf_token(&b, token, sizeof(token)) != 0
        || strcmp(token, "P6") != 0) {
        log_warn("'%s' is not a binary PPM (P6)", path);
        return false;
    }

    int width, height, maxval;
    if (!ppm_buf_read_int(&b, &width) || !ppm_buf_read_int(&b, &height)
        || !ppm_buf_read_int(&b, &maxval)) {
        log_warn("Malformed PPM header in '%s'", path);
        return false;
    }

    if (width <= 0 || height <= 0 || maxval != 255) {
        log_warn(
            "Unsupported PPM in '%s' (w=%d h=%d maxval=%d)", path, width,
            height, maxval
        );

        return false;
    }

    if (!image_size_ok(path, (uint64_t)width, (uint64_t)height)) return false;

    size_t pixel_count = (size_t)width * (size_t)height;
    size_t needed = pixel_count * 3;
    if (needed > b.size - b.pos) {
        log_warn("Truncated pixel data in '%s'", path);
        return false;
    }

    const uint8_t *rgb = b.data + b.pos;
    uint8_t *px = malloc(pixel_count * 4);
    if (px == NULL) {
        log_warn("OOM decoding (PPM) '%s'", path);
        return false;
    }

    for (size_t i = 0; i < pixel_count; i++) {
        px[i * 4 + 0] = rgb[i * 3 + 2];
        px[i * 4 + 1] = rgb[i * 3 + 1];
        px[i * 4 + 2] = rgb[i * 3 + 0];
        px[i * 4 + 3] = 255;
    }

    *out_px = px;
    *out_w = width;
    *out_h = height;
    return true;
}

static bool is_png(const uint8_t *data, size_t size) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    return size >= sizeof(sig) && memcmp(data, sig, sizeof(sig)) == 0;
}

static uint8_t *png_decode_with(
    spng_ctx *ctx, const char *path, const uint8_t *data, size_t size,
    int *out_w, int *out_h
) {
    int ret = spng_set_image_limits(ctx, MAX_DIMENSION, MAX_DIMENSION);

    if (ret == 0)
        ret = spng_set_chunk_limits(ctx, PNG_CHUNK_LIMIT, PNG_CACHE_LIMIT);

    if (ret == 0) ret = spng_set_png_buffer(ctx, data, size);
    if (ret != 0) {
        log_warn(
            "Failed to configure PNG decoder for '%s': %s", path,
            spng_strerror(ret)
        );

        return NULL;
    }

    struct spng_ihdr ihdr;
    ret = spng_get_ihdr(ctx, &ihdr);
    if (ret != 0) {
        log_warn(
            "Failed to read PNG header of '%s': %s", path, spng_strerror(ret)
        );

        return NULL;
    }

    if (!image_size_ok(path, ihdr.width, ihdr.height)) return NULL;

    size_t pixel_count = (size_t)ihdr.width * (size_t)ihdr.height;
    size_t out_size = pixel_count * 4;

    uint8_t *px = malloc(out_size);
    if (px == NULL) {
        log_warn("OOM decoding '%s'", path);
        return NULL;
    }

    ret =
        spng_decode_image(ctx, px, out_size, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
    if (ret != 0) {
        log_warn("Failed to decode PNG '%s': %s", path, spng_strerror(ret));
        free(px);
        return NULL;
    }

    for (size_t i = 0; i < pixel_count; i++) {
        uint8_t *p = &px[i * 4];
        uint32_t a = p[3];
        uint8_t r = p[0], g = p[1], b = p[2];
        if (a != 255) {
            r = (uint8_t)((r * a + 127) / 255);
            g = (uint8_t)((g * a + 127) / 255);
            b = (uint8_t)((b * a + 127) / 255);
        }

        put_px(p, r, g, b);
    }

    *out_w = (int)ihdr.width;
    *out_h = (int)ihdr.height;

    return px;
}

static bool png_decode(
    const char *path, const uint8_t *data, size_t size, uint8_t **out_px,
    int *out_w, int *out_h
) {
    spng_ctx *ctx = spng_ctx_new(0);
    if (ctx == NULL) return false;

    uint8_t *px = png_decode_with(ctx, path, data, size, out_w, out_h);
    spng_ctx_free(ctx);

    if (px == NULL) return false;

    *out_px = px;
    return true;
}

static bool
image_load(const char *path, uint8_t **out_px, int *out_w, int *out_h) {
    uint8_t *filebuf = NULL;
    size_t filebuf_size = 0;

    if (!read_whole_file(path, &filebuf, &filebuf_size)) return false;

    bool ok = is_png(filebuf, filebuf_size)
                ? png_decode(path, filebuf, filebuf_size, out_px, out_w, out_h)
                : ppm_decode(path, filebuf, filebuf_size, out_px, out_w, out_h);

    free(filebuf);
    return ok;
}

static uint8_t *
box_downscale(const uint8_t *src, int sw, int sh, int k, int *ow, int *oh) {
    int w = sw / k, h = sh / k;
    if (w <= 0 || h <= 0) return NULL;

    uint8_t *out = malloc((size_t)w * (size_t)h * 4);
    uint32_t *acc = malloc((size_t)w * 4 * sizeof(*acc));
    if (out == NULL || acc == NULL) {
        free(out);
        free(acc);
        return NULL;
    }

    uint32_t div = (uint32_t)k * (uint32_t)k;
    for (int y = 0; y < h; y++) {
        memset(acc, 0, (size_t)w * 4 * sizeof(*acc));
        for (int j = 0; j < k; j++) {
            const uint8_t *row =
                src + ((size_t)y * (size_t)k + (size_t)j) * (size_t)sw * 4;
            for (int x = 0; x < w; x++) {
                const uint8_t *p = row + (size_t)x * (size_t)k * 4;
                for (int i = 0; i < k; i++, p += 4) {
                    acc[x * 4 + 0] += p[0];
                    acc[x * 4 + 1] += p[1];
                    acc[x * 4 + 2] += p[2];
                }
            }
        }

        uint8_t *o = out + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++) {
            o[x * 4 + 0] = (uint8_t)(acc[x * 4 + 0] / div);
            o[x * 4 + 1] = (uint8_t)(acc[x * 4 + 1] / div);
            o[x * 4 + 2] = (uint8_t)(acc[x * 4 + 2] / div);
            o[x * 4 + 3] = 255;
        }
    }

    free(acc);
    *ow = w;
    *oh = h;
    return out;
}

static bool
scale_cover(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;

    double cover =
        (double)dw / sw > (double)dh / sh ? (double)dw / sw : (double)dh / sh;
    uint8_t *tmp = NULL;
    int k = (int)(1.0 / cover);

    if (k >= 2) {
        int nw, nh;
        tmp = box_downscale(src, sw, sh, k, &nw, &nh);
        if (tmp == NULL) return false;

        src = tmp;
        sw = nw;
        sh = nh;
        cover = (double)dw / sw > (double)dh / sh ? (double)dw / sw
                                                  : (double)dh / sh;
    }

    double inv = 1.0 / cover;
    double off_x = ((double)sw - dw * inv) / 2.0;
    double off_y = ((double)sh - dh * inv) / 2.0;

    pixman_image_t *s = pixman_image_create_bits(
        PIXMAN_x8r8g8b8, sw, sh, (uint32_t *)(uintptr_t)src, sw * 4
    );
    pixman_image_t *d = pixman_image_create_bits(
        PIXMAN_x8r8g8b8, dw, dh, (uint32_t *)dst, dw * 4
    );

    bool ok = s != NULL && d != NULL;
    if (ok) {
        pixman_transform_t t = {
            {
             {pixman_double_to_fixed(inv), 0, pixman_double_to_fixed(off_x)},
             {0, pixman_double_to_fixed(inv), pixman_double_to_fixed(off_y)},
             {0, 0, pixman_fixed_1},
             }
        };

        pixman_image_set_transform(s, &t);
        pixman_image_set_repeat(s, PIXMAN_REPEAT_PAD);
        pixman_image_set_filter(s, PIXMAN_FILTER_BILINEAR, NULL, 0);
        pixman_image_composite32(
            PIXMAN_OP_SRC, s, NULL, d, 0, 0, 0, 0, 0, 0, dw, dh
        );
    }

    if (s) pixman_image_unref(s);
    if (d) pixman_image_unref(d);
    free(tmp);
    return ok;
}

static void box_blur(uint8_t *pixels, int w, int h, int radius) {
    if (radius <= 0 || w <= 0 || h <= 0) return;

    uint8_t *scratch = malloc((size_t)w * (size_t)h * 4);
    if (scratch == NULL) return;

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
                if (add_x >= w) add_x = w - 1;
                if (rem_x < 0) rem_x = 0;

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
                if (add_y >= h) add_y = h - 1;
                if (rem_y < 0) rem_y = 0;

                sum += scratch
                    [((size_t)add_y * (size_t)w + (size_t)x) * 4 + (size_t)c];
                sum -= scratch
                    [((size_t)rem_y * (size_t)w + (size_t)x) * 4 + (size_t)c];
            }
        }
    }

    free(scratch);
}

static void wpjob_free(struct WpJob *job) {
    if (job == NULL) return;
    free(job->src);
    free(job->sharp);
    free(job->blurred);
    free(job);
}

static void wpjob_compute(struct WpJob *job) {
    size_t image_size = (size_t)job->width * (size_t)job->height * 4;

    job->sharp = calloc(1, image_size);
    job->blurred = calloc(1, image_size);
    if (job->sharp == NULL || job->blurred == NULL) {
        free(job->sharp);
        free(job->blurred);
        job->sharp = NULL;
        job->blurred = NULL;
        return;
    }

    if (!job->src_loaded
        || !scale_cover(
            job->src, job->src_w, job->src_h, job->sharp, job->width,
            job->height
        )) {
        paint_default_pattern(
            job->sharp, job->width, job->height, job->pattern_bg_r,
            job->pattern_bg_g, job->pattern_bg_b, job->pattern_dot_r,
            job->pattern_dot_g, job->pattern_dot_b, job->pattern_grid_spacing,
            job->pattern_dot_radius
        );
    }

    memcpy(job->blurred, job->sharp, image_size);

    if (job->blur_enabled) {
        for (int p = 0; p < job->blur_passes; p++) {
            box_blur(job->blurred, job->width, job->height, job->blur_radius);
        }
    }
}

static void *worker_main(void *arg) {
    (void)arg;

    pthread_mutex_lock(&g_worker.mutex);
    for (;;) {
        while (wl_list_empty(&g_worker.pending) && !g_worker.quit) {
            pthread_cond_wait(&g_worker.cond, &g_worker.mutex);
        }

        if (g_worker.quit) break;

        struct WpJob *job = wl_container_of(g_worker.pending.next, job, link);
        wl_list_remove(&job->link);

        if (job->gen != g_worker.gen) {
            wpjob_free(job);
            continue;
        }

        pthread_mutex_unlock(&g_worker.mutex);
        wpjob_compute(job);
        pthread_mutex_lock(&g_worker.mutex);

        if (job->gen != g_worker.gen || g_worker.quit) {
            wpjob_free(job);
            continue;
        }

        wl_list_insert(&g_worker.completed, &job->link);
        (void)!write(g_worker.wake_pipe[1], "x", 1);
    }

    pthread_mutex_unlock(&g_worker.mutex);
    return NULL;
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
    (void)flags;

    struct wallpaper_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    *data = buf->data;
    *format = DRM_FORMAT_XRGB8888;
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

static struct wallpaper_buffer *wallpaper_buffer_create(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;

    size_t stride = (size_t)w * 4;
    if ((size_t)h > SIZE_MAX / stride) return NULL;

    uint8_t *data = malloc((size_t)h * stride);
    if (data == NULL) return NULL;

    struct wallpaper_buffer *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        free(data);
        return NULL;
    }

    wlr_buffer_init(&buf->base, &wallpaper_buffer_impl, w, h);
    buf->data = data;
    buf->stride = stride;
    return buf;
}

bool wallpaper_blur_wanted(void) {
    if (!wm.wallpaper.blur_enabled) return false;

    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->closed) continue;
        if (w->space_hidden) continue;
        if (w->space != wm.current_space) continue;
        return true;
    }

    return false;
}

static void wpo_free_cache(struct WallpaperOutput *wpo) {
    free(wpo->cached_sharp);
    wpo->cached_sharp = NULL;
    free(wpo->cached_blurred);
    wpo->cached_blurred = NULL;
    wpo->drawn_valid = false;
}

static void wpo_record_state(struct WallpaperOutput *wpo) {
    wpo->drawn_valid = true;
    wpo->drawn_w = wpo->width;
    wpo->drawn_h = wpo->height;
    wpo->drawn_loaded = g_wallpaper.loaded;
    wpo->drawn_blur_active = wallpaper_blur_wanted();
    wpo->drawn_blur_enabled = wm.wallpaper.blur_enabled;

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
}

static void wpo_redraw(struct WallpaperOutput *wpo) {
    if (wpo->cached_sharp == NULL || wpo->cached_blurred == NULL) return;
    if (wpo->width <= 0 || wpo->height <= 0) return;

    struct wallpaper_buffer *buf =
        wallpaper_buffer_create(wpo->width, wpo->height);
    if (buf == NULL) return;

    size_t image_size = (size_t)wpo->width * (size_t)wpo->height * 4;
    if (wallpaper_blur_wanted()) {
        memcpy(buf->data, wpo->cached_blurred, image_size);
    } else {
        memcpy(buf->data, wpo->cached_sharp, image_size);
    }

    wlr_scene_buffer_set_buffer(wpo->scene_buffer, &buf->base);
    wlr_buffer_drop(&buf->base);

    pixman_region32_t opaque;
    pixman_region32_init_rect(
        &opaque, 0, 0, (unsigned)wpo->width, (unsigned)wpo->height
    );

    wlr_scene_buffer_set_opaque_region(wpo->scene_buffer, &opaque);
    pixman_region32_fini(&opaque);

    wpo_record_state(wpo);
}

static void apply_job(struct WpJob *job) {
    struct WallpaperOutput *wpo = job->wpo;

    if (wpo == NULL || job->sharp == NULL || job->blurred == NULL
        || wpo->width != job->width || wpo->height != job->height) {
        wpjob_free(job);
        return;
    }

    free(wpo->cached_sharp);
    wpo->cached_sharp = job->sharp;
    job->sharp = NULL;

    free(wpo->cached_blurred);
    wpo->cached_blurred = job->blurred;
    job->blurred = NULL;

    wpo->drawn_valid = false;
    wpo_redraw(wpo);
    wpjob_free(job);
}

static int on_worker_wake(int fd, uint32_t mask, void *data) {
    (void)mask;
    (void)data;

    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {}

    pthread_mutex_lock(&g_worker.mutex);

    struct WpJob *job, *tmp;
    wl_list_for_each_safe(job, tmp, &g_worker.completed, link) {
        wl_list_remove(&job->link);
        pthread_mutex_unlock(&g_worker.mutex);
        apply_job(job);
        pthread_mutex_lock(&g_worker.mutex);
    }

    pthread_mutex_unlock(&g_worker.mutex);

    return 0;
}

void wallpaper_worker_init(void) {
    if (g_worker.started) return;
    if (server.event_loop == NULL) return;

    wl_list_init(&g_worker.pending);
    wl_list_init(&g_worker.completed);

    if (pipe(g_worker.wake_pipe) < 0) {
        log_err("pipe() failed: %s", strerror(errno));
        return;
    }

    fcntl(g_worker.wake_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_worker.wake_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(g_worker.wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_worker.wake_pipe[1], F_SETFL, O_NONBLOCK);

    g_worker.wake_source = wl_event_loop_add_fd(
        server.event_loop, g_worker.wake_pipe[0], WL_EVENT_READABLE,
        on_worker_wake, NULL
    );

    if (g_worker.wake_source == NULL) {
        log_err("Failed to register wake source");
        close(g_worker.wake_pipe[0]);
        close(g_worker.wake_pipe[1]);
        g_worker.wake_pipe[0] = g_worker.wake_pipe[1] = -1;
        return;
    }

    if (pthread_create(&g_worker.thread, NULL, worker_main, NULL) != 0) {
        log_err("pthread_create failed");
        wl_event_source_remove(g_worker.wake_source);
        g_worker.wake_source = NULL;
        close(g_worker.wake_pipe[0]);
        close(g_worker.wake_pipe[1]);
        g_worker.wake_pipe[0] = g_worker.wake_pipe[1] = -1;
        return;
    }

    g_worker.started = true;
}

void wallpaper_worker_finish(void) {
    if (!g_worker.started) return;

    pthread_mutex_lock(&g_worker.mutex);
    g_worker.quit = true;
    pthread_cond_signal(&g_worker.cond);
    pthread_mutex_unlock(&g_worker.mutex);

    pthread_join(g_worker.thread, NULL);

    struct WpJob *job, *tmp;
    wl_list_for_each_safe(job, tmp, &g_worker.pending, link) {
        wl_list_remove(&job->link);
        wpjob_free(job);
    }
    wl_list_for_each_safe(job, tmp, &g_worker.completed, link) {
        wl_list_remove(&job->link);
        wpjob_free(job);
    }

    if (g_worker.wake_source != NULL) {
        wl_event_source_remove(g_worker.wake_source);
        g_worker.wake_source = NULL;
    }
    if (g_worker.wake_pipe[0] >= 0) close(g_worker.wake_pipe[0]);
    if (g_worker.wake_pipe[1] >= 0) close(g_worker.wake_pipe[1]);
    g_worker.wake_pipe[0] = g_worker.wake_pipe[1] = -1;

    g_worker.started = false;
    g_worker.quit = false;
    g_worker.gen = 0;
}

static void wpo_enqueue_rebuild(struct WallpaperOutput *wpo) {
    if (!g_worker.started) return;
    if (wpo->width <= 0 || wpo->height <= 0) return;

    struct WpJob *job = calloc(1, sizeof(*job));
    if (job == NULL) return;

    job->wpo = wpo;
    job->width = wpo->width;
    job->height = wpo->height;

    if (g_wallpaper.loaded && g_wallpaper.image_pixels != NULL) {
        size_t sz = (size_t)g_wallpaper.image_width
                  * (size_t)g_wallpaper.image_height * 4;
        job->src = malloc(sz);
        if (job->src != NULL) {
            memcpy(job->src, g_wallpaper.image_pixels, sz);
            job->src_w = g_wallpaper.image_width;
            job->src_h = g_wallpaper.image_height;
            job->src_loaded = true;
        }
    }

    job->pattern_bg_r = wm.wallpaper.pattern_bg_r;
    job->pattern_bg_g = wm.wallpaper.pattern_bg_g;
    job->pattern_bg_b = wm.wallpaper.pattern_bg_b;
    job->pattern_dot_r = wm.wallpaper.pattern_dot_r;
    job->pattern_dot_g = wm.wallpaper.pattern_dot_g;
    job->pattern_dot_b = wm.wallpaper.pattern_dot_b;
    job->pattern_grid_spacing = wm.wallpaper.pattern_grid_spacing;
    job->pattern_dot_radius = wm.wallpaper.pattern_dot_radius;
    job->blur_radius = wm.wallpaper.blur_radius;
    job->blur_passes = wm.wallpaper.blur_passes;
    job->blur_enabled = wm.wallpaper.blur_enabled;

    pthread_mutex_lock(&g_worker.mutex);

    struct WpJob *j, *tmp;
    wl_list_for_each_safe(j, tmp, &g_worker.pending, link) {
        if (j->wpo == wpo) {
            wl_list_remove(&j->link);
            wpjob_free(j);
        }
    }
    wl_list_for_each_safe(j, tmp, &g_worker.completed, link) {
        if (j->wpo == wpo) {
            wl_list_remove(&j->link);
            wpjob_free(j);
        }
    }

    job->gen = g_worker.gen;
    wl_list_insert(g_worker.pending.prev, &job->link);
    pthread_cond_signal(&g_worker.cond);

    pthread_mutex_unlock(&g_worker.mutex);
}

bool wallpaper_load_image(const char *path) {
    uint8_t *pixels = NULL;
    int w = 0, h = 0;

    if (path == NULL || !image_load(path, &pixels, &w, &h)) {
        if (g_wallpaper.loaded) {
            log_warn("Using previous wallpaper");
        } else {
            log_warn("Using default pattern");
        }

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
    if (!g_worker.started) return;

    pthread_mutex_lock(&g_worker.mutex);
    g_worker.gen++;

    struct WpJob *job, *tmp;
    wl_list_for_each_safe(job, tmp, &g_worker.pending, link) {
        wl_list_remove(&job->link);
        wpjob_free(job);
    }

    wl_list_for_each_safe(job, tmp, &g_worker.completed, link) {
        wl_list_remove(&job->link);
        wpjob_free(job);
    }

    pthread_mutex_unlock(&g_worker.mutex);

    struct WallpaperOutput *wpo;
    wl_list_for_each(wpo, &g_wallpaper.outputs, link) {
        wpo->drawn_valid = false;
        wpo_enqueue_rebuild(wpo);
    }
}

void wallpaper_finish(void) {
    wallpaper_worker_finish();

    if (!wl_list_empty(&g_wallpaper.outputs)) {
        log_warn("wallpaper_finish with live outputs");
    }

    free(g_wallpaper.image_pixels);
    g_wallpaper.image_pixels = NULL;
    g_wallpaper.image_width = 0;
    g_wallpaper.image_height = 0;
    g_wallpaper.loaded = false;
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
    return wpo;
}

void wallpaper_output_destroy(struct WallpaperOutput *wpo) {
    if (wpo == NULL) return;

    if (g_worker.started) {
        pthread_mutex_lock(&g_worker.mutex);
        struct WpJob *j, *tmp;
        wl_list_for_each_safe(j, tmp, &g_worker.pending, link) {
            if (j->wpo == wpo) {
                wl_list_remove(&j->link);
                wpjob_free(j);
            }
        }

        wl_list_for_each_safe(j, tmp, &g_worker.completed, link) {
            if (j->wpo == wpo) {
                wl_list_remove(&j->link);
                wpjob_free(j);
            }
        }

        pthread_mutex_unlock(&g_worker.mutex);
    }

    free(wpo->cached_sharp);
    free(wpo->cached_blurred);

    if (wpo->scene_buffer != NULL) {
        wlr_scene_node_destroy(&wpo->scene_buffer->node);
    }

    wl_list_remove(&wpo->link);
    free(wpo);
}

static bool wpo_needs_rebuild(const struct WallpaperOutput *wpo) {
    if (wpo->cached_sharp == NULL || wpo->cached_blurred == NULL) return true;
    if (wpo->drawn_w != wpo->width || wpo->drawn_h != wpo->height) return true;
    if (wpo->drawn_loaded != g_wallpaper.loaded) return true;

    if (!g_wallpaper.loaded) {
        if (wpo->drawn_pattern_bg_r != wm.wallpaper.pattern_bg_r) return true;
        if (wpo->drawn_pattern_bg_g != wm.wallpaper.pattern_bg_g) return true;
        if (wpo->drawn_pattern_bg_b != wm.wallpaper.pattern_bg_b) return true;
        if (wpo->drawn_pattern_dot_r != wm.wallpaper.pattern_dot_r) return true;
        if (wpo->drawn_pattern_dot_g != wm.wallpaper.pattern_dot_g) return true;
        if (wpo->drawn_pattern_dot_b != wm.wallpaper.pattern_dot_b) return true;
        if (wpo->drawn_pattern_grid_spacing
            != wm.wallpaper.pattern_grid_spacing)
            return true;

        if (wpo->drawn_pattern_dot_radius != wm.wallpaper.pattern_dot_radius)
            return true;
    }

    if (wpo->drawn_blur_radius != wm.wallpaper.blur_radius) return true;
    if (wpo->drawn_blur_passes != wm.wallpaper.blur_passes) return true;
    if (wpo->drawn_blur_enabled != wm.wallpaper.blur_enabled) return true;

    return false;
}

static bool wpo_needs_redraw(const struct WallpaperOutput *wpo) {
    if (wpo->cached_sharp == NULL || wpo->cached_blurred == NULL) return false;
    if (!wpo->drawn_valid) return true;
    return wpo->drawn_blur_active != wallpaper_blur_wanted();
}

static void wpo_update(struct WallpaperOutput *wpo) {
    struct Output *output = wpo->output;
    if (output->width <= 0 || output->height <= 0) return;

    if (wpo->width != output->width || wpo->height != output->height) {
        wpo->width = output->width;
        wpo->height = output->height;
        wpo_free_cache(wpo);
    }

    wlr_scene_node_set_position(
        &wpo->scene_buffer->node, output->pos_x, output->pos_y
    );

    if (wpo_needs_rebuild(wpo)) {
        wpo_enqueue_rebuild(wpo);
    } else if (wpo_needs_redraw(wpo)) {
        wpo_redraw(wpo);
    }
}

void wallpaper_update_all(void) {
    struct WallpaperOutput *wpo;
    wl_list_for_each(wpo, &g_wallpaper.outputs, link) { wpo_update(wpo); }
}

void wallpaper_update_output(struct Output *output) {
    struct WallpaperOutput *wpo;
    wl_list_for_each(wpo, &g_wallpaper.outputs, link) {
        if (wpo->output == output) {
            wpo_update(wpo);
            return;
        }
    }
}
