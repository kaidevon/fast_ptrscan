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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/sysmacros.h>

#include "vma/vm_area.h"

#define EI_NIDENT    16
#define ELFCLASS32   1
#define ELFCLASS64   2
#define ELFDATA2LSB  1

#define PT_LOAD      1
#define PF_X         1
#define PF_W         2

#define ELF_MAX_PHDRS 64

struct elf32_ehdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_ehdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

struct elf_seg {
    uint64_t vaddr;
    uint64_t offset;
    uint64_t filesz;
    uint64_t memsz;
    uint32_t flags;
};

struct elf_meta {
    int is_elf;
    int nsegs;
    struct elf_seg segs[ELF_MAX_PHDRS];
};

static int read_exact(int fd, void *buf, size_t len, off_t off)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = pread(fd, p, len, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p   += n;
        off += n;
        len -= (size_t)n;
    }
    return 0;
}

static int elf_parse(const char *path, struct elf_meta *out)
{
    memset(out, 0, sizeof(*out));

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    unsigned char ident[EI_NIDENT];
    if (read_exact(fd, ident, EI_NIDENT, 0) != 0)
        goto fail;
    if (ident[0] != 0x7f || ident[1] != 'E' ||
        ident[2] != 'L'  || ident[3] != 'F')
        goto fail;
    if (ident[5] != ELFDATA2LSB)
        goto fail;

    int k = 0;

    if (ident[4] == ELFCLASS64) {
        struct elf64_ehdr eh;
        if (read_exact(fd, &eh, sizeof(eh), 0) != 0)          goto fail;
        if (eh.e_phentsize != sizeof(struct elf64_phdr))       goto fail;
        if (eh.e_phnum == 0)                                   goto fail;

        int n = eh.e_phnum > ELF_MAX_PHDRS ? ELF_MAX_PHDRS : eh.e_phnum;
        for (int i = 0; i < n; i++) {
            struct elf64_phdr ph;
            off_t poff = (off_t)eh.e_phoff + (off_t)i * sizeof(ph);
            if (read_exact(fd, &ph, sizeof(ph), poff) != 0) goto fail;
            if (ph.p_type != PT_LOAD) continue;
            out->segs[k].vaddr  = ph.p_vaddr;
            out->segs[k].offset = ph.p_offset;
            out->segs[k].filesz = ph.p_filesz;
            out->segs[k].memsz  = ph.p_memsz;
            out->segs[k].flags  = ph.p_flags;
            k++;
        }
    } else if (ident[4] == ELFCLASS32) {
        struct elf32_ehdr eh;
        if (read_exact(fd, &eh, sizeof(eh), 0) != 0)          goto fail;
        if (eh.e_phentsize != sizeof(struct elf32_phdr))       goto fail;
        if (eh.e_phnum == 0)                                   goto fail;

        int n = eh.e_phnum > ELF_MAX_PHDRS ? ELF_MAX_PHDRS : eh.e_phnum;
        for (int i = 0; i < n; i++) {
            struct elf32_phdr ph;
            off_t poff = (off_t)eh.e_phoff + (off_t)i * sizeof(ph);
            if (read_exact(fd, &ph, sizeof(ph), poff) != 0) goto fail;
            if (ph.p_type != PT_LOAD) continue;
            out->segs[k].vaddr  = ph.p_vaddr;
            out->segs[k].offset = ph.p_offset;
            out->segs[k].filesz = ph.p_filesz;
            out->segs[k].memsz  = ph.p_memsz;
            out->segs[k].flags  = ph.p_flags;
            k++;
        }
    } else {
        goto fail;
    }

    close(fd);
    out->is_elf = 1;
    out->nsegs  = k;
    return 0;

fail:
    close(fd);
    out->is_elf = 0;
    out->nsegs  = 0;
    return -1;
}

struct elf_cache_entry {
    char            *path;
    struct elf_meta  meta;
};

struct elf_cache {
    struct elf_cache_entry *entries;
    int count;
    int cap;
};

static const struct elf_meta *elf_cache_get(struct elf_cache *c,
                                            const char *path)
{
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->entries[i].path, path) == 0)
            return c->entries[i].meta.is_elf ? &c->entries[i].meta : NULL;
    }

    if (c->count == c->cap) {
        int nc = c->cap ? c->cap * 2 : 16;
        struct elf_cache_entry *ne =
            realloc(c->entries, (size_t)nc * sizeof(*ne));
        if (!ne) return NULL;
        c->entries = ne;
        c->cap = nc;
    }

    struct elf_cache_entry *e = &c->entries[c->count];
    e->path = strdup(path);
    if (!e->path) return NULL;

    if (elf_parse(path, &e->meta) != 0) {
        e->meta.is_elf = 0;
        e->meta.nsegs  = 0;
    }
    c->count++;
    return e->meta.is_elf ? &e->meta : NULL;
}

