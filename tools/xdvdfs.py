# reads one file from the root of an Xbox disc image (XDVDFS): xdvdfs.py <iso> <name> <out>
import struct, sys

MAGIC = b"MICROSOFT*XBOX*MEDIA"
iso, want, out = sys.argv[1], sys.argv[2].lower(), sys.argv[3]
with open(iso, "rb") as f:
    base = None
    for part in (0, 0x18300000, 0xFD90000, 0x2080000):  # game partition, redump, XGD3, XGD1 video offsets
        f.seek(part + 32 * 2048)
        if f.read(20) == MAGIC:
            base = part
            break
    if base is None:
        sys.exit("not an Xbox disc image")
    root_sector, root_size = struct.unpack("<II", f.read(8))
    f.seek(base + root_sector * 2048)
    table = f.read(root_size)

    def walk(off):
        if off * 4 + 14 > len(table):
            return None
        left, right, sector, size, attr, nlen = struct.unpack_from("<HHIIBB", table, off * 4)
        if left == 0xFFFF:
            return None
        name = table[off * 4 + 14: off * 4 + 14 + nlen].decode("latin-1")
        if name.lower() == want:
            return sector, size
        for nxt in (left, right):
            if nxt:
                r = walk(nxt)
                if r:
                    return r
        return None

    hit = walk(0)
    if not hit:
        sys.exit(want + " not found")
    sector, size = hit
    f.seek(base + sector * 2048)
    with open(out, "wb") as o:
        o.write(f.read(size))
    print("wrote", out, size, "bytes from partition at", hex(base))
