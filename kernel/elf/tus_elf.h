/*
 * tus_elf.h - running static ELF images on TUS
 *
 * Only ET_EXEC binaries (linked with `-static`) are supported; the
 * image currently runs on the kernel stack in ring 0.
 */

#ifndef TUS_ELF_TUS_ELF_H
#define TUS_ELF_TUS_ELF_H

/* Load the static ELF at `path` and jump to its entry point.
 * Returns 0 on success, a negative errno otherwise. */
long elf_exec(const char *path, int argc, char **argv);

#endif /* TUS_ELF_TUS_ELF_H */