static void elf_cache_free(struct elf_cache *c)
{
    if (!c) return;
    for (int i = 0; i < c->count; i++)
        free(c->entries[i].path);
    free(c->entries);
    c->entries = NULL;
    c->count = c->cap = 0;
}

static int elf_load_bias(const struct vm_area *v,
                         const struct elf_meta *m,
                         uint64_t *bias_out)
{
    for (int i = 0; i < m->nsegs; i++) {
        const struct elf_seg *s = &m->segs[i];
        if ((uint64_t)v->offset >= s->offset &&
            (uint64_t)v->offset <  s->offset + s->filesz) {
            *bias_out = v->start - (s->vaddr +
                        ((uint64_t)v->offset - s->offset));
            return 0;
        }
    }
    return -1;
}

/*
 * Hash table used by the fourth pass of vma_elf() to assign seg_index in O(n).
 *
 * Key: (module pointer, seg_type).
 * The VMA list built by parse_maps() follows /proc/pid/maps order, which is
 * ascending by start address, so a running per-key counter gives the same
 * result as the original "count w->start < v->start" loop.
 */
struct seg_idx_entry {
    struct vm_area *module;
    uint32_t        seg_type;
    uint32_t        count;
    uint8_t         used;
};

static size_t seg_idx_hash(struct vm_area *module, uint32_t seg_type)
{
    uint64_t h = (uint64_t)(uintptr_t)module;
    h ^= (uint64_t)seg_type * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (size_t)h;
}

static uint32_t seg_idx_assign(struct seg_idx_entry *tab, size_t mask,
                               struct vm_area *module, uint32_t seg_type)
{
    size_t i = seg_idx_hash(module, seg_type) & mask;
    for (;;) {
        if (!tab[i].used) {
            tab[i].used     = 1;
            tab[i].module   = module;
            tab[i].seg_type = seg_type;
            tab[i].count    = 1;
            return 0;
        }
        if (tab[i].module == module && tab[i].seg_type == seg_type)
            return tab[i].count++;
        i = (i + 1) & mask;
    }
}

