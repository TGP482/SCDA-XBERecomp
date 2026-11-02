"""The Xbox game's files, from an xiso or the extracted folder, and the Unreal packages inside its .lin files."""
import os
import struct
import zlib

SECTOR = 2048
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
# a whole disc dump keeps the game partition behind the video one
PARTITIONS = (0, 0x18300000, 0x2080000, 0xFD90000)


class Game:
    """Files by backslash path, any case."""

    def __init__(self, path):
        self.path = path
        self.iso = None
        self.files = {}  # lower path -> (offset, size) on the disc, or host path
        if os.path.isdir(path):
            for root, _, names in os.walk(path):
                for n in names:
                    host = os.path.join(root, n)
                    rel = os.path.relpath(host, path).replace("/", "\\")
                    self.files[rel.lower()] = host
        else:
            self.iso = open(path, "rb")
            base = next((b for b in PARTITIONS if self._read(b + 32 * SECTOR, 20) == XDVDFS_MAGIC), None)
            if base is None:
                raise ValueError(f"{path}: not an Xbox disc image")
            sector, size = struct.unpack("<II", self._read(base + 32 * SECTOR + 20, 8))
            self._walk(base, sector, size, "")

    def _read(self, at, n):
        self.iso.seek(at)
        return self.iso.read(n)

    def _walk(self, base, sector, size, prefix):
        table = self._read(base + sector * SECTOR, size)

        def node(off):
            if off * 4 + 14 > len(table):
                return
            left, right, start, fsize, attr, nlen = struct.unpack_from("<HHIIBB", table, off * 4)
            if left == 0xFFFF:
                return
            name = table[off * 4 + 14: off * 4 + 14 + nlen].decode("latin-1")
            if left:
                node(left)
            if attr & 0x10:
                if fsize:
                    self._walk(base, start, fsize, prefix + name + "\\")
            else:
                self.files[(prefix + name).lower()] = (base + start * SECTOR, fsize)
            if right:
                node(right)

        node(0)

    def names(self):
        return list(self.files)

    def read(self, path):
        f = self.files[path.lower()]
        if isinstance(f, str):
            with open(f, "rb") as h:
                return h.read()
        return self._read(*f)


def find_game(assets):
    """The extracted game in assets, else the first Xbox image there."""
    if os.path.isfile(os.path.join(assets, "default.xbe")):
        return Game(assets)
    for n in sorted(os.listdir(assets)):
        if n.lower().endswith(".iso"):
            try:
                return Game(os.path.join(assets, n))
            except ValueError:
                continue
    raise SystemExit(f"no Xbox game in {assets}")


def unlin(data):
    """A .lin: [u32 raw size][u32 packed size][zlib] chunks."""
    out = bytearray()
    at = 0
    while at + 8 <= len(data):
        raw, packed = struct.unpack_from("<II", data, at)
        if not packed or at + 8 + packed > len(data):
            break
        out += zlib.decompress(data[at + 8: at + 8 + packed])
        at += 8 + packed
    return bytes(out)


def compact(b, i):
    """Unreal's compact index: sign and 6 bits, then 7 bits a byte."""
    x = b[i]
    i += 1
    v = x & 0x3F
    shift = 6
    more = x & 0x40
    while more:
        y = b[i]
        i += 1
        v |= (y & 0x7F) << shift
        shift += 7
        more = y & 0x80
    return (-v if x & 0x80 else v), i


PACKAGE_MAGIC = b"\xde\xad\xf0\x0f"


class Package:
    """One linker block of a .lin: names, imports, exports. Export offsets count from the block's object data."""

    def __init__(self, data, at):
        self.data = data
        self.at = at
        i = at + 4
        i += 1 + data[i]  # an obfuscated source path
        # zero padding, then u32 ? (small, non zero), exports, names
        while data[i] == 0:
            i += 1
        _, n_exports, n_names = struct.unpack_from("<III", data, i)
        i += 12
        self.names = []
        for _ in range(n_names):
            n = data[i]
            self.names.append(data[i + 1: i + 1 + n].decode("latin-1"))
            i += 1 + n + 1 + 4
        self.imports = []
        while data[i] < 0x80:  # an import starts with its class package's name, an export with an import's index
            cp, i = compact(data, i)
            cn, i = compact(data, i)
            (pkg,) = struct.unpack_from("<i", data, i)
            i += 4
            on, i = compact(data, i)
            self.imports.append((self.names[cp], self.names[cn], pkg, self.names[on]))
        self.exports = []
        for _ in range(n_exports):
            cls, i = compact(data, i)
            sup, i = compact(data, i)
            (pkg,) = struct.unpack_from("<i", data, i)
            i += 4
            on, i = compact(data, i)
            (flags,) = struct.unpack_from("<I", data, i)
            i += 4
            size, i = compact(data, i)
            off = 0
            if size > 0:
                off, i = compact(data, i)
            self.exports.append({"class": self.class_name(cls), "name": self.names[on], "package": pkg,
                                 "size": size, "offset": off})
        self.tables_end = i

    def class_name(self, index):
        if index < 0:
            return self.imports[-index - 1][3]
        return "Class" if index == 0 else self.exports[index - 1]["name"]


def packages(data):
    at = data.find(PACKAGE_MAGIC)
    while at >= 0:
        try:
            yield Package(data, at)
        except (IndexError, struct.error, UnicodeDecodeError):
            pass
        at = data.find(PACKAGE_MAGIC, at + 4)
