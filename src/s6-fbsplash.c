#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <poll.h>
#include <pty.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define S6_FBSPLASH_VERSION "1.0.0"
#define PSF2_MAGIC 0x864ab572u
#define MAX_ROWS 128
#define MAX_COLS 320

typedef struct {
    uint32_t cp;
    uint32_t fg;
} cell_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t headersize;
    uint32_t flags;
    uint32_t length;
    uint32_t charsize;
    uint32_t height;
    uint32_t width;
} psf2_hdr_t;

typedef struct {
    const char *bg_path;
    const char *font_path;
    const char *fb_path;
    const char *tty_path;
    const char *badge;
    const char *title;
    const char *render_ppm;
    uint32_t accent;
    uint32_t bg_w;
    uint32_t bg_h;
    uint32_t render_w;
    uint32_t render_h;
    uint8_t panel_alpha;
    int margin;
    int forward_input;
} config_t;

static const uint32_t PALETTE[16] = {
    0xFF121212u, /* 0 black */
    0xFFE60000u, /* 1 red */
    0xFF4EC970u, /* 2 green */
    0xFFE5B567u, /* 3 yellow */
    0xFF6E82B4u, /* 4 blue */
    0xFFB05AA8u, /* 5 magenta */
    0xFF7AA2C8u, /* 6 cyan */
    0xFFE0E0E0u, /* 7 white */
    0xFF5A5A64u, /* 8 bright black */
    0xFFFF3333u, /* 9 bright red */
    0xFF6EE08Cu, /* 10 bright green */
    0xFFF2CD7Fu, /* 11 bright yellow */
    0xFF8CA0D2u, /* 12 bright blue */
    0xFFD078C4u, /* 13 bright magenta */
    0xFFA0C4E0u, /* 14 bright cyan */
    0xFFFFFFFFu  /* 15 bright white */
};

static uint16_t umap_cp[65536];
static cell_t grid[MAX_ROWS][MAX_COLS];
static int cur_row = 0, cur_col = 0;
static int grid_rows = 40, grid_cols = 120;
static uint32_t cur_fg = 0xFFE0E0E0u;

static uint32_t *bg_pixels = NULL;
static int bg_pixels_mapped = 0;
static size_t bg_pixels_sz = 0;
static uint32_t *bg_panel = NULL;
static uint32_t *backbuf = NULL;

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] -- <command...>\n"
        "\n"
        "Zero-dependency Linux framebuffer bootsplash & PTY console wrapper for s6/init.\n"
        "\n"
        "Options:\n"
        "  --bg <path>         Background image (.ff Farbfeld or raw .bgra)\n"
        "  --font <path>       PSF2 console font (.psfu)\n"
        "  --fb <path>         Framebuffer device (default: /dev/fb0)\n"
        "  --tty <path>        TTY device for KD_GRAPHICS/input (default: /dev/tty1)\n"
        "  --bg-size <WxH>     Raw .bgra dimensions (default: 1920x1200; ignored for .ff)\n"
        "  --title <text>      Header title (default: \"Gentoo Linux // s6-svscan\")\n"
        "  --badge <text>      Header badge (default: \"s6\")\n"
        "  --accent <RRGGBB>   Accent bar hex color (default: e60000)\n"
        "  --alpha <0-255>     Panel darkness opacity (default: 165)\n"
        "  --margin <px>       Screen margin in pixels (default: 64)\n"
        "  --no-input          Do not forward TTY keyboard input to child PTY\n"
        "  --render-ppm <out>  Render output to a PPM file instead of /dev/fb0 (preview/CI)\n"
        "  --render-size <WxH> Canvas size for --render-ppm (default: 1280x720)\n"
        "  -v, --version       Show version and exit\n"
        "  -h, --help          Show this help and exit\n",
        prog);
}

static int parse_hex_rgb(const char *s, uint32_t *out) {
    if (*s == '#') s++;
    if (strlen(s) != 6) return -1;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (!end || *end != '\0') return -1;
    *out = 0xFF000000u | (uint32_t)v;
    return 0;
}

