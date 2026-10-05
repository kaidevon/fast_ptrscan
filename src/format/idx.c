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

#include "idx.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zlib.h>
#include <limits.h>

#define IDX_HEADER_SIZE     80
#define IDX_SEG_SIZE        40
#define IDX_ENTRY_SIZE      16
#define IDX_ENTRY_BATCH     512

static inline uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t read_u64_le(const uint8_t *p)
{
    return (uint64_t)p[0] |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static inline void write_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void write_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void write_u64_le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (i * 8)) & 0xFF);
}

static uLong crc32_block(uLong crc, const void *data, uint64_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        uInt chunk = (len > 0x40000000u) ? 0x40000000u : (uInt)len;
        crc = crc32(crc, p, chunk);
        p += chunk;
        len -= chunk;
    }
    return crc;
}

const char *idx_strerror(int err)
{
    switch (err) {
    case IDX_OK:                     return "ok";
    case IDX_ERR_INVALID_ARG:        return "invalid argument";
    case IDX_ERR_IO:                 return "i/o error";
    case IDX_ERR_BAD_MAGIC:          return "bad magic";
    case IDX_ERR_UNSUPPORTED_VERSION:return "unsupported version";
    case IDX_ERR_CORRUPT:            return "corrupt file";
    case IDX_ERR_NOMEM:              return "out of memory";
    case IDX_ERR_INTERNAL:           return "internal error";
    case IDX_ERR_PTR_SIZE:           return "pointer size mismatch";
    }
    return "unknown error";
}

int idx_save(FILE *fp, const struct idx *ix)
{
    if (!fp || !ix)
        return IDX_ERR_INVALID_ARG;

    if (ix->nseg > 0xFFFFFFFEu)
        return IDX_ERR_INVALID_ARG;

    /* 1. 计算字符串池大小 */
    uint64_t str_size = 0;
    for (uint32_t i = 0; i < ix->nseg; i++) {
        const char *p = ix->segs[i].pathname;
        if (p && p[0]) {
            size_t len = strlen(p);
            if (len > UINT32_MAX - 1)
                return IDX_ERR_INVALID_ARG;
            str_size += len + 1;
        }
    }

    /* 2. 计算布局 */
    uint64_t off_seg   = IDX_HEADER_SIZE;
    uint64_t off_str   = off_seg + (uint64_t)ix->nseg * IDX_SEG_SIZE;
    uint64_t off_entry = off_str + str_size;
    uint64_t off_crc   = off_entry + ix->n * IDX_ENTRY_SIZE;

    /* 3. 构造头部 */
    uint8_t hbuf[IDX_HEADER_SIZE] = {0};
    write_u32_le(hbuf +  0, IDX_MAGIC);
    write_u16_le(hbuf +  4, IDX_VERSION);
    write_u16_le(hbuf +  6, IDX_FLAG_LITTLE_ENDIAN);
    write_u64_le(hbuf +  8, ix->vmin);
    write_u64_le(hbuf + 16, ix->vmax);
    write_u64_le(hbuf + 24, ix->n);
    write_u32_le(hbuf + 32, ix->nseg);
    write_u32_le(hbuf + 36, (uint32_t)sizeof(uintptr_t));
    write_u64_le(hbuf + 40, off_seg);
    write_u64_le(hbuf + 48, off_str);
    write_u64_le(hbuf + 56, str_size);
    write_u64_le(hbuf + 64, off_entry);

    uLong hcrc = crc32(0L, Z_NULL, 0);
    hcrc = crc32(hcrc, hbuf, 72);
    write_u32_le(hbuf + 72, (uint32_t)hcrc);
    /* hbuf[76..79] 保留 */

    /* 4. 写头部, 开始累计全局 CRC */
    uLong gcrc = crc32(0L, Z_NULL, 0);
    gcrc = crc32(gcrc, hbuf, IDX_HEADER_SIZE);
    if (fwrite(hbuf, 1, IDX_HEADER_SIZE, fp) != IDX_HEADER_SIZE)
        return IDX_ERR_IO;

    /* 5. 段表 */
    uint32_t str_off = 0;
    uint8_t  sbuf[IDX_SEG_SIZE];
    for (uint32_t i = 0; i < ix->nseg; i++) {
        const struct seg *s = &ix->segs[i];
        write_u64_le(sbuf +  0, s->start);
        write_u64_le(sbuf +  8, s->end);
        write_u64_le(sbuf + 16, s->mod_start);
        write_u32_le(sbuf + 24, s->type);
        write_u32_le(sbuf + 28, s->index);

        if (s->pathname && s->pathname[0]) {
            size_t len = strlen(s->pathname);
            write_u32_le(sbuf + 32, str_off);
            write_u32_le(sbuf + 36, (uint32_t)len);
            str_off += (uint32_t)(len + 1);
        } else {
            write_u32_le(sbuf + 32, 0);
            write_u32_le(sbuf + 36, 0);
        }

        gcrc = crc32(gcrc, sbuf, IDX_SEG_SIZE);
        if (fwrite(sbuf, 1, IDX_SEG_SIZE, fp) != IDX_SEG_SIZE)
            return IDX_ERR_IO;
    }

    /* 6. 字符串池 */
    for (uint32_t i = 0; i < ix->nseg; i++) {
        const char *p = ix->segs[i].pathname;
        if (!p || !p[0])
            continue;
        size_t len = strlen(p);
        gcrc = crc32_block(gcrc, p, len);
        if (fwrite(p, 1, len, fp) != len)
            return IDX_ERR_IO;
        gcrc = crc32(gcrc, (const Bytef *)"", 1);
        if (fwrite("", 1, 1, fp) != 1)
            return IDX_ERR_IO;
    }

    /* 7. 条目表 */
    uint8_t ebuf[IDX_ENTRY_BATCH * IDX_ENTRY_SIZE];
    size_t  n_buf = 0;
    for (uint64_t i = 0; i < ix->n; i++) {
        uint8_t *p = ebuf + n_buf * IDX_ENTRY_SIZE;
        write_u64_le(p + 0, ix->e[i].v);
        write_u64_le(p + 8, ix->e[i].meta);
        n_buf++;
        if (n_buf == IDX_ENTRY_BATCH) {
            size_t bytes = n_buf * IDX_ENTRY_SIZE;
            gcrc = crc32(gcrc, ebuf, (uInt)bytes);
            if (fwrite(ebuf, 1, bytes, fp) != bytes)
                return IDX_ERR_IO;
            n_buf = 0;
        }
    }
    if (n_buf > 0) {
        size_t bytes = n_buf * IDX_ENTRY_SIZE;
        gcrc = crc32(gcrc, ebuf, (uInt)bytes);
        if (fwrite(ebuf, 1, bytes, fp) != bytes)
            return IDX_ERR_IO;
    }

    /* 8. 全局 CRC */
    uint8_t crcbuf[4];
    write_u32_le(crcbuf, (uint32_t)gcrc);
    if (fwrite(crcbuf, 1, 4, fp) != 4)
        return IDX_ERR_IO;

    (void)off_crc;
    return IDX_OK;
}