int vma_elf(struct vm_area *head)
{
    if (!head)
        return -1;

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        page_size = 4096;
    uint64_t pmask = (uint64_t)page_size - 1;

    /* Initialize all VMAs */
    for (struct vm_area *v = head; v; v = v->next) {
        v->seg_type  = VMA_TYPE_UNKNOWN;
        v->seg_index = 0;
        v->module    = NULL;
    }

    struct elf_cache cache = {0};

    /* First pass: identify executable segments (.text) as module roots */
    for (struct vm_area *v = head; v; v = v->next) {
        if (!v->pathname)
            continue;

        const struct elf_meta *m = elf_cache_get(&cache, v->pathname);
        if (!m)
            continue;

        uint64_t bias;
        if (elf_load_bias(v, m, &bias) != 0)
            bias = v->start - (uint64_t)v->offset;

        for (int i = 0; i < m->nsegs; i++) {
            const struct elf_seg *s = &m->segs[i];
            if (!(s->flags & PF_X))
                continue;
            uint64_t seg_start = bias + s->vaddr;
            uint64_t seg_end   = seg_start + s->filesz;
            if (v->start >= seg_start && v->start < seg_end) {
                v->seg_type = VMA_TYPE_TEXT;
                v->module   = v;
                break;
            }
        }
    }

    /* Second pass: assign segment types and modules to the remaining VMAs based on ELF program headers */
    for (struct vm_area *v = head; v; v = v->next) {
        if (v->seg_type != VMA_TYPE_UNKNOWN || !v->pathname)
            continue;

        const struct elf_meta *m = elf_cache_get(&cache, v->pathname);
        if (!m)
            continue;

        uint64_t bias;
        if (elf_load_bias(v, m, &bias) != 0)
            bias = v->start - (uint64_t)v->offset;

        int seg_type = VMA_TYPE_UNKNOWN;
        for (int i = 0; i < m->nsegs; i++) {
            const struct elf_seg *s = &m->segs[i];
            uint64_t seg_start = bias + s->vaddr;
            uint64_t seg_end   = seg_start + s->filesz;
            if (v->start >= seg_start && v->start < seg_end) {
                if (s->flags & PF_X)
                    seg_type = VMA_TYPE_TEXT;
                else if (!(s->flags & PF_W))
                    seg_type = VMA_TYPE_RODATA;
                else
                    seg_type = VMA_TYPE_DATA;
                break;
            }
        }

        if (seg_type != VMA_TYPE_UNKNOWN) {
            v->seg_type = seg_type;

            struct vm_area *root = NULL;
            for (struct vm_area *w = head; w; w = w->next) {
                if (w->seg_type == VMA_TYPE_TEXT &&
                    w->pathname && strcmp(w->pathname, v->pathname) == 0) {
                    root = w;
                    break;
                }
            }
            if (!root)
                root = v;
            v->module = root;
        }
    }

    /* Third pass: identify .bss (anonymous writable segments that are writable and do not belong to any file) */
    for (struct vm_area *v = head; v; v = v->next) {
        if (v->seg_type != VMA_TYPE_UNKNOWN)
            continue;
        if (v->perm[1] != 'w')
            continue;

        for (struct vm_area *root = head; root; root = root->next) {
            if (root->seg_type != VMA_TYPE_TEXT || !root->pathname)
                continue;

            const struct elf_meta *m =
                elf_cache_get(&cache, root->pathname);
            if (!m)
                continue;

            uint64_t bias;
            if (elf_load_bias(root, m, &bias) != 0)
                bias = root->start - (uint64_t)root->offset;

            int in_bss = 0;
            for (int i = 0; i < m->nsegs && !in_bss; i++) {
                const struct elf_seg *s = &m->segs[i];
                if (s->memsz <= s->filesz)
                    continue;

                uint64_t bss_start = bias + s->vaddr + s->filesz;
                uint64_t bss_end   = bias + s->vaddr + s->memsz;
                bss_start = (bss_start + pmask) & ~pmask;
                bss_end   = (bss_end   + pmask) & ~pmask;

                if (v->start >= bss_start && v->start < bss_end)
                    in_bss = 1;
            }

            if (in_bss) {
                v->seg_type = VMA_TYPE_BSS;
                v->module   = root;
                break;
            }
        }
    }

    elf_cache_free(&cache);

    /*
     * Fourth pass: compute seg_index (numbered in ascending order of start
     * within the same module and seg_type).
     *
     * The list from parse_maps() is in /proc/pid/maps order, i.e. ascending
     * by start address, so we can assign indices with a single pass using a
     * hash table keyed by (module, seg_type). This replaces the previous
     * O(n^2) double loop with O(n).
     */
    {
        size_t n = 0;
        for (struct vm_area *v = head; v; v = v->next)
            n++;

        size_t cap = 16;
        while (cap < n * 2)
            cap <<= 1;
        size_t mask = cap - 1;

        struct seg_idx_entry *tab = calloc(cap, sizeof(*tab));
        if (tab) {
            for (struct vm_area *v = head; v; v = v->next) {
                if (v->seg_type == VMA_TYPE_UNKNOWN || !v->module) {
                    v->seg_index = 0;
                    continue;
                }
                v->seg_index =
                    seg_idx_assign(tab, mask, v->module, v->seg_type);
            }
            free(tab);
        } else {
            /* Out of memory: degrade gracefully by zeroing all indices. */
            for (struct vm_area *v = head; v; v = v->next)
                v->seg_index = 0;
        }
    }

    return 0;
}

