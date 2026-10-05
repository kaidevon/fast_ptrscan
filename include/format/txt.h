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

#ifndef TXT_H
#define TXT_H

#include <stdio.h>
#include <stdint.h>
#include "pc_list.h"

/*
 * TXT format: human-readable dump of a pointer chain list.
 *
 * Layout:
 *   # ptrscan
 *   # groups: <n_groups> chains: <n_chains>
 *   # <module> .<seg>[<index>] chains: <n>
 *   ...
 *   (blank line)
 *
 *   <module> .<seg>[<index>]
 *     0xOFF[->0xOFF]...
 *     ...
 *   (blank line)
 *   ...
 *
 * Notes:
 *   - Lines starting with '#' are comments and are skipped on load.
 *   - Group header: "<module> .<seg>[<index>]" on its own line.
 *   - Chain lines are indented with two spaces.
 *   - Offsets are printed as hex (0x...), negative offsets with '-0x...'.
 *   - Segment names: text / rodata / data / bss.
 *   - All content is ASCII / UTF-8 compatible.
 *
 * Endianness:
 *   Text format, no endianness concerns.
 */

#define TXT_MAGIC       "ptrscan"
#define TXT_MAGIC_LEN   7

#define TXT_OK                          0
#define TXT_ERR_INVALID_ARG             1
#define TXT_ERR_IO                      2
#define TXT_ERR_BAD_MAGIC               3
#define TXT_ERR_UNSUPPORTED_VERSION     4
#define TXT_ERR_CORRUPT                 5
#define TXT_ERR_NOMEM                   6
#define TXT_ERR_INTERNAL                7

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serialize a pc_list into fp as text.
 * Returns TXT_OK on success, TXT_ERR_* on failure.
 */
int txt_save(FILE *fp, const struct pc_list *pc_list);

/*
 * Parse a pc_list from fp.
 * On success, *pc_list_out points to a freshly allocated list
 * (caller frees with free_pc_list).
 */
int txt_load(FILE *fp, struct pc_list **pc_list_out);

const char *txt_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif  /* TXT_H */