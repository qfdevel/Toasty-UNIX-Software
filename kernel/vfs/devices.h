/*
 * devices.h - built-in device nodes
 *
 * Registers the kernel's devices under /dev. Each device implements
 * the VFS file_ops interface; the shell reaches them through the
 * normal open/read/write/ioctl path (i.e. through POSIX syscalls).
 */

#ifndef TUS_VFS_DEVICES_H
#define TUS_VFS_DEVICES_H

#include <stdint.h>

#include <limine.h>

/* ioctl requests for /dev/fb0. */
#define FB_IOCTL_GET_INFO 0x1 /* arg: struct fb_device_info * */
#define FB_IOCTL_FILL     0x2 /* arg: uint32_t rgb color        */

/* Console input ownership is handled by the keyboard driver
 * (kbd_input_owner/release); see keyboard.h. */

struct fb_device_info {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint64_t pitch;
    uint64_t address;
};

/* Create all /dev nodes. Called by vfs_init(). */
void devices_init(void);

#endif /* TUS_VFS_DEVICES_H */
