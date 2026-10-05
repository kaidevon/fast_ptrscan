/*
 * Copyright (c) 2026 kaidev <kaidevonmail@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ptr_index.h"
#include "fs_ptrscan.h"
#include "callback.h"
#include "pc_list.h"
#include "format/idx.h"
#include "format/pcf.h"
#include "format/txt.h"
#include "vma/vm_area.h"
#include "vma/vma_select.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#include <limits.h>
#include <pthread.h>
#include <sys/uio.h>
#include <sys/types.h>

#define VERSION "0.1.0"

#define BLUE       "\x1b[34m"
#define BLUE_BOLD  "\x1b[1m\x1b[34m"
#define BOLD       "\x1b[1m"
#define RED        "\x1b[31m"
#define GREEN      "\x1b[32m"
#define YELLOW     "\x1b[33m"
#define RED_BOLD   "\x1b[1m\x1b[31m"
#define RESET      "\x1b[0m"

static int use_color = 1;

#define C_BLUE       (use_color ? BLUE     : "")
#define C_BLUE_BOLD  (use_color ? BLUE_BOLD: "")
#define C_BOLD       (use_color ? BOLD     : "")
#define C_RED        (use_color ? RED      : "")
#define C_GREEN      (use_color ? GREEN    : "")
#define C_YELLOW     (use_color ? YELLOW   : "")
#define C_RED_BOLD   (use_color ? RED_BOLD : "")
#define C_RESET      (use_color ? RESET    : "")

#define OPT_ARR_LEN 1024
#define TAIL_FLAT_MAX 512
#define TAIL_LAYERS_MAX 64

static int detect_color(void)
{
    if (getenv("NO_COLOR")) return 0;
    if (getenv("CLICOLOR_FORCE")) return 1;
    const char *term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0) return 0;
    return isatty(STDOUT_FILENO);
}

static void fill_max_off(uint64_t *arr, uint64_t off)
{
    for (int i = 0; i < OPT_ARR_LEN; i++)
        arr[i] = off;
}

static int parse_int_arg(const char *s, int *out)
{
    if (!s || !*s) return -1;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE) return -1;
    if (v < INT_MIN || v > INT_MAX) return -1;
    *out = (int)v;
    return 0;
}

static int parse_u64_arg(const char *s, uint64_t *out)
{
    if (!s || !*s) return -1;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE) return -1;
    *out = (uint64_t)v;
    return 0;
}

/* Parse comma-separated int list; last value extends to fill array. */
static int parse_k_list(const char *s, int *arr)
{
    for (int i = 0; i < OPT_ARR_LEN; i++) arr[i] = 0;
    if (!s || !*s) return 0;

    int i = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (i >= OPT_ARR_LEN) return -1;

        char *end = NULL;
        errno = 0;
        long v = strtol(p, &end, 0);
        if (end == p || errno == ERANGE || v < 0 || v > INT_MAX)
            return -1;
        arr[i++] = (int)v;

        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') { p++; continue; }
        if (*p == '\0') break;
        return -1;
    }

    if (i > 0 && i < OPT_ARR_LEN) {
        int last = arr[i - 1];
        while (i < OPT_ARR_LEN) arr[i++] = last;
    }
    return 0;
}

/* Parse one -t layer; each occurrence adds a layer, ordered near to far. */
static int parse_tail_layer(const char *s,
                            int32_t *flat, int flat_cap, int *flat_n,
                            int *starts, int starts_cap, int *layer_count)
{
    if (!s || !*s) return -1;
    if (*layer_count >= starts_cap - 1) return -1;

    int s_idx = *flat_n;
    int32_t *dst = flat + s_idx;
    int cap = flat_cap - s_idx;
    if (cap <= 0) return -1;

    int n = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char *end = NULL;
        errno = 0;
        long v = strtol(p, &end, 0);
        if (end == p || errno == ERANGE ||
            v < INT32_MIN || v > INT32_MAX)
            return -1;
        if (n >= cap) return -1;
        dst[n++] = (int32_t)v;

        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') { p++; continue; }
        if (*p == '\0') break;
        return -1;
    }

    if (n == 0) return -1;

    *flat_n = s_idx + n;
    (*layer_count)++;
    starts[*layer_count] = *flat_n;
    return 0;
}

