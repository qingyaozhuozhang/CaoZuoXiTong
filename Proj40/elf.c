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
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include "ludp_internal.h"

static int read_at(int fd, void *buf, size_t len, off_t off)
{
    ssize_t got = pread(fd, buf, len, off);
    return got == (ssize_t)len ? 0 : -1;
}

static int vaddr_to_offset(int fd, const Elf64_Ehdr *eh, Elf64_Addr vaddr,
                           off_t *offset)
{
    for (int i = 0; i < eh->e_phnum; ++i) {
        Elf64_Phdr ph;
        if (read_at(fd, &ph, sizeof ph,
                    eh->e_phoff + (off_t)i * eh->e_phentsize) != 0)
            return -1;
        if (ph.p_type != PT_LOAD || vaddr < ph.p_vaddr ||
            vaddr - ph.p_vaddr >= ph.p_filesz)
            continue;
        *offset = (off_t)ph.p_offset + (off_t)(vaddr - ph.p_vaddr);
        return 0;
    }
    errno = ENOEXEC;
    return -1;
}

/* Derive the dynsym count from DT_GNU_HASH when section headers are absent. */
static int gnu_hash_symbol_count(int fd, off_t hash_off, off_t file_size,
                                 size_t *count)
{
    uint32_t header[4];
    if (read_at(fd, header, sizeof header, hash_off) != 0)
        return -1;
    uint32_t nbuckets = header[0], symoffset = header[1];
    uint32_t bloom_size = header[2];
    if (!nbuckets || !bloom_size) {
        errno = ENOEXEC;
        return -1;
    }
    uint64_t buckets_pos = (uint64_t)hash_off + sizeof header +
                           (uint64_t)bloom_size * sizeof(Elf64_Xword);
    uint64_t chains_pos = buckets_pos + (uint64_t)nbuckets * sizeof(uint32_t);
    if (buckets_pos > (uint64_t)file_size ||
        chains_pos > (uint64_t)file_size) {
        errno = ENOEXEC;
        return -1;
    }

    uint32_t max_bucket = 0;
    for (uint32_t i = 0; i < nbuckets; ++i) {
        uint32_t bucket;
        if (read_at(fd, &bucket, sizeof bucket,
                    (off_t)(buckets_pos + (uint64_t)i * sizeof bucket)) != 0)
            return -1;
        if (bucket > max_bucket)
            max_bucket = bucket;
    }
    if (max_bucket < symoffset) {
        *count = symoffset;
        return 0;
    }

    uint64_t index = max_bucket;
    for (;;) {
        uint64_t chain_pos = chains_pos +
            (index - symoffset) * sizeof(uint32_t);
        if (chain_pos + sizeof(uint32_t) > (uint64_t)file_size) {
            errno = ENOEXEC;
            return -1;
        }
        uint32_t hash;
        if (read_at(fd, &hash, sizeof hash, (off_t)chain_pos) != 0)
            return -1;
        ++index;
        if (hash & 1)
            break;
    }
    *count = (size_t)index;
    return 0;
}

