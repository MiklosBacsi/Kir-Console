/*
 * Spinning 3D extrusion of a 2D silhouette, for the terminal.
 *
 * Loads a PNG, builds a thin slab (front, back, and side walls) from the
 * opaque pixels, then shades it with the same single directional light as
 * donut.c. Logo colors are kept and dimmed by the lighting.
 *
 * Build:  make
 * Run:    ./Kir-Console [path.png] [--tilt degrees]
 * Quit:   Ctrl+C
 *
 * Defaults to Kir-Dev-White.png if present.
 */

#define _POSIX_C_SOURCE 200809L

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define PI 3.14159265358979323846f
/* Dark to bright, in the usual ASCII density order. Twenty-four steps
 * fill the gap between the sparse glyphs and the solid ones. */
static const char PALETTE[] = ".`,;!~-1trncYC0mpka*M&%$";
enum { PALETTE_LEN = (int)(sizeof(PALETTE) - 1) };
_Static_assert(PALETTE_LEN == 24, "shade ramp length");
/* 4x4 Bayer thresholds in (0, 1). A fraction of 0 never promotes. */
static const float BAYER4[4][4] = {
    {0.5f / 16.0f, 8.5f / 16.0f, 2.5f / 16.0f, 10.5f / 16.0f},
    {12.5f / 16.0f, 4.5f / 16.0f, 14.5f / 16.0f, 6.5f / 16.0f},
    {3.5f / 16.0f, 11.5f / 16.0f, 1.5f / 16.0f, 9.5f / 16.0f},
    {15.5f / 16.0f, 7.5f / 16.0f, 13.5f / 16.0f, 5.5f / 16.0f},
};
#define TARGET_FPS 30
#define FRAME_NS (1000000000L / TARGET_FPS)

static const float K2 = 5.0f;
/* Thin coin-like depth: visible as a rim, not a brick. */
static const float THICKNESS = 0.32f;
static const float EXTENT = 2.8f;
static const int GRID_MAX = 120;

typedef struct {
    float x, y, z;
    float nx, ny, nz;
    unsigned char r, g, b;
} Vert;

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t resized = 0;

static void on_sigint(int sig) {
    (void)sig;
    running = 0;
}

static void on_sigwinch(int sig) {
    (void)sig;
    resized = 1;
}

static int term_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
        ws.ws_row > 0) {
        *cols = (int)ws.ws_col;
        *rows = (int)ws.ws_row;
        return 0;
    }
    *cols = 80;
    *rows = 24;
    return -1;
}

static void restore_terminal(void) {
    fputs("\x1b[?25h\x1b[0m\x1b[2J\x1b[H", stdout);
    fflush(stdout);
}

static int occupied(const unsigned char *occ, int gw, int gh, int i, int j) {
    if (i < 0 || j < 0 || i >= gw || j >= gh) {
        return 0;
    }
    return occ[j * gw + i] ? 1 : 0;
}

static void cell_color(const unsigned char *rgb, int gw, int i, int j,
                       unsigned char *r, unsigned char *g, unsigned char *b) {
    const unsigned char *p = rgb + (j * gw + i) * 3;
    *r = p[0];
    *g = p[1];
    *b = p[2];
}

static void push_vert(Vert **verts, int *n, int *cap, float x, float y, float z,
                      float nx, float ny, float nz, unsigned char r,
                      unsigned char g, unsigned char b) {
    Vert *v;
    if (*n >= *cap) {
        int ncap = *cap ? *cap * 2 : 4096;
        Vert *nv = realloc(*verts, (size_t)ncap * sizeof(Vert));
        if (!nv) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        *verts = nv;
        *cap = ncap;
    }
    v = &(*verts)[*n];
    v->x = x;
    v->y = y;
    v->z = z;
    v->nx = nx;
    v->ny = ny;
    v->nz = nz;
    v->r = r;
    v->g = g;
    v->b = b;
    (*n)++;
}

static void emit_face_samples(Vert **verts, int *n, int *cap, float x, float y,
                              float z, float nx, float ny, float nz, float tx,
                              float ty, float tz, float bx, float by, float bz,
                              int su, int sv, unsigned char r, unsigned char g,
                              unsigned char b) {
    int u, v;
    for (u = 0; u < su; u++) {
        for (v = 0; v < sv; v++) {
            float tu = (su == 1) ? 0.0f : ((float)u / (float)(su - 1) - 0.5f);
            float tv = (sv == 1) ? 0.0f : ((float)v / (float)(sv - 1) - 0.5f);
            push_vert(verts, n, cap, x + tx * tu + bx * tv, y + ty * tu + by * tv,
                      z + tz * tu + bz * tv, nx, ny, nz, r, g, b);
        }
    }
}

