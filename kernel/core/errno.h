/*
 * errno.h - kernel error codes (POSIX-style)
 *
 * Syscalls and VFS operations return a negative errno on failure,
 * mirroring the UNIX convention. Values match the classic errno.h.
 */

#ifndef TUS_CORE_ERRNO_H
#define TUS_CORE_ERRNO_H

#define EPERM   1  /* operation not permitted */
#define ENOENT  2  /* no such file or directory */
#define EIO     5  /* I/O error */
#define EBADF   9  /* bad file descriptor */
#define EAGAIN  11 /* resource temporarily unavailable */
#define ENOMEM  12 /* out of memory */
#define EEXIST  17 /* file exists */
#define ENOTDIR 20 /* not a directory */
#define EISDIR  21 /* is a directory */
#define EINVAL  22 /* invalid argument */
#define ENOTTY  25 /* inappropriate ioctl for device */
#define ENOSPC  28 /* no space left on device */
#define ENOSYS  38 /* function not implemented */

#endif /* TUS_CORE_ERRNO_H */
