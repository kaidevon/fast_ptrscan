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

#ifndef IDX_H
#define IDX_H

#include <stdio.h>
#include <stdint.h>
#include "ptr_index.h"

/*
 * IDX format: memory index file for pointer scanning.
 *
 * Layout:
 *   [file header]     fixed 80 bytes
 *   [seg table]       nseg * 40 bytes
 *   [string pool]     variable, NUL-terminated pathnames
 *   [entry table]     n * 16 bytes, sorted by v
 *   [global crc32]    4 bytes
 *
 * Endianness:
 *   All integers are little-endian.
 *
 * CRC:
 *   Header CRC covers the header up to (but not including) the crc32 field.
 *   Global CRC covers the whole file except the trailing 4 bytes.
 */

#define IDX_MAGIC               (('I' << 24) | ('D' << 16) | ('X' << 8) | '1')

#define IDX_VERSION_MAJOR       1
#define IDX_VERSION_MINOR       0
#define IDX_VERSION             ((IDX_VERSION_MAJOR << 8) | IDX_VERSION_MINOR)

#define IDX_FLAG_LITTLE_ENDIAN  0x0001

#define IDX_OK                          0
#define IDX_ERR_INVALID_ARG             1
#define IDX_ERR_IO                      2
#define IDX_ERR_BAD_MAGIC               3
#define IDX_ERR_UNSUPPORTED_VERSION     4
#define IDX_ERR_CORRUPT                 5
#define IDX_ERR_NOMEM                   6
#define IDX_ERR_INTERNAL                7
#define IDX_ERR_PTR_SIZE                8

#pragma pack(push, 1)

struct idx_file_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint64_t vmin;
    uint64_t vmax;
    uint64_t entry_count;
    uint32_t seg_count;
    uint32_t ptr_size;
    uint64_t seg_offset;
    uint64_t str_offset;
    uint64_t str_size;
    uint64_t entry_offset;
    uint32_t crc32;
    uint32_t reserved;
};

struct idx_seg_raw {
    uint64_t start;
    uint64_t end;
    uint64_t mod_start;
    uint32_t type;
    uint32_t index;
    uint32_t str_off;
    uint32_t str_len;
};

#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serialize an in-memory idx to fp.
 * Returns IDX_OK on success.
 */
int idx_save(FILE *fp, const struct idx *ix);

/*
 * Load an idx from fp.
 * On success, *out points to a freshly allocated idx (caller frees with idx_free).
 */
int idx_load(FILE *fp, struct idx **out);

const char *idx_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif  /* IDX_H */