struct progress_monitor {
    struct fs_scan_progress *pg;
    pthread_t                tid;
    _Atomic int              stop;
};

static const char *phase_name(enum fs_scan_phase p)
{
    switch (p) {
    case FS_SCAN_PHASE_IDLE:   return "idle";
    case FS_SCAN_PHASE_BFS:    return "bfs";
    case FS_SCAN_PHASE_ENUM:   return "enum";
    case FS_SCAN_PHASE_DONE:   return "done";
    case FS_SCAN_PHASE_FAILED: return "failed";
    }
    return "?";
}

static void *progress_thread(void *arg)
{
    struct progress_monitor *pm = arg;
    while (!atomic_load(&pm->stop)) {
        struct fs_scan_progress_info cur;
        fs_scan_progress_get(pm->pg, &cur);

        fprintf(stderr,
                "\r%s[%-6s]%s d=%-2d in=%-7lu out=%-7lu hits=%-9lu "
                "pm=%-8lu anc=%-7lu chains=%-7lu %.1fs     ",
                C_BLUE_BOLD, phase_name(cur.phase), C_RESET,
                cur.depth,
                (unsigned long)cur.layer_in,
                (unsigned long)cur.layer_out,
                (unsigned long)cur.layer_hits,
                (unsigned long)cur.total_pm,
                (unsigned long)cur.total_anchors,
                (unsigned long)cur.total_chains,
                cur.elapsed_ms / 1000.0);
        fflush(stderr);
        usleep(200 * 1000);
    }
    fprintf(stderr, "\n");
    return NULL;
}

static ssize_t default_reader(pid_t pid,
                              const struct iovec *local_iov, unsigned long liovcnt,
                              const struct iovec *remote_iov, unsigned long riovcnt,
                              void *userdata)
{
    (void)userdata;
    return process_vm_readv(pid, local_iov, liovcnt, remote_iov, riovcnt, 0);
}

static struct vm_area *load_vma(pid_t pid)
{
    struct vm_area *vma = NULL;
    int rc = parse_maps(pid, &vma);
    if (rc != 0) {
        fprintf(stderr, "%serror:%s cannot parse maps for pid %d: %s\n",
                C_RED_BOLD, C_RESET, (int)pid, strerror(rc < 0 ? -rc : rc));
        return NULL;
    }
    (void)vma_elf(vma);
    return vma;
}

static int run_scan_and_output(const struct idx *ix, uintptr_t target,
                               int depth, int min_depth, int max_chains,
                               const int *mt, uint64_t offset,
                               const int32_t *tail_flat, int tail_flat_n,
                               const int *tail_starts, int tail_layer_count,
                               const char *out_file)
{
    struct fs_scan_opts *opts = fs_scan_opts_create();
    if (!opts) {
        fprintf(stderr, "%serror:%s out of memory\n", C_RED_BOLD, C_RESET);
        return 1;
    }

    opts->target     = target;
    opts->max_depth  = depth;
    opts->min_depth  = min_depth;
    opts->max_chains = max_chains;

    if (fs_scan_opts_set_max_targets_per_node(opts, mt, OPT_ARR_LEN) != 0) {
        fprintf(stderr, "%serror:%s out of memory\n", C_RED_BOLD, C_RESET);
        fs_scan_opts_free(opts);
        return 1;
    }

    uint64_t off[OPT_ARR_LEN];
    fill_max_off(off, offset);
    if (fs_scan_opts_set_max_off(opts, off, OPT_ARR_LEN) != 0) {
        fprintf(stderr, "%serror:%s out of memory\n", C_RED_BOLD, C_RESET);
        fs_scan_opts_free(opts);
        return 1;
    }

    if (tail_layer_count > 0 && tail_flat_n > 0) {
        if (fs_scan_opts_set_tail_layers(opts, tail_flat, tail_flat_n,
                                         tail_starts, tail_layer_count) != 0) {
            fprintf(stderr, "%serror:%s out of memory\n", C_RED_BOLD, C_RESET);
            fs_scan_opts_free(opts);
            return 1;
        }
    }

    struct fs_scan_progress *pg = fs_scan_progress_create();
    struct progress_monitor  pm;
    memset(&pm, 0, sizeof(pm));
    int have_pg = 0;

    if (pg) {
        opts->progress = pg;
        pm.pg = pg;
        atomic_init(&pm.stop, 0);
        if (pthread_create(&pm.tid, NULL, progress_thread, &pm) == 0)
            have_pg = 1;
        else
            fprintf(stderr, "%swarn:%s cannot start progress thread\n",
                    C_YELLOW, C_RESET);
    }

    struct pc_list *cl = fs_ptrscan(ix, opts);

    if (have_pg) {
        atomic_store(&pm.stop, 1);
        pthread_join(pm.tid, NULL);
    }
    if (pg) fs_scan_progress_free(pg);

    fs_scan_opts_free(opts);

    if (!cl) {
        int e = errno;
        fprintf(stderr, "%serror:%s scan failed: %s (errno=%d)\n",
                C_RED_BOLD, C_RESET,
                e ? strerror(e) : "unknown", e);
        return 1;
    }

    int rc = 0;
    if (out_file) {
        FILE *fp = fopen(out_file, "wb");
        if (!fp) {
            fprintf(stderr, "%serror:%s cannot open '%s': %s\n",
                    C_RED_BOLD, C_RESET, out_file, strerror(errno));
            rc = 1;
        } else {
            const char *ext = strrchr(out_file, '.');
            int sv;
            if (ext && strcasecmp(ext, ".pcf") == 0)
                sv = pcf_write_pc_list_everything(fp, cl);
            else
                sv = txt_save(fp, cl);
            fclose(fp);
            if (sv != 0) {
                fprintf(stderr, "%serror:%s save failed\n", C_RED_BOLD, C_RESET);
                rc = 1;
            } else {
                printf("%swrote:%s %s\n", C_GREEN, C_RESET, out_file);
            }
        }
    } else {
        txt_save(stdout, cl);
    }

    free_pc_list(cl);
    return rc;
}