static void normalize_map_path(char *path)
{
    path[strcspn(path, "\r\n")] = '\0';
    size_t len = strlen(path);
    static const char deleted[] = " (deleted)";
    if (len >= sizeof deleted - 1 &&
        strcmp(path + len - (sizeof deleted - 1), deleted) == 0)
        path[len - (sizeof deleted - 1)] = '\0';
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Find the lowest mapping of the requested ELF object.  For the main
 * executable, matching /proc/<pid>/exe exactly avoids accidentally using a
 * lower anonymous mapping as the PIE load base.  Library basenames and full
 * paths are preferred over the legacy substring fallback. */
static int find_mapping(pid_t pid, const char *lib, uintptr_t *min_start,
                        char *path_out, size_t path_cap)
{
    if (pid <= 0 || !min_start || !path_out || path_cap == 0) {
        errno = EINVAL;
        return -1;
    }

    bool main_executable = lib == NULL || lib[0] == '\0';
    char exe_link[64] = {0}, exe_target[512] = {0};
    if (main_executable) {
        snprintf(exe_link, sizeof exe_link, "/proc/%d/exe", pid);
        ssize_t n = readlink(exe_link, exe_target, sizeof exe_target - 1);
        if (n <= 0)
            return -1;
        exe_target[n] = '\0';
        normalize_map_path(exe_target);
    }

    char mp[64];
    snprintf(mp, sizeof mp, "/proc/%d/maps", pid);
    FILE *f = fopen(mp, "r");
    if (!f)
        return -1;

    char line[1024];
    int found = 0, best_priority = 0;
    uintptr_t lo = (uintptr_t)-1;
    char first_path[512] = {0};

    while (fgets(line, sizeof line, f)) {
        uintptr_t a, b;
        unsigned long file_offset = 0;
        char perms[8];
        int pos = 0;
        int n = sscanf(line, "%lx-%lx %7s %lx %*s %*s %n",
                       &a, &b, perms, &file_offset, &pos);
        (void)b;
        (void)file_offset;
        if (n < 4 || pos <= 0)
            continue;
        char *path = line + pos;
        while (*path && isspace((unsigned char)*path))
            ++path;
        if (*path != '/')
            continue;
        normalize_map_path(path);

        int priority = 0;
        if (main_executable) {
            priority = strcmp(path, exe_target) == 0 ? 3 : 0;
        } else {
            if (strcmp(path, lib) == 0 ||
                strcmp(path_basename(path), lib) == 0)
                priority = 2;
            else if (strstr(path, lib))
                priority = 1;
        }
        if (!priority || priority < best_priority)
            continue;
        if (priority > best_priority) {
            best_priority = priority;
            lo = (uintptr_t)-1;
            first_path[0] = '\0';
        }
        if (a < lo) {
            lo = a;
            snprintf(first_path, sizeof first_path, "%s", path);
        }
        found = 1;
    }
    fclose(f);
    if (!found) {
        errno = ENOENT;
        return -1;
    }

    *min_start = lo;
    int written = snprintf(path_out, path_cap, "%s",
                           main_executable ? exe_link : first_path);
    if (written < 0 || (size_t)written >= path_cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int elf_resolve(pid_t pid, const char *lib, const char *sym, uintptr_t *addr,
                char *path_out, size_t path_cap)
{
    if (pid <= 0 || !sym || !*sym || !addr || (path_out && !path_cap)) {
        errno = EINVAL;
        return -1;
    }

    uintptr_t min_start = 0;
    char path[512] = {0};
    uint16_t *versym = NULL;
    char *strtab = NULL;
    int fd = -1, rc = -1;

    if (find_mapping(pid, (lib && lib[0]) ? lib : NULL, &min_start,
                     path, sizeof path) != 0)
        return -1;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0)
        goto cleanup;
    if (st.st_size <= 0)
        goto malformed;

    Elf64_Ehdr eh;
    if (read_at(fd, &eh, sizeof eh, 0) != 0 ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB ||
        eh.e_ident[EI_VERSION] != EV_CURRENT ||
#ifdef __aarch64__
        eh.e_machine != EM_AARCH64 ||
#else
        eh.e_machine != EM_X86_64 ||
#endif
        eh.e_version != EV_CURRENT || eh.e_ehsize != sizeof(Elf64_Ehdr) ||
        eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0) {
        errno = ENOEXEC;
        goto cleanup;
    }

    /* load bias = lowest mapping - lowest PT_LOAD vaddr */
    uintptr_t min_pvaddr = (uintptr_t)-1;
    {
        Elf64_Phdr ph;
        for (int i = 0; i < eh.e_phnum; i++) {
            if (read_at(fd, &ph, sizeof ph,
                        eh.e_phoff + (off_t)i * eh.e_phentsize) != 0)
                goto malformed;
            if (ph.p_type == PT_LOAD && ph.p_vaddr < min_pvaddr)
                min_pvaddr = ph.p_vaddr;
        }
    }
    if (min_pvaddr == (uintptr_t)-1)
        goto malformed;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || ((unsigned long)page_size &
                           ((unsigned long)page_size - 1)) != 0)
        page_size = 4096;
    min_pvaddr &= ~((uintptr_t)page_size - 1);
    if (min_start < min_pvaddr)
        goto malformed;
    uintptr_t bias = min_start - min_pvaddr;

    /* find .dynsym + .dynstr */
    uintptr_t sym_off = 0, str_off = 0;
    size_t sym_sz = 0, str_sz = 0;
    size_t sym_entsz = sizeof(Elf64_Sym);

    /* GNU symbol versioning: needed so `exp`/`hypot` resolve to the same
     * address dlsym() picks -- the DEFAULT (base) version, not the first
     * symbol-table entry with the same name. */
    size_t versym_n = 0;

    if (eh.e_shnum > 0 && eh.e_shoff != 0) {
        if (eh.e_shentsize != sizeof(Elf64_Shdr) ||
            eh.e_shstrndx >= eh.e_shnum)
            goto malformed;
        Elf64_Shdr *sh = malloc((size_t)eh.e_shnum * sizeof(Elf64_Shdr));
        if (!sh)
            goto cleanup;
        if (read_at(fd, sh, (size_t)eh.e_shnum * sizeof(Elf64_Shdr),
                    eh.e_shoff) != 0) {
            free(sh);
            goto malformed;
        }
        Elf64_Shdr *strtab = &sh[eh.e_shstrndx];
        if (!strtab->sh_size || strtab->sh_offset > (Elf64_Off)st.st_size ||
            strtab->sh_size > (Elf64_Xword)st.st_size - strtab->sh_offset) {
            free(sh);
            goto malformed;
        }
        char *shnames = malloc(strtab->sh_size);
        if (!shnames) { free(sh); goto cleanup; }
        if (read_at(fd, shnames, strtab->sh_size, strtab->sh_offset) != 0) {
            free(shnames); free(sh); goto malformed;
        }
        for (int i = 0; i < eh.e_shnum; i++) {
            if (sh[i].sh_name >= strtab->sh_size)
                continue;
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

    if (sym_off == 0 || str_off == 0 || sym_sz == 0) {
        /* Sectionless ELF fallback: translate dynamic virtual addresses back
         * to file offsets and derive the symbol count from DT_HASH/GNU_HASH. */
        Elf64_Addr sym_va = 0, str_va = 0, hash_va = 0, gnu_hash_va = 0;
        size_t dyn_str_sz = 0, dyn_sym_ent = sizeof(Elf64_Sym);
        Elf64_Phdr ph;
        for (int i = 0; i < eh.e_phnum; i++) {
            if (read_at(fd, &ph, sizeof ph,
                        eh.e_phoff + (off_t)i * eh.e_phentsize) != 0)
                goto malformed;
            if (ph.p_type != PT_DYNAMIC)
                continue;
            Elf64_Dyn d;
            off_t off = ph.p_offset;
            size_t entries = (size_t)(ph.p_filesz / sizeof d);
            for (size_t k = 0; k < entries; ++k) {
                if (read_at(fd, &d, sizeof d, off) != 0)
                    goto malformed;
                if (d.d_tag == DT_NULL)
                    break;
                if (d.d_tag == DT_SYMTAB) sym_va = d.d_un.d_ptr;
                if (d.d_tag == DT_STRTAB) str_va = d.d_un.d_ptr;
                if (d.d_tag == DT_STRSZ) dyn_str_sz = d.d_un.d_val;
                if (d.d_tag == DT_SYMENT) dyn_sym_ent = d.d_un.d_val;
                if (d.d_tag == DT_HASH) hash_va = d.d_un.d_ptr;
                if (d.d_tag == DT_GNU_HASH) gnu_hash_va = d.d_un.d_ptr;
                off += sizeof d;
            }
        }
        off_t converted = 0;
        if (!sym_va || !str_va || !dyn_str_sz ||
            vaddr_to_offset(fd, &eh, sym_va, &converted) != 0)
            goto malformed;
        sym_off = (uintptr_t)converted;
        if (vaddr_to_offset(fd, &eh, str_va, &converted) != 0)
            goto malformed;
        str_off = (uintptr_t)converted;
        str_sz = dyn_str_sz;
        sym_entsz = dyn_sym_ent;

        size_t symbol_count = 0;
        if (hash_va && vaddr_to_offset(fd, &eh, hash_va, &converted) == 0) {
            uint32_t hash_header[2];
            if (read_at(fd, hash_header, sizeof hash_header, converted) != 0)
                goto malformed;
            symbol_count = hash_header[1];
        } else if (gnu_hash_va &&
                   vaddr_to_offset(fd, &eh, gnu_hash_va, &converted) == 0) {
            if (gnu_hash_symbol_count(fd, converted, st.st_size,
                                      &symbol_count) != 0)
                goto malformed;
        }
        if (!symbol_count || sym_entsz < sizeof(Elf64_Sym) ||
            symbol_count > SIZE_MAX / sym_entsz)
            goto malformed;
        sym_sz = symbol_count * sym_entsz;
    }

    if (!str_sz || sym_entsz < sizeof(Elf64_Sym) ||
        sym_off > (uintptr_t)st.st_size || str_off > (uintptr_t)st.st_size ||
        sym_sz > (size_t)st.st_size - sym_off ||
        str_sz > (size_t)st.st_size - str_off)
        goto malformed;

    strtab = malloc(str_sz);
    if (!strtab)
        goto cleanup;
    if (read_at(fd, strtab, str_sz, (off_t)str_off) != 0) {
        goto malformed;
    }

    int found_any = 0;
    uintptr_t any_addr = 0;
    Elf64_Sym s;
    for (size_t i = 0; i < sym_sz / sym_entsz; i++) {
        if (read_at(fd, &s, sizeof s, (off_t)(sym_off + i * sym_entsz)) != 0)
            break;
        uint32_t name = s.st_name;
        if (name == 0 || name >= str_sz)
            continue;
        if (!memchr(strtab + name, '\0', str_sz - name) ||
            strcmp(strtab + name, sym) != 0)
            continue;
        if (s.st_shndx == SHN_UNDEF)
            continue;
        uint8_t type = ELF64_ST_TYPE(s.st_info);
        if (type == STT_GNU_IFUNC) {
            /* IFUNC: the static st_value is the resolver, not the target. */
            errno = ENOTSUP;
            goto cleanup;
        }
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE)
            continue;

        if (s.st_value > UINTPTR_MAX - bias)
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

    if (rc != 0)
        errno = ENOENT;
    if (rc == 0 && path_out) {
        int written = snprintf(path_out, path_cap, "%s", path);
        if (written < 0 || (size_t)written >= path_cap) {
            errno = ENAMETOOLONG;
            rc = -1;
        }
    }
    goto cleanup;

malformed:
    errno = ENOEXEC;
cleanup: {
        int err = errno;
        free(strtab);
        free(versym);
        if (fd >= 0)
            close(fd);
        errno = err;
        return rc;
    }
}
