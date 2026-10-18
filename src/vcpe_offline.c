/* Offline PSP libc bridge. SDK libcglue's _read/_write/_close pull its weak socket handlers into programs that
 * only use files. Override those hooks before libcglue is linked so an offline EBOOT does not import sceNetInet.
 * File descriptors still use the SDK's ordinary descriptor map and sceIo functions. No file I/O is replaced.
 * Link it as the vcpe_offline module (5.1); games that need networking must leave it out. */
#include <errno.h>
#include <sys/socket.h>

ssize_t recv(int fd, void *buffer, size_t size, int flags)
{
    (void)fd; (void)buffer; (void)size; (void)flags;
    errno = ENOSYS;
    return -1;
}

ssize_t send(int fd, const void *buffer, size_t size, int flags)
{
    (void)fd; (void)buffer; (void)size; (void)flags;
    errno = ENOSYS;
    return -1;
}

int __socket_close(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return -1;
}