static unsigned char *load_silhouette(const char *path, int *gw, int *gh,
                                      unsigned char **out_rgb) {
    int w, h, nchan, i, j, x, y;
    int has_alpha = 0, solid_count = 0;
    unsigned char *img;
    unsigned char *occ;
    unsigned char *rgb;
    float scale;
    int sw, sh;
    long luma_acc = 0;
    int dark_bg;

    img = stbi_load(path, &w, &h, &nchan, 4);
    if (!img) {
        fprintf(stderr, "could not load PNG '%s': %s\n", path, stbi_failure_reason());
        return NULL;
    }

    for (i = 0; i < w * h; i++) {
        unsigned char *p = img + i * 4;
        luma_acc += (p[0] * 3 + p[1] * 6 + p[2]) / 10;
        if (p[3] < 250) {
            has_alpha = 1;
        }
    }
    dark_bg = (luma_acc / (w * h)) < 80;

    scale = (float)GRID_MAX / (float)(w > h ? w : h);
    if (scale > 1.0f) {
        scale = 1.0f;
    }
    sw = (int)(w * scale + 0.5f);
    sh = (int)(h * scale + 0.5f);
    if (sw < 8) {
        sw = 8;
    }
    if (sh < 8) {
        sh = 8;
    }

    occ = calloc((size_t)sw * (size_t)sh, 1);
    rgb = calloc((size_t)sw * (size_t)sh * 3, 1);
    if (!occ || !rgb) {
        stbi_image_free(img);
        free(occ);
        free(rgb);
        fprintf(stderr, "out of memory\n");
        return NULL;
    }

    for (j = 0; j < sh; j++) {
        int y0 = (int)((float)j * (float)h / (float)sh);
        int y1 = (int)((float)(j + 1) * (float)h / (float)sh);
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        if (y1 > h) {
            y1 = h;
        }
        for (i = 0; i < sw; i++) {
            int x0 = (int)((float)i * (float)w / (float)sw);
            int x1 = (int)((float)(i + 1) * (float)w / (float)sw);
            int hits = 0, cells = 0;
            long sr = 0, sg = 0, sb = 0, sa = 0;
            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            if (x1 > w) {
                x1 = w;
            }
            for (y = y0; y < y1; y++) {
                for (x = x0; x < x1; x++) {
                    unsigned char *p = img + (y * w + x) * 4;
                    int solid;
                    cells++;
                    if (has_alpha) {
                        solid = p[3] > 128;
                    } else if (dark_bg) {
                        int luma = (p[0] * 3 + p[1] * 6 + p[2]) / 10;
                        solid = luma > 40;
                    } else {
                        int luma = (p[0] * 3 + p[1] * 6 + p[2]) / 10;
                        solid = luma < 240;
                    }
                    if (solid) {
                        int a = p[3] > 0 ? p[3] : 255;
                        hits++;
                        sr += p[0] * a;
                        sg += p[1] * a;
                        sb += p[2] * a;
                        sa += a;
                    }
                }
            }
            /* Keep a cell if any opaque source pixel lands in it so thin
             * strokes (the wordmark, the KR arms) survive downsampling. */
            if (hits > 0) {
                unsigned char *c = rgb + (j * sw + i) * 3;
                occ[j * sw + i] = 1;
                if (sa > 0) {
                    c[0] = (unsigned char)(sr / sa);
                    c[1] = (unsigned char)(sg / sa);
                    c[2] = (unsigned char)(sb / sa);
                } else {
                    c[0] = c[1] = c[2] = 220;
                }
                solid_count++;
            }
        }
    }

    stbi_image_free(img);

    if (solid_count < 8) {
        fprintf(stderr,
                "'%s': silhouette is almost empty. Use a PNG with a transparent "
                "background, or a high-contrast logo.\n",
                path);
        free(occ);
        free(rgb);
        return NULL;
    }

    /* Crop to the opaque bounding box so padding does not shrink the logo. */
    {
        int minx = sw, miny = sh, maxx = -1, maxy = -1;
        int cw, ch, y2;
        unsigned char *nocc, *nrgb;
        for (j = 0; j < sh; j++) {
            for (i = 0; i < sw; i++) {
                if (!occ[j * sw + i]) {
                    continue;
                }
                if (i < minx) {
                    minx = i;
                }
                if (i > maxx) {
                    maxx = i;
                }
                if (j < miny) {
                    miny = j;
                }
                if (j > maxy) {
                    maxy = j;
                }
            }
        }
        cw = maxx - minx + 1;
        ch = maxy - miny + 1;
        nocc = calloc((size_t)cw * (size_t)ch, 1);
        nrgb = calloc((size_t)cw * (size_t)ch * 3, 1);
        if (!nocc || !nrgb) {
            free(nocc);
            free(nrgb);
            free(occ);
            free(rgb);
            fprintf(stderr, "out of memory\n");
            return NULL;
        }
        for (y2 = 0; y2 < ch; y2++) {
            memcpy(nocc + y2 * cw, occ + (miny + y2) * sw + minx, (size_t)cw);
            memcpy(nrgb + y2 * cw * 3, rgb + ((miny + y2) * sw + minx) * 3,
                   (size_t)cw * 3);
        }
        free(occ);
        free(rgb);
        occ = nocc;
        rgb = nrgb;
        sw = cw;
        sh = ch;
    }

    *gw = sw;
    *gh = sh;
    *out_rgb = rgb;
    return occ;
}

