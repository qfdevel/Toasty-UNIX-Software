#!/usr/bin/env python3
"""
test_boot.py - automated boot test for TUS

Boots tus.iso in headless QEMU, waits for the TUS shell on the serial
log, then types commands through the virtual PS/2 keyboard (QMP
"sendkey") and verifies every response. Ends by triggering the kernel
panic handler and checking the register dump.

Usage: python3 tests/test_boot.py   (from the project root)
"""

import json
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/tus-serial.log"
QEMU_LOG   = "/tmp/tus-qemu.log"
QMP_SOCK   = "/tmp/tus-qmp.sock"
SCREEN_PPM = "/tmp/tus-screen.ppm"
BOOT_TIMEOUT = 60

PASS = 0


def ok(name):
    global PASS
    PASS += 1
    print(f"  [PASS] {name}")


def wait_for(needle, timeout=BOOT_TIMEOUT, offset=0):
    """Wait until `needle` appears in the serial log; return new offset."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(SERIAL_LOG, "rb") as f:
                f.seek(offset)
                data = f.read().decode("utf-8", "replace")
        except FileNotFoundError:
            time.sleep(0.1)  # QEMU has not created the log file yet
            continue
        if needle in data:
            return offset + data.index(needle) + len(needle)
        time.sleep(0.1)
    with open(SERIAL_LOG, "rb") as f:
        tail = f.read().decode("utf-8", "replace")[-800:]
    raise AssertionError(f"timeout waiting for {needle!r}; log tail:\n{tail}")


def qmp_connect():
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            sock.connect(QMP_SOCK)
            break
        except (ConnectionRefusedError, FileNotFoundError):
            time.sleep(0.1)
    else:
        raise AssertionError("cannot connect to QEMU QMP socket")
    sock.recv(4096)  # greeting
    sock.sendall(b'{"execute":"qmp_capabilities"}\n')
    sock.recv(4096)
    return sock


def qmp_cmd(sock, execute, arguments=None):
    cmd = {"execute": execute}
    if arguments is not None:
        cmd["arguments"] = arguments
    sock.sendall((json.dumps(cmd) + "\n").encode())
    return sock.recv(65536)


def sendkey(sock, key):
    qmp_cmd(sock, "human-monitor-command",
            {"command-line": f"sendkey {key}"})
    time.sleep(0.03)


def type_text(sock, text):
    for ch in text:
        if ch == "\r":
            sendkey(sock, "ret")
        elif ch == "\b":
            sendkey(sock, "backspace")
        elif ch == " ":
            sendkey(sock, "spc")
        elif ch == "/":
            sendkey(sock, "slash")
        elif ch == ">":
            sendkey(sock, "shift-dot")
        elif ch == ".":
            sendkey(sock, "dot")
        elif ch == "-":
            sendkey(sock, "minus")
        elif ch == "_":
            sendkey(sock, "shift-minus")
        elif ch.isupper():
            sendkey(sock, f"shift-{ch.lower()}")
        elif ch.isalpha() or ch.isdigit():
            sendkey(sock, ch)
        else:
            raise AssertionError(f"no sendkey mapping for {ch!r}")
        time.sleep(0.05)


def start_qemu():
    for stale in (SERIAL_LOG, QEMU_LOG, QMP_SOCK, SCREEN_PPM):
        try:
            os.unlink(stale)
        except FileNotFoundError:
            pass
    return subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", "tus.iso", "-m", "512M",
         "-display", "none", "-no-reboot",
         "-serial", f"file:{SERIAL_LOG}",
         "-qmp", f"unix:{QMP_SOCK},server=on,wait=off",
         "-pidfile", "/tmp/tus-qemu.pid"],
        stdout=open(QEMU_LOG, "w"), stderr=subprocess.STDOUT)


def screendump(sock, name=None):
    path = name or SCREEN_PPM
    qmp_cmd(sock, "screendump", {"filename": path})
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            with open(path, "rb") as f:
                f.seek(0, 2)
                if f.tell() > 0:
                    break
        except FileNotFoundError:
            pass
        time.sleep(0.1)
    assert os.path.exists(path), "screendump produced no file"


def count_nonblack_pixels(path):
    """P6 PPM: count pixels whose RGB channels are not all near zero."""
    with open(path, "rb") as f:
        data = f.read()
    assert data.startswith(b"P6"), "not a P6 PPM"
    parts = data[:200].split()
    width, height = int(parts[1]), int(parts[2])
    body = data[data.index(b"\n255\n") + 5:]
    assert len(body) >= width * height * 3, "truncated PPM payload"
    count = 0
    for i in range(0, width * height * 3, 3):
        r, g, b = body[i], body[i + 1], body[i + 2]
        if r > 8 or g > 8 or b > 8:
            count += 1
    return count


def count_white_pixels(path):
    """P6 PPM: count pixels that are pure white (255,255,255)."""
    with open(path, "rb") as f:
        data = f.read()
    parts = data[:200].split()
    width, height = int(parts[1]), int(parts[2])
    body = data[data.index(b"\n255\n") + 5:]
    count = 0
    for i in range(0, width * height * 3, 3):
        if body[i] == 255 and body[i + 1] == 255 and body[i + 2] == 255:
            count += 1
    return count


def main():
    assert os.path.exists("tus.iso"), "tus.iso missing - run `make iso` first"

    print("== TUS boot test ==")
    proc = start_qemu()

    try:
        offset = wait_for("tsh ready", timeout=BOOT_TIMEOUT)
        ok("kernel boots and shell banner appears")

        sock = qmp_connect()
        ok("QMP connected")

        # 1. help
        type_text(sock, "help\r")
        offset = wait_for("Available commands:", offset=offset)
        offset = wait_for("fbfill", offset=offset)
        ok("help lists the command table (incl. new fs commands)")

        # 2. echo + backspace editing (types "echoo" then fixes it)
        type_text(sock, "echoo\b hi\r")
        offset = wait_for("hi", offset=offset)
        ok("echo prints its arguments")

        # 3. ver (0.2.0 now)
        type_text(sock, "ver\r")
        offset = wait_for("TUS kernel 0.2.0", offset=offset)
        ok("ver reports the kernel version")

        # 4. sysinfo (now with PMM stats and uptime)
        type_text(sock, "sysinfo\r")
        offset = wait_for("MiB usable", offset=offset)
        offset = wait_for("PMM", offset=offset)
        offset = wait_for("Uptime", offset=offset)
        offset = wait_for("Framebuffer", offset=offset)
        ok("sysinfo reports memory, PMM, uptime and framebuffer")

        # 5. about
        type_text(sock, "about\r")
        offset = wait_for("Toasty Unix Software", offset=offset)
        ok("about shows the TUS identity")

        # 6. uptime (PIT timer through the syscall ABI)
        type_text(sock, "uptime\r")
        offset = wait_for("uptime:", offset=offset)
        ok("uptime reports elapsed time via the PIT")

        # 7. VFS: ls /
        type_text(sock, "ls /\r")
        offset = wait_for("etc", offset=offset)
        offset = wait_for("dev", offset=offset)
        ok("ls / lists the root directory tree")

        # 8. VFS: ls /dev shows the device nodes (list order is
        #    reverse creation order: zero, null, serial0, kbd0, tty0, fb0)
        type_text(sock, "ls /dev\r")
        offset = wait_for("zero", offset=offset)
        offset = wait_for("tty0", offset=offset)
        offset = wait_for("fb0", offset=offset)
        ok("ls /dev lists the built-in devices")

        # 9. cat /etc/motd
        type_text(sock, "cat /etc/motd\r")
        offset = wait_for("Welcome to TUS", offset=offset)
        offset = wait_for("Work everywhere", offset=offset)
        ok("cat /etc/motd reads a seeded file")

        # 10. echo with redirection, then cat it back
        type_text(sock, "echo hello world > /tmp/greet\r")
        type_text(sock, "cat /tmp/greet\r")
        offset = wait_for("hello world", offset=offset)
        ok("echo > file writes through the syscall ABI")

        # 11. mkdir + ls
        type_text(sock, "mkdir /tmp/sub\r")
        type_text(sock, "ls /tmp\r")
        offset = wait_for("sub", offset=offset)
        ok("mkdir creates a directory")

        # 12. ELF loader: run the embedded static binary as a ring-3 task
        type_text(sock, "ls /boot\r")
        offset = wait_for("hello.elf", offset=offset)
        type_text(sock, "exec /boot/hello.elf\r")
        offset = wait_for("started as pid", offset=offset)
        offset = wait_for("Hello from a static ELF", offset=offset)
        ok("exec runs a static ELF image as a ring-3 task")

        # 13. scrollback: overflow the screen, PageUp shows older
        #     lines, PageDown returns to the exact live view.
        #     (Runs BEFORE fbfill: that test paints the screen and
        #     would leave white remnants that skew the pixel counts.)
        for _ in range(3):
            type_text(sock, "help\r")
        time.sleep(1.5)  # let all output settle (TCG is slow)
        screendump(sock, "/tmp/tus-live.ppm")
        live_lit = count_nonblack_pixels("/tmp/tus-live.ppm")
        sendkey(sock, "pgup")
        time.sleep(0.25)
        sendkey(sock, "pgup")
        time.sleep(0.25)
        sendkey(sock, "pgup")
        time.sleep(0.3)
        screendump(sock, "/tmp/tus-back.ppm")
        back_lit = count_nonblack_pixels("/tmp/tus-back.ppm")
        assert back_lit != live_lit, \
            f"PageUp did not change the screen ({live_lit} == {back_lit})"
        sendkey(sock, "pgdn")
        time.sleep(0.25)
        sendkey(sock, "pgdn")
        time.sleep(0.25)
        sendkey(sock, "pgdn")
        time.sleep(0.3)
        screendump(sock, "/tmp/tus-restored.ppm")
        restored_lit = count_nonblack_pixels("/tmp/tus-restored.ppm")
        assert restored_lit == live_lit, \
            f"PageDown did not restore the live view ({restored_lit} != {live_lit})"
        ok(f"scrollback: PageUp/PageDown navigate history "
           f"({live_lit} live, {back_lit} back, {restored_lit} restored)")

        # 14. fbfill paints the whole framebuffer white (ioctl)
        type_text(sock, "fbfill ffffff\r")
        offset = wait_for("filled with #ffffff", offset=offset)
        screendump(sock)
        white = count_white_pixels(SCREEN_PPM)
        assert white > 900000, f"fbfill did not paint the screen ({white} white)"
        ok(f"fbfill fills the framebuffer via /dev/fb0 ioctl ({white} px)")

        # 15. unknown command error path
        type_text(sock, "nosuchcmd\r")
        offset = wait_for("command not found", offset=offset)
        ok("unknown command produces an error")

        # 16. crash -> kernel panic handler with register dump
        type_text(sock, "crash\r")
        offset = wait_for("KERNEL PANIC", offset=offset)
        offset = wait_for("Invalid Opcode", offset=offset)
        wait_for("System halted", offset=offset)
        ok("crash triggers the panic handler with a register dump")

        print(f"\nALL {PASS} TESTS PASSED")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
