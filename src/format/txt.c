/***************************************************************
 *  
 * project   ____   _____  ____   ____   ____     _     _   _
 *       ___|  _ \ |_   _|| _  \ / ___| / ___|   / \   | \ | |
 *      / __| |_) |  | |  | |_) |\___ \|| |     / _ \  |  \| |
 *     | (__|  __/   | |  |  _ <  ___) || |__  / ___ \ | |\  |
 *      \___|_|      |_|  |_| \_|\____/ \____//_/   \_\|_| \_|
 *
 * Copyright (C) 2026 kaidev, <kaidevonmail@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#include "txt.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <limits.h>

#include "vma/vm_area.h"

const char *txt_strerror(int err)
{
    switch (err) {
    case TXT_OK:                     return "ok";
    case TXT_ERR_INVALID_ARG:        return "invalid argument";
    case TXT_ERR_IO:                 return "i/o error";
    case TXT_ERR_BAD_MAGIC:          return "bad magic";
    case TXT_ERR_UNSUPPORTED_VERSION:return "unsupported version";
    case TXT_ERR_CORRUPT:            return "corrupt file";
    case TXT_ERR_NOMEM:              return "out of memory";
    case TXT_ERR_INTERNAL:           return "internal error";
    }
    return "unknown error";
}

static const char *seg_name_short(uint8_t seg_type)
{
    switch (seg_type) {
    case VMA_TYPE_TEXT:   return "text";
    case VMA_TYPE_RODATA: return "rodata";
    case VMA_TYPE_DATA:   return "data";
    case VMA_TYPE_BSS:    return "bss";
    default:              return "unknown";
    }
}

static int seg_type_from_name(const char *s, size_t len)
{
    if (len == 4 && memcmp(s, "text",   4) == 0) return VMA_TYPE_TEXT;
    if (len == 6 && memcmp(s, "rodata", 6) == 0) return VMA_TYPE_RODATA;
    if (len == 4 && memcmp(s, "data",   4) == 0) return VMA_TYPE_DATA;
    if (len == 3 && memcmp(s, "bss",    3) == 0) return VMA_TYPE_BSS;
    return -1;
}

static long count_chains(const struct pc_list *p)
{
    long n = 0;
    for (const struct oc_block *b = p->chains; b; b = b->next)
        n += atomic_load(&b->used);
    return n;
}

static int append_chain(struct pc_list *p, const int32_t *offs, int n)
{
    if (n <= 0) return 0;

    struct oc_block *b = p->chains, *last = NULL;
    while (b && atomic_load(&b->used) >= OC_LIST_CAPACITY) {
        last = b;
        b = b->next;
    }
    if (!b) {
        b = calloc(1, sizeof(*b));
        if (!b) return -1;
        atomic_init(&b->used, 0);
        b->next = NULL;
        if (last) last->next = b; else p->chains = b;
    }
    int slot = atomic_load(&b->used);
    struct offset_chain *oc = &b->oc[slot];
    oc->offsets.count  = n;
    oc->offsets.offset = malloc((size_t)n * sizeof(int32_t));
    if (!oc->offsets.offset) return -1;
    memcpy(oc->offsets.offset, offs, (size_t)n * sizeof(int32_t));
    b->oc_cost[slot] = 0;
    atomic_store(&b->used, slot + 1);
    p->chain_count++;
    return 0;
}

int txt_save(FILE *fp, const struct pc_list *pc_list)
{
    if (!fp || !pc_list) return TXT_ERR_INVALID_ARG;

    int  n_groups = 0;
    long n_chains = 0;
    for (const struct pc_list *p = pc_list; p; p = p->next) {
        n_groups++;
        n_chains += count_chains(p);
    }

    if (fprintf(fp, "# " TXT_MAGIC "\n") < 0)
        return TXT_ERR_IO;
    if (fprintf(fp, "# groups: %d chains: %ld\n", n_groups, n_chains) < 0)
        return TXT_ERR_IO;

    for (const struct pc_list *p = pc_list; p; p = p->next) {
        if (fprintf(fp, "# %s .%s[%u] chains: %ld\n",
                    p->filename ? p->filename : "(null)",
                    seg_name_short(p->seg_type),
                    (unsigned)p->seg_index,
                    count_chains(p)) < 0)
            return TXT_ERR_IO;
    }

    if (fputc('\n', fp) == EOF)
        return TXT_ERR_IO;

    for (const struct pc_list *p = pc_list; p; p = p->next) {
        if (fprintf(fp, "%s .%s[%u]\n",
                    p->filename ? p->filename : "(null)",
                    seg_name_short(p->seg_type),
                    (unsigned)p->seg_index) < 0)
            return TXT_ERR_IO;

        for (struct oc_block *b = p->chains; b; b = b->next) {
            int used = atomic_load(&b->used);
            for (int i = 0; i < used; i++) {
                struct offset_chain *oc = &b->oc[i];

                if (fputs("  ", fp) == EOF) return TXT_ERR_IO;
                for (int j = 0; j < oc->offsets.count; j++) {
                    if (j && fputs("->", fp) == EOF) return TXT_ERR_IO;

                    int64_t v = oc->offsets.offset[j];
                    if (v < 0) {
                        if (fprintf(fp, "-0x%" PRIx64,
                                    (uint64_t)(-v)) < 0)
                            return TXT_ERR_IO;
                    } else {
                        if (fprintf(fp, "0x%" PRIx64,
                                    (uint64_t)v) < 0)
                            return TXT_ERR_IO;
                    }
                }
                if (fputc('\n', fp) == EOF) return TXT_ERR_IO;
            }
        }
        if (fputc('\n', fp) == EOF) return TXT_ERR_IO;
    }

    return TXT_OK;
}

static int parse_chain_line(const char *line,
                            int32_t **offsets_out, int *count_out)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return -1;

    int cap = 8, n = 0;
    int32_t *offs = malloc((size_t)cap * sizeof(int32_t));
    if (!offs) return -1;

    while (*p) {
        char *endp;
        errno = 0;
        long v = strtol(p, &endp, 0);
        if (endp == p || errno == ERANGE) {
            free(offs);
            return -1;
        }
        if (v < INT32_MIN || v > INT32_MAX) {
            free(offs);
            return -1;
        }
        if (n >= cap) {
            cap *= 2;
            int32_t *no = realloc(offs, (size_t)cap * sizeof(int32_t));
            if (!no) { free(offs); return -1; }
            offs = no;
        }
        offs[n++] = (int32_t)v;
        p = endp;

        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '-' && p[1] == '>') {
            p += 2;
            while (*p == ' ' || *p == '\t') p++;
            continue;
        }
        break;
    }

    if (n == 0) { free(offs); return -1; }

    *offsets_out = offs;
    *count_out   = n;
    return 0;
}

static int parse_group_line(const char *line, char **filename_out,
                            int *seg_type_out, int *seg_index_out)
{
    const char *bracket = strrchr(line, '[');
    if (!bracket) return -1;

    /* Find " ." before the bracket */
    const char *dot = NULL;
    for (const char *p = bracket - 1; p > line; p--) {
        if (p[0] == '.' && (p[-1] == ' ' || p[-1] == '\t')) {
            dot = p;
            break;
        }
    }
    if (!dot) return -1;

    size_t fname_len   = (size_t)(dot - 1 - line);
    size_t segname_len = (size_t)(bracket - dot - 1);
    if (fname_len == 0 || segname_len == 0) return -1;

    int st = seg_type_from_name(dot + 1, segname_len);
    if (st < 0) return -1;

    int idx = atoi(bracket + 1);
    if (idx < 0 || idx > 255) return -1;

    char *fname = malloc(fname_len + 1);
    if (!fname) return -1;
    memcpy(fname, line, fname_len);
    fname[fname_len] = '\0';

    *filename_out  = fname;
    *seg_type_out  = st;
    *seg_index_out = idx;
    return 0;
}