static void usage(void)
{
    printf("Fast in-process pointer-chain scanner\n\n");
    printf("%sUsage:%s ptrscan [COMMAND] [ARGS]\n\n", C_BOLD, C_RESET);
    printf("%sCommands:%s\n", C_BOLD, C_RESET);

#define CMD_ROW(name, desc) \
    printf("  %s%-14s%s%s\n", C_GREEN, name, C_RESET, desc)

    CMD_ROW("index", "Build a pointer index from a running process and save to IDX");
    CMD_ROW("scan",  "Build index on the fly (or load from IDX) and scan pointer chains");
    CMD_ROW("help",  "Print this message or the help of the given subcommand(s)");

#undef CMD_ROW

    printf("\n%sOptions:%s\n", C_BOLD, C_RESET);
    printf("  %s%-14s%s%s\n", C_GREEN, "-h, --help",    C_RESET, "Print help");
    printf("  %s%-14s%s%s\n", C_GREEN, "-V, --version", C_RESET, "Print version");
}

static int cmd_index(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "%serror:%s missing <PID>\n", C_RED_BOLD, C_RESET);
        return 2;
    }
    pid_t pid = (pid_t)atoi(argv[1]);
    const char *out_file = NULL;
    int jobs = 0;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];

        if (a[0] == '-' && a[1] && a[1] != '-' && a[2]) {
            if      (a[1] == 'j') { if (parse_int_arg(a + 2, &jobs) == 0) continue; }
            else if (a[1] == 'o') { out_file = a + 2; continue; }
        }

        if ((!strcmp(a, "-o") || !strcmp(a, "--output")) && i + 1 < argc)
            out_file = argv[++i];
        else if ((!strcmp(a, "-j") || !strcmp(a, "--jobs")) && i + 1 < argc) {
            if (parse_int_arg(argv[++i], &jobs) != 0) {
                fprintf(stderr, "%serror:%s invalid -j value\n",
                        C_RED_BOLD, C_RESET);
                return 2;
            }
        } else {
            fprintf(stderr, "%serror:%s unknown option '%s'\n",
                    C_RED_BOLD, C_RESET, a);
            return 2;
        }
    }

    if (pid <= 0) {
        fprintf(stderr, "%serror:%s invalid pid '%s'\n",
                C_RED_BOLD, C_RESET, argv[1]);
        return 2;
    }

    struct vm_area *vma = load_vma(pid);
    if (!vma) return 1;

    struct idx *ix = idx_build(pid, vma, default_reader, NULL, jobs, NULL);
    free_vm_area(vma);
    if (!ix) {
        fprintf(stderr, "%serror:%s index build failed\n", C_RED_BOLD, C_RESET);
        return 1;
    }

    printf("%ssegments:%s %u\n", C_GREEN, C_RESET, idx_seg_count(ix));
    printf("%sentries:%s  %lu\n", C_GREEN, C_RESET, (unsigned long)idx_count(ix));

    int rc = 0;
    if (out_file) {
        FILE *fp = fopen(out_file, "wb");
        if (!fp) {
            fprintf(stderr, "%serror:%s cannot open '%s': %s\n",
                    C_RED_BOLD, C_RESET, out_file, strerror(errno));
            rc = 1;
        } else {
            int sv = idx_save(fp, ix);
            fclose(fp);
            if (sv != IDX_OK) {
                fprintf(stderr, "%serror:%s save failed: %s\n",
                        C_RED_BOLD, C_RESET, idx_strerror(sv));
                rc = 1;
            } else {
                printf("%swrote:%s    %s\n", C_GREEN, C_RESET, out_file);
            }
        }
    }

    idx_free(ix);
    return rc;
}

