/*
 * Copyright (C) 2026 kaidev
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; version 3 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * vma select format:
 *  - <module>[:<suffix>]
 *      - a) <module>:.{text|rodata|data|bss}[[<index>]][:<perm>]
 *          Examples:
 *            "libc.so:.text"               only the TEXT segment of libc.so
 *            "libc.so:.text:r-xp"          only the TEXT segment of libc.so with perm r-xp
 *            "libc.so:.rodata[1]"          only the 1st RODATA segment of libc.so
 *            "libc.so:.rodata[1]:r--p"     only the 1st RODATA segment of libc.so with perm r--p
 *            "libc.so:.bss[0]"             only the 0th BSS segment of libc.so
 *          Note: [<index>] must come before :<perm>; otherwise it is treated as part of the permission string.
 *      - b) <module>:<perm>
 *          Examples:
 *            "libc.so:r-xp"                only VMAs of libc.so with perm r-xp
 *      - c) <module>
 *          Examples:
 *            "libc.so"                     only VMAs whose pathname contains "libc.so"
 *      - d) <module>:.bss
 *          Examples:
 *            "libc.so:.bss"                only the BSS segment of libc.so
 *          Note: .bss is usually an anonymous writable segment, so area->pathname may be empty;
 *                when matching, it is recommended to fall back to area->module->pathname.
 *
 *  Colon splitting rules:
 *    1st colon ->  separates <module> from <suffix>
 *    2nd colon ->  in the ".seg..." branch, separates <seg[[index]]> from <perm>
 *                    (if the suffix does not start with '.', the 1st colon is directly followed by <perm>)
 *    Further colons -> all belong to the <perm> content; no further splitting is done
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "vma/vma_select.h"

static int select_compar(const struct vm_area *area, void *ud)
{
    struct vma_select *ctx = ud;

    if (ctx->seg_type >= 0 && (int)area->seg_type != ctx->seg_type)
        return 0;

    if (ctx->index >= 0 && (int)area->seg_index != ctx->index)
        return 0;

    if (ctx->module && ctx->module[0]) {
        const char *path = area->pathname;
        if (!path && area->module)
            path = area->module->pathname;
        if (!path)
            return 0;
        if (strstr(path, ctx->module) == NULL)
            return 0;
    }

    if (ctx->perm) {
        if (strlen(ctx->perm) < 4)
            return 0;
        if (memcmp(area->perm, ctx->perm, 4) != 0)
            return 0;
    }

    return 1;
}

static int select_any(const struct vm_area *area,
                      struct vma_select **select,
                      int count)
{
    for (int i = 0; i < count; i++) {
        if (select[i] && select_compar(area, select[i]))
            return 1;
    }
    return 0;
}

int vma_filter_select(struct vm_area **area,
                      struct vma_select **select,
                      int count)
{
    if (!area || !*area || !select || count <= 0)
        return -1;

    struct vm_area *prev = NULL;
    struct vm_area *curr = *area;
    int kept = 0;

    while (curr) {
        if (select_any(curr, select, count)) {
            prev = curr;
            curr = curr->next;
            kept++;
        } else {
            struct vm_area *next = curr->next;

            if (prev)
                prev->next = next;
            else
                *area = next;

            free(curr->pathname);
            free(curr);

            curr = next;
        }
    }

    return kept;
}

static struct vma_select *parse_vma_select(const char *s)
{
    if (!s || !*s)
        return NULL;

    struct vma_select *sel = calloc(1, sizeof(*sel));
    if (!sel)
        return NULL;

    sel->seg_type = -1;
    sel->index    = -1;

    char *buf = strdup(s);
    if (!buf) {
        free(sel);
        return NULL;
    }

    char *colon = strchr(buf, ':');
    char *rest  = NULL;
    if (colon) {
        *colon = '\0';
        rest = colon + 1;
    }

    sel->module = strdup(buf);
    if (!sel->module)
        goto cleanup;

    if (rest && *rest) {
        if (*rest == '.') {
            char *seg_start = rest + 1;

            char *colon2 = strchr(seg_start, ':');
            if (colon2) {
                *colon2 = '\0';
                sel->perm = strdup(colon2 + 1);
                if (!sel->perm)
                    goto cleanup;
            }

            char *bracket = strchr(seg_start, '[');
            if (bracket) {
                *bracket = '\0';
                sel->index = atoi(bracket + 1);
                if (sel->index < 0)
                    sel->index = 0;
            }

            if (!strcmp(seg_start, "text"))
                sel->seg_type = VMA_TYPE_TEXT;
            else if (!strcmp(seg_start, "rodata"))
                sel->seg_type = VMA_TYPE_RODATA;
            else if (!strcmp(seg_start, "data"))
                sel->seg_type = VMA_TYPE_DATA;
            else if (!strcmp(seg_start, "bss"))
                sel->seg_type = VMA_TYPE_BSS;
            else
                goto cleanup;
        } else {
            sel->perm = strdup(rest);
            if (!sel->perm)
                goto cleanup;
        }
    }

    free(buf);
    return sel;

cleanup:
    free(buf);
    if (sel) {
        free(sel->module);
        free(sel->perm);
        free(sel);
    }
    return NULL;
}

struct vma_select **to_vma_select(const char **s, int count)
{
    if (!s || count <= 0)
        return NULL;

    struct vma_select **select = calloc((size_t)count, sizeof(*select));
    if (!select)
        return NULL;

    for (int i = 0; i < count; i++) {
        select[i] = parse_vma_select(s[i]);
        if (!select[i]) {
            free_vma_select(select, i);
            return NULL;
        }
    }

    return select;
}

void free_vma_select(struct vma_select **select, int count)
{
    if (!select)
        return;

    for (int i = 0; i < count; i++) {
        if (select[i]) {
            free(select[i]->module);
            free(select[i]->perm);
            free(select[i]);
        }
    }

    free(select);
}