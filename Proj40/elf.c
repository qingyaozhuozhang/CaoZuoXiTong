/*
 * elf.c - resolve a dynamic symbol's runtime address inside a tracee, by
 * combining /proc/<pid>/maps load bias with the on-disk ELF symbol table.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <libgen.h>
#include "ludp_internal.h"

static int read_at(int fd, void *buf, size_t len, off_t off)
{
    ssize_t got = pread(fd, buf, len, off);
    return got == (ssize_t)len ? 0 : -1;
}

/* find the lowest mapping whose path contains `lib`; also record its path */
static int find_mapping(pid_t pid, const char *lib, uintptr_t *min_start,
                        char *path_out, size_t path_cap)
{
    char mp[64];
    snprintf(mp, sizeof mp, "/proc/%d/maps", pid);
    FILE *f = fopen(mp, "r");
    if (!f)
        return -1;

    char line[1024];
    int found = 0;
    uintptr_t lo = (uintptr_t)-1;
    char first_path[512] = {0};

    while (fgets(line, sizeof line, f)) {
        uintptr_t a, b;
        char perms[8], path[512] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %511s", &a, &b, perms, path);
        if (n < 3)
            continue;
        if (lib == NULL || lib[0] == '\0') {
            /* main executable: keep a marker, path will come from /proc/pid/exe */
            if (strstr(path, "vdso") || strstr(path, "vsyscall"))
                continue;
            if (a < lo) { lo = a; }
            found = 1;
        } else {
            if (strstr(path, lib)) {
                if (a < lo) { lo = a; }
                if (first_path[0] == '\0')
                    snprintf(first_path, sizeof first_path, "%s", path);
                found = 1;
            }
        }
    }
    fclose(f);
    if (!found)
        return -1;

    *min_start = lo;
    if (lib == NULL || lib[0] == '\0') {
        char exe[64];
        snprintf(exe, sizeof exe, "/proc/%d/exe", pid);
        ssize_t r = readlink(exe, path_out, path_cap - 1);
        if (r <= 0)
            return -1;
        path_out[r] = '\0';
    } else {
        snprintf(path_out, path_cap, "%s", first_path);
    }
    return 0;
}