int idx_load(FILE *fp, struct idx **out)
{
    if (!fp || !out)
        return IDX_ERR_INVALID_ARG;
    *out = NULL;

    struct idx *ix       = NULL;
    char       *strings  = NULL;
    uint32_t   *seg_soff = NULL;
    uint32_t   *seg_slen = NULL;
    int         rc       = IDX_ERR_INTERNAL;

    /* 1. 文件大小 */
    if (fseek(fp, 0, SEEK_END) != 0)
        return IDX_ERR_IO;
    long fs = ftell(fp);
    if (fs < IDX_HEADER_SIZE + 4)
        return IDX_ERR_CORRUPT;
    if (fseek(fp, 0, SEEK_SET) != 0)
        return IDX_ERR_IO;

    /* 2. 头部 */
    uint8_t hbuf[IDX_HEADER_SIZE];
    if (fread(hbuf, 1, IDX_HEADER_SIZE, fp) != IDX_HEADER_SIZE)
        return IDX_ERR_IO;

    uLong gcrc = crc32(0L, Z_NULL, 0);
    gcrc = crc32(gcrc, hbuf, IDX_HEADER_SIZE);

    uint32_t magic        = read_u32_le(hbuf +  0);
    uint16_t version      = read_u16_le(hbuf +  4);
    uint16_t flags        = read_u16_le(hbuf +  6);
    uint64_t vmin         = read_u64_le(hbuf +  8);
    uint64_t vmax         = read_u64_le(hbuf + 16);
    uint64_t entry_count  = read_u64_le(hbuf + 24);
    uint32_t seg_count    = read_u32_le(hbuf + 32);
    uint32_t ptr_size     = read_u32_le(hbuf + 36);
    uint64_t seg_offset   = read_u64_le(hbuf + 40);
    uint64_t str_offset   = read_u64_le(hbuf + 48);
    uint64_t str_size     = read_u64_le(hbuf + 56);
    uint64_t entry_offset = read_u64_le(hbuf + 64);
    uint32_t hdr_crc      = read_u32_le(hbuf + 72);

    if (magic != IDX_MAGIC)
        return IDX_ERR_BAD_MAGIC;
    if (version != IDX_VERSION)
        return IDX_ERR_UNSUPPORTED_VERSION;
    if (!(flags & IDX_FLAG_LITTLE_ENDIAN))
        return IDX_ERR_UNSUPPORTED_VERSION;
    if (ptr_size != (uint32_t)sizeof(uintptr_t))
        return IDX_ERR_PTR_SIZE;

    uLong hcrc = crc32(0L, Z_NULL, 0);
    hcrc = crc32(hcrc, hbuf, 72);
    if ((uint32_t)hcrc != hdr_crc)
        return IDX_ERR_CORRUPT;

    /* 3. 布局校验 */
    uint64_t exp_seg_size   = (uint64_t)seg_count * IDX_SEG_SIZE;
    uint64_t exp_entry_size = entry_count * IDX_ENTRY_SIZE;
    if (seg_offset   != IDX_HEADER_SIZE)
        return IDX_ERR_CORRUPT;
    if (str_offset   != seg_offset + exp_seg_size)
        return IDX_ERR_CORRUPT;
    if (entry_offset != str_offset + str_size)
        return IDX_ERR_CORRUPT;
    if (entry_offset + exp_entry_size + 4 != (uint64_t)fs)
        return IDX_ERR_CORRUPT;

    /* 4. 分配 idx */
    ix = calloc(1, sizeof(*ix));
    if (!ix) {
        rc = IDX_ERR_NOMEM;
        goto fail;
    }
    ix->vmin = vmin;
    ix->vmax = vmax;
    ix->n    = entry_count;
    ix->cap  = entry_count;
    ix->nseg = seg_count;

    if (seg_count > 0) {
        ix->segs = calloc(seg_count, sizeof(*ix->segs));
        if (!ix->segs) {
            rc = IDX_ERR_NOMEM;
            goto fail;
        }
        seg_soff = malloc((size_t)seg_count * sizeof(uint32_t));
        seg_slen = malloc((size_t)seg_count * sizeof(uint32_t));
        if (!seg_soff || !seg_slen) {
            rc = IDX_ERR_NOMEM;
            goto fail;
        }
    }

    /* 5. 段表 */
    uint8_t sbuf[IDX_SEG_SIZE];
    for (uint32_t i = 0; i < seg_count; i++) {
        if (fread(sbuf, 1, IDX_SEG_SIZE, fp) != IDX_SEG_SIZE) {
            rc = IDX_ERR_IO;
            goto fail;
        }
        gcrc = crc32(gcrc, sbuf, IDX_SEG_SIZE);
        ix->segs[i].start     = read_u64_le(sbuf +  0);
        ix->segs[i].end       = read_u64_le(sbuf +  8);
        ix->segs[i].mod_start = read_u64_le(sbuf + 16);
        ix->segs[i].type      = read_u32_le(sbuf + 24);
        ix->segs[i].index     = read_u32_le(sbuf + 28);
        seg_soff[i]           = read_u32_le(sbuf + 32);
        seg_slen[i]           = read_u32_le(sbuf + 36);
        ix->segs[i].pathname  = NULL;
    }

    /* 6. 字符串池 */
    if (str_size > 0) {
        strings = malloc((size_t)str_size);
        if (!strings) {
            rc = IDX_ERR_NOMEM;
            goto fail;
        }
        if (fread(strings, 1, (size_t)str_size, fp) != (size_t)str_size) {
            rc = IDX_ERR_IO;
            goto fail;
        }
        gcrc = crc32_block(gcrc, strings, str_size);
    }

    /* 7. 填充 pathname */
    for (uint32_t i = 0; i < seg_count; i++) {
        if (seg_slen[i] == 0)
            continue;
        if ((uint64_t)seg_soff[i] + seg_slen[i] + 1 > str_size) {
            rc = IDX_ERR_CORRUPT;
            goto fail;
        }
        if (strings[seg_soff[i] + seg_slen[i]] != '\0') {
            rc = IDX_ERR_CORRUPT;
            goto fail;
        }
        char *p = malloc(seg_slen[i] + 1);
        if (!p) {
            rc = IDX_ERR_NOMEM;
            goto fail;
        }
        memcpy(p, strings + seg_soff[i], seg_slen[i]);
        p[seg_slen[i]] = '\0';
        ix->segs[i].pathname = p;
    }

    free(strings);
    strings = NULL;

    /* 8. 条目表 */
    if (entry_count > 0) {
        ix->e = malloc((size_t)entry_count * sizeof(struct ent));
        if (!ix->e) {
            rc = IDX_ERR_NOMEM;
            goto fail;
        }
    }

    uint8_t ebuf[IDX_ENTRY_BATCH * IDX_ENTRY_SIZE];
    uint64_t i = 0;
    while (i < entry_count) {
        size_t batch = IDX_ENTRY_BATCH;
        if (entry_count - i < batch)
            batch = (size_t)(entry_count - i);
        size_t bytes = batch * IDX_ENTRY_SIZE;
        if (fread(ebuf, 1, bytes, fp) != bytes) {
            rc = IDX_ERR_IO;
            goto fail;
        }
        gcrc = crc32(gcrc, ebuf, (uInt)bytes);
        for (size_t j = 0; j < batch; j++) {
            ix->e[i + j].v    = read_u64_le(ebuf + j * IDX_ENTRY_SIZE + 0);
            ix->e[i + j].meta = read_u64_le(ebuf + j * IDX_ENTRY_SIZE + 8);
        }
        i += batch;
    }

    /* 9. 全局 CRC */
    uint8_t crcbuf[4];
    if (fread(crcbuf, 1, 4, fp) != 4) {
        rc = IDX_ERR_IO;
        goto fail;
    }
    if ((uint32_t)gcrc != read_u32_le(crcbuf)) {
        rc = IDX_ERR_CORRUPT;
        goto fail;
    }

    free(seg_soff);
    free(seg_slen);
    *out = ix;
    return IDX_OK;

fail:
    free(strings);
    free(seg_soff);
    free(seg_slen);
    idx_free(ix);
    return rc;
}