#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run the pinned netdev_coal_syscall.o program once.

bpftool can't do this: bpf_prog_test_run_syscall() rejects ctx_out and
repeat, and bpftool always sets repeat=1. The program writes its results
back into ctx_in, so read them from there.

usage: netdev_coal_run.py <pin> <ifindex> <mode> <usecs>
prints: ret=<ret> got=<usecs>
"""
import ctypes
import os
import struct
import sys

NR_BPF = 321  # x86_64
BPF_OBJ_GET = 7
BPF_PROG_TEST_RUN = 10
ARGS = "<IIIiI"  # struct netdev_coal_args

libc = ctypes.CDLL(None, use_errno=True)
libc.syscall.restype = ctypes.c_long


def bpf(cmd, attr):
    buf = ctypes.create_string_buffer(attr, len(attr))
    ret = libc.syscall(NR_BPF, cmd, buf, len(attr))
    if ret < 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))
    return ret, buf


def main():
    pin, ifindex, mode, usecs = sys.argv[1:5]
    path = ctypes.create_string_buffer(pin.encode())
    fd, _ = bpf(BPF_OBJ_GET, struct.pack("<QIIi", ctypes.addressof(path), 0, 0, 0))

    args = struct.pack(ARGS, int(ifindex), int(usecs), int(mode), 0, 0)
    ctx = ctypes.create_string_buffer(args, len(args))
    attr = bytearray(80)
    struct.pack_into("<I", attr, 0, fd)                  # prog_fd
    struct.pack_into("<I", attr, 40, len(ctx.raw))       # ctx_size_in
    struct.pack_into("<Q", attr, 48, ctypes.addressof(ctx))  # ctx_in
    bpf(BPF_PROG_TEST_RUN, bytes(attr))
    os.close(fd)

    v = struct.unpack(ARGS, ctx.raw[:struct.calcsize(ARGS)])
    print("ret=%d got=%u" % (v[3], v[4]))


if __name__ == "__main__":
    main()
