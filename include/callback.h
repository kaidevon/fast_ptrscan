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

#ifndef FAST_PTRSCAN_CALLBACK_H
#define FAST_PTRSCAN_CALLBACK_H
    
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

/* fastscan ptrscan process reader callback */
typedef ssize_t (*fs_ptrscan_process_reader_t)(
    pid_t pid,
    const struct iovec* local_iov, unsigned long liovcnt,
    const struct iovec* remote_iov, unsigned long riovcnt,
    void* userdata
);

#endif  /* FAST_PTRSCAN_CALLBACK_H */