struct scan_args {
    pid_t    pid;               /* -p / --pid */
    int      depth;
    int      min_depth;
    int      max_chains;
    int      jobs;
    int      k_list[OPT_ARR_LEN];
    uint64_t offset;
    const char *out_file;
    const char *idx_file;       /* -i / --idx */
    int32_t  tail_flat[TAIL_FLAT_MAX];
    int      tail_starts[TAIL_LAYERS_MAX + 1];
    int      tail_flat_n;
    int      tail_layer_count;
};

static int parse_scan_args(int argc, char **argv, int start_i,
                           int allow_jobs, struct scan_args *out)
{
    out->pid        = 0;
    out->depth      = 6;
    out->min_depth  = 0;
    out->max_chains = 0;
    out->jobs       = 0;
    out->offset     = 0x1000;
    out->out_file   = NULL;
    out->idx_file   = NULL;
    for (int j = 0; j < OPT_ARR_LEN; j++) out->k_list[j] = 3;

    out->tail_flat_n      = 0;
    out->tail_layer_count = 0;
    out->tail_starts[0]   = 0;

    int i = start_i;
    for (; i < argc; i++) {
        const char *a = argv[i];

        /* Combined short options: -d20 -k0,0,32 -O0x1000 -j8 -oFILE -p1234 */
        if (a[0] == '-' && a[1] && a[1] != '-' && a[2]) {
            const char *v = a + 2;
            switch (a[1]) {
            case 'd': if (parse_int_arg(v, &out->depth)      == 0) continue; break;
            case 'm': if (parse_int_arg(v, &out->min_depth)  == 0) continue; break;
            case 'n': if (parse_int_arg(v, &out->max_chains) == 0) continue; break;
            case 'j':
                if (allow_jobs && parse_int_arg(v, &out->jobs) == 0) continue;
                break;
            case 'k': if (parse_k_list(v, out->k_list)       == 0) continue; break;
            case 'O': if (parse_u64_arg(v, &out->offset)     == 0) continue; break;
            case 'o': out->out_file = v; continue;
            case 'i': out->idx_file = v; continue;
            case 'p': {
                int pid_v;
                if (parse_int_arg(v, &pid_v) == 0) {
                    out->pid = (pid_t)pid_v;
                    continue;
                }
                break;
            }
            case 't':
                if (parse_tail_layer(v, out->tail_flat, TAIL_FLAT_MAX,
                                     &out->tail_flat_n,
                                     out->tail_starts, TAIL_LAYERS_MAX + 1,
                                     &out->tail_layer_count) == 0)
                    continue;
                break;
            default: break;
            }
        }

        if      ((!strcmp(a, "-d") || !strcmp(a, "--depth"))      && i + 1 < argc)
            { if (parse_int_arg(argv[++i], &out->depth) != 0) goto bad; }
        else if ((!strcmp(a, "-m") || !strcmp(a, "--min-depth"))  && i + 1 < argc)
            { if (parse_int_arg(argv[++i], &out->min_depth) != 0) goto bad; }
        else if ((!strcmp(a, "-n") || !strcmp(a, "--max-chains")) && i + 1 < argc)
            { if (parse_int_arg(argv[++i], &out->max_chains) != 0) goto bad; }
        else if ((!strcmp(a, "-k") || !strcmp(a, "--max-targets"))&& i + 1 < argc)
            { if (parse_k_list(argv[++i], out->k_list) != 0) goto bad; }
        else if ((!strcmp(a, "-O") || !strcmp(a, "--offset"))     && i + 1 < argc)
            { if (parse_u64_arg(argv[++i], &out->offset) != 0) goto bad; }
        else if ((!strcmp(a, "-o") || !strcmp(a, "--output"))     && i + 1 < argc)
            out->out_file = argv[++i];
        else if ((!strcmp(a, "-i") || !strcmp(a, "--idx"))        && i + 1 < argc)
            out->idx_file = argv[++i];
        else if ((!strcmp(a, "-p") || !strcmp(a, "--pid"))        && i + 1 < argc) {
            int pid_v;
            if (parse_int_arg(argv[++i], &pid_v) != 0) goto bad;
            out->pid = (pid_t)pid_v;
        }
        else if ((!strcmp(a, "-t") || !strcmp(a, "--tail"))       && i + 1 < argc)
            { if (parse_tail_layer(argv[++i], out->tail_flat, TAIL_FLAT_MAX,
                                   &out->tail_flat_n,
                                   out->tail_starts, TAIL_LAYERS_MAX + 1,
                                   &out->tail_layer_count) != 0) goto bad; }
        else if (allow_jobs &&
                 (!strcmp(a, "-j") || !strcmp(a, "--jobs"))       && i + 1 < argc)
            { if (parse_int_arg(argv[++i], &out->jobs) != 0) goto bad; }
        else
            goto bad;
    }
    return 0;

bad:
    fprintf(stderr, "%serror:%s unknown or malformed option '%s'\n",
            C_RED_BOLD, C_RESET, argv[i]);
    return -1;
}

