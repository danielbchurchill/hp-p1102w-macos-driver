/*
 * rastertozjs - CUPS filter for the HP LaserJet Professional P1102 / P1102w
 * on macOS, without Ghostscript.
 *
 *   PDF --cgpdftoraster (macOS)--> CUPS raster, 8-bit gray, 1200x600
 *       --rastertozjs--> 1-bit PBM pages (halftoned, full page)
 *       --foo2zjs -z2 -P -L0--> ZjStream --usb backend--> printer
 *
 * The whole ZjStream job is buffered in a temp file and only written to
 * stdout once every stage has succeeded.  A truncated job leaves the P1102
 * waiting forever for page data (blinking Ready light), so it is better to
 * send nothing than half a job.
 *
 * Copyright (c) 2026 Daniel B. Churchill
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See the LICENSE file.
 */

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef FOO2ZJS_PATH
#define FOO2ZJS_PATH "/Library/Printers/HP_P1102w_ZJS/bin/foo2zjs"
#endif

extern char **environ;

/* ------------------------------------------------------------------ */
/* Paper codes understood by the P1102 (foo2zjs -z2).                 */

static const struct { unsigned w, h; int code; const char *name; } papers[] = {
    { 612,  792,  1, "Letter"     },
    { 612, 1008,  5, "Legal"      },
    { 522,  756,  7, "Executive"  },
    { 595,  842,  9, "A4"         },
    { 420,  595, 11, "A5"         },
    { 516,  729, 13, "B5"         },
    { 297,  684, 20, "Env10"      },
    { 312,  624, 27, "EnvDL"      },
    { 459,  649, 28, "EnvC5"      },
    { 499,  709, 34, "EnvB5"      },
    { 279,  540, 37, "EnvMonarch" },
    { 283,  420, 43, "Postcard"   },
    { 297,  420, 70, "A6"         },
};

static int paper_code(double w, double h)
{
    for (size_t i = 0; i < sizeof(papers) / sizeof(papers[0]); i++)
        if (fabs(w - papers[i].w) <= 3.0 && fabs(h - papers[i].h) <= 3.0)
            return papers[i].code;
    return 256;                         /* custom: foo2zjs sends W x H */
}

/* ------------------------------------------------------------------ */
/* Clustered-dot halftone.  Tile is 16x8 device pixels = 1/75" square  */
/* at 1200x600; two dots per tile on a 45-degree lattice -> ~106 lpi,  */
/* 128 thresholds.  Clustered dots reproduce far more evenly on a      */
/* laser engine than error diffusion's isolated pixels.                */

#define HT_W 16
#define HT_H 8
static unsigned char thresh[HT_H][HT_W];

static void build_screen(void)
{
    struct { double v; int x, y; } cell[HT_W * HT_H], tmp;
    int n = 0;

    for (int y = 0; y < HT_H; y++)
        for (int x = 0; x < HT_W; x++) {
            double u = (x + 0.5) / HT_W, v = (y + 0.5) / HT_H;
            /* Spot function: maxima at (0,0) and (.5,.5) = 45-deg screen.
             * The two dots have identical values pixel-for-pixel, so a
             * stable sort grows them in lock-step. */
            cell[n].v = cos(2 * M_PI * u) * cos(2 * M_PI * v);
            cell[n].x = x;
            cell[n].y = y;
            n++;
        }
    /* Sort by spot value, highest first (simple insertion sort, n=128). */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && cell[j].v > cell[j - 1].v + 1e-12; j--) {
            tmp = cell[j]; cell[j] = cell[j - 1]; cell[j - 1] = tmp;
        }
    for (int r = 0; r < n; r++)
        thresh[cell[r].y][cell[r].x] = (unsigned char)((r + 0.5) * 255.0 / n);
}

/* ------------------------------------------------------------------ */