static Vert *build_mesh(const unsigned char *occ, const unsigned char *rgb,
                        int gw, int gh, int *out_n) {
    Vert *verts = NULL;
    int n = 0, cap = 0;
    int i, j;
    float sx, sy, hz;
    int long_axis = gw > gh ? gw : gh;
    int z_samples = 6;
    int edge_u = 2;

    sx = (2.0f * EXTENT) / (float)long_axis;
    sy = sx;
    hz = THICKNESS * 0.5f;

    for (j = 0; j < gh; j++) {
        for (i = 0; i < gw; i++) {
            float cx, cy;
            unsigned char r, g, b;
            if (!occupied(occ, gw, gh, i, j)) {
                continue;
            }
            cell_color(rgb, gw, i, j, &r, &g, &b);
            cx = ((float)i + 0.5f - (float)gw * 0.5f) * sx;
            cy = ((float)gh * 0.5f - (float)j - 0.5f) * sy;

            emit_face_samples(&verts, &n, &cap, cx, cy, -hz, 0.0f, 0.0f, -1.0f,
                              sx, 0.0f, 0.0f, 0.0f, sy, 0.0f, 2, 2, r, g, b);
            emit_face_samples(&verts, &n, &cap, cx, cy, hz, 0.0f, 0.0f, 1.0f, sx,
                              0.0f, 0.0f, 0.0f, sy, 0.0f, 2, 2, r, g, b);

            if (!occupied(occ, gw, gh, i + 1, j)) {
                emit_face_samples(&verts, &n, &cap, cx + sx * 0.5f, cy, 0.0f, 1.0f,
                                  0.0f, 0.0f, 0.0f, sy, 0.0f, 0.0f, 0.0f, THICKNESS,
                                  edge_u, z_samples, r, g, b);
            }
            if (!occupied(occ, gw, gh, i - 1, j)) {
                emit_face_samples(&verts, &n, &cap, cx - sx * 0.5f, cy, 0.0f, -1.0f,
                                  0.0f, 0.0f, 0.0f, sy, 0.0f, 0.0f, 0.0f, THICKNESS,
                                  edge_u, z_samples, r, g, b);
            }
            if (!occupied(occ, gw, gh, i, j - 1)) {
                emit_face_samples(&verts, &n, &cap, cx, cy + sy * 0.5f, 0.0f, 0.0f,
                                  1.0f, 0.0f, sx, 0.0f, 0.0f, 0.0f, 0.0f, THICKNESS,
                                  edge_u, z_samples, r, g, b);
            }
            if (!occupied(occ, gw, gh, i, j + 1)) {
                emit_face_samples(&verts, &n, &cap, cx, cy - sy * 0.5f, 0.0f, 0.0f,
                                  -1.0f, 0.0f, sx, 0.0f, 0.0f, 0.0f, 0.0f, THICKNESS,
                                  edge_u, z_samples, r, g, b);
            }
        }
    }

    *out_n = n;
    return verts;
}

static const char *default_path(void) {
    static const char *defaults[] = {"Kir-Dev-White.png", "Kir-25.png",
                                     "Simonyi.png", NULL};
    int i;
    for (i = 0; defaults[i]; i++) {
        if (access(defaults[i], R_OK) == 0) {
            return defaults[i];
        }
    }
    return NULL;
}