int parse_maps(pid_t pid, struct vm_area **vm_area_out) {
    if (!vm_area_out)
        return -EINVAL;

    *vm_area_out = NULL;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;

    /* Read the entire maps file */
    size_t cap = 64 * 1024;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        return -ENOMEM;
    }

    for (;;) {
        if (len == cap) {
            size_t new_cap = cap * 2;
            char *tmp = realloc(buf, new_cap);
            if (!tmp) {
                free(buf);
                close(fd);
                return -ENOMEM;
            }
            buf = tmp;
            cap = new_cap;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return -errno;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    close(fd);

    struct vm_area *head = NULL, *tail = NULL;
    int ret = 0;

    char *p   = buf;
    char *end = buf + len;

    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len;
        if (nl) {
            line_len = (size_t)(nl - p);
        } else {
            line_len = (size_t)(end - p);
        }

        if (line_len == 0) {
            if (!nl)
                break;
            p = nl + 1;
            continue;
        }

        /* Copy into an independent NUL-terminated string before parsing */
        char *line = malloc(line_len + 1);
        if (!line) {
            ret = -ENOMEM;
            goto cleanup;
        }
        memcpy(line, p, line_len);
        line[line_len] = '\0';

        uint64_t start = 0, end_addr = 0;
        uint64_t offset = 0, inode = 0;
        uint64_t dev_major = 0, dev_minor = 0;
        char perm_str[5] = {0};
        int consumed = 0;

        /*
         * Note: do not put a space before %n, otherwise it will skip the newline.
         * Spaces should only appear between required delimiter fields.
         */
        int matched = sscanf(line,
            "%" SCNx64 "-%" SCNx64 " %4s %" SCNx64
            " %" SCNx64 ":%" SCNx64 " %" SCNu64 "%n",
            &start, &end_addr, perm_str,
            &offset, &dev_major, &dev_minor,
            &inode, &consumed);

        if (matched != 7) {
            free(line);
            if (!nl)
                break;
            p = nl + 1;
            continue;
        }

        size_t pos = ((size_t)consumed <= line_len) ? (size_t)consumed : line_len;

        while (pos < line_len && (line[pos] == ' ' || line[pos] == '\t'))
            pos++;

        char *pathname = NULL;
        if (pos < line_len) {
            size_t path_len = line_len - pos;
            while (path_len > 0 &&
                   (line[pos + path_len - 1] == ' ' ||
                    line[pos + path_len - 1] == '\t')) {
                path_len--;
            }
            if (path_len > 0) {
                pathname = malloc(path_len + 1);
                if (!pathname) {
                    free(line);
                    ret = -ENOMEM;
                    goto cleanup;
                }
                memcpy(pathname, line + pos, path_len);
                pathname[path_len] = '\0';
            }
        }

        free(line);

        struct vm_area *node = calloc(1, sizeof(struct vm_area));
        if (!node) {
            free(pathname);
            ret = -ENOMEM;
            goto cleanup;
        }

        node->start  = start;
        node->end    = end_addr;
        node->offset = (off_t)offset;
        node->dev    = makedev((unsigned)dev_major, (unsigned)dev_minor);
        node->inode  = (ino_t)inode;
        node->pathname = pathname;

        memcpy(node->perm, perm_str, 4);
        node->perm[4] = '\0';

        node->seg_type  = 0;
        node->seg_index = 0;
        node->module    = NULL;
        node->next      = NULL;

        if (!head) head = tail = node;
        else { tail->next = node; tail = node; }

        if (!nl)
            break;
        p = nl + 1;
    }

    free(buf);
    *vm_area_out = head;
    return 0;

cleanup:
    free(buf);
    free_vm_area(head);
    return ret;
}

void free_vm_area(struct vm_area *head) {
    while (head) {
        struct vm_area *next = head->next;
        free(head->pathname);
        free(head);
        head = next;
    }
}

struct vm_area* vm_area_copy(const struct vm_area *src) {
    if (!src) return NULL;

    struct vm_area *head = NULL, *tail = NULL;

    for (const struct vm_area *cur = src; cur; cur = cur->next) {
        struct vm_area *node = malloc(sizeof(struct vm_area));
        if (!node) {
            while (head) {
                struct vm_area *tmp = head;
                head = head->next;
                free(tmp->pathname);
                free(tmp);
            }
            return NULL;
        }

        *node = *cur;
        node->next = NULL;
        node->module = NULL;

        if (cur->pathname) {
            size_t len = strlen(cur->pathname) + 1;
            node->pathname = malloc(len);
            if (!node->pathname) {
                free(node);
                while (head) {
                    struct vm_area *tmp = head;
                    head = head->next;
                    free(tmp->pathname);
                    free(tmp);
                }
                return NULL;
            }
            memcpy(node->pathname, cur->pathname, len);
        } else {
            node->pathname = NULL;
        }

        if (!head) {
            head = tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
    }

    return head;
}

struct vm_list* vma_copy2_vm(const struct vm_area *src) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return NULL;

    struct vm_list *head = NULL, *tail = NULL;
    struct vm_list *current = NULL;

    for (const struct vm_area *vma = src; vma; vma = vma->next) {
        uint64_t start = vma->start;
        uint64_t end   = vma->end;

        for (uint64_t addr = start; addr < end; addr += page_size) {
            if (!current || current->used == VM_LIST_CAPACITY) {
                struct vm_list *node = calloc(1, sizeof(*node));
                if (!node) {
                    while (head) {
                        struct vm_list *tmp = head;
                        head = head->next;
                        free(tmp);
                    }
                    return NULL;
                }
                if (!head) {
                    head = tail = node;
                } else {
                    tail->next = node;
                    tail = node;
                }
                current = node;
            }

            current->addr[current->used++] = addr;
        }
    }

    return head;
}