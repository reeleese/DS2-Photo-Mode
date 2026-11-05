#!/usr/bin/env python3
"""Make a linked PE file reproducible: zero the timestamps and replace the random CodeView
GUID (lld derives it from the link time) with a hash of the file itself.

    normalize_pe.py FILE.dll

Everything else in the DLL is already identical between builds (checked by comparing two
builds byte for byte: only these fields differed). No other byte is touched.
"""
import hashlib
import struct
import sys


def main(path):
    d = bytearray(open(path, "rb").read())
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[pe:pe + 4] == b"PE\0\0"
    nsec, = struct.unpack_from("<H", d, pe + 6)
    optsz, = struct.unpack_from("<H", d, pe + 20)
    opt = pe + 24
    assert struct.unpack_from("<H", d, opt)[0] == 0x20B  # PE32+
    struct.pack_into("<I", d, pe + 8, 0)  # COFF TimeDateStamp
    struct.pack_into("<I", d, opt + 64, 0)  # optional header CheckSum (lld leaves 0)
    dd = opt + 112
    dbg_rva, dbg_size = struct.unpack_from("<II", d, dd + 6 * 8)
    secs = opt + optsz
    guid_at = []
    if dbg_size:
        for i in range(nsec):
            s = secs + i * 40
            vsz, va, rawsz, raw = struct.unpack_from("<IIII", d, s + 8)
            if va <= dbg_rva < va + max(vsz, rawsz):
                base = raw + dbg_rva - va
                for e in range(base, base + dbg_size, 28):
                    struct.pack_into("<I", d, e + 4, 0)  # entry TimeDateStamp
                    typ, size, _, ptr = struct.unpack_from("<IIII", d, e + 12)
                    if typ == 2 and d[ptr:ptr + 4] == b"RSDS":  # CodeView
                        guid_at.append(ptr + 4)
                break
    for g in guid_at:
        d[g:g + 16] = b"\0" * 16
    digest = hashlib.sha256(bytes(d)).digest()[:16]
    for g in guid_at:
        d[g:g + 16] = digest
    open(path, "wb").write(d)


main(sys.argv[1])