static int       job_fd = -1;           /* buffered ZjStream output      */
static pid_t     enc_pid = -1;          /* running foo2zjs               */
static FILE     *enc_in = NULL;         /* its stdin (PBM pages)          */
static int       enc_code = -1, enc_w = 0, enc_h = 0, enc_rx = 0, enc_ry = 0;
static volatile sig_atomic_t canceled = 0;

static void on_term(int sig) { (void)sig; canceled = 1; }

static int finish_encoder(void)
{
    int status = 0;

    if (enc_pid < 0)
        return 0;
    if (enc_in) {
        if (fclose(enc_in) != 0)
            fprintf(stderr, "ERROR: writing to foo2zjs failed: %s\n", strerror(errno));
        enc_in = NULL;
    }
    while (waitpid(enc_pid, &status, 0) < 0 && errno == EINTR)
        ;
    enc_pid = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "ERROR: foo2zjs failed (status 0x%x)\n", status);
        return -1;
    }
    return 0;
}

static int start_encoder(int code, int w, int h, int rx, int ry, int media,
                         int density, int econo, const char *title,
                         const char *user)
{
    char res[32], geom[32], ul[32], lr[32], p[16], m[16], t[8];
    const char *argv[32];
    int argc = 0, pipefd[2];
    posix_spawn_file_actions_t fa;

    snprintf(res, sizeof(res), "-r%dx%d", rx, ry);
    snprintf(geom, sizeof(geom), "-g%dx%d", w, h);
    snprintf(p, sizeof(p), "-p%d", code);
    snprintf(m, sizeof(m), "-m%d", media);
    snprintf(t, sizeof(t), "-T%d", density);
    /* Engine clip used by upstream for -z2: 0.16" on each edge. */
    snprintf(ul, sizeof(ul), "%dx%d", (int)lround(0.16 * rx), (int)lround(0.16 * ry));
    snprintf(lr, sizeof(lr), "%dx%d", (int)lround(0.16 * rx), (int)lround(0.16 * ry));

    argv[argc++] = "foo2zjs";
    argv[argc++] = "-z2";               /* HP LaserJet Pro P1102 family  */
    argv[argc++] = "-P";                /* no START_PLANE/END_PLANE      */
    argv[argc++] = "-L0";               /* no logical clipping in stream */
    argv[argc++] = res;
    argv[argc++] = geom;
    argv[argc++] = p;
    argv[argc++] = m;
    argv[argc++] = "-n1";               /* copies are produced by the RIP */
    argv[argc++] = "-d1";               /* no duplexer                   */
    argv[argc++] = "-s7";               /* auto source                   */
    argv[argc++] = t;
    argv[argc++] = "-u"; argv[argc++] = ul;
    argv[argc++] = "-l"; argv[argc++] = lr;
    if (econo)
        argv[argc++] = "-t";
    argv[argc++] = "-J"; argv[argc++] = title;
    argv[argc++] = "-U"; argv[argc++] = user;
    argv[argc] = NULL;

    if (pipe(pipefd) < 0) {
        fprintf(stderr, "ERROR: pipe: %s\n", strerror(errno));
        return -1;
    }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[0], 0);
    posix_spawn_file_actions_adddup2(&fa, job_fd, 1);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);

    const char *path = getenv("FOO2ZJS");   /* override for testing */
    if (!path || !*path)
        path = FOO2ZJS_PATH;
    int rc = posix_spawn(&enc_pid, path, &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[0]);
    if (rc != 0) {
        fprintf(stderr, "ERROR: cannot run %s: %s\n", path, strerror(rc));
        close(pipefd[1]);
        enc_pid = -1;
        return -1;
    }
    enc_in = fdopen(pipefd[1], "w");
    enc_code = code; enc_w = w; enc_h = h; enc_rx = rx; enc_ry = ry;

    fprintf(stderr, "DEBUG: %s", path);
    for (int i = 1; i < argc; i++)
        fprintf(stderr, " %s", argv[i]);
    fputc('\n', stderr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Options from the job (with PPD defaults).                           */

static const char *choice(ppd_file_t *ppd, int nopt, cups_option_t *opt,
                          const char *name, const char *dflt)
{
    const char *v = cupsGetOption(name, nopt, opt);
    if (v)
        return v;
    if (ppd) {
        ppd_choice_t *c = ppdFindMarkedChoice(ppd, name);
        if (c)
            return c->choice;
    }
    return dflt;
}

static int media_code(const char *s)
{
    static const struct { const char *n; int c; } t[] = {
        { "Plain", 1 }, { "Transparency", 2 }, { "Light", 258 },
        { "Cardstock", 261 }, { "Rough", 263 }, { "Labels", 265 },
        { "Envelope", 267 }, { "Letterhead", 513 }, { "Preprinted", 514 },
        { "Prepunched", 515 }, { "Recycled", 516 },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcasecmp(s, t[i].n))
            return t[i].c;
    return 1;
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    int in_fd = 0, nopt, pages = 0, failed = 0;
    cups_option_t *opt = NULL;
    ppd_file_t *ppd = NULL;
    cups_raster_t *ras;
    cups_page_header2_t hdr;
    unsigned char *line = NULL, *row = NULL;
    size_t line_cap = 0, row_cap = 0;
    char tmpname[1024];

    setbuf(stderr, NULL);
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n", argv[0]);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (argc == 7 && (in_fd = open(argv[6], O_RDONLY)) < 0) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n", argv[6], strerror(errno));
        return 1;
    }

    nopt = cupsParseOptions(argv[5], 0, &opt);
    if (getenv("PPD") && (ppd = ppdOpenFile(getenv("PPD"))) != NULL) {
        ppdMarkDefaults(ppd);
        cupsMarkOptions(ppd, nopt, opt);
    }
    int media   = media_code(choice(ppd, nopt, opt, "MediaType", "Plain"));
    int density = atoi(choice(ppd, nopt, opt, "PrintDensity", "3"));
    const char *eco = choice(ppd, nopt, opt, "EconoMode", "False");
    int econo   = !strcasecmp(eco, "True") || !strcasecmp(eco, "On");
    if (density < 1 || density > 5)
        density = 3;

    if ((job_fd = cupsTempFd(tmpname, sizeof(tmpname))) < 0) {
        fprintf(stderr, "ERROR: cannot create temp file: %s\n", strerror(errno));
        return 1;
    }
    unlink(tmpname);

    build_screen();
    ras = cupsRasterOpen(in_fd, CUPS_RASTER_READ);

    while (!canceled && !failed && cupsRasterReadHeader2(ras, &hdr)) {
        int rx = (int)hdr.HWResolution[0], ry = (int)hdr.HWResolution[1];
        int bpc = (int)hdr.cupsBitsPerColor;
        int cs = (int)hdr.cupsColorSpace;
        int black_is_high = (cs == CUPS_CSPACE_K);

        if ((bpc != 8 && bpc != 1) || hdr.cupsBitsPerPixel != hdr.cupsBitsPerColor ||
            (cs != CUPS_CSPACE_W && cs != CUPS_CSPACE_SW && cs != CUPS_CSPACE_K) ||
            rx <= 0 || ry <= 0) {
            fprintf(stderr, "ERROR: unsupported raster (bpc=%d bpp=%u cs=%d)\n",
                    bpc, hdr.cupsBitsPerPixel, cs);
            failed = 1;
            break;
        }

        /* Full page in device pixels, and where the imageable raster sits. */
        double pw = hdr.cupsPageSize[0] > 0 ? hdr.cupsPageSize[0] : hdr.PageSize[0];
        double ph = hdr.cupsPageSize[1] > 0 ? hdr.cupsPageSize[1] : hdr.PageSize[1];
        int fullW = (int)lround(pw * rx / 72.0);
        int fullH = (int)lround(ph * ry / 72.0);
        int offX = 0, offY = 0;
        if (hdr.cupsImagingBBox[2] > hdr.cupsImagingBBox[0]) {
            offX = (int)lround(hdr.cupsImagingBBox[0] * rx / 72.0);
            offY = (int)lround((ph - hdr.cupsImagingBBox[3]) * ry / 72.0);
        }
        if (fullW < (int)hdr.cupsWidth + offX)  fullW = (int)hdr.cupsWidth + offX;
        if (fullH < (int)hdr.cupsHeight + offY) fullH = (int)hdr.cupsHeight + offY;

        int code = paper_code(pw, ph);
        if (enc_pid < 0 || code != enc_code || fullW != enc_w || fullH != enc_h ||
            rx != enc_rx || ry != enc_ry) {
            if (finish_encoder() < 0 ||
                start_encoder(code, fullW, fullH, rx, ry, media, density, econo,
                              argv[3], argv[2]) < 0) {
                failed = 1;
                break;
            }
        }

        pages++;
        fprintf(stderr, "INFO: Rendering page %d\n", pages);

        size_t obpl = ((size_t)fullW + 7) / 8;
        if (row_cap < obpl && !(row = realloc(row, row_cap = obpl))) {
            fprintf(stderr, "ERROR: out of memory\n");
            failed = 1;
            break;
        }
        if (line_cap < hdr.cupsBytesPerLine &&
            !(line = realloc(line, line_cap = hdr.cupsBytesPerLine))) {
            fprintf(stderr, "ERROR: out of memory\n");
            failed = 1;
            break;
        }

        fprintf(enc_in, "P4\n%d %d\n", fullW, fullH);
        for (int y = 0; y < fullH && !failed; y++) {
            memset(row, 0, obpl);
            int ry_ = y - offY;
            if (ry_ >= 0 && ry_ < (int)hdr.cupsHeight) {
                if (cupsRasterReadPixels(ras, line, hdr.cupsBytesPerLine) != hdr.cupsBytesPerLine) {
                    fprintf(stderr, "ERROR: short raster read on page %d\n", pages);
                    failed = 1;
                    break;
                }
                const unsigned char *tr = thresh[y % HT_H];
                for (int x = 0; x < (int)hdr.cupsWidth; x++) {
                    int on, gx = x + offX;
                    if (bpc == 8) {
                        int d = black_is_high ? line[x] : 255 - line[x];
                        on = d > tr[gx % HT_W];
                    } else {
                        int bit = (line[x >> 3] >> (7 - (x & 7))) & 1;
                        on = black_is_high ? bit : !bit;
                    }
                    if (on)
                        row[gx >> 3] |= (unsigned char)(0x80 >> (gx & 7));
                }
            }
            if (fwrite(row, 1, obpl, enc_in) != obpl) {
                fprintf(stderr, "ERROR: foo2zjs stopped reading: %s\n", strerror(errno));
                failed = 1;
            }
        }
        if (!failed)
            fprintf(stderr, "PAGE: %d %d\n", pages, 1);
    }

    cupsRasterClose(ras);
    if (finish_encoder() < 0)
        failed = 1;
    if (canceled) {
        fprintf(stderr, "INFO: Job canceled\n");
        return 0;
    }
    if (failed) {
        fprintf(stderr, "ERROR: Job not sent to the printer (rendering failed)\n");
        return 1;
    }
    if (pages == 0) {
        fprintf(stderr, "INFO: No pages to print\n");
        return 0;
    }

    /* Everything succeeded: hand the complete job to the backend. */
    off_t total = lseek(job_fd, 0, SEEK_END);
    lseek(job_fd, 0, SEEK_SET);
    char buf[65536];
    ssize_t n;
    while ((n = read(job_fd, buf, sizeof(buf))) > 0) {
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(1, buf + done, (size_t)(n - done));
            if (w < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "ERROR: writing to backend: %s\n", strerror(errno));
                return 1;
            }
            done += w;
        }
    }
    fprintf(stderr, "DEBUG: rastertozjs sent %d page(s), %lld bytes of ZjStream\n",
            pages, (long long)total);
    close(job_fd);
    return 0;
}
