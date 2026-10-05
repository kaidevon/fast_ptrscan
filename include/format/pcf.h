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

#ifndef PCF_H
#define PCF_H

#include <stdio.h>
#include <stdint.h>
#include "pc_list.h"

#define PCF_MAGIC        ( ('F' << 24) | ('C' << 16) | ('P' << 8) | 0x7F )
#define PCF_SUPER_MAGIC  ( ('R' << 24) | ('P' << 16) | ('U' << 8) | 'S' )

#define PCF_VERSION_MAJOR   1
#define PCF_VERSION_MINOR   0
#define PCF_VERSION         ((PCF_VERSION_MAJOR << 8) | PCF_VERSION_MINOR)

#define PCF_FLAG_LITTLE_ENDIAN   0x0001

#define PCF_OK                          0
#define PCF_ERR_INVALID_ARG             1
#define PCF_ERR_IO                      2
#define PCF_ERR_BAD_MAGIC               3
#define PCF_ERR_UNSUPPORTED_VERSION     4
#define PCF_ERR_CORRUPT                 5
#define PCF_ERR_NOMEM                   6
#define PCF_ERR_INTERNAL                7

#define PCF_MAX_DEPTH           64

#pragma pack(push, 1)

struct pcf_file_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint64_t group_count;
    uint64_t super_offset;
    uint32_t super_size;
    uint32_t reserved;
    uint32_t crc32;
};

struct pcf_super_entry_raw {
    uint64_t group_offset;
    uint32_t group_size;
    uint32_t chain_count;
    uint8_t  seg_type;
    uint8_t  seg_index;
    uint16_t path_len;
};

struct pcf_super_block {
    uint32_t magic;
    uint32_t entry_count;
    struct pcf_super_entry_raw entries[];
};

#pragma pack(pop)

struct pcf_super_entry {
    struct {
        char* pathname;
        uint32_t group_size;
        uint32_t chain_count;
        uint8_t  seg_type;
        uint8_t  seg_index;
    } info;
    uint64_t group_offset;
    struct pcf_super_entry* next;
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Write entire pc_list to PCF file.
 */
int pcf_write_pc_list_everything(FILE *fp, const struct pc_list* pc_list);

/*
 * Read super block entries from PCF file.
 */
int pcf_read_super_entries(FILE *fp, struct pcf_super_entry **entries_out);

void pcf_free_super_entries(struct pcf_super_entry *entries);

/*
 * Read selected groups into pc_list. Pass NULL for entries to load all.
 */
int pcf_read_pc_list_everything(FILE *fp, struct pc_list **pc_list_out,
                                const struct pcf_super_entry* entries);

/*
 * Filter super entry list in place. Returns number of remaining entries.
 */
int pcf_super_entries_filter(struct pcf_super_entry **entries,
                             const char *module_path,
                             int seg_type, int seg_index);

#ifdef __cplusplus
}
#endif

#endif  /* PCF_H */

/* Static Pointer Chain Format
 * Chain layout:
 *      libmodule.so:.seg_type[seg_index] + offset[]
 * Supported VM areas:
 *      .bss / .data / .rodata
 * Endianness:
 *      All integers are little-endian.
 */