static int parse_degrees(const char *s, float *out) {
    char *end = NULL;
    float v;
    if (!s || !s[0]) {
        return -1;
    }
    errno = 0;
    v = strtof(s, &end);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

/* Image path is the optional positional argument. --tilt N sets the
 * back-tilt in degrees. The default tilt is 10. */
static int parse_args(int argc, char **argv, const char **path, float *tilt_deg) {
    int i;
    *path = NULL;
    *tilt_deg = 10.0f;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tilt") == 0) {
            if (i + 1 >= argc || parse_degrees(argv[i + 1], tilt_deg) != 0) {
                fprintf(stderr, "usage: --tilt N  (N is the tilt in degrees)\n");
                return -1;
            }
            i++;
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return -1;
        }
        if (*path) {
            fprintf(stderr, "extra argument '%s'\n", argv[i]);
            return -1;
        }
        *path = argv[i];
    }
    if (!*path) {
        *path = default_path();
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *path;
    unsigned char *occ;
    unsigned char *rgb = NULL;
    int gw = 0, gh = 0, nverts = 0, v;
    Vert *verts;
    int width = 80, height = 24;
    int buf_cap = 0;
    char *fb = NULL;
    unsigned char *col = NULL;
    float *zb = NULL;
    char *out = NULL;
    int out_cap = 0;
    /* Back-tilt around X (screen-horizontal). Spin is around Y
     * (vertical in this Y-up frame; Z-up in a left-handed Z-up frame). */
    float tilt_deg = 10.0f;
    float A;
    float B = 0.0f;
    int use_color;
    struct timespec frame_start, now, sleep_for;

    if (parse_args(argc, argv, &path, &tilt_deg) != 0) {
        return 1;
    }
    if (!path) {
        fprintf(stderr,
                "usage: %s [image.png] [--tilt degrees]\n"
                "  PNG with a transparent background (preferred).\n"
                "  Defaults to Kir-Dev-White.png if it is in the current directory.\n"
                "  --tilt sets the back-tilt in degrees (default 10).\n",
                argv[0]);
        return 1;
    }
    A = tilt_deg * PI / 180.0f;

    occ = load_silhouette(path, &gw, &gh, &rgb);
    if (!occ) {
        return 1;
    }
    verts = build_mesh(occ, rgb, gw, gh, &nverts);
    free(occ);
    free(rgb);
    if (!verts || nverts == 0) {
        fprintf(stderr, "no surface samples\n");
        free(verts);
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
#ifdef SIGWINCH
    signal(SIGWINCH, on_sigwinch);
#endif

    use_color = isatty(STDOUT_FILENO);
    if (use_color) {
        fputs("\x1b[?25l\x1b[2J\x1b[?7l", stdout);
        fflush(stdout);
        atexit(restore_terminal);
    }

    term_size(&width, &height);

    while (running) {
        int x, y, cells, out_len;
        float cosA, sinA, cosB, sinB, k1;
        int last_r = -1, last_g = -1, last_b = -1;

        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        if (resized) {
            resized = 0;
        }
        term_size(&width, &height);
        if (width < 20) {
            width = 20;
        }
        if (height < 10) {
            height = 10;
        }
        cells = width * height;

        if (cells > buf_cap) {
            int ncap = cells * 24 + 64;
            char *nfb = realloc(fb, (size_t)cells);
            unsigned char *ncol = realloc(col, (size_t)cells * 3);
            float *nzb;
            char *nout;
            if (!nfb || !ncol) {
                fprintf(stderr, "out of memory\n");
                return 1;
            }
            fb = nfb;
            col = ncol;
            nzb = realloc(zb, (size_t)cells * sizeof(float));
            if (!nzb) {
                fprintf(stderr, "out of memory\n");
                return 1;
            }
            zb = nzb;
            nout = realloc(out, (size_t)ncap);
            if (!nout) {
                fprintf(stderr, "out of memory\n");
                return 1;
            }
            out = nout;
            buf_cap = cells;
            out_cap = ncap;
        }

        memset(fb, ' ', (size_t)cells);
        memset(col, 0, (size_t)cells * 3);
        memset(zb, 0, (size_t)cells * sizeof(float));

        {
            float half_w = (float)width * 0.42f;
            float half_h = (float)height * 0.42f;
            float k1_x = half_w * K2 / EXTENT;
            float k1_y = (half_h * K2 / EXTENT) / 0.5f;
            k1 = k1_x < k1_y ? k1_x : k1_y;
        }

        cosA = cosf(A);
        sinA = sinf(A);
        cosB = cosf(B);
        sinB = sinf(B);

        for (v = 0; v < nverts; v++) {
            float px = verts[v].x, py = verts[v].y, pz = verts[v].z;
            float nx = verts[v].nx, ny = verts[v].ny, nz = verts[v].nz;
            float x1, y1, z1, ny1, nz1;
            float x2, y2, z2, ny2, nz2;
            float z3, ooz, L, nd, eased, shade_f, frac, lit;
            int xp, yp, idx, shade;

            /* Turntable spin around vertical Y, then tilt backwards around X
             * so the lean stays camera-relative. */
            x1 = px * cosB + pz * sinB;
            y1 = py;
            z1 = -px * sinB + pz * cosB;
            ny1 = ny;
            nz1 = -nx * sinB + nz * cosB;

            x2 = x1;
            y2 = y1 * cosA - z1 * sinA;
            z2 = y1 * sinA + z1 * cosA;
            ny2 = ny1 * cosA - nz1 * sinA;
            nz2 = ny1 * sinA + nz1 * cosA;

            z3 = z2 + K2;
            if (z3 <= 0.1f) {
                continue;
            }
            ooz = 1.0f / z3;

            xp = (int)((float)width * 0.5f + k1 * ooz * x2);
            yp = (int)((float)height * 0.5f - k1 * ooz * y2 * 0.5f);
            if (xp < 0 || xp >= width || yp < 0 || yp >= height) {
                continue;
            }

            L = ny2 - nz2;
            if (L <= 0.0f) {
                continue;
            }

            idx = xp + yp * width;
            if (ooz <= zb[idx]) {
                continue;
            }

            /* One eased light, so a face still fades smoothly as it turns.
             * Walls sit halfway between the cap and the darker rim band,
             * so the edge reads as a separate face without going black.
             * Bayer mixes the two glyphs around the fractional step. */
            nd = fminf(L * 0.70710678f, 1.0f);
            eased = powf(nd, 0.6f);
            shade_f = eased * (float)(PALETTE_LEN - 1);
            lit = 0.34f + 0.66f * eased;
            if (fabsf(nz) <= 0.5f) {
                shade_f = 0.5f * (shade_f + eased * 7.0f);
                lit = 0.5f * (lit + 0.06f + 0.24f * eased);
            }
            shade = (int)shade_f;
            frac = shade_f - (float)shade;
            if (frac > BAYER4[yp & 3][xp & 3]) {
                shade++;
            }
            if (shade >= PALETTE_LEN) {
                shade = PALETTE_LEN - 1;
            }
            zb[idx] = ooz;
            fb[idx] = PALETTE[shade];
            col[idx * 3 + 0] = (unsigned char)(verts[v].r * lit + 0.5f);
            col[idx * 3 + 1] = (unsigned char)(verts[v].g * lit + 0.5f);
            col[idx * 3 + 2] = (unsigned char)(verts[v].b * lit + 0.5f);
        }

        memcpy(out, "\x1b[H", 3);
        out_len = 3;

        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                int idx = x + y * width;
                char ch = fb[idx];
                if (use_color) {
                    int r = col[idx * 3 + 0];
                    int g = col[idx * 3 + 1];
                    int b = col[idx * 3 + 2];
                    if (r != last_r || g != last_g || b != last_b) {
                        int n = snprintf(out + out_len, (size_t)(out_cap - out_len),
                                         "\x1b[38;2;%d;%d;%dm", r, g, b);
                        if (n < 0 || out_len + n >= out_cap) {
                            break;
                        }
                        out_len += n;
                        last_r = r;
                        last_g = g;
                        last_b = b;
                    }
                }
                if (out_len + 2 >= out_cap) {
                    break;
                }
                out[out_len++] = ch;
            }
            if (y + 1 < height) {
                if (out_len + 1 >= out_cap) {
                    break;
                }
                out[out_len++] = '\n';
            }
        }

        fwrite(out, 1, (size_t)out_len, stdout);
        fflush(stdout);

        B += 0.035f;
        if (B > 2.0f * PI) {
            B -= 2.0f * PI;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        {
            long elapsed = (now.tv_sec - frame_start.tv_sec) * 1000000000L +
                           (now.tv_nsec - frame_start.tv_nsec);
            long remain = FRAME_NS - elapsed;
            if (remain > 0) {
                sleep_for.tv_sec = 0;
                sleep_for.tv_nsec = remain;
                nanosleep(&sleep_for, NULL);
            }
        }
    }

    free(verts);
    free(fb);
    free(col);
    free(zb);
    free(out);
    return 0;
}