static int cmd_scan(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "%serror:%s usage:\n", C_RED_BOLD, C_RESET);
        fprintf(stderr, "  ptrscan scan <PID> <TARGET> [options]\n");
        fprintf(stderr, "  ptrscan scan <TARGET> -p <PID> [options]\n");
        fprintf(stderr, "  ptrscan scan <TARGET> -i <FILE.idx> [options]\n");
        return 2;
    }

    pid_t     pos_pid = 0;
    uintptr_t target  = 0;
    int       scan_start;

    /*
     * Legacy positional form: scan <PID> <TARGET> ...
     * Detected when argv[2] exists and does not start with '-'.
     * Otherwise the first positional is TARGET, PID comes from -p.
     */
    if (argc >= 3 && argv[2][0] != '-') {
        pos_pid    = (pid_t)atoi(argv[1]);
        target     = (uintptr_t)strtoull(argv[2], NULL, 0);
        scan_start = 3;
    } else {
        target     = (uintptr_t)strtoull(argv[1], NULL, 0);
        scan_start = 2;
    }

    struct scan_args sa;
    if (parse_scan_args(argc, argv, scan_start, 1, &sa) != 0)
        return 2;

    if (target == 0) {
        fprintf(stderr, "%serror:%s invalid target\n", C_RED_BOLD, C_RESET);
        return 2;
    }

    /* -p takes precedence over positional PID */
    pid_t pid = sa.pid > 0 ? sa.pid : pos_pid;

    struct idx *ix = NULL;

    if (sa.idx_file) {
        FILE *fp = fopen(sa.idx_file, "rb");
        if (!fp) {
            fprintf(stderr, "%serror:%s cannot open '%s': %s\n",
                    C_RED_BOLD, C_RESET, sa.idx_file, strerror(errno));
            return 1;
        }
        int lrc = idx_load(fp, &ix);
        fclose(fp);
        if (lrc != IDX_OK) {
            fprintf(stderr, "%serror:%s load failed: %s\n",
                    C_RED_BOLD, C_RESET, idx_strerror(lrc));
            return 1;
        }
    } else {
        if (pid <= 0) {
            fprintf(stderr,
                    "%serror:%s no PID (use <PID> <TARGET>, -p <PID>, or -i <FILE.idx>)\n",
                    C_RED_BOLD, C_RESET);
            return 2;
        }
        struct vm_area *vma = load_vma(pid);
        if (!vma) return 1;

        ix = idx_build(pid, vma, default_reader, NULL, sa.jobs, NULL);
        free_vm_area(vma);
        if (!ix) {
            fprintf(stderr, "%serror:%s index build failed\n",
                    C_RED_BOLD, C_RESET);
            return 1;
        }
    }

    int rc = run_scan_and_output(ix, target, sa.depth, sa.min_depth,
                                 sa.max_chains, sa.k_list, sa.offset,
                                 sa.tail_flat, sa.tail_flat_n,
                                 sa.tail_starts, sa.tail_layer_count,
                                 sa.out_file);
    idx_free(ix);
    return rc;
}

