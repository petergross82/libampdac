/* glibc 2.30 added gettid(). Volumio 3 is Debian Buster, glibc 2.28.
 * The shipped libampdac.so still calls gettid; this library defines it. */
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>

long gettid(void) {
    return syscall(SYS_gettid);
}
