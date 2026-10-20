# SPDX-License-Identifier: AGPL-3.0-only
"""Writes sce_sys/package/head.bin for an app: the package header the Vita's installer
(scePromoterUtilityPromotePkgWithRif) wants in a folder before installing it as an app.

Like VitaShell's makeHeadBin(): its template (third_party/vitashell/head.bin, fetched by
fetch_deps.sh) with the content id written in and three checksums redone. Our param.sfo has no
CONTENT_ID, so the id is made from the title id the way VitaShell does.
Run: python3 make_head_bin.py TEMPLATE TITLE_ID OUT
"""
import hashlib
import re
import struct
import sys


def fpkg_hmac(data):
    sha = hashlib.sha1(data).digest()
    buf = bytearray(64)
    buf[0:8] = sha[4:12]
    buf[8:16] = sha[4:12]
    buf[16:20] = sha[12:16]
    buf[20] = sha[16]
    buf[21:24] = sha[1:4]
    buf[24:32] = buf[16:24]
    return hashlib.sha1(buf).digest()[:16]


def head_bin(template, title_id):
    assert re.fullmatch(r"[A-Z0-9]{9}", title_id), title_id
    h = bytearray(template)
    h[0x30:0x30 + 48] = f"EP9000-{title_id}_00-0000000000000000".encode().ljust(48, b"\0")

    def be32(off):
        return struct.unpack(">I", h[off:off + 4])[0]

    n = be32(0xD0)                          # the package header
    h[n:n + 16] = fpkg_hmac(h[:n])
    off, n, out = be32(0x8), be32(0x10), be32(0xD4)  # the package info
    h[out:out + 16] = fpkg_hmac(h[off:off + n - 64])
    n = be32(0xE8)                          # everything
    h[n:n + 16] = fpkg_hmac(h[:n])
    return bytes(h)


def main():
    template, title_id, out = sys.argv[1:]
    with open(template, "rb") as f:
        data = head_bin(f.read(), title_id)
    with open(out, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main()