static int cmd_help(int argc, char **argv)
{
    if (argc >= 2) {
        const char *c = argv[1];

        if (strcmp(c, "index") == 0) {
            printf("Build a pointer index from a running process and save to IDX\n\n");
            printf("%sUsage:%s ptrscan index <PID> [FLAGS]\n\n", C_BOLD, C_RESET);
            printf("%sFlags:%s\n", C_BOLD, C_RESET);
            printf("  %s%-24s%s%s\n", C_GREEN, "-o, --output <FILE>", C_RESET,
                   "Write IDX to FILE (optional)");
            printf("  %s%-24s%s%s\n", C_GREEN, "-j, --jobs <N>", C_RESET,
                   "Worker threads (default: number of CPUs)");
            return 0;
        }
        if (strcmp(c, "scan") == 0) {
            printf("Scan pointer chains\n\n");
            printf("%sUsage:%s\n", C_BOLD, C_RESET);
            printf("  ptrscan scan <PID> <TARGET> [FLAGS]\n");
            printf("  ptrscan scan <TARGET> -p <PID> [FLAGS]\n");
            printf("  ptrscan scan <TARGET> -i <FILE.idx> [FLAGS]\n\n");
            printf("%sFlags:%s\n", C_BOLD, C_RESET);
            printf("  %s%-24s%s%s\n", C_GREEN, "-p, --pid <PID>", C_RESET,
                   "Target PID (alternative to positional <PID>)");
            printf("  %s%-24s%s%s\n", C_GREEN, "-i, --idx <FILE>", C_RESET,
                   "Load IDX from FILE instead of building from PID");
            printf("  %s%-24s%s%s\n", C_GREEN, "-d, --depth <N>", C_RESET,
                   "Max pointer depth (default 6, 0 = unlimited)");
            printf("  %s%-24s%s%s\n", C_GREEN, "-m, --min-depth <N>", C_RESET,
                   "Min pointer depth");
            printf("  %s%-24s%s%s\n", C_GREEN, "-n, --max-chains <N>", C_RESET,
                   "Stop after N chains");
            printf("  %s%-24s%s%s\n", C_GREEN, "-j, --jobs <N>", C_RESET,
                   "Worker threads (only when building index)");
            printf("  %s%-24s%s%s\n", C_GREEN, "-k, --max-targets <L>", C_RESET,
                   "Per-layer target cap, comma list (default 3)");
            printf("  %s%-24s%s%s\n", C_GREEN, "-O, --offset <N>", C_RESET,
                   "Pointer search window per layer (default 0x1000)");
            printf("  %s%-24s%s%s\n", C_GREEN, "-t, --tail <LIST>", C_RESET,
                   "Per-layer tail offset whitelist, one -t per layer");
            printf("  %s%-24s%s%s\n", C_GREEN, "-o, --output <FILE>", C_RESET,
                   "Write chains to FILE (.txt or .pcf)");
            return 0;
        }
    }
    usage();
    return 0;
}

int main(int argc, char **argv)
{
    use_color = detect_color();

    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(cmd, "-V") == 0 || strcmp(cmd, "--version") == 0) {
        printf("ptrscan %s\n", VERSION);
        return 0;
    }

    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;

    if (strcmp(cmd, "index") == 0) return cmd_index(sub_argc, sub_argv);
    if (strcmp(cmd, "scan")  == 0) return cmd_scan(sub_argc, sub_argv);
    if (strcmp(cmd, "help")  == 0) return cmd_help(sub_argc, sub_argv);

    fprintf(stderr, "%serror:%s unrecognized subcommand '%s'\n\n",
            C_RED_BOLD, C_RESET, cmd);
    usage();
    return 1;
}