int txt_load(FILE *fp, struct pc_list **pc_list_out)
{
    if (!fp || !pc_list_out) return TXT_ERR_INVALID_ARG;
    *pc_list_out = NULL;

    char  *line = NULL;
    size_t cap  = 0;
    ssize_t nread;

    struct pc_list *head = NULL, *tail = NULL;
    struct pc_list *cur  = NULL;
    int rc = TXT_OK;

    /* 1. magic */
    nread = getline(&line, &cap, fp);
    if (nread <= 0) { rc = TXT_ERR_BAD_MAGIC; goto fail; }

    while (nread > 0 && (line[nread-1] == '\n' || line[nread-1] == '\r'))
        line[--nread] = '\0';

    if (strncmp(line, "# " TXT_MAGIC, 2 + TXT_MAGIC_LEN) != 0) {
        rc = TXT_ERR_BAD_MAGIC;
        goto fail;
    }

    /* 2. body */
    while ((nread = getline(&line, &cap, fp)) > 0) {
        while (nread > 0 && (line[nread-1] == '\n' || line[nread-1] == '\r'))
            line[--nread] = '\0';

        if (nread == 0) continue;
        if (line[0] == '#') continue;

        if (line[0] == ' ' || line[0] == '\t') {
            /* chain line */
            if (!cur) { rc = TXT_ERR_CORRUPT; goto fail; }

            int32_t *offs = NULL;
            int n = 0;
            if (parse_chain_line(line, &offs, &n) != 0) {
                rc = TXT_ERR_CORRUPT;
                goto fail;
            }
            if (append_chain(cur, offs, n) != 0) {
                free(offs);
                rc = TXT_ERR_NOMEM;
                goto fail;
            }
            free(offs);
        } else {
            /* group header */
            char *fname = NULL;
            int st = 0, si = 0;
            if (parse_group_line(line, &fname, &st, &si) != 0) {
                rc = TXT_ERR_CORRUPT;
                goto fail;
            }
            struct pc_list *node = calloc(1, sizeof(*node));
            if (!node) { free(fname); rc = TXT_ERR_NOMEM; goto fail; }
            node->filename    = fname;
            node->seg_type    = (uint8_t)st;
            node->seg_index   = (uint8_t)si;
            node->chain_count = 0;
            node->chains      = NULL;
            node->next        = NULL;

            if (!head) head = tail = node;
            else { tail->next = node; tail = node; }
            cur = node;
        }
    }

    if (!head) { rc = TXT_ERR_CORRUPT; goto fail; }

    free(line);
    *pc_list_out = head;
    return TXT_OK;

fail:
    free(line);
    free_pc_list(head);
    return rc;
}