static int parse_wxh(const char *s, uint32_t *w, uint32_t *h) {
    unsigned long vw = 0, vh = 0;
    char *end = NULL;
    vw = strtoul(s, &end, 10);
    if (!end || (*end != 'x' && *end != 'X') || vw < 64 || vw > 16384) return -1;
    vh = strtoul(end + 1, &end, 10);
    if (!end || *end != '\0' || vh < 64 || vh > 16384) return -1;
    *w = (uint32_t)vw;
    *h = (uint32_t)vh;
    return 0;
}

static inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static int load_background(const char *path, uint32_t *w_out, uint32_t *h_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 16) {
        close(fd);
        return -1;
    }

    size_t fsz = (size_t)st.st_size;
    uint8_t *raw = mmap(NULL, fsz, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (raw == MAP_FAILED) return -1;

    if (memcmp(raw, "farbfeld", 8) == 0) {
        uint32_t fw = read_be32(raw + 8);
        uint32_t fh = read_be32(raw + 12);
        size_t need = 16 + (size_t)fw * fh * 8;
        if (fw == 0 || fh == 0 || fw > 16384 || fh > 16384 || fsz < need) {
            munmap(raw, fsz);
            return -1;
        }
        uint32_t *pix = malloc((size_t)fw * fh * sizeof(uint32_t));
        if (!pix) {
            munmap(raw, fsz);
            return -1;
        }
        const uint8_t *src = raw + 16;
        size_t total = (size_t)fw * fh;
        for (size_t i = 0; i < total; i++) {
            uint8_t r = src[i * 8 + 0];
            uint8_t g = src[i * 8 + 2];
            uint8_t b = src[i * 8 + 4];
            pix[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
        munmap(raw, fsz);
        bg_pixels = pix;
        bg_pixels_mapped = 0;
        bg_pixels_sz = total * sizeof(uint32_t);
        *w_out = fw;
        *h_out = fh;
        return 0;
    }

    size_t expected = (size_t)(*w_out) * (*h_out) * 4;
    if (fsz < expected) {
        munmap(raw, fsz);
        return -1;
    }
    bg_pixels = (uint32_t *)(void *)raw;
    bg_pixels_mapped = 1;
    bg_pixels_sz = fsz;
    return 0;
}

static void free_background(void) {
    if (!bg_pixels) return;
    if (bg_pixels_mapped)
        munmap(bg_pixels, bg_pixels_sz);
    else
        free(bg_pixels);
    bg_pixels = NULL;
}

static void init_umap(const uint8_t *font_buf, size_t font_sz, const psf2_hdr_t *hdr) {
    for (int i = 0; i < 65536; i++)
        umap_cp[i] = (i < (int)hdr->length) ? (uint16_t)i : (uint16_t)'?';

    if (!(hdr->flags & 1))
        return;

    size_t off = hdr->headersize + (size_t)hdr->length * hdr->charsize;
    uint32_t glyph = 0;
    while (off < font_sz && glyph < hdr->length) {
        uint8_t b = font_buf[off++];
        if (b == 0xFF) {
            glyph++;
            continue;
        }
        if (b == 0xFE) {
            while (off < font_sz && font_buf[off] != 0xFF)
                off++;
            continue;
        }
        uint32_t cp = 0;
        if (b < 0x80) {
            cp = b;
        } else if ((b & 0xE0) == 0xC0 && off < font_sz) {
            cp = ((uint32_t)(b & 0x1F) << 6) | (font_buf[off++] & 0x3F);
        } else if ((b & 0xF0) == 0xE0 && off + 1 < font_sz) {
            cp = ((uint32_t)(b & 0x0F) << 12) |
                 ((uint32_t)(font_buf[off] & 0x3F) << 6) |
                 (font_buf[off + 1] & 0x3F);
            off += 2;
        } else if ((b & 0xF8) == 0xF0 && off + 2 < font_sz) {
            off += 3;
            continue;
        }
        if (cp < 65536 && umap_cp[cp] == (uint16_t)'?')
            umap_cp[cp] = (uint16_t)glyph;
        if (cp < 256)
            umap_cp[cp] = (uint16_t)glyph;
    }
}

static void scroll_up(void) {
    for (int r = 1; r < grid_rows; r++)
        memcpy(grid[r - 1], grid[r], sizeof(grid[0]));
    for (int c = 0; c < grid_cols; c++) {
        grid[grid_rows - 1][c].cp = ' ';
        grid[grid_rows - 1][c].fg = PALETTE[7];
    }
    if (cur_row > 0)
        cur_row--;
}

static void put_cp(uint32_t cp) {
    if (cp == '\r') {
        cur_col = 0;
        return;
    }
    if (cp == '\n') {
        cur_col = 0;
        if (++cur_row >= grid_rows) {
            scroll_up();
            cur_row = grid_rows - 1;
        }
        return;
    }
    if (cp == '\t') {
        int next = (cur_col + 8) & ~7;
        while (cur_col < next && cur_col < grid_cols)
            put_cp(' ');
        return;
    }
    if (cp < 32)
        return;
    if (cur_col >= grid_cols) {
        cur_col = 0;
        if (++cur_row >= grid_rows) {
            scroll_up();
            cur_row = grid_rows - 1;
        }
    }
    grid[cur_row][cur_col].cp = cp;
    grid[cur_row][cur_col].fg = cur_fg;
    cur_col++;
}

static void handle_sgr(const char *params) {
    if (!*params) {
        cur_fg = PALETTE[7];
        return;
    }
    int bold = 0;
    const char *p = params;
    while (*p) {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) {
            p++;
            continue;
        }
        if (v == 0) {
            bold = 0;
            cur_fg = PALETTE[7];
        } else if (v == 1) {
            bold = 1;
        } else if (v >= 30 && v <= 37) {
            cur_fg = PALETTE[(v - 30) + (bold ? 8 : 0)];
        } else if (v >= 90 && v <= 97) {
            cur_fg = PALETTE[(v - 90) + 8];
        } else if (v == 39) {
            cur_fg = PALETTE[7];
        }
        p = (*end == ';') ? end + 1 : end;
    }
}

static void feed_bytes(const uint8_t *buf, ssize_t len) {
    static int esc_state = 0; /* 0=normal, 1=ESC, 2=CSI, 3=OSC */
    static char csi_buf[64];
    static int csi_len = 0;
    static uint32_t utf8_cp = 0;
    static int utf8_rem = 0;

    for (ssize_t i = 0; i < len; i++) {
        uint8_t b = buf[i];
        if (esc_state == 1) {
            if (b == '[') {
                esc_state = 2;
                csi_len = 0;
            } else if (b == ']') {
                esc_state = 3;
            } else {
                esc_state = 0;
            }
            continue;
        }
        if (esc_state == 2) {
            if (b >= 0x40 && b <= 0x7E) {
                csi_buf[csi_len] = '\0';
                if (b == 'm') {
                    handle_sgr(csi_buf);
                } else if (b == 'J' && strcmp(csi_buf, "2") == 0) {
                    for (int r = 0; r < grid_rows; r++)
                        for (int c = 0; c < grid_cols; c++) {
                            grid[r][c].cp = ' ';
                            grid[r][c].fg = PALETTE[7];
                        }
                    cur_row = 0;
                    cur_col = 0;
                } else if (b == 'K') {
                    for (int c = cur_col; c < grid_cols; c++) {
                        grid[cur_row][c].cp = ' ';
                        grid[cur_row][c].fg = PALETTE[7];
                    }
                } else if (b == 'H') {
                    cur_row = 0;
                    cur_col = 0;
                }
                esc_state = 0;
            } else if (csi_len < (int)sizeof(csi_buf) - 1) {
                csi_buf[csi_len++] = (char)b;
            }
            continue;
        }
        if (esc_state == 3) {
            if (b == '\007' || b == '\n')
                esc_state = 0;
            continue;
        }
        if (b == 0x1B) {
            esc_state = 1;
            continue;
        }
        if (utf8_rem > 0) {
            if ((b & 0xC0) == 0x80) {
                utf8_cp = (utf8_cp << 6) | (b & 0x3F);
                if (--utf8_rem == 0)
                    put_cp(utf8_cp);
                continue;
            }
            utf8_rem = 0;
        }
        if (b < 0x80) {
            put_cp(b);
        } else if ((b & 0xE0) == 0xC0) {
            utf8_cp = b & 0x1F;
            utf8_rem = 1;
        } else if ((b & 0xF0) == 0xE0) {
            utf8_cp = b & 0x0F;
            utf8_rem = 2;
        } else if ((b & 0xF8) == 0xF0) {
            utf8_cp = b & 0x07;
            utf8_rem = 3;
        }
    }
}

static inline uint32_t blend_dark(uint32_t bg, uint32_t alpha_255) {
    uint32_t inv = 255 - alpha_255;
    uint32_t r = (((bg >> 16) & 0xFF) * inv + 14 * alpha_255) >> 8;
    uint32_t g = (((bg >> 8) & 0xFF) * inv + 14 * alpha_255) >> 8;
    uint32_t b = ((bg & 0xFF) * inv + 18 * alpha_255) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void init_bg_panel(uint32_t stride, uint32_t xres, uint32_t yres,
                          const config_t *cfg) {
    bg_panel = malloc((size_t)stride * yres);
    backbuf = malloc((size_t)stride * yres);
    if (!bg_panel || !backbuf)
        return;

    int pad = (cfg->margin >= 24) ? 24 : 0;
    uint32_t box_x0 = (uint32_t)(cfg->margin - pad);
    uint32_t box_y0 = (uint32_t)(cfg->margin - pad);
    uint32_t box_x1 = xres - box_x0;
    uint32_t box_y1 = yres - box_y0;

    for (uint32_t y = 0; y < yres; y++) {
        uint32_t *dst = (uint32_t *)(void *)((uint8_t *)bg_panel + (size_t)y * stride);
        uint32_t by = (y * cfg->bg_h) / yres;
        const uint32_t *src_row = bg_pixels + (size_t)by * cfg->bg_w;
        int in_box_y = (y >= box_y0 && y < box_y1);
        int is_accent = (y >= box_y0 && y < box_y0 + 3);
        for (uint32_t x = 0; x < xres; x++) {
            uint32_t bx = (x * cfg->bg_w) / xres;
            uint32_t px = src_row[bx];
            if (in_box_y && x >= box_x0 && x < box_x1) {
                px = is_accent ? cfg->accent : blend_dark(px, cfg->panel_alpha);
            }
            dst[x] = px;
        }
    }
}

static void render_frame(uint8_t *fb, uint32_t stride, uint32_t xres, uint32_t yres,
                         const uint8_t *glyphs, const psf2_hdr_t *hdr,
                         const config_t *cfg) {
    if (!bg_panel || !backbuf)
        init_bg_panel(stride, xres, yres, cfg);
    if (!bg_panel || !backbuf)
        return;

    memcpy(backbuf, bg_panel, (size_t)stride * yres);

    uint32_t row_bytes = (hdr->width + 7) / 8;
    for (int r = 0; r < grid_rows; r++) {
        uint32_t py0 = (uint32_t)cfg->margin + (uint32_t)r * hdr->height;
        if (py0 + hdr->height > yres)
            break;
        for (int c = 0; c < grid_cols; c++) {
            uint32_t cp = grid[r][c].cp;
            if (cp <= ' ')
                continue;
            uint16_t gi = (cp < 65536) ? umap_cp[cp] : umap_cp['?'];
            const uint8_t *gdata = glyphs + (size_t)gi * hdr->charsize;
            uint32_t px0 = (uint32_t)cfg->margin + (uint32_t)c * hdr->width;
            if (px0 + hdr->width > xres)
                break;
            uint32_t fg = grid[r][c].fg;
            for (uint32_t gy = 0; gy < hdr->height; gy++) {
                uint32_t *dst = (uint32_t *)(void *)((uint8_t *)backbuf + (size_t)(py0 + gy) * stride) + px0;
                const uint8_t *grow = gdata + (size_t)gy * row_bytes;
                for (uint32_t gx = 0; gx < hdr->width; gx++) {
                    if (grow[gx >> 3] & (0x80u >> (gx & 7)))
                        dst[gx] = fg;
                }
            }
        }
    }

    if (fb)
        memcpy(fb, backbuf, (size_t)stride * yres);
}

static int write_ppm(const char *path, uint32_t xres, uint32_t yres) {
    if (!backbuf) return -1;
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    fprintf(fp, "P6\n%u %u\n255\n", xres, yres);
    uint8_t *row = malloc((size_t)xres * 3);
    if (!row) {
        fclose(fp);
        return -1;
    }
    for (uint32_t y = 0; y < yres; y++) {
        const uint32_t *src = backbuf + (size_t)y * xres;
        for (uint32_t x = 0; x < xres; x++) {
            uint32_t px = src[x];
            row[x * 3 + 0] = (uint8_t)((px >> 16) & 0xFF);
            row[x * 3 + 1] = (uint8_t)((px >> 8) & 0xFF);
            row[x * 3 + 2] = (uint8_t)(px & 0xFF);
        }
        fwrite(row, 1, (size_t)xres * 3, fp);
    }
    free(row);
    fclose(fp);
    return 0;
}

static void init_grid(uint32_t xres, uint32_t yres, const psf2_hdr_t *hdr, const config_t *cfg) {
    grid_cols = (int)((xres - (uint32_t)cfg->margin * 2) / hdr->width);
    grid_rows = (int)((yres - (uint32_t)cfg->margin * 2) / hdr->height);
    if (grid_cols < 1) grid_cols = 1;
    if (grid_rows < 1) grid_rows = 1;
    if (grid_cols > MAX_COLS) grid_cols = MAX_COLS;
    if (grid_rows > MAX_ROWS) grid_rows = MAX_ROWS;
    for (int r = 0; r < grid_rows; r++) {
        for (int c = 0; c < grid_cols; c++) {
            grid[r][c].cp = ' ';
            grid[r][c].fg = PALETTE[7];
        }
    }

    char banner[512];
    snprintf(banner, sizeof(banner),
             "\033[1;31m  [%s]\033[0m \033[90m//\033[0m \033[1;37m%s\033[0m\n"
             "\033[90m  ----------------------------------------\033[0m\n",
             cfg->badge, cfg->title);
    feed_bytes((const uint8_t *)banner, (ssize_t)strlen(banner));
}

static int run_pty_loop(char **child_argv, uint8_t *fb, uint32_t stride,
                        uint32_t xres, uint32_t yres, const uint8_t *glyphs,
                        const psf2_hdr_t *hdr, const config_t *cfg, int tty_fd) {
    int master_fd = -1, slave_fd = -1, use_pty = 1;
    struct winsize ws = { .ws_row = (unsigned short)grid_rows, .ws_col = (unsigned short)grid_cols };
    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) < 0) {
        int pfd_arr[2];
        if (pipe(pfd_arr) < 0)
            return -1;
        master_fd = pfd_arr[0];
        slave_fd = pfd_arr[1];
        use_pty = 0;
    }

    struct termios saved_tio;
    int restored_tio = 0;
    if (cfg->forward_input && tty_fd >= 0 && tcgetattr(tty_fd, &saved_tio) == 0) {
        struct termios raw = saved_tio;
        cfmakeraw(&raw);
        if (tcsetattr(tty_fd, TCSANOW, &raw) == 0)
            restored_tio = 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (restored_tio) tcsetattr(tty_fd, TCSANOW, &saved_tio);
        close(master_fd);
        close(slave_fd);
        return -1;
    }
    if (pid == 0) {
        close(master_fd);
        if (use_pty) setsid();
        if (use_pty) dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > 2) close(slave_fd);
        execvp(child_argv[0], child_argv);
        _exit(127);
    }
    close(slave_fd);

    render_frame(fb, stride, xres, yres, glyphs, hdr, cfg);

    struct pollfd pfds[2];
    pfds[0].fd = master_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = (cfg->forward_input && tty_fd >= 0) ? tty_fd : -1;
    pfds[1].events = POLLIN;

    uint8_t rbuf[4096];
    for (;;) {
        nfds_t nfds = (pfds[1].fd >= 0) ? 2 : 1;
        int pr = poll(pfds, nfds, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        if (pfds[0].revents & POLLIN) {
            ssize_t n = read(master_fd, rbuf, sizeof(rbuf));
            if (n <= 0) break;
            feed_bytes(rbuf, n);
            render_frame(fb, stride, xres, yres, glyphs, hdr, cfg);
        } else if (pfds[0].revents & (POLLHUP | POLLERR)) {
            break;
        }

        if (nfds == 2 && (pfds[1].revents & POLLIN)) {
            ssize_t n = read(tty_fd, rbuf, sizeof(rbuf));
            if (n > 0) {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(master_fd, rbuf + off, (size_t)(n - off));
                    if (w <= 0) break;
                    off += w;
                }
            }
        }
    }

    int status = 0;
    waitpid(pid, &status, 0);
    close(master_fd);
    if (restored_tio) tcsetattr(tty_fd, TCSANOW, &saved_tio);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int main(int argc, char **argv) {
    config_t cfg = {
        .bg_path = NULL,
        .font_path = NULL,
        .fb_path = "/dev/fb0",
        .tty_path = "/dev/tty1",
        .badge = "s6",
        .title = "Gentoo Linux // s6-svscan",
        .render_ppm = NULL,
        .accent = 0xFFE60000u,
        .bg_w = 1920,
        .bg_h = 1200,
        .render_w = 1280,
        .render_h = 720,
        .panel_alpha = 165,
        .margin = 64,
        .forward_input = 1
    };

    int sep = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            sep = i;
            break;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("s6-fbsplash %s\n", S6_FBSPLASH_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--no-input") == 0) {
            cfg.forward_input = 0;
        } else if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            return 2;
        } else if (strcmp(argv[i], "--bg") == 0) {
            cfg.bg_path = argv[++i];
        } else if (strcmp(argv[i], "--font") == 0) {
            cfg.font_path = argv[++i];
        } else if (strcmp(argv[i], "--fb") == 0) {
            cfg.fb_path = argv[++i];
        } else if (strcmp(argv[i], "--tty") == 0) {
            cfg.tty_path = argv[++i];
        } else if (strcmp(argv[i], "--title") == 0) {
            cfg.title = argv[++i];
        } else if (strcmp(argv[i], "--badge") == 0) {
            cfg.badge = argv[++i];
        } else if (strcmp(argv[i], "--render-ppm") == 0) {
            cfg.render_ppm = argv[++i];
        } else if (strcmp(argv[i], "--accent") == 0) {
            if (parse_hex_rgb(argv[++i], &cfg.accent) < 0) {
                fprintf(stderr, "Invalid --accent hex color\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--bg-size") == 0) {
            if (parse_wxh(argv[++i], &cfg.bg_w, &cfg.bg_h) < 0) {
                fprintf(stderr, "Invalid --bg-size (expected WxH)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--render-size") == 0) {
            if (parse_wxh(argv[++i], &cfg.render_w, &cfg.render_h) < 0) {
                fprintf(stderr, "Invalid --render-size (expected WxH)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--alpha") == 0) {
            long a = strtol(argv[++i], NULL, 10);
            if (a < 0 || a > 255) {
                fprintf(stderr, "Invalid --alpha (expected 0-255)\n");
                return 2;
            }
            cfg.panel_alpha = (uint8_t)a;
        } else if (strcmp(argv[i], "--margin") == 0) {
            long m = strtol(argv[++i], NULL, 10);
            if (m < 0 || m > 512) {
                fprintf(stderr, "Invalid --margin\n");
                return 2;
            }
            cfg.margin = (int)m;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (sep < 0 || sep + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
    }

    char **child_argv = &argv[sep + 1];

    if (!cfg.bg_path || !cfg.font_path)
        execvp(child_argv[0], child_argv);

    if (load_background(cfg.bg_path, &cfg.bg_w, &cfg.bg_h) < 0)
        execvp(child_argv[0], child_argv);

    int fn_fd = open(cfg.font_path, O_RDONLY);
    struct stat fn_st;
    uint8_t *fn_buf = MAP_FAILED;
    if (fn_fd >= 0 && fstat(fn_fd, &fn_st) == 0 && fn_st.st_size > (off_t)sizeof(psf2_hdr_t))
        fn_buf = mmap(NULL, (size_t)fn_st.st_size, PROT_READ, MAP_SHARED, fn_fd, 0);
    if (fn_fd >= 0) close(fn_fd);
    if (fn_buf == MAP_FAILED) {
        free_background();
        execvp(child_argv[0], child_argv);
    }

    const psf2_hdr_t *hdr = (const psf2_hdr_t *)(const void *)fn_buf;
    if (hdr->magic != PSF2_MAGIC || hdr->width == 0 || hdr->height == 0 ||
        (size_t)hdr->headersize + (size_t)hdr->length * hdr->charsize > (size_t)fn_st.st_size) {
        munmap(fn_buf, (size_t)fn_st.st_size);
        free_background();
        execvp(child_argv[0], child_argv);
    }

    init_umap(fn_buf, (size_t)fn_st.st_size, hdr);
    const uint8_t *glyphs = fn_buf + hdr->headersize;

    if (cfg.render_ppm) {
        init_grid(cfg.render_w, cfg.render_h, hdr, &cfg);
        int rc = run_pty_loop(child_argv, NULL, cfg.render_w * 4,
                              cfg.render_w, cfg.render_h, glyphs, hdr, &cfg, -1);
        if (rc < 0) rc = 1;
        write_ppm(cfg.render_ppm, cfg.render_w, cfg.render_h);
        free(bg_panel);
        free(backbuf);
        munmap(fn_buf, (size_t)fn_st.st_size);
        free_background();
        return rc;
    }

    int fb_fd = open(cfg.fb_path, O_RDWR);
    if (fb_fd < 0) {
        munmap(fn_buf, (size_t)fn_st.st_size);
        free_background();
        execvp(child_argv[0], child_argv);
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0 ||
        vinfo.bits_per_pixel != 32 ||
        vinfo.xres < 640 || vinfo.yres < 480) {
        close(fb_fd);
        munmap(fn_buf, (size_t)fn_st.st_size);
        free_background();
        execvp(child_argv[0], child_argv);
    }

    size_t fb_sz = (size_t)finfo.line_length * vinfo.yres;
    uint8_t *fb = mmap(NULL, fb_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb == MAP_FAILED) {
        close(fb_fd);
        munmap(fn_buf, (size_t)fn_st.st_size);
        free_background();
        execvp(child_argv[0], child_argv);
    }

    if ((uint32_t)(cfg.margin * 2) >= vinfo.xres || (uint32_t)(cfg.margin * 2) >= vinfo.yres)
        cfg.margin = 16;

    init_grid(vinfo.xres, vinfo.yres, hdr, &cfg);

    int tty_fd = open(cfg.tty_path, O_RDWR);
    if (tty_fd >= 0)
        ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);

    int rc = run_pty_loop(child_argv, fb, finfo.line_length,
                          vinfo.xres, vinfo.yres, glyphs, hdr, &cfg, tty_fd);

    if (tty_fd >= 0) {
        ioctl(tty_fd, KDSETMODE, KD_TEXT);
        close(tty_fd);
    }
    free(bg_panel);
    free(backbuf);
    munmap(fn_buf, (size_t)fn_st.st_size);
    free_background();
    munmap(fb, fb_sz);
    close(fb_fd);

    if (rc < 0)
        execvp(child_argv[0], child_argv);

    return rc;
}
