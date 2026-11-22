#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# BCPack decoding for rebox's DirectStorage shim (shims/xcrdapi), as a separate
# program: Leonida's clean-room decoder (GPLv3) can't be loaded into the title's process, which
# also loads AnyPS5 (GPL-2.0-only). The shim sends each BCPack request here over a localhost TCP
# connection and gets the BC blocks back. See helpers/bcpack/README.md.
#
#   bcpack-helper.py --leonida LEONIDA_DIR --port-file FILE [--watch-pid PID] [--history PATH]
#                    [--strict]
#
# Each connection gets its own process (one per DirectStorage worker thread in the title), so
# queues decode in parallel despite the interpreter lock. rebox-run uses PyPy, about five
# times faster than CPython here.
#
# Protocol (little-endian), one request at a time per connection:
#   request:  u32 magic 'BCPK', u32 codec (DSTORAGE_BCPACK_MODE: 1 BC1, 3 BC3, 4 BC4, 5 BC5,
#             6 BC6H, 7 BC7), u32 out_size, u32 raw_size, raw_size bytes
#   response: i32 status (0, or an HRESULT), u32 size, size bytes
#
# The port goes to --port-file once listening. With --watch-pid the helper exits when that
# process does (rebox-run passes its own PID, which becomes Wine's). By default chunks that
# need Leonida's provisional history entries decode anyway (BC6H and BC7 often do, and the
# calibrated tables are local data); --strict refuses them, and --history supplies calibrated
# BC6H/BC7 history tables.

import argparse
import os
import socket
import socketserver
import struct
import sys
import threading
import time

MAGIC = 0x4B504342  # 'BCPK'
E_NOTIMPL = -2147467263
E_FAIL = -2147467259
ERROR_INVALID_DATA = -2147024883  # HRESULT_FROM_WIN32(ERROR_INVALID_DATA)

# Per connection process.
stats = {"ok": 0, "provisional": 0, "failed": 0, "bytes": 0}


def log(message):
    print(f"bcpack-helper: {message}", file=sys.stderr, flush=True)


def report():
    log(f"connection {os.getpid()}: {stats['ok']} chunks decoded ({stats['provisional']} with provisional "
        f"history, {stats['bytes'] >> 20} MiB), {stats['failed']} failed")


def recv_exact(sock, n):
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("closed")
        data += chunk
    return bytes(data)


def make_handler(bcpack, args):
    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            while True:
                try:
                    magic, codec, out_size, raw_size = struct.unpack("<4I", recv_exact(self.request, 16))
                    if magic != MAGIC:
                        return
                    raw = recv_exact(self.request, raw_size)
                except ConnectionError:
                    return
                status, out, provisional = 0, b"", False
                try:
                    out, info = bcpack.decode_with_info(raw, codec, out_size,
                                                        allow_provisional=not args.strict, history=args.history)
                    provisional = bool(info)
                except bcpack.BcpackUnsupportedCodec:
                    status = E_NOTIMPL
                except bcpack.BcpackError as e:
                    status = ERROR_INVALID_DATA
                    if stats["failed"] < 16:
                        log(f"codec {codec}, {raw_size} -> {out_size} bytes: {e}")
                except Exception as e:  # never take the connection down for one chunk
                    status = E_FAIL
                    log(f"codec {codec}: {type(e).__name__}: {e}")
                if status:
                    stats["failed"] += 1
                else:
                    stats["ok"] += 1
                    stats["provisional"] += provisional
                    stats["bytes"] += len(out)
                done = stats["ok"] + stats["failed"]
                if done >= 64 and done & (done - 1) == 0:  # at 64, 128, 256, ... chunks
                    report()
                self.request.sendall(struct.pack("<iI", status, len(out)) + out)

        def finish(self):
            if stats["ok"] + stats["failed"]:
                report()

    return Handler


class Server(socketserver.ForkingTCPServer):
    allow_reuse_address = True
    max_children = 64


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--leonida", required=True, help="Leonida checkout (its tools/bcpack.py is used)")
    ap.add_argument("--port-file", required=True)
    ap.add_argument("--watch-pid", type=int)
    ap.add_argument("--history", help="calibrated BC6H/BC7 history tables (Leonida's --history)")
    ap.add_argument("--strict", action="store_true", help="refuse chunks that need provisional history")
    args = ap.parse_args()

    sys.path.insert(0, os.path.join(args.leonida, "tools"))
    import bcpack  # Leonida's decoder

    server = Server(("127.0.0.1", 0), make_handler(bcpack, args))
    port = server.server_address[1]
    tmp = args.port_file + ".tmp"
    with open(tmp, "w") as f:
        f.write(f"{port}\n")
    os.replace(tmp, args.port_file)
    log(f"listening on 127.0.0.1:{port} ({sys.implementation.name}, "
        f"{'strict' if args.strict else 'provisional history allowed'})")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        while True:
            time.sleep(2)
            if args.watch_pid:
                try:
                    os.kill(args.watch_pid, 0)
                except ProcessLookupError:
                    break
    except KeyboardInterrupt:
        pass
    server.shutdown()


if __name__ == "__main__":
    main()