int elf_resolve(pid_t pid, const char *lib, const char *sym, uintptr_t *addr,
                char *path_out, size_t path_cap)
{
    uintptr_t min_start = 0;
    char path[512];

    if (find_mapping(pid, (lib && lib[0]) ? lib : NULL, &min_start,
                     path, sizeof path) != 0)
        return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    Elf64_Ehdr eh;
    if (read_at(fd, &eh, sizeof eh, 0) != 0)
        goto fail;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64)
        goto fail;

    /* load bias = lowest mapping - lowest PT_LOAD vaddr */
    uintptr_t min_pvaddr = (uintptr_t)-1;
    {
        Elf64_Phdr ph;
        for (int i = 0; i < eh.e_phnum; i++) {
            if (read_at(fd, &ph, sizeof ph,
                        eh.e_phoff + (off_t)i * eh.e_phentsize) != 0)
                goto fail;
            if (ph.p_type == PT_LOAD && ph.p_vaddr < min_pvaddr)
                min_pvaddr = ph.p_vaddr;
        }
    }
    if (min_pvaddr == (uintptr_t)-1)
        min_pvaddr = 0;
    uintptr_t bias = min_start - min_pvaddr;

    /* find .dynsym + .dynstr */
    uintptr_t sym_off = 0, str_off = 0;
    size_t sym_sz = 0, str_sz = 0;
    size_t sym_entsz = sizeof(Elf64_Sym);

    /* GNU symbol versioning: needed so `exp`/`hypot` resolve to the same
     * address dlsym() picks -- the DEFAULT (base) version, not the first
     * symbol-table entry with the same name. */
    uint16_t *versym = NULL;
    size_t versym_n = 0;

    if (eh.e_shnum > 0 && eh.e_shoff != 0) {
        Elf64_Shdr *sh = malloc((size_t)eh.e_shnum * sizeof(Elf64_Shdr));
        if (!sh)
            goto fail;
        if (read_at(fd, sh, (size_t)eh.e_shnum * sizeof(Elf64_Shdr),
                    eh.e_shoff) != 0) {
            free(sh);
            goto fail;
        }
        Elf64_Shdr *strtab = &sh[eh.e_shstrndx];
        char *shnames = malloc(strtab->sh_size);
        if (!shnames) { free(sh); goto fail; }
        if (read_at(fd, shnames, strtab->sh_size, strtab->sh_offset) != 0) {
            free(shnames); free(sh); goto fail;
        }
        for (int i = 0; i < eh.e_shnum; i++) {
            const char *name = shnames + sh[i].sh_name;
            if (sh[i].sh_type == SHT_DYNSYM) {
                sym_off = sh[i].sh_offset;
                sym_sz = sh[i].sh_size;
                sym_entsz = sh[i].sh_entsize;
            } else if (sh[i].sh_type == SHT_STRTAB &&
                       strcmp(name, ".dynstr") == 0) {
                str_off = sh[i].sh_offset;
                str_sz = sh[i].sh_size;
            } else if (sh[i].sh_type == SHT_GNU_versym) {
                versym = malloc(sh[i].sh_size);
                if (versym && read_at(fd, versym, sh[i].sh_size,
                                      sh[i].sh_offset) == 0)
                    versym_n = sh[i].sh_size / sizeof(uint16_t);
                else {
                    free(versym); versym = NULL; versym_n = 0;
                }
            }
        }
        free(shnames);
        free(sh);
    }

    if (sym_off == 0 || str_off == 0) {
        /* fall back to PT_DYNAMIC (DT_SYMTAB / DT_STRTAB) */
        Elf64_Phdr ph;
        for (int i = 0; i < eh.e_phnum; i++) {
            if (read_at(fd, &ph, sizeof ph,
                        eh.e_phoff + (off_t)i * eh.e_phentsize) != 0)
                goto fail;
            if (ph.p_type != PT_DYNAMIC)
                continue;
            Elf64_Dyn d;
            off_t off = ph.p_offset;
            while (read_at(fd, &d, sizeof d, off) == 0 && d.d_tag != DT_NULL) {
                if (d.d_tag == DT_SYMTAB) sym_off = d.d_un.d_ptr - bias;
                if (d.d_tag == DT_STRTAB) str_off = d.d_un.d_ptr - bias;
                if (d.d_tag == DT_STRSZ) str_sz = d.d_un.d_val;
                off += sizeof d;
            }
        }
        if (sym_off == 0 || str_off == 0)
            goto fail;
    }

    char *strtab = malloc(str_sz);
    if (!strtab)
        goto fail;
    if (read_at(fd, strtab, str_sz, (off_t)str_off) != 0) {
        free(strtab);
        goto fail;
    }

    int rc = -1;
    int found_any = 0;
    uintptr_t any_addr = 0;
    Elf64_Sym s;
    for (size_t i = 0; i < sym_sz / sym_entsz; i++) {
        if (read_at(fd, &s, sizeof s, (off_t)(sym_off + i * sym_entsz)) != 0)
            break;
        uint32_t name = s.st_name;
        if (name == 0 || name >= str_sz)
            continue;
        if (strcmp(strtab + name, sym) != 0)
            continue;
        if (s.st_shndx == SHN_UNDEF)
            continue;
        uint8_t type = ELF64_ST_TYPE(s.st_info);
        if (type == STT_GNU_IFUNC) {
            /* IFUNC: the static st_value is the resolver, not the target. */
            rc = -2;
            break;
        }
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE)
            continue;

        uintptr_t a = bias + s.st_value;
        if (!found_any) {
            found_any = 1;
            any_addr = a;
        }

        /* Prefer the default-version definition (matches dlsym). In
         * .gnu.version, the hidden bit (0x8000) marks a non-default (@)
         * definition; an entry without it (value 1 or an unhidden index) is the
         * default (@@). No verdef walk is needed. */
        int is_default = 1;  /* assume default when no version info exists */
        if (versym && versym_n && i < versym_n) {
            uint16_t v = versym[i];
            is_default = (v == 1) || (!(v & 0x8000) && (v & 0x7fff) > 1);
        }

        if (is_default) {
            *addr = a;
            rc = 0;
            break;
        }
    }

    /* No explicit default version found; fall back to the first definition. */
    if (rc != 0 && found_any) {
        *addr = any_addr;
        rc = 0;
    }

    free(strtab);
    free(versym);
    if (path_out && path_cap)
        snprintf(path_out, path_cap, "%s", path);
    close(fd);
    return rc;

fail:
    close(fd);
    return -1;
}
