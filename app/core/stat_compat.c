/*
 * The Tizen 6 TV libc exports glibc's versioned stat entry points but not the
 * public stat/fstat aliases that the cross sysroot's headers emit. Keep the
 * structure ABI in the application toolchain headers and call the matching
 * glibc 2.4 ARM entry points directly.
 */
#include <sys/stat.h>
#include <stdlib.h>

extern int __xstat(int version, const char *path, struct stat *buffer);
extern int __fxstat(int version, int fd, struct stat *buffer);

int stat(const char *path, struct stat *buffer)
{
    return __xstat(3, path, buffer);
}

int fstat(int fd, struct stat *buffer)
{
    return __fxstat(3, fd, buffer);
}

/* Newer host ARM headers redirect strtol for C23 semantics; TV glibc 2.30
 * exports the older entry point only. PCSX's option strings do not need the
 * C23 0b-prefix behavior. */
long __isoc23_strtol(const char *text, char **end, int base)
{
    return strtol(text, end, base);
}
