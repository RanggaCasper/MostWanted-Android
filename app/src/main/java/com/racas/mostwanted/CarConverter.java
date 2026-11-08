package com.racas.mostwanted;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Collection;
import java.util.Collections;
import java.util.TreeSet;
import java.util.Set;
import java.util.HashSet;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.function.Consumer;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

// PC add-on cars (Binary and VltEd install files) as files the Xbox game reads: convert makes one car's Package, which is
// kept, and build makes the game's files for all of them. Add-ons get car types of their own after the game's 84 (84 is
// its "no car" answer), which src/game/car_mods.cpp makes the game look through; a car named as one of the game's takes
// that car's place instead. The formats are as tools/car_mod.py has them, whose comments and CLAUDE.md say how they were
// found: a change to a format goes in both. No Android here, so it can run on a PC too.
final class CarConverter {
    static final String TEMPLATE_PARTS = "BMWM3";  // no pvehicle's MODEL names it; its 191 parts are laid out as every car's
    static final int STOCK_TYPES = 84, FIRST_ADDON_TYPE = 85;
    // the frontend layout's make (byte +0x40), the names by 0x134250's table
    private static final int MAKE_OFFSET = 0x40;
    private static final String[] MAKES = {"", "BMW", "FORD", "SUBARU", "PORSCHE", "AUDI", "MAZDA", "GENERAL_MOTORS", "DODGE", "TOYOTA",
            "MITSUBISHI", "MCLAREN", "MERCEDES", "NISSAN", "LOTUS", "LAMBORGHINI", "RENAULT", "LEXUS", "PONTIAC", "CHEVROLET", "VAUXHALL",
            "ASTONMARTIN", "VOLKSWAGEN", "FIAT", "CADILLAC", "CORVETTE"};

    // a mod's make may be spaced: ASTON MARTIN, GENERAL MOTORS
    private static int makeIndex(String manufacturer) {
        List<String> makes = Arrays.asList(MAKES);
        int at = makes.indexOf(manufacturer);
        if (at < 0) at = makes.indexOf(manufacturer.replace(" ", ""));
        return at >= 0 ? at : makes.indexOf(manufacturer.replace(' ', '_'));
    }
    // the parts' car index is a byte, and the game's cars take 99 of them
    static final int MAX_ADDONS = 150;
    static final int MAX_TEXTURES_KB = 1536;

    // what a mod cannot be made into, for the player
    static final class Failure extends Exception {
        enum Reason { NO_CAR, NAME_TOO_LONG, UNSUPPORTED }
        final Reason reason;

        Failure(Reason reason, String detail) {
            super(detail);
            this.reason = reason;
        }
    }

    // one car, converted; its big files are on the storage once kept
    static final class Package {
        String car, title, manufacturer;
        byte[] geometry, textures, script, logo;  // script and logo may be null
        int logoWidth, logoHeight, logoFormat;
    }

    // a kept car for build: its two big files by path
    static final class Kept {
        String car, title, manufacturer;
        File geometry, textures;
        byte[] script, logo;
        int logoWidth, logoHeight, logoFormat;
    }

    static final class Built {
        final Map<String, byte[]> files = new LinkedHashMap<>();  // paths under the mods folder
        final List<String> addons = new ArrayList<>(), replaced = new ArrayList<>();
    }

    private static final Pattern WANTED = Pattern.compile(
            "(.*/)?([^/]+/(geometry|textures)\\.bin|cartypeinfo[^/]*\\.bin|[^/]*\\.nfsms|[^/]*\\.end"
            + "|secondary_logo_[^/]*\\.dds)");

    // an archive's files worth keeping for convert, its paths with / in them
    static boolean wanted(String path) {
        String p = path.replace('\\', '/').toLowerCase(Locale.ROOT);
        return WANTED.matcher(p).matches() && !p.substring(p.lastIndexOf('/') + 1).contains("uninstall");
    }

    // bytes

    private static int align(int n, int a) {
        return (n + a - 1) & ~(a - 1);
    }

    private static int i32(byte[] b, int o) {
        return (b[o] & 255) | (b[o + 1] & 255) << 8 | (b[o + 2] & 255) << 16 | (b[o + 3] & 255) << 24;
    }

    private static int u16(byte[] b, int o) {
        return (b[o] & 255) | (b[o + 1] & 255) << 8;
    }

    private static float f32(byte[] b, int o) {
        return Float.intBitsToFloat(i32(b, o));
    }

    private static void put32(byte[] b, int o, int v) {
        b[o] = (byte) v;
        b[o + 1] = (byte) (v >> 8);
        b[o + 2] = (byte) (v >> 16);
        b[o + 3] = (byte) (v >> 24);
    }

    private static void put16(byte[] b, int o, int v) {
        b[o] = (byte) v;
        b[o + 1] = (byte) (v >> 8);
    }

    private static void putF(byte[] b, int o, float v) {
        put32(b, o, Float.floatToRawIntBits(v));
    }

    private static byte[] ints(int... v) {
        byte[] b = new byte[v.length * 4];
        for (int i = 0; i < v.length; i++) put32(b, i * 4, v[i]);
        return b;
    }

    private static byte[] latin(String s) {
        return s.getBytes(StandardCharsets.ISO_8859_1);
    }

    private static String latin(byte[] b, int off, int n) {
        return new String(b, off, n, StandardCharsets.ISO_8859_1);
    }

    // a text up to its 0
    private static String text(byte[] b, int off, int end) {
        int e = off;
        while (e < end && b[e] != 0) e++;
        return latin(b, off, e - off);
    }

    private static byte[] fill(int n, int v) {
        byte[] b = new byte[Math.max(0, n)];
        if (v != 0) Arrays.fill(b, (byte) v);
        return b;
    }

    private static byte[] sub(byte[] b, int off, int end) {
        return Arrays.copyOfRange(b, off, Math.min(end, b.length));
    }

    private static int find(byte[] b, byte[] what, int from) {
        outer:
        for (int i = from; i + what.length <= b.length; i++) {
            for (int j = 0; j < what.length; j++)
                if (b[i + j] != what[j]) continue outer;
            return i;
        }
        return -1;
    }

    private static int rfind(byte[] b, byte[] what) {
        outer:
        for (int i = b.length - what.length; i >= 0; i--) {
            for (int j = 0; j < what.length; j++)
                if (b[i + j] != what[j]) continue outer;
            return i;
        }
        return -1;
    }

    // a bytearray
    private static final class Buf {
        byte[] a;
        int n;

        Buf() {
            this(new byte[256], 0);
        }

        Buf(byte[] start) {
            this(start, start.length);
        }

        private Buf(byte[] a, int n) {
            this.a = a;
            this.n = n;
        }

        private void room(int more) {
            if (n + more > a.length) a = Arrays.copyOf(a, Math.max(a.length * 2, n + more));
        }

        Buf add(byte[] b) {
            return add(b, 0, b.length);
        }

        Buf add(byte[] b, int off, int len) {
            room(len);
            System.arraycopy(b, off, a, n, len);
            n += len;
            return this;
        }

        Buf zeros(int len) {
            if (len <= 0) return this;
            room(len);
            Arrays.fill(a, n, n + len, (byte) 0);
            n += len;
            return this;
        }

        byte[] bytes() {
            return Arrays.copyOf(a, n);
        }
    }

    private static byte[] cat(byte[]... parts) {
        Buf out = new Buf();
        for (byte[] p : parts) out.add(p);
        return out.bytes();
    }

    // hashes

    static int bhash(String s) {
        int h = -1;
        for (byte c : latin(s)) h = h * 33 + (c & 255);
        return h;
    }

    private static int[] mix(int a, int b, int c) {
        a = a - b - c; a ^= c >>> 13;
        b = b - c - a; b ^= a << 8;
        c = c - a - b; c ^= b >>> 13;
        a = a - b - c; a ^= c >>> 12;
        b = b - c - a; b ^= a << 16;
        c = c - a - b; c ^= b >>> 5;
        a = a - b - c; a ^= c >>> 3;
        b = b - c - a; b ^= a << 10;
        c = c - a - b; c ^= b >>> 15;
        return new int[] {a, b, c};
    }

    // Attrib::StringToKey, Jenkins' lookup2 from 0xABCDEF00 (0x047520)
    static int attribKey(String s) {
        byte[] k = latin(s);
        int a = 0x9E3779B9, b = 0x9E3779B9, c = 0xABCDEF00, i = 0;
        for (; k.length - i >= 12; i += 12) {
            int[] m = mix(a + i32(k, i), b + i32(k, i + 4), c + i32(k, i + 8));
            a = m[0];
            b = m[1];
            c = m[2];
        }
        c += k.length;
        int left = k.length - i;
        for (int j = left - 1; j >= 0; j--) {
            int v = k[i + j] & 255;
            if (j >= 8) c += v << (8 * (j - 7));
            else if (j >= 4) b += v << (8 * (j - 4));
            else a += v << (8 * j);
        }
        return mix(a, b, c)[2];
    }

    // Jenkins' lookup8 (0x047740)
    static long stringKey64(String name) {
        byte[] k = latin(name);
        long a = 0xABCDEF0011223344L, b = a, c = 0x9E3779B97F4A7C13L;
        int i = 0;
        for (; k.length - i >= 24; i += 24) {
            a += i64(k, i);
            b += i64(k, i + 8);
            c += i64(k, i + 16);
            long[] m = mix64(a, b, c);
            a = m[0];
            b = m[1];
            c = m[2];
        }
        c += k.length;
        int left = k.length - i;
        for (int j = left - 1; j >= 0; j--) {
            long v = k[i + j] & 255;
            if (j >= 16) c += v << (8 * (j - 15));
            else if (j >= 8) b += v << (8 * (j - 8));
            else a += v << (8 * j);
        }
        return mix64(a, b, c)[2];
    }

    private static long i64(byte[] b, int o) {
        return (i32(b, o) & 0xFFFFFFFFL) | (long) i32(b, o + 4) << 32;
    }

    private static long[] mix64(long a, long b, long c) {
        a = a - b - c; a ^= c >>> 43;
        b = b - c - a; b ^= a << 9;
        c = c - a - b; c ^= b >>> 8;
        a = a - b - c; a ^= c >>> 38;
        b = b - c - a; b ^= a << 23;
        c = c - a - b; c ^= b >>> 5;
        a = a - b - c; a ^= c >>> 35;
        b = b - c - a; b ^= a << 49;
        c = c - a - b; c ^= b >>> 11;
        a = a - b - c; a ^= c >>> 12;
        b = b - c - a; b ^= a << 18;
        c = c - a - b; c ^= b >>> 22;
        return new long[] {a, b, c};
    }

    // JDLZ

    // as the game's (0x1BAD20): it counts the input down and stops only at exactly 0, so a stream that ends anywhere else
    // runs on over the heap. That is an error here. The next flags are read right after the token that ran them out
    static byte[] jdlzDecode(byte[] b) {
        if (!isJdlz(b, 0)) throw new IllegalArgumentException("no JDLZ header");
        int usize = i32(b, 8), csize = i32(b, 12);
        byte[] out = new byte[usize];
        int f1 = (b[16] & 255) | 0x100, f2 = (b[17] & 255) | 0x100;
        int ip = 18, op = 0, left = csize - 18;
        while (left != 0) {
            if (left < 0) throw new IllegalArgumentException("JDLZ input does not end where its size says");
            if ((f1 & 1) != 0) {
                int n, t;
                if ((f2 & 1) != 0) {
                    n = ((b[ip + 1] & 255) | (b[ip] & 0xF0) << 4) + 3;
                    t = (b[ip] & 0x0F) + 1;
                } else {
                    t = ((b[ip + 1] & 255) | (b[ip] & 0xE0) << 3) + 17;
                    n = (b[ip] & 0x1F) + 3;
                }
                if (op + n > usize) throw new IllegalArgumentException("JDLZ output past its size");
                for (int i = 0; i < n; i++) out[op + i] = out[op + i - t];
                op += n;
                ip += 2;
                left -= 2;
                f2 >>= 1;
            } else {
                if (op < usize) {
                    out[op++] = b[ip++];
                }
                left -= 1;
            }
            f1 >>= 1;
            if (f1 == 1) {
                f1 = (ip < b.length ? b[ip] & 255 : 0) | 0x100;
                ip++;
                left--;
            }
            if (f2 == 1) {
                f2 = (ip < b.length ? b[ip] & 255 : 0) | 0x100;
                ip++;
                left--;
            }
        }
        if (op != usize) throw new IllegalArgumentException("JDLZ output short of its size");
        return out;
    }

    private static boolean isJdlz(byte[] b, int o) {
        return o + 18 <= b.length && b[o] == 'J' && b[o + 1] == 'D' && b[o + 2] == 'L' && b[o + 3] == 'Z';
    }

    // the two flag bytes come where the decoder reads them: each is fetched when the last one ran out, before a token
    static byte[] jdlzEncode(byte[] d) {
        Buf out = new Buf(new byte[Math.max(256, d.length / 2)], 0);
        out.add(new byte[] {'J', 'D', 'L', 'Z', 2, 0x10, 0, 0});
        out.add(ints(d.length, 0));
        int n = d.length;
        // earlier places of the same three bytes, newest first; hash neighbours are told apart as the walk goes
        int[] head = new int[1 << 18], prev = new int[n];
        Arrays.fill(head, -1);
        int f1At = -1, f2At = -1, f1N = 8, f2N = 8, f1 = 0, f2 = 0, i = 0;
        while (i < n) {
            if (f1N == 8) {
                if (f1At >= 0) out.a[f1At] = (byte) f1;
                f1At = out.n;
                out.zeros(1);
                f1 = 0;
                f1N = 0;
            }
            if (f2N == 8) {
                if (f2At >= 0) out.a[f2At] = (byte) f2;
                f2At = out.n;
                out.zeros(1);
                f2 = 0;
                f2N = 0;
            }
            int bestLen = 0, bestDist = 0;
            if (i + 3 <= n) {
                int seen = 0;
                for (int p = head[tri(d, i)]; p >= 0 && seen < 16; p = prev[p]) {
                    int dist = i - p;
                    if (dist > 2064) break;
                    if (d[p] != d[i] || d[p + 1] != d[i + 1] || d[p + 2] != d[i + 2]) continue;
                    seen++;
                    int limit = Math.min(n - i, dist <= 16 ? 4098 : 34), m = 3;
                    while (m < limit && d[p + m] == d[i + m]) m++;
                    if (m > bestLen) {
                        bestLen = m;
                        bestDist = dist;
                        if (m == limit) break;
                    }
                }
                int h = tri(d, i);
                prev[i] = head[h];
                head[h] = i;
            }
            if (bestLen >= 3) {
                f1 |= 1 << f1N;
                if (bestDist <= 16) {
                    f2 |= 1 << f2N;
                    out.add(new byte[] {(byte) (((bestLen - 3) >> 8) << 4 | (bestDist - 1)), (byte) (bestLen - 3)});
                } else {
                    int t = bestDist - 17;
                    out.add(new byte[] {(byte) (((t >> 3) & 0xE0) | (bestLen - 3)), (byte) t});
                }
                f2N++;
                for (int j = i + 1, end = Math.min(i + bestLen, n - 2); j < end; j++) {
                    int h = tri(d, j);
                    prev[j] = head[h];
                    head[h] = j;
                }
                i += bestLen;
            } else {
                out.add(d, i, 1);
                i++;
            }
            f1N++;
        }
        if (f1At >= 0) out.a[f1At] = (byte) f1;
        if (f2At >= 0) out.a[f2At] = (byte) f2;
        // the decoder fetches flags that ran out on the last token too, and those count
        if (f1N == 8) out.zeros(1);
        if (f2N == 8) out.zeros(1);
        put32(out.a, 12, out.n);
        return out.bytes();
    }

    private static int tri(byte[] d, int i) {
        int v = (d[i] & 255) | (d[i + 1] & 255) << 8 | (d[i + 2] & 255) << 16;
        return (v * 0x9E3779B1) >>> 14;
    }

    // the game's files: a disc image, or its files extracted

    static final class Disc implements AutoCloseable {
        private static final long[] PARTITIONS = {0, 0x18300000L, 0xFD90000L, 0x2080000L};
        final Map<Integer, int[]> recs = new TreeMap<>(Integer::compareUnsigned);
        private final RandomAccessFile iso;
        private final File folder;
        private long base;
        private final Map<String, long[]> nfs = new HashMap<>();  // the NFS folder's files: sector and size in an image

        Disc(File path) throws IOException {
            byte[] zdir;
            if (path.isDirectory()) {
                iso = null;
                folder = child(path, "NFS");
                File[] files = folder == null ? null : folder.listFiles();
                if (files == null) throw new IOException("no NFS folder in " + path);
                for (File f : files) nfs.put(f.getName().toUpperCase(Locale.ROOT), new long[] {0, f.length()});
                zdir = readAll(child(folder, "ZDIR.BIN"));
            } else {
                folder = null;
                iso = new RandomAccessFile(path, "r");
                byte[] magic = new byte[28];
                boolean found = false;
                for (long part : PARTITIONS) {
                    if (part + 32 * 2048 + 28 > iso.length()) continue;
                    iso.seek(part + 32 * 2048);
                    iso.readFully(magic);
                    if (latin(magic, 0, 20).equals("MICROSOFT*XBOX*MEDIA")) {
                        base = part;
                        found = true;
                        break;
                    }
                }
                if (!found) throw new IOException(path + " is not an Xbox disc image");
                Map<String, long[]> root = dir(i32(magic, 20) & 0xFFFFFFFFL, i32(magic, 24));
                long[] nf = root.get("NFS");
                if (nf == null) throw new IOException("no NFS folder in " + path);
                nfs.putAll(dir(nf[0], (int) nf[1]));
                long[] z = nfs.get("ZDIR.BIN");
                if (z == null) throw new IOException("no ZDIR.BIN in " + path);
                zdir = read(z[0], (int) z[1]);
            }
            for (int i = 0; i + 24 <= zdir.length; i += 24) {
                int[] r = new int[6];
                for (int j = 0; j < 6; j++) r[j] = i32(zdir, i + j * 4);
                recs.put(r[0], r);
            }
        }

        private static File child(File dir, String name) {
            File[] files = dir.listFiles();
            if (files != null)
                for (File f : files)
                    if (f.getName().equalsIgnoreCase(name)) return f;
            return null;
        }

        private static byte[] readAll(File f) throws IOException {
            if (f == null) throw new IOException("a game file is missing");
            try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
                byte[] b = new byte[(int) r.length()];
                r.readFully(b);
                return b;
            }
        }

        private byte[] read(long sector, int size) throws IOException {
            byte[] b = new byte[size];
            iso.seek(base + sector * 2048);
            iso.readFully(b);
            return b;
        }

        private Map<String, long[]> dir(long sector, int size) throws IOException {
            Map<String, long[]> out = new HashMap<>();
            walk(read(sector, size), 0, out, 0);
            return out;
        }

        private static void walk(byte[] t, int off, Map<String, long[]> out, int depth) {
            if (off * 4 + 14 > t.length || depth > 64) return;
            int left = u16(t, off * 4), right = u16(t, off * 4 + 2), nl = t[off * 4 + 13] & 255;
            if (left == 0xFFFF) return;
            out.put(latin(t, off * 4 + 14, nl).toUpperCase(Locale.ROOT),
                    new long[] {i32(t, off * 4 + 4) & 0xFFFFFFFFL, i32(t, off * 4 + 8) & 0xFFFFFFFFL});
            if (left != 0) walk(t, left, out, depth + 1);
            if (right != 0) walk(t, right, out, depth + 1);
        }

        long archiveSize(int i) {
            long[] e = nfs.get("ZZDATA" + i + ".BIN");
            return e == null ? -1 : e[1];
        }

        byte[] load(String path) throws IOException {
            int[] r = recs.get(bhash(path));
            if (r == null) throw new IOException("no " + path + " in the game's files");
            long at = (r[2] & 0xFFFFFFFFL) * 2048;
            if (iso != null) {
                long[] arc = nfs.get("ZZDATA" + r[1] + ".BIN");
                if (arc == null) throw new IOException("no archive " + r[1]);
                return read(arc[0] + at / 2048, r[4]);
            }
            File arc = child(folder, "ZZDATA" + r[1] + ".BIN");
            if (arc == null) throw new IOException("no archive " + r[1]);
            try (RandomAccessFile f = new RandomAccessFile(arc, "r")) {
                byte[] b = new byte[r[4]];
                f.seek(at);
                f.readFully(b);
                return b;
            }
        }

        @Override
        public void close() throws IOException {
            if (iso != null) iso.close();
        }
    }

    // chunks: id, size, the data's offset

    private static List<int[]> chunks(byte[] b, int off, int end) {
        List<int[]> out = new ArrayList<>();
        while (off + 8 <= end) {
            int c = i32(b, off), s = i32(b, off + 4);
            if (s < 0 || off + 8 + (long) s > end) break;
            out.add(new int[] {c, s, off + 8});
            off += 8 + s;
        }
        return out;
    }

    private static List<int[]> walkAll(byte[] b, int off, int end) {
        List<int[]> out = new ArrayList<>();
        walkInto(b, off, end, out);
        return out;
    }

    private static void walkInto(byte[] b, int off, int end, List<int[]> out) {
        for (int[] c : chunks(b, off, end)) {
            out.add(c);
            if (c[0] < 0) walkInto(b, c[2], c[2] + c[1], out);
        }
    }

    private static int[] first(byte[] b, int id) {
        for (int[] c : walkAll(b, 0, b.length))
            if (c[0] == id) return c;
        return null;
    }

    // chunk data is aligned with a fill of 0x11
    private static int payload(byte[] b, int o, int s) {
        int p = o;
        while (p + 4 <= o + s && b[p] == 0x11 && b[p + 1] == 0x11 && b[p + 2] == 0x11 && b[p + 3] == 0x11) p += 4;
        return p;
    }

    private static byte[] chunk(int id, byte[] data) {
        byte[] out = new byte[8 + data.length];
        put32(out, 0, id);
        put32(out, 4, data.length);
        System.arraycopy(data, 0, out, 8, data.length);
        return out;
    }

    // the header at file offset at, its data on a multiple of al
    private static byte[] chunkAligned(int id, byte[] data, int at, int al) {
        int start = at + 8;
        return chunk(id, cat(fill(align(start, al) - start, 0x11), data));
    }

    // geometry

    private static final class Solid {
        String name;
        int hash, flags, verts, ngroups, tris;
        byte[] head, tex, sha, groups, ib;
        List<byte[]> vb = new ArrayList<>();
    }

    // a block of a pack: EA's compressed forms have a 16 byte header, the size within at +8. RAWW only stores; a block with
    // no header is stored as it is
    private static byte[] unpack(byte[] block, int usize) throws Failure {
        if (isJdlz(block, 0)) return jdlzDecode(block);
        String magic = block.length >= 4 ? latin(block, 0, 4) : "";
        if (magic.equals("RAWW")) {
            if (block.length < 16) throw new Failure(Failure.Reason.UNSUPPORTED, "a RAWW block too short");
            int n = i32(block, 8);
            if (n < 0 || 16L + n > block.length) throw new Failure(Failure.Reason.UNSUPPORTED, "a RAWW block's size");
            return Arrays.copyOfRange(block, 16, 16 + n);
        }
        if (magic.equals("HUFF") || magic.equals("COMP") || magic.equals("LZMA"))
            throw new Failure(Failure.Reason.UNSUPPORTED, "packed as " + magic);
        return Arrays.copyOf(block, Math.min(usize, block.length));
    }

    private static List<byte[]> readModSolids(byte[] mg) throws Failure {
        int[] c = first(mg, 0x134004);
        if (c == null) throw new Failure(Failure.Reason.UNSUPPORTED, "GEOMETRY.BIN has no solid table");
        List<byte[]> out = new ArrayList<>();
        for (int i = 0; i + 24 <= c[1]; i += 24) {
            int off = i32(mg, c[2] + i + 4), cs = i32(mg, c[2] + i + 8), us = i32(mg, c[2] + i + 12);
            out.add(unpack(sub(mg, off, off + Math.max(cs, us)), us));
        }
        return out;
    }

    // a mesh chunk's data starts on 16 bytes, vertices on 0x80 (0x0C7490), the gap filled with 0x11 or, by some tools, with 0: the
    // zeros are the gap if they lead to that place and what is left is whole records. Read as data, they shifted everything after
    private static int modPayload(byte[] d, int o, int size, int alignment, int record) {
        int p = payload(d, o, size);
        if (p != o) return p;
        int at = align(o, alignment);
        if (at == o || at > o + size || (o + size - at) % record != 0) return o;
        for (int i = o; i < at; i++) if (d[i] != 0) return o;
        return at;
    }

    private static final int MAX_GROUP_INDICES = 0xFFFC, MAX_GROUP_VERTICES = 0xFFF0;
    private static final int MAX_SOLID_VERTICES = 60000;  // the solid's count is 16 bits too

    // a group's flag 0x100 gives it the environment's reflection on the Xbox; the PC takes that from the shader, and mods' tools
    // leave the bit off (every mod's paint was 0x14080, the disc's 0x14180: no reflection). It goes by the shader, as on every
    // disc car's solids: these always have it, and 0x78743CA1 mostly
    private static final Set<Integer> REFLECTIVE = new HashSet<>(Arrays.asList(
            0x0000D916, 0x02A05578, 0x02DDDAD9, 0x02EE77E1, 0x05BC3A3C, 0x0FEDEE40, 0x12049251, 0x12C9453C, 0x1FBD2378, 0x1FF8C329,
            0x22719FA9, 0x2E65E067, 0x33A26CB6, 0x3ED70C43, 0x471A1DCA, 0x54949AFD, 0x721AFDDC, 0xA6348EE3, 0xB51B3ACF, 0xB57AB98E,
            0xD6640DFF, 0xD6D6080A, 0xD79597D6, 0xDCD01662, 0xF0A43B67, 0xFD102A92, 0x78743CA1));
    // and these never (interior, plate, underside, ...), nor mostly 0x010CB64A
    private static final Set<Integer> MATTE = new HashSet<>(Arrays.asList(0x2787EDAB, 0x4C95C6F8, 0x52BF4C34, 0x5799E60B, 0x010CB64A));

    private static int reflective(int shader, int flags) {
        if (REFLECTIVE.contains(shader)) return flags | 0x100;
        if (MATTE.contains(shader)) return flags & ~0x100;
        return flags;
    }

    private static Solid convertSolid(byte[] d, String car) {
        byte[] info = null, groups = null, idx = null, verts = null, tex = new byte[0], sha = new byte[0];
        for (int[] c : walkAll(d, 0, d.length)) {
            int sz = c[1], o = c[2];
            switch (c[0]) {
                case 0x134011: info = sub(d, o, o + sz); break;
                case 0x134012: tex = sub(d, o, o + sz); break;
                case 0x134013: sha = sub(d, o, o + sz); break;
                case 0x134b02: groups = sub(d, modPayload(d, o, sz, 16, 104), o + sz); break;
                case 0x134b03: idx = sub(d, modPayload(d, o, sz, 16, 2), o + sz); break;
                case 0x134b01: verts = sub(d, modPayload(d, o, sz, 0x80, 36), o + sz); break;
                default: break;
            }
        }
        if (info == null || info.length < 0xA0 || groups == null || idx == null || verts == null) return null;
        String name = text(info, 0xA0, info.length);
        // a car made from another keeps that one's name inside (SUPRA_KIT00_BODY_A in BRIANSUPRA), with the hash of its own, which
        // is what the game looks a model up by: the name goes with the hash
        int underscore = name.indexOf('_');
        if (underscore > 0 && i32(info, 0x10) != bhash(name) && i32(info, 0x10) == bhash(car + name.substring(underscore)))
            name = car + name.substring(underscore);
        // LODs move up one: the Xbox cars' best is B; only placeholders have an A
        int lod = name.isEmpty() ? -1 : "ABCD".indexOf(name.charAt(name.length() - 1));
        if (lod < 0 || name.length() < 2 || name.charAt(name.length() - 2) != '_') return null;
        Solid s = new Solid();
        s.name = name.substring(0, name.length() - 1) + "BCDE".charAt(lod);
        s.hash = bhash(s.name);
        int nIdx = idx.length / 2;
        Buf gr = new Buf(), ib = new Buf();
        int start = 0, flagsOr = 0, totalV = 0;
        for (int g = 0; g < groups.length / 104; g++) {
            int r = g * 104;
            int flags = i32(groups, r + 0x38) & 0x1FFFF;  // the disc's cars have no more
            int shader = groups[r + 0x1D] & 255;
            if (shader * 8 + 4 <= sha.length) flags = reflective(i32(sha, shader * 8), flags);
            int tris = i32(groups, r + 0x40), firstIdx = i32(groups, r + 0x44), count = tris * 3;
            // the mod's groups share one buffer of pos, normal, colour, uv; the Xbox's have one each of pos, uv, normal
            // a group's counts and indices are 16 bits: a denser one goes on in another of the same material
            int k = firstIdx, end = Math.min(firstIdx + count, nIdx);
            do {
                Map<Integer, Integer> remap = new HashMap<>();
                Buf b = new Buf();
                int locals = 0;
                for (; k + 3 <= end && locals + 3 <= MAX_GROUP_INDICES && remap.size() + 3 <= MAX_GROUP_VERTICES; k += 3) {
                    for (int c = 0; c < 3; c++) {
                        int i = u16(idx, (k + c) * 2);
                        Integer l = remap.get(i);
                        if (l == null) {
                            l = remap.size();
                            remap.put(i, l);
                            int v = i * 36;
                            b.add(verts, v, 12).add(verts, v + 28, 8).add(verts, v + 12, 12);
                        }
                        byte[] le = new byte[2];
                        put16(le, 0, l);
                        ib.add(le);
                        locals++;
                    }
                }
                byte[] rec = new byte[96];
                System.arraycopy(groups, r, rec, 0, 12);
                System.arraycopy(groups, r + 12, rec, 16, 12);
                put16(rec, 0x20, locals);
                put16(rec, 0x22, 3);
                put32(rec, 0x24, groups[r + 0x18] & 255);
                put32(rec, 0x28, flags);
                put32(rec, 0x2C, start);
                put32(rec, 0x40, groups[r + 0x1D] & 255);
                gr.add(rec);
                s.vb.add(b.bytes());
                start += locals;
                totalV += remap.size();
                s.ngroups++;
            } while (k + 3 <= end);
            flagsOr |= flags;
        }
        if ((start & 1) != 0) ib.zeros(2);  // the disc's index chunks end on 4 bytes
        s.groups = gr.bytes();
        s.ib = ib.bytes();
        s.flags = flagsOr & 0xFFFF;
        s.verts = totalV;
        s.tex = tex;
        s.sha = sha;
        s.tris = start / 3;
        // the Xbox's header: a flag more, its name, the radius and tris per radius the mod leaves 0
        byte[] head = Arrays.copyOf(info, 0xA0);
        put32(head, 0xC, 0x400016);
        put32(head, 0x10, s.hash);
        put32(head, 0x14, s.tris);
        double sum = 0;
        for (int a = 0; a < 3; a++) {
            double e = (double) f32(head, 0x30 + a * 4) - (double) f32(head, 0x20 + a * 4);
            sum += e * e;
        }
        double radius = Math.max(0.001, Math.sqrt(sum) / 2);
        putF(head, 0x98, (float) radius);
        putF(head, 0x9C, (float) (s.tris / radius));
        byte[] nm = latin(s.name + "\0");
        s.head = cat(head, nm, fill(align(nm.length, 4) - nm.length, 0));
        return s;
    }

    // the car pool holds every car of a race (src/game/car_mods.cpp makes it 39 MB, the disc's is 7550 KB), and a car loads
    // about its whole file. PC mods often repeat their best model at every LOD: those lower LODs are thinned to their share,
    // and only a car over this limit has its best models cut
    private static final double[] LOD_SHARE = {1, 0.12, 0.04, 0.015};  // of the part's best, by LODs below it: the player sees the best
    private static final long MAX_GEOMETRY = 12L << 20;
    private static final int VERTEX = 32;  // pos, uv, normal

    private static long solidBytes(Solid s) {
        long n = s.ib.length;
        for (byte[] v : s.vb) n += v.length + 0x80;
        return n;
    }

    private static long totalBytes(List<Solid> solids) {
        long n = 0;
        for (Solid s : solids) n += solidBytes(s);
        return n;
    }

    // the most triangles a part may keep for them all to fit the budget, a part's bytes going with its triangles
    private static int triangleCap(List<Solid> tops, long budget) {
        int low = 1, high = 1;
        for (Solid s : tops) high = Math.max(high, s.tris);
        while (low < high) {
            int mid = (low + high + 1) >>> 1;
            long total = 0;
            for (Solid s : tops) total += solidBytes(s) * Math.min(s.tris, mid) / Math.max(s.tris, 1);
            if (total <= budget) low = mid;
            else high = mid - 1;
        }
        return low;
    }

    private static void reduceLods(List<Solid> solids, Consumer<String> log) {
        Map<String, Solid> best = new HashMap<>();
        for (Solid s : solids) {
            String part = s.name.substring(0, s.name.length() - 1);
            Solid b = best.get(part);
            if (b == null || s.name.compareTo(b.name) < 0) best.put(part, s);
        }
        long before = 0;
        for (Solid s : solids) before += solidBytes(s);
        for (Solid s : solids)
            if (s.verts > MAX_SOLID_VERTICES) reduceSolid(s, (int) (s.tris * (double) MAX_SOLID_VERTICES / s.verts));
        double all = 0;
        for (double share : LOD_SHARE) all += share;
        List<String> parts = new ArrayList<>(best.keySet());
        Collections.sort(parts);
        List<Solid> tops = new ArrayList<>();
        for (String part : parts) tops.add(best.get(part));
        long budget = (long) (MAX_GEOMETRY / all);
        // what stands out is cut first, the small parts that show most (a wheel) last; a vertex costs more than was reckoned, so again
        for (int round = 0; round < 4 && totalBytes(tops) > budget; round++) {
            int cap = triangleCap(tops, budget);
            for (Solid b : tops) reduceSolid(b, cap);
        }
        for (Solid s : solids) {
            Solid b = best.get(s.name.substring(0, s.name.length() - 1));
            int below = s.name.charAt(s.name.length() - 1) - b.name.charAt(b.name.length() - 1);
            if (below > 0) reduceSolid(s, (int) (b.tris * LOD_SHARE[Math.min(below, LOD_SHARE.length - 1)]));
        }
        // the lower LODs may have stopped above their share: the best ones pay for it
        for (int round = 0; round < 4 && totalBytes(solids) > MAX_GEOMETRY; round++) {
            int cap = triangleCap(tops, totalBytes(tops) - (totalBytes(solids) - MAX_GEOMETRY));
            for (Solid b : tops) reduceSolid(b, cap);
        }
        long after = totalBytes(solids);
        log.accept("geometry: " + before / 1024 + " KB, to " + after / 1024 + " KB");
    }

    // a PC mod may keep its body in attachment slots (a kit's ATTACHMENT0 to 9), which no car of the disc uses and the parts
    // table made from BMWM3's has no row for: they never drew, leaving a car's panels out. A kit's are its body, so they go
    // into its BODY, LOD by LOD; a style's are options no menu here can pick, so they go
    private static final Pattern ATTACHMENT = Pattern.compile("(.*_)(KIT\\d\\d|STYLE\\d\\d)_ATTACHMENT\\d_([A-E])");

    private static boolean mergeAttachments(List<Solid> solids, Consumer<String> log) {
        List<Solid> sorted = new ArrayList<>(solids);
        sorted.sort(Comparator.comparing((Solid x) -> x.name));
        Map<String, Solid> byName = new HashMap<>();
        for (Solid x : solids) byName.put(x.name, x);
        int merged = 0, dropped = 0;
        for (Solid part : sorted) {
            Matcher m = ATTACHMENT.matcher(part.name);
            if (!m.matches()) continue;
            if (m.group(2).startsWith("STYLE")) {
                solids.remove(part);
                dropped++;
                continue;
            }
            String target = m.group(1) + m.group(2) + "_BODY_" + m.group(3);
            Solid body = byName.get(target);
            if (body == null) {
                renameSolid(part, target);
                byName.put(target, part);
                merged++;
            } else if (mergeSolid(body, part)) {
                solids.remove(part);
                merged++;
            }
        }
        if (merged + dropped > 0) log.accept("geometry: " + merged + " attachments into bodies, " + dropped + " left out");
        return merged + dropped > 0;
    }

    private static void renameSolid(Solid s, String name) {
        s.name = name;
        s.hash = bhash(name);
        byte[] nm = latin(name + "\0");
        s.head = cat(Arrays.copyOf(s.head, 0xA0), nm, fill(align(nm.length, 4) - nm.length, 0));
        put32(s.head, 0x10, s.hash);
    }

    // an 8 byte entry's place in a table, added if it is new
    private static int tableEntry(Buf table, byte[] entries, int at) {
        for (int i = 0; i + 8 <= table.n; i += 8)
            if (i32(table.a, i) == i32(entries, at) && i32(table.a, i + 4) == i32(entries, at + 4)) return i / 8;
        table.add(entries, at, 8);
        return table.n / 8 - 1;
    }

    private static int indexCount(Solid s) {
        int n = 0;
        for (int g = 0; g < s.ngroups; g++) n += u16(s.groups, g * 96 + 0x20);
        return n;
    }

    // false when the two will not fit one solid: vertices and groups are 16 bit counts, a group's table indices bytes
    private static boolean mergeSolid(Solid body, Solid part) {
        Buf tex = new Buf(), sha = new Buf();
        tex.add(body.tex);
        sha.add(body.sha);
        int[] texAt = new int[part.tex.length / 8], shaAt = new int[part.sha.length / 8];
        for (int i = 0; i < texAt.length; i++) texAt[i] = tableEntry(tex, part.tex, i * 8);
        for (int i = 0; i < shaAt.length; i++) shaAt[i] = tableEntry(sha, part.sha, i * 8);
        if (body.verts + part.verts > 0xFFFF || tex.n / 8 > 255 || sha.n / 8 > 255) return false;
        int bodyIndices = indexCount(body), partIndices = indexCount(part);
        byte[] groups = part.groups.clone();
        for (int g = 0; g < part.ngroups; g++) {
            int rec = g * 96;
            put32(groups, rec + 0x24, texAt[i32(groups, rec + 0x24) & 255]);
            put32(groups, rec + 0x2C, i32(groups, rec + 0x2C) + bodyIndices);
            put32(groups, rec + 0x40, shaAt[i32(groups, rec + 0x40) & 255]);
        }
        Buf ib = new Buf();
        ib.add(body.ib, 0, bodyIndices * 2).add(part.ib, 0, partIndices * 2);
        if (((bodyIndices + partIndices) & 1) != 0) ib.zeros(2);  // the disc's index chunks end on 4 bytes
        body.ib = ib.bytes();
        body.groups = cat(body.groups, groups);
        body.vb.addAll(part.vb);
        body.ngroups += part.ngroups;
        body.verts += part.verts;
        body.tris += part.tris;
        body.flags |= part.flags;
        body.tex = tex.bytes();
        body.sha = sha.bytes();
        body.head[0x19] = (byte) (tex.n / 8);
        body.head[0x1A] = (byte) (sha.n / 8);
        put32(body.head, 0x14, body.tris);
        double sum = 0;
        for (int a = 0; a < 3; a++) {
            float low = Math.min(f32(body.head, 0x20 + a * 4), f32(part.head, 0x20 + a * 4));
            float high = Math.max(f32(body.head, 0x30 + a * 4), f32(part.head, 0x30 + a * 4));
            putF(body.head, 0x20 + a * 4, low);
            putF(body.head, 0x30 + a * 4, high);
            double e = (double) high - (double) low;
            sum += e * e;
        }
        double radius = Math.max(0.001, Math.sqrt(sum) / 2);
        putF(body.head, 0x98, (float) radius);
        putF(body.head, 0x9C, (float) (body.tris / radius));
        return true;
    }

    // a mod with no script of its own had the Gallardo's wheel places, its tires out of the body. They are measured off the model:
    // TireOffsets' x is the middle of an arch in the lower edge of the skin, y the skin's width there (a disc car's tire model
    // runs inward from 0, a mod's may be centred on 0, so what is on the far side of 0 comes off). On the disc's cars and a
    // scripted mod that gives x within 2.5 cm of their TireOffsets and y within 1.5 cm
    private static final double WHEEL_BIN = 0.02;
    private static final Pattern NOT_BODY = Pattern.compile(".*(TIRE|BRAKE|WINDOW|INTERIOR|DRIVER|DAMAGE|DECAL|MIRROR|SPOILER|HOOD|GLASS|LIGHT).*");

    private static final class WheelPlaces {
        double frontX, rearX, frontY, rearY, radius, width;
    }

    private static List<double[]> solidTriangles(Solid s) {
        List<double[]> out = new ArrayList<>();
        for (int g = 0; g < s.ngroups; g++) {
            int count = u16(s.groups, g * 96 + 0x20), start = i32(s.groups, g * 96 + 0x2C);
            byte[] v = s.vb.get(g);
            for (int t = 0; t + 3 <= count; t += 3) {
                double[] tri = new double[9];
                for (int c = 0; c < 3; c++) {
                    int o = u16(s.ib, (start + t + c) * 2) * VERTEX;
                    for (int a = 0; a < 3; a++) tri[c * 3 + a] = f32(v, o + a * 4);
                }
                out.add(tri);
            }
        }
        return out;
    }

    // null when the model will not say: no arches at both ends, or no tire
    private static WheelPlaces measureWheels(List<Solid> solids) {
        List<double[]> tris = new ArrayList<>();
        double[] tire = null;
        List<Solid> sorted = new ArrayList<>(solids);
        sorted.sort(Comparator.comparing((Solid x) -> x.name));
        for (Solid s : sorted) {
            if (s.name.endsWith("_FRONT_TIRE_B")) {
                tire = new double[] {Double.MAX_VALUE, -Double.MAX_VALUE, Double.MAX_VALUE, -Double.MAX_VALUE};
                for (double[] t : solidTriangles(s)) {
                    for (int c = 0; c < 3; c++) {
                        tire[0] = Math.min(tire[0], t[c * 3 + 1]);
                        tire[1] = Math.max(tire[1], t[c * 3 + 1]);
                        tire[2] = Math.min(tire[2], t[c * 3 + 2]);
                        tire[3] = Math.max(tire[3], t[c * 3 + 2]);
                    }
                }
            }
            if (s.name.endsWith("_B") && (s.name.contains("_BASE_") || s.name.contains("_KIT00_")) && !NOT_BODY.matcher(s.name).matches())
                tris.addAll(solidTriangles(s));
        }
        if (tris.isEmpty() || tire == null) return null;
        double x0 = Double.MAX_VALUE, x1 = -Double.MAX_VALUE, half = 0, zLow = Double.MAX_VALUE, zTop = -Double.MAX_VALUE;
        for (double[] t : tris) {
            for (int c = 0; c < 3; c++) {
                x0 = Math.min(x0, t[c * 3]);
                x1 = Math.max(x1, t[c * 3]);
                half = Math.max(half, Math.abs(t[c * 3 + 1]));
                zLow = Math.min(zLow, t[c * 3 + 2]);
                zTop = Math.max(zTop, t[c * 3 + 2]);
            }
        }
        int bins = (int) ((x1 - x0) / WHEEL_BIN) + 2;
        double[] bottom = new double[bins];
        Arrays.fill(bottom, Double.NaN);
        for (double[] t : tris) {
            if (Math.min(Math.abs(t[1]), Math.min(Math.abs(t[4]), Math.abs(t[7]))) < half * 0.7) continue;
            double ax = t[3] - t[0], ay = t[4] - t[1], az = t[5] - t[2], bx = t[6] - t[0], by = t[7] - t[1], bz = t[8] - t[2];
            double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
            double nl = Math.sqrt(nx * nx + ny * ny + nz * nz);
            if (nl == 0 || Math.abs(ny) / nl < 0.5) continue;
            for (int e = 0; e < 3; e++) {
                int a = e * 3, b = (e + 1) % 3 * 3;
                int steps = (int) (Math.abs(t[a] - t[b]) / WHEEL_BIN) + 1;
                for (int i = 0; i <= steps; i++) {
                    double f = (double) i / steps;
                    double x = t[a] + (t[b] - t[a]) * f, z = t[a + 2] + (t[b + 2] - t[a + 2]) * f;
                    int k = (int) ((x - x0) / WHEEL_BIN);
                    if (Double.isNaN(bottom[k]) || z < bottom[k]) bottom[k] = z;
                }
            }
        }
        double length = x1 - x0;
        double[] known = Arrays.stream(bottom).filter(z -> !Double.isNaN(z)).sorted().toArray();
        if (known.length == 0) return null;
        double limit = known[known.length / 4] + (zTop - zLow) * 0.12, middle = (x0 + x1) / 2;
        // the widest run of the lower edge above the sill, or of no skin at all, in each half of the car
        double[] front = null, rear = null;
        int start = -1;
        for (int k = 0; k <= bins; k++) {
            double x = x0 + k * WHEEL_BIN;
            boolean high = k < bins && x > x0 + length * 0.04 && x < x1 - length * 0.04 && (Double.isNaN(bottom[k]) || bottom[k] > limit);
            if (high && start < 0) {
                start = k;
            } else if (!high && start >= 0) {
                if ((k - start) * WHEEL_BIN > length * 0.08) {
                    double[] run = {x0 + start * WHEEL_BIN, x0 + k * WHEEL_BIN};
                    if ((run[0] + run[1]) / 2 > middle) {
                        if (front == null || run[1] - run[0] > front[1] - front[0]) front = run;
                    } else if (rear == null || run[1] - run[0] > rear[1] - rear[0]) {
                        rear = run;
                    }
                }
                start = -1;
            }
        }
        if (front == null || rear == null) return null;
        double frontHalf = 0, rearHalf = 0;
        for (double[] t : tris) {
            for (int c = 0; c < 3; c++) {
                double x = t[c * 3], y = Math.abs(t[c * 3 + 1]);
                if (x >= front[0] && x <= front[1]) frontHalf = Math.max(frontHalf, y);
                if (x >= rear[0] && x <= rear[1]) rearHalf = Math.max(rearHalf, y);
            }
        }
        WheelPlaces w = new WheelPlaces();
        w.frontX = (front[0] + front[1]) / 2;
        w.rearX = (rear[0] + rear[1]) / 2;
        w.frontY = frontHalf + Math.min(tire[0], 0);
        w.rearY = rearHalf + Math.min(tire[0], 0);
        w.radius = (tire[3] - tire[2]) / 2;
        w.width = tire[1] - tire[0];
        if (w.frontY < 0.4 || w.rearY < 0.4 || w.radius < 0.1 || w.radius > 0.7) return null;
        return w;
    }

    // a built GEOMETRY.BIN read back, for a car kept from before the limit
    private static List<Solid> readBuiltSolids(byte[] g) {
        List<Solid> out = new ArrayList<>();
        Solid s = null;
        for (int[] c : walkAll(g, 0, g.length)) {
            int o = c[2], end = o + c[1];
            if (c[0] == 0x134011) {
                s = new Solid();
                s.head = sub(g, o, end);
                s.name = text(s.head, 0xA0, s.head.length);
                s.hash = i32(s.head, 0x10);
                s.tris = i32(s.head, 0x14);
                s.tex = new byte[0];
                s.sha = new byte[0];
                out.add(s);
            } else if (s != null) {
                int p = payload(g, o, c[1]);
                switch (c[0]) {
                    case 0x134012: s.tex = sub(g, o, end); break;
                    case 0x134013: s.sha = sub(g, o, end); break;
                    case 0x134B00:
                        s.flags = u16(g, p + 4);
                        s.verts = u16(g, p + 6);
                        s.ngroups = u16(g, p + 8);
                        break;
                    case 0x134B02: s.groups = sub(g, p, end); break;
                    case 0x134B03: s.ib = sub(g, p, end); break;
                    case 0x134B01: s.vb.add(sub(g, p, end)); break;
                    default: break;
                }
            }
        }
        return out;
    }

    // a kept car built before the reflection flag went by the shader: its groups' flags, and its meshes' OR of them, set in
    // place. True if anything changed
    private static boolean relight(byte[] g) {
        boolean changed = false;
        byte[] sha = null;
        int mesh = -1;
        for (int[] c : walkAll(g, 0, g.length)) {
            int o = c[2], end = o + c[1];
            switch (c[0]) {
                case 0x134011: sha = null; mesh = -1; break;
                case 0x134013: sha = sub(g, o, end); break;
                case 0x134B00: mesh = payload(g, o, c[1]); break;
                case 0x134B02:
                    if (sha == null) break;
                    int or = 0;
                    for (int p = payload(g, o, c[1]); p + 96 <= end; p += 96) {
                        int shader = g[p + 0x40] & 255, was = i32(g, p + 0x28), now = was;
                        if (shader * 8 + 4 <= sha.length) now = reflective(i32(sha, shader * 8), was);
                        if (now != was) {
                            put32(g, p + 0x28, now);
                            changed = true;
                        }
                        or |= now;
                    }
                    if (mesh >= 0 && (u16(g, mesh + 4) | (or & 0xFFFF)) != u16(g, mesh + 4)) {
                        put16(g, mesh + 4, u16(g, mesh + 4) | (or & 0xFFFF));
                        changed = true;
                    }
                    break;
                default: break;
            }
        }
        return changed;
    }

    // a kept car with attachments or over the limit put right where it lies, so a car from an older install needs no new download
    static void fitGeometry(Disc disc, File geometry, Consumer<String> log) throws Failure, IOException {
        byte[] g = Files.readAllBytes(geometry.toPath());
        if (relight(g)) {
            File temp = new File(geometry.getPath() + ".new");
            Files.write(temp.toPath(), g);
            Files.move(temp.toPath(), geometry.toPath(), StandardCopyOption.REPLACE_EXISTING);
            log.accept(geometry.getParentFile().getName() + ": reflection flags set by shader");
        }
        List<Solid> solids = readBuiltSolids(g);
        if (solids.isEmpty()) return;
        for (Solid s : solids)
            if (s.groups == null || s.ib == null || s.vb.size() != s.ngroups || s.groups.length != s.ngroups * 96) return;
        boolean merged = mergeAttachments(solids, log);
        if (!merged && geometry.length() <= MAX_GEOMETRY + (64L << 10)) return;
        reduceLods(solids, log);
        File temp = new File(geometry.getPath() + ".new");
        Files.write(temp.toPath(), buildGeometry(solids, disc.load("CARS\\GALLARDO\\GEOMETRY.BIN")));
        Files.move(temp.toPath(), geometry.toPath(), StandardCopyOption.REPLACE_EXISTING);
    }

    // a mesh far over its target, or with too many open edges for edge collapse to get there: the vertices of a grid cell facing
    // the same octant become the first of them, each shading group alone, and triangles left with a side of no length go. The
    // finest grid that gets to the goal: on a mesh this dense its cells are too small to see (coarse ones tore a low one's wheels)
    private static void clusterToward(Solid s, int goal) {
        if (s.tris <= goal) return;
        Object[] found = null;
        for (int low = 2, high = 1024; low <= high; ) {
            int mid = (low + high) >>> 1;
            Object[] r = clusterSolid(s, mid);
            if ((int) r[4] <= goal) {
                found = r;
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
        if (found == null) found = clusterSolid(s, 2);
        applyMesh(s, (byte[]) found[0], castVb(found[1]), (byte[]) found[2], (int) found[3], (int) found[4]);
    }

    @SuppressWarnings("unchecked")
    private static List<byte[]> castVb(Object o) {
        return (List<byte[]>) o;
    }

    private static void applyMesh(Solid s, byte[] groups, List<byte[]> vb, byte[] ib, int verts, int tris) {
        s.groups = groups;
        s.vb = vb;
        s.ib = ib;
        s.verts = verts;
        s.tris = tris;
        put32(s.head, 0x14, s.tris);
        putF(s.head, 0x9C, (float) (s.tris / (double) f32(s.head, 0x98)));
    }

    private static Object[] clusterSolid(Solid s, int grid) {
        double[] min = {Double.MAX_VALUE, Double.MAX_VALUE, Double.MAX_VALUE}, max = {-Double.MAX_VALUE, -Double.MAX_VALUE, -Double.MAX_VALUE};
        for (byte[] v : s.vb) {
            for (int o = 0; o + VERTEX <= v.length; o += VERTEX) {
                for (int a = 0; a < 3; a++) {
                    double p = f32(v, o + a * 4);
                    if (p < min[a]) min[a] = p;
                    if (p > max[a]) max[a] = p;
                }
            }
        }
        double extent = Math.max(max[0] - min[0], Math.max(max[1] - min[1], max[2] - min[2]));
        double cell = extent > 0 ? extent / grid : 1;
        byte[] groups = s.groups.clone();
        List<byte[]> vbs = new ArrayList<>();
        Buf ib = new Buf();
        byte[] le = new byte[2];
        int start = 0, verts = 0;
        for (int g = 0; g < s.ngroups; g++) {
            int rec = g * 96, count = u16(s.groups, rec + 0x20), first = i32(s.groups, rec + 0x2C);
            byte[] v = s.vb.get(g);
            Map<Long, Integer> ids = new HashMap<>();
            Buf nv = new Buf();
            int kept = 0;
            long[] keys = new long[3];
            for (int t = 0; t + 3 <= count; t += 3) {
                for (int c = 0; c < 3; c++) {
                    int o = u16(s.ib, (first + t + c) * 2) * VERTEX;
                    long key = 0;
                    for (int a = 0; a < 3; a++) key |= (long) ((f32(v, o + a * 4) - min[a]) / cell) << (a * 20);
                    for (int a = 0; a < 3; a++) if (f32(v, o + 20 + a * 4) < 0) key |= 1L << (60 + a);
                    keys[c] = key;
                }
                if (keys[0] == keys[1] || keys[1] == keys[2] || keys[0] == keys[2]) continue;
                for (int c = 0; c < 3; c++) {
                    Integer id = ids.get(keys[c]);
                    if (id == null) {
                        id = ids.size();
                        ids.put(keys[c], id);
                        nv.add(v, u16(s.ib, (first + t + c) * 2) * VERTEX, VERTEX);
                    }
                    put16(le, 0, id);
                    ib.add(le);
                }
                kept += 3;
            }
            // a group left empty would be a chunk of nothing: its first triangle stays
            if (kept == 0 && count >= 3) {
                for (int c = 0; c < 3; c++) {
                    nv.add(v, u16(s.ib, (first + c) * 2) * VERTEX, VERTEX);
                    put16(le, 0, c);
                    ib.add(le);
                }
                kept = 3;
                verts += 3;
            }
            put16(groups, rec + 0x20, kept);
            put32(groups, rec + 0x2C, start);
            vbs.add(nv.bytes());
            start += kept;
            verts += ids.size();
        }
        if ((start & 1) != 0) ib.zeros(2);  // the disc's index chunks end on 4 bytes
        return new Object[] {groups, vbs, ib.bytes(), verts, start / 3};
    }

    // each shading group thinned by as much (quadric edge collapse), from a mesh brought near by clustering if it is far over,
    // and to the target by clustering if collapse stops short. Clustering alone tore a low mesh's wheels and faceted its body
    private static void reduceSolid(Solid s, int target) {
        target = Math.max(target, 1);
        if (s.tris <= target) return;
        clusterToward(s, target * 2);
        collapseToward(s, target);
        if (s.tris > target + target / 4) clusterToward(s, target);
    }

    private static void collapseToward(Solid s, int target) {
        if (s.tris <= target) return;
        double share = (double) target / s.tris;
        byte[] groups = s.groups.clone();
        List<byte[]> vb = new ArrayList<>();
        Buf ib = new Buf();
        int start = 0, verts = 0;
        for (int g = 0; g < s.ngroups; g++) {
            int rec = g * 96, count = u16(s.groups, rec + 0x20), first = i32(s.groups, rec + 0x2C);
            int[] tri = new int[count - count % 3];
            for (int i = 0; i < tri.length; i++) tri[i] = u16(s.ib, (first + i) * 2);
            Decimator d = new Decimator(s.vb.get(g), tri);
            d.run(Math.max(1, (int) (tri.length / 3 * share)));
            Buf nv = new Buf();
            int kept = d.emit(s.vb.get(g), nv, ib);
            put16(groups, rec + 0x20, kept);
            put32(groups, rec + 0x2C, start);
            vb.add(nv.bytes());
            verts += nv.n / VERTEX;
            start += kept;
        }
        if ((start & 1) != 0) ib.zeros(2);  // the disc's index chunks end on 4 bytes
        applyMesh(s, groups, vb, ib.bytes(), verts, start / 3);
    }

    // one group's mesh. A vertex goes into a neighbour, the pair costing least by their planes' quadrics, so what is left keeps
    // its own position, uv and normal. An open edge is a seam with the next group or a change of uv: its vertices stay until
    // nothing else can go, then go only along it, at a price
    private static final class Decimator {
        final int n;
        final double[] pos, normal, uv, q;
        final int[] tri, version;
        final boolean[] deadTri, deadVert, border;
        final List<List<Integer>> adj = new ArrayList<>();
        final Set<Long> borderEdges = new HashSet<>();
        double scale;
        int live;

        Decimator(byte[] v, int[] tri) {
            n = v.length / VERTEX;
            this.tri = tri;
            pos = new double[n * 3];
            normal = new double[n * 3];
            uv = new double[n * 2];
            q = new double[n * 10];
            version = new int[n];
            deadVert = new boolean[n];
            border = new boolean[n];
            deadTri = new boolean[tri.length / 3];
            live = tri.length / 3;
            for (int i = 0; i < n; i++) {
                int o = i * VERTEX;
                for (int a = 0; a < 3; a++) {
                    pos[i * 3 + a] = f32(v, o + a * 4);
                    normal[i * 3 + a] = f32(v, o + 20 + a * 4);
                }
                uv[i * 2] = f32(v, o + 12);
                uv[i * 2 + 1] = f32(v, o + 16);
                adj.add(new ArrayList<>());
            }
            Map<Long, Integer> edges = new HashMap<>();
            double area = 0, edge = 0;
            for (int t = 0; t < live; t++) {
                int a = tri[t * 3], b = tri[t * 3 + 1], c = tri[t * 3 + 2];
                adj.get(a).add(t);
                adj.get(b).add(t);
                adj.get(c).add(t);
                edges.merge(key(a, b), 1, Integer::sum);
                edges.merge(key(b, c), 1, Integer::sum);
                edges.merge(key(c, a), 1, Integer::sum);
                edge += dist2(a, b) + dist2(b, c) + dist2(c, a);
                double[] m = cross(a, b, c);
                double len = Math.sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                if (len <= 0) continue;
                area += len / 2;
                double x = m[0] / len, y = m[1] / len, z = m[2] / len;
                double w = -(x * pos[a * 3] + y * pos[a * 3 + 1] + z * pos[a * 3 + 2]);
                double[] plane = {x * x, x * y, x * z, x * w, y * y, y * z, y * w, z * z, z * w, w * w};
                for (int k = 0; k < 10; k++) {
                    double add = plane[k] * (len / 2);
                    q[a * 10 + k] += add;
                    q[b * 10 + k] += add;
                    q[c * 10 + k] += add;
                }
            }
            for (Map.Entry<Long, Integer> e : edges.entrySet()) {
                if (e.getValue() != 1) continue;
                borderEdges.add(e.getKey());
                border[(int) (e.getKey() >>> 32)] = true;
                border[(int) (long) e.getKey()] = true;
            }
            int count = Math.max(live, 1);
            scale = area / count * (edge / (3.0 * count));
            if (scale <= 0) scale = 1;
        }

        private static long key(int a, int b) {
            return a < b ? (long) a << 32 | b : (long) b << 32 | a;
        }

        private double dist2(int a, int b) {
            double x = pos[a * 3] - pos[b * 3], y = pos[a * 3 + 1] - pos[b * 3 + 1], z = pos[a * 3 + 2] - pos[b * 3 + 2];
            return x * x + y * y + z * z;
        }

        private double[] cross(int a, int b, int c) {
            double ux = pos[b * 3] - pos[a * 3], uy = pos[b * 3 + 1] - pos[a * 3 + 1], uz = pos[b * 3 + 2] - pos[a * 3 + 2];
            double vx = pos[c * 3] - pos[a * 3], vy = pos[c * 3 + 1] - pos[a * 3 + 1], vz = pos[c * 3 + 2] - pos[a * 3 + 2];
            return new double[] {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
        }

        private double cost(int u, int v) {
            double x = pos[v * 3], y = pos[v * 3 + 1], z = pos[v * 3 + 2];
            int a = u * 10, b = v * 10;
            double e = (q[a] + q[b]) * x * x + 2 * (q[a + 1] + q[b + 1]) * x * y + 2 * (q[a + 2] + q[b + 2]) * x * z
                    + 2 * (q[a + 3] + q[b + 3]) * x + (q[a + 4] + q[b + 4]) * y * y + 2 * (q[a + 5] + q[b + 5]) * y * z
                    + 2 * (q[a + 6] + q[b + 6]) * y + (q[a + 7] + q[b + 7]) * z * z + 2 * (q[a + 8] + q[b + 8]) * z + (q[a + 9] + q[b + 9]);
            double du = uv[u * 2] - uv[v * 2], dv = uv[u * 2 + 1] - uv[v * 2 + 1];
            double dot = normal[u * 3] * normal[v * 3] + normal[u * 3 + 1] * normal[v * 3 + 1] + normal[u * 3 + 2] * normal[v * 3 + 2];
            e += scale * (0.1 * Math.min(du * du + dv * dv, 1.0) + 0.1 * Math.max(1.0 - dot, 0.0));
            if (border[u]) e += scale;
            return e;
        }

        private TreeSet<Integer> neighbours(int u) {
            TreeSet<Integer> out = new TreeSet<>();
            for (int t : adj.get(u)) {
                if (deadTri[t]) continue;
                for (int c = 0; c < 3; c++) if (tri[t * 3 + c] != u) out.add(tri[t * 3 + c]);
            }
            return out;
        }

        // u may go into v: an open edge only along itself, no fold where the two share more than an edge's two neighbours,
        // no triangle turned over or left with no area
        private boolean can(int u, int v, boolean seams) {
            if (border[u] && (!seams || !borderEdges.contains(key(u, v)))) return false;
            TreeSet<Integer> shared = neighbours(u);
            shared.retainAll(neighbours(v));
            if (shared.size() > 2) return false;
            for (int t : adj.get(u)) {
                if (deadTri[t]) continue;
                int a = tri[t * 3], b = tri[t * 3 + 1], c = tri[t * 3 + 2];
                if (a == v || b == v || c == v) continue;
                double[] before = cross(a, b, c);
                double[] after = cross(a == u ? v : a, b == u ? v : b, c == u ? v : c);
                double bb = before[0] * before[0] + before[1] * before[1] + before[2] * before[2];
                double aa = after[0] * after[0] + after[1] * after[1] + after[2] * after[2];
                if (aa <= 1e-24) return false;
                double dot = before[0] * after[0] + before[1] * after[1] + before[2] * after[2];
                if (dot <= 0 || dot * dot < 0.04 * aa * bb) return false;
            }
            return true;
        }

        private void push(java.util.PriorityQueue<double[]> heap, int u, int v) {
            heap.add(new double[] {cost(u, v), u, v, version[u], version[v]});
        }

        void run(int target) {
            for (int pass = 0; pass < 2 && live > target; pass++) {
                boolean seams = pass == 1;
                java.util.PriorityQueue<double[]> heap = new java.util.PriorityQueue<>((x, y) -> {
                    int c = Double.compare(x[0], y[0]);
                    if (c != 0) return c;
                    c = Double.compare(x[1], y[1]);
                    return c != 0 ? c : Double.compare(x[2], y[2]);
                });
                for (int u = 0; u < n; u++)
                    for (int v : neighbours(u)) push(heap, u, v);
                while (live > target && !heap.isEmpty()) {
                    double[] e = heap.poll();
                    int u = (int) e[1], v = (int) e[2];
                    if (deadVert[u] || deadVert[v] || version[u] != (int) e[3] || version[v] != (int) e[4] || !can(u, v, seams)) continue;
                    collapse(u, v);
                    for (int w : neighbours(v)) {
                        push(heap, v, w);
                        push(heap, w, v);
                    }
                }
            }
        }

        private void collapse(int u, int v) {
            for (int w : neighbours(u))
                if (w != v && borderEdges.contains(key(u, w))) borderEdges.add(key(v, w));
            if (border[u]) border[v] = true;
            for (int t : adj.get(u)) {
                if (deadTri[t]) continue;
                int a = tri[t * 3], b = tri[t * 3 + 1], c = tri[t * 3 + 2];
                if (a == v || b == v || c == v) {
                    deadTri[t] = true;
                    live--;
                    continue;
                }
                for (int k = 0; k < 3; k++) if (tri[t * 3 + k] == u) tri[t * 3 + k] = v;
                adj.get(v).add(t);
            }
            for (int k = 0; k < 10; k++) q[v * 10 + k] += q[u * 10 + k];
            deadVert[u] = true;
            version[v]++;
        }

        // what is left, vertices in the order first used; its index count
        int emit(byte[] v, Buf nv, Buf ib) {
            Map<Integer, Integer> ids = new HashMap<>();
            byte[] le = new byte[2];
            int kept = 0;
            for (int t = 0; t < deadTri.length; t++) {
                if (deadTri[t]) continue;
                for (int c = 0; c < 3; c++) {
                    int i = tri[t * 3 + c];
                    Integer id = ids.get(i);
                    if (id == null) {
                        id = ids.size();
                        ids.put(i, id);
                        nv.add(v, i * VERTEX, VERTEX);
                    }
                    put16(le, 0, id);
                    ib.add(le);
                }
                kept += 3;
            }
            return kept;
        }
    }

    // at: the file offset of its header; vertices on 0x80, groups and indices on 16, as on the disc
    private static byte[] buildSolid(Solid s, int at) {
        byte[] head = cat(chunk(0x134011, s.head), chunk(0x134012, s.tex), chunk(0x134013, s.sha));
        int p = at + 8 + head.length + 8;  // 80134010 header, then 80134100 header
        Buf mesh = new Buf();
        byte[] hdr = new byte[12 + 40];
        put32(hdr, 0, 0xF);
        put16(hdr, 4, s.flags);
        put16(hdr, 6, s.verts);
        put16(hdr, 8, s.ngroups);
        put16(hdr, 10, 0x20);
        // each read at its header + 0x17 rounded down to 16 (0x0C7490), vertices + 0x87 rounded down to 0x80
        mesh.add(chunkAligned(0x134B00, hdr, p, 16));
        mesh.add(chunkAligned(0x134B02, s.groups, p + mesh.n, 16));
        mesh.add(chunkAligned(0x134B03, s.ib, p + mesh.n, 16));
        for (byte[] v : s.vb) mesh.add(chunkAligned(0x134B01, v, p + mesh.n, 0x80));
        return chunk(0x80134010, cat(head, chunk(0x80134100, mesh.bytes())));
    }

    private static byte[] buildGeometry(List<Solid> solids, byte[] template) throws Failure {
        int[] tc = first(template, 0x134002);
        if (tc == null) throw new Failure(Failure.Reason.UNSUPPORTED, "the game's geometry has no table");
        byte[] info = sub(template, tc[2], tc[2] + tc[1]);
        int tableSize = 8 + 144 + 8 + solids.size() * 8 + 8 + solids.size() * 24 + 8;
        int firstAt = 8 + 8 + 8 + tableSize;  // 80134000, an empty 0, 80134001
        int at = align(firstAt + 8, 0x80);
        List<Solid> sorted = new ArrayList<>(solids);
        sorted.sort(Comparator.comparingLong(x -> x.hash & 0xFFFFFFFFL));
        Buf body = new Buf(), hashes = new Buf(), offs = new Buf();
        for (Solid s : sorted) {
            if (((at + body.n) & 0x7F) != 0) {
                int pad = Math.floorMod(0x80 - (at + body.n + 8) % 0x80, 0x80);
                body.add(chunk(0, new byte[pad]));
            }
            int off = at + body.n;
            byte[] c = buildSolid(s, off);
            hashes.add(ints(s.hash, 0));
            offs.add(ints(s.hash, off, c.length, c.length, 0, 0));
            body.add(c);
        }
        int size = align(at + body.n, 0x80);
        put32(info, 0xC, solids.size());
        put32(info, 0x68, size);
        byte[] table = cat(chunk(0x134002, info), chunk(0x134003, hashes.bytes()), chunk(0x134004, offs.bytes()),
                chunk(0x80134008, new byte[0]));
        byte[] head = cat(chunk(0, new byte[0]), chunk(0x80134001, table));
        int gap = at - (8 + head.length) - 8;
        byte[] out = chunk(0x80134000, cat(head, chunk(0, fill(gap, 0)), body.bytes()));
        return out.length < size ? Arrays.copyOf(out, size) : out;
    }

    // textures

    private static final class Texture {
        int hash, w, h;
        String name, fmt;
        byte[] px;
    }

    private static int xboxFormat(String fmt) {
        switch (fmt) {
            case "DXT1": return 0x0C;
            case "DXT3": return 0x0E;
            case "DXT5": return 0x0F;
            default: return -1;
        }
    }

    private static int blockSize(String fmt) {
        return fmt.equals("DXT1") ? 8 : 16;
    }

    private static double[] rgb565(int c) {
        int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        return new double[] {r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2};
    }

    private static int to565(double r, double g, double b) {
        return ((int) r >> 3) << 11 | ((int) g >> 2) << 5 | (int) b >> 3;
    }

    private static double[] blend(double[] a, double[] b, double wa, double wb, double div) {
        return new double[] {(wa * a[0] + wb * b[0]) / div, (wa * a[1] + wb * b[1]) / div, (wa * a[2] + wb * b[2]) / div};
    }

    // a block's colours as the four 2x2 averages, each r, g, b, opaque pixels
    private static double[][] blockQuarters(byte[] px, int cb, boolean fourColour) {
        int c0 = u16(px, cb), c1 = u16(px, cb + 2), bits = i32(px, cb + 4);
        double[] a = rgb565(c0), b = rgb565(c1);
        double[][] pal;
        int clear;
        if (fourColour || c0 > c1) {
            pal = new double[][] {a, b, blend(a, b, 2, 1, 3), blend(a, b, 1, 2, 3)};
            clear = 4;
        } else {
            pal = new double[][] {a, b, blend(a, b, 1, 1, 2), {0, 0, 0}};
            clear = 3;
        }
        double[][] out = new double[4][];
        int q = 0;
        for (int qy = 0; qy <= 2; qy += 2) {
            for (int qx = 0; qx <= 2; qx += 2) {
                double r = 0, g = 0, bl = 0;
                int n = 0;
                for (int py = qy; py < qy + 2; py++) {
                    for (int x = qx; x < qx + 2; x++) {
                        int i = (bits >>> (2 * (py * 4 + x))) & 3;
                        r += pal[i][0];
                        g += pal[i][1];
                        bl += pal[i][2];
                        if (i != clear) n++;
                    }
                }
                out[q++] = new double[] {r / 4, g / 4, bl / 4, n};
            }
        }
        return out;
    }

    // half across and down, a real average: each source block gives four pixels of the destination
    private static void shrink(Texture t) {
        int bs = blockSize(t.fmt);
        byte[] px = t.px;
        int ow = Math.max(1, (t.w + 3) / 4), oh = Math.max(1, (t.h + 3) / 4);
        int nw = Math.max(4, t.w / 2), nh = Math.max(4, t.h / 2);
        int bw = (nw + 3) / 4, bh = (nh + 3) / 4;
        byte[] out = new byte[bw * bh * bs];
        double[][] cols = new double[16][];
        int[] alpha = new int[16];
        for (int y = 0; y < bh; y++) {
            for (int x = 0; x < bw; x++) {
                for (int sy = 0; sy < 2; sy++) {
                    for (int sx = 0; sx < 2; sx++) {
                        int gy = Math.min(y * 2 + sy, oh - 1), gx = Math.min(x * 2 + sx, ow - 1);
                        int src = (gy * ow + gx) * bs;
                        double[][] q = blockQuarters(px, src + bs - 8, bs == 16);
                        for (int i = 0; i < 4; i++) {
                            int at = (sy * 2 + (i >> 1)) * 4 + sx * 2 + (i & 1);
                            cols[at] = q[i];
                            if (bs == 16) alpha[at] = alphaQuarter(px, src, i);
                        }
                    }
                }
                int at = (y * bw + x) * bs;
                if (bs == 16)
                    for (int r = 0; r < 4; r++)
                        put16(out, at + r * 2, alpha[r * 4] | alpha[r * 4 + 1] << 4 | alpha[r * 4 + 2] << 8 | alpha[r * 4 + 3] << 12);
                packBlock(out, at + bs - 8, cols, bs);
            }
        }
        t.px = out;
        t.w = nw;
        t.h = nh;
    }

    // DXT3's explicit alpha: 4 bits a pixel, averaged the same way
    private static int alphaQuarter(byte[] px, int a, int q) {
        int qy = (q >> 1) * 2, qx = (q & 1) * 2;
        int sum = 0;
        for (int y = qy; y < qy + 2; y++) {
            int row = u16(px, a + y * 2);
            for (int x = qx; x < qx + 2; x++) sum += (row >> (4 * x)) & 15;
        }
        return sum / 4;
    }

    // the ends along the block's brightness; pixels mostly transparent stay so in DXT1
    private static void packBlock(byte[] out, int cb, double[][] cols, int bs) {
        double[] hi = null, lo = null;
        double hiV = 0, loV = 0;
        int seen = 0;
        for (double[] c : cols) {
            if (c[3] < 2 && bs != 16) continue;
            seen++;
            double v = c[0] + c[1] + c[2];
            if (hi == null || v > hiV) {
                hi = c;
                hiV = v;
            }
            if (lo == null || v < loV) {
                lo = c;
                loV = v;
            }
        }
        if (seen == 0) {
            put16(out, cb, 0);
            put16(out, cb + 2, 0);
            put32(out, cb + 4, -1);
            return;
        }
        boolean clear = bs == 8 && seen < 16;
        int cHi = to565(hi[0], hi[1], hi[2]), cLo = to565(lo[0], lo[1], lo[2]), c0, c1;
        double[][] pal;
        if (clear) {
            // three colours and transparent: the smaller end first
            c0 = Math.min(cHi, cLo);
            c1 = Math.max(cHi, cLo);
            double[] a = rgb565(c0), b = rgb565(c1);
            pal = new double[][] {a, b, blend(a, b, 1, 1, 2)};
        } else {
            if (cHi < cLo) {
                int t = cHi;
                cHi = cLo;
                cLo = t;
            }
            if (cHi == cLo) {
                put16(out, cb, cHi);
                put16(out, cb + 2, cLo);
                put32(out, cb + 4, 0);
                return;
            }
            c0 = cHi;
            c1 = cLo;
            double[] a = rgb565(c0), b = rgb565(c1);
            pal = new double[][] {a, b, blend(a, b, 2, 1, 3), blend(a, b, 1, 2, 3)};
        }
        int bits = 0;
        for (int i = 0; i < 16; i++) {
            double[] c = cols[i];
            int v = 0;
            if (clear && c[3] < 2) {
                v = 3;
            } else {
                double best = Double.MAX_VALUE;
                for (int k = 0; k < pal.length; k++) {
                    double dr = c[0] - pal[k][0], dg = c[1] - pal[k][1], db = c[2] - pal[k][2];
                    double e = dr * dr + dg * dg + db * db;
                    if (e < best) {
                        best = e;
                        v = k;
                    }
                }
            }
            bits |= v << (2 * i);
        }
        put16(out, cb, c0);
        put16(out, cb + 2, c1);
        put32(out, cb + 4, bits);
    }

    private static final Pattern DXT = Pattern.compile("DXT[1-5]");

    private static List<Texture> readModTextures(byte[] mt) throws Failure {
        int[] table = null;
        for (int[] c : walkAll(mt, 0, mt.length))
            if (c[0] == 0x33310003) table = c;
        if (table == null) throw new Failure(Failure.Reason.UNSUPPORTED, "TEXTURES.BIN is not a compressed pack");
        List<Texture> out = new ArrayList<>();
        for (int i = 0; i + 24 <= table[1]; i += 24) {
            int h = i32(mt, table[2] + i), off = i32(mt, table[2] + i + 4), cs = i32(mt, table[2] + i + 8), us = i32(mt, table[2] + i + 12);
            byte[] d = unpack(sub(mt, off, off + cs), us);
            int at = rfind(d, ints(h));
            if (at < 24) throw new Failure(Failure.Reason.UNSUPPORTED, "a texture has no record");
            Texture t = new Texture();
            t.hash = h;
            t.name = text(d, at - 24, at);
            t.w = u16(d, at + 0x20);
            t.h = u16(d, at + 0x22);
            Matcher m = DXT.matcher(latin(d, at, d.length - at));
            if (!m.find()) continue;  // a format no Xbox record is copied for: one texture less, not one car less
            t.fmt = m.group();
            if (xboxFormat(t.fmt) < 0) throw new Failure(Failure.Reason.UNSUPPORTED, t.name + " is " + t.fmt);
            t.px = Arrays.copyOf(d, ((t.w + 3) / 4) * ((t.h + 3) / 4) * blockSize(t.fmt));  // the first level alone
            out.add(t);
        }
        return out;
    }

    private static void shrinkTextures(List<Texture> tex, int limit, Consumer<String> log) {
        long before = total(tex);
        while (total(tex) > limit) {
            Texture big = tex.get(0);
            for (Texture t : tex)
                if (t.px.length > big.px.length) big = t;
            if (big.w <= 32 || big.h <= 32) break;
            shrink(big);
        }
        log.accept("textures: " + tex.size() + ", " + before / 1024 + " KB, to " + total(tex) / 1024 + " KB");
    }

    private static long total(List<Texture> tex) {
        long n = 0;
        for (Texture t : tex) n += t.px.length;
        return n;
    }

    private static int ilog2(int n) {
        int r = 0;
        while ((1 << r) < n) r++;
        return r;
    }

    // the Xbox's info record of a DXT1 and of a DXT3 texture, whose fields the mod's are filled over
    private static Map<String, byte[]> templateRecords(byte[] xt) throws Failure {
        int[] table = null;
        for (int[] c : walkAll(xt, 0, xt.length))
            if (c[0] == 0x33310003) table = c;
        Map<String, byte[]> tpl = new HashMap<>();
        if (table != null) {
            for (int i = 0; i + 24 <= table[1] && tpl.size() < 2; i += 24) {
                int off = i32(xt, table[2] + i + 4), cs = i32(xt, table[2] + i + 8);
                if (!isJdlz(xt, off)) continue;
                byte[] d = jdlzDecode(sub(xt, off, off + cs));
                byte[] rec = sub(d, Math.max(0, d.length - 172), d.length);
                if (!tpl.containsKey("DXT1") && find(rec, ints(0x302, 0x303, 0x0C), 0) >= 0) tpl.put("DXT1", rec);
                if (!tpl.containsKey("DXT3") && find(rec, ints(0x302, 0x303, 0x0E), 0) >= 0) tpl.put("DXT3", rec);
            }
        }
        if (tpl.size() < 2) throw new Failure(Failure.Reason.UNSUPPORTED, "no Xbox texture record to copy from");
        // DXT5 is the DXT3 record with the NV2A's own format for it
        byte[] five = tpl.get("DXT3").clone();
        put32(five, find(five, ints(0x302, 0x303, 0x0E), 0) + 8, 0x0F);
        tpl.put("DXT5", five);
        return tpl;
    }

    private static byte[] buildTextures(List<Texture> tex, byte[] xt, String car) throws Failure {
        Map<String, byte[]> tpl = templateRecords(xt);
        int[] ci = first(xt, 0x33310001), ch = first(xt, 0x33320001);
        if (ci == null || ch == null) throw new Failure(Failure.Reason.UNSUPPORTED, "the game's texture pack has no head");
        byte[] tInfo = sub(xt, ci[2], ci[2] + ci[1]), tHead = sub(xt, ch[2], ch[2] + ch[1]);
        // the pack's name, its hash in both places
        String path = "Global\\Pipeline\\CarTemplateTextures_" + car + ".tpk";
        byte[] nameBytes = Arrays.copyOf(latin(path), 0x40);
        System.arraycopy(nameBytes, 0, tInfo, 0x20, 0x40);
        put32(tInfo, 0x60, bhash(path));
        put32(tHead, 12, bhash(path));
        List<Texture> sorted = new ArrayList<>(tex);
        sorted.sort(Comparator.comparingLong(x -> x.hash & 0xFFFFFFFFL));
        List<byte[]> blocks = new ArrayList<>();
        Buf hashes = new Buf();
        int offset = 0;
        for (Texture t : sorted) {
            byte[] rec = tpl.get(t.fmt).clone();
            byte[] nm = latin(t.name);
            Arrays.fill(rec, 0, 24, (byte) 0);
            System.arraycopy(nm, 0, rec, 0, Math.min(nm.length, 23));
            put32(rec, 24, t.hash);
            int n = t.px.length;
            put32(rec, 24 + 0xC, offset);
            put32(rec, 24 + 0x10, offset + n);
            put32(rec, 24 + 0x14, n);
            put32(rec, 24 + 0x18, 0);
            put32(rec, 24 + 0x1C, n);
            put16(rec, 24 + 0x20, t.w);
            put16(rec, 24 + 0x22, t.h);
            rec[24 + 0x24] = (byte) ilog2(t.w);
            rec[24 + 0x25] = (byte) ilog2(t.h);
            int pad = Math.floorMod(8 - (n + rec.length), 16);
            blocks.add(cat(t.px, new byte[pad], rec));
            hashes.add(ints(t.hash, 0));
            offset = align(offset + n, 0x80);
        }
        // the table's offsets count from the file's start, so the head is laid out first; its size does not depend on them
        byte[][] sized = textureHead(tInfo, tHead, hashes.bytes(), new byte[sorted.size() * 24]);
        int dataChunk = 8 + sized[0].length + 8 + sized[1].length + 8;  // b3300000, then b3320000, then 33320002's data
        int dataAt = align(dataChunk, 0x80);
        Buf data = new Buf(fill(dataAt - dataChunk, 0x11));
        Buf rows = new Buf();
        for (int i = 0; i < blocks.size(); i++) {
            byte[] block = blocks.get(i);
            byte[] z = jdlzEncode(block);
            if (!Arrays.equals(jdlzDecode(z), block)) throw new IllegalStateException("JDLZ encoder failed its round trip");
            rows.add(ints(sorted.get(i).hash, dataChunk + data.n, z.length, block.length, 0x100, 0));
            data.add(z);
            data.zeros(align(dataChunk + data.n, 16) - dataChunk - data.n);
        }
        byte[][] done = textureHead(tInfo, tHead, hashes.bytes(), rows.bytes());
        return chunk(0xB3300000, cat(done[0], chunk(0xB3320000, cat(done[1], chunk(0x33320002, data.bytes())))));
    }

    private static byte[][] textureHead(byte[] tInfo, byte[] tHead, byte[] hashes, byte[] table) {
        byte[] head = cat(chunk(0x33310001, tInfo), chunk(0x33310002, hashes), chunk(0x33310003, table));
        return new byte[][] {
            cat(chunk(0, new byte[48]), chunk(0xB3310000, head), chunk(0, new byte[28])),
            cat(chunk(0x33320001, tHead), chunk(0, new byte[80])),
        };
    }

    // the vault (GLOBAL\ATTRIBUTES.BIN)

    private static final int CLASS_ROW = 0x5E970CBC, COLL_ROW = 0x8E112EB7;
    private static final Pattern TYPE_NAME = Pattern.compile("[A-Za-z_]\\w*::[\\w:]+");

    private static long pair(int a, int b) {
        return (long) a << 32 | (b & 0xFFFFFFFFL);
    }

    private static final class VltClass {
        int layout;
        final Map<Integer, int[]> fields = new HashMap<>();  // type, offset, size, count, flags
    }

    private static final class Place {
        int bin = -1, inline = -1;
        int[] def;
    }

    private static int fourcc(String s) {
        return i32(latin(s), 0);
    }

    private static final class Vault {
        final byte[] head;
        final Buf bin;
        final byte[] vlt;
        final List<int[]> order = new ArrayList<>();   // id, offset, size
        final int datAt, datSize, dataEnd;
        final List<int[]> rows = new ArrayList<>(), newRows = new ArrayList<>();
        final List<int[]> ptrs = new ArrayList<>();    // offset, two kinds, target
        final Map<Integer, int[]> fixVlt = new HashMap<>(), binFix = new HashMap<>();
        final List<int[]> newFixVlt = new ArrayList<>(), newFixBin = new ArrayList<>();
        final Map<Integer, int[]> newFixVltAt = new HashMap<>();
        final Buf newVlt = new Buf();
        Map<Long, VltClass> unused;
        Map<Integer, VltClass> classes = new HashMap<>();
        Map<Integer, String> typeNames = new HashMap<>();
        final Map<Long, Integer> aligns = new HashMap<>(), colls = new HashMap<>();

        Vault(byte[] b) {
            head = Arrays.copyOf(b, 0x80);
            int binsz = i32(b, 0x14), vltsz = i32(b, 0x18), binoff = i32(b, 0x1C), vltoff = i32(b, 0x20);
            bin = new Buf(sub(b, binoff, binoff + binsz));
            vlt = sub(b, vltoff, vltoff + vltsz);
            int off = 0, dat = -1, datN = 0, exp = -1, ptr = -1, ptrN = 0;
            while (off + 8 <= vlt.length) {
                int id = i32(vlt, off), s = i32(vlt, off + 4);
                if (s < 8) break;
                order.add(new int[] {id, off, s});
                if (id == fourcc("NtaD")) {
                    dat = off;
                    datN = s;
                } else if (id == fourcc("NpxE")) {
                    exp = off;
                } else if (id == fourcc("NrtP")) {
                    ptr = off;
                    ptrN = s;
                }
                off += s;
            }
            if (dat < 0 || exp < 0 || ptr < 0) throw new IllegalArgumentException("not a vault");
            datAt = dat;
            datSize = datN;
            dataEnd = dat + datN;
            int n = i32(vlt, exp + 8);
            for (int i = 0; i < n; i++) {
                int[] r = new int[5];
                for (int j = 0; j < 5; j++) r[j] = i32(vlt, exp + 12 + i * 20 + j * 4);
                rows.add(r);
            }
            for (int i = 0; i < (ptrN - 8) / 12; i++) {
                int at = ptr + 8 + i * 12;
                ptrs.add(new int[] {i32(vlt, at), u16(vlt, at + 4), u16(vlt, at + 6), i32(vlt, at + 8)});
            }
            int cur = -1;
            Map<Integer, int[]> fixBin = new HashMap<>();
            for (int[] p : ptrs) {
                if (p[1] == 2) cur = p[2];
                else if (p[1] == 0) break;
                else (cur == 0 ? fixVlt : fixBin).put(p[0], p);
            }
            binFix.putAll(fixBin);
            Matcher m = TYPE_NAME.matcher(latin(bin.a, 0, bin.n));
            while (m.find()) typeNames.put(attribKey(m.group()), m.group());
            for (int[] r : rows) {
                if (r[1] == CLASS_ROW) {
                    int k = i32(vlt, r[4]), nf = i32(vlt, r[4] + 8);
                    VltClass c = new VltClass();
                    c.layout = i32(vlt, r[4] + 16);
                    int f = fixVlt.get(r[4] + 12)[3];
                    for (int i = 0; i < nf; i++) {
                        int at = f + i * 16;
                        int key = i32(bin.a, at);
                        c.fields.put(key, new int[] {i32(bin.a, at + 4), u16(bin.a, at + 8), u16(bin.a, at + 10),
                                u16(bin.a, at + 12), bin.a[at + 14] & 255});
                        aligns.put(pair(k, key), 1 << (bin.a[at + 15] & 255));
                    }
                    classes.put(k, c);
                } else if (r[1] == COLL_ROW) {
                    colls.put(pair(i32(vlt, r[4] + 4), i32(vlt, r[4])), r[4]);
                }
            }
        }

        boolean has(String cls, String node) {
            return colls.containsKey(pair(attribKey(cls), attribKey(node)));
        }

        private int[] allFix(int off) {
            int[] f = newFixVltAt.get(off);
            return f != null ? f : fixVlt.get(off);
        }

        private byte[] read(int off, int n) {
            if (off >= dataEnd) return Arrays.copyOfRange(newVlt.a, off - dataEnd, off - dataEnd + n);
            return Arrays.copyOfRange(vlt, off, off + n);
        }

        private void write(int off, byte[] b) {
            if (off >= dataEnd) System.arraycopy(b, 0, newVlt.a, off - dataEnd, b.length);
            else System.arraycopy(b, 0, vlt, off, b.length);
        }

        private void addFixVlt(int[] f) {
            newFixVlt.add(f);
            newFixVltAt.putIfAbsent(f[0], f);
        }

        // the layout and each value out of the bin, with their null and string pointers
        private int copy(int off, int n) {
            int base = align(bin.n, 16);
            bin.zeros(base - bin.n);
            byte[] part = Arrays.copyOfRange(bin.a, off, off + n);
            bin.add(part);
            for (int q = off; q < off + n; q += 4) {
                int[] f = binFix.get(q);
                if (f != null) {
                    int[] moved = {base + q - off, f[1], f[2], f[3]};
                    newFixBin.add(moved);
                    binFix.put(moved[0], moved);
                }
            }
            return base;
        }

        void cloneNode(String cls, String src, String dst, String parent) {
            int c = attribKey(cls);
            int at = colls.get(pair(c, attribKey(src)));
            byte[] h = read(at, 32);
            int ne = i32(h, 20), nt = i32(h, 24) & 0xFFFF;
            VltClass vc = classes.get(c);
            int size = 32 + nt * 4 + ne * 12;
            byte[] rec = read(at, size);
            put32(rec, 0, attribKey(dst));
            if (parent != null) put32(rec, 8, attribKey(parent));
            newVlt.zeros(align(newVlt.n, 8) - newVlt.n);
            int newAt = dataEnd + newVlt.n;
            int[] lf = allFix(at + 0x1C);
            if (lf != null) {
                int nl = copy(lf[3], vc.layout);
                addFixVlt(new int[] {newAt + 0x1C, lf[1], lf[2], nl});
            }
            int entsAt = 32 + nt * 4;
            for (int e = 0; e < ne; e++) {
                int fk = i32(rec, entsAt + e * 12), fl = u16(rec, entsAt + e * 12 + 10);
                int[] pf = allFix(at + entsAt + e * 12 + 4);
                int[] d = vc.fields.get(fk);
                if (pf == null || (fl & 0x20) != 0 || d == null) continue;
                Integer al = aligns.get(pair(c, fk));
                int nv = copy(pf[3], valueSize(pf[3], d, al == null ? 4 : al));
                addFixVlt(new int[] {newAt + entsAt + e * 12 + 4, pf[1], pf[2], nv});
            }
            newVlt.add(rec);
            newRows.add(new int[] {attribKey(String.format("%08x/%s", c, dst)), COLL_ROW, 0, size, newAt});
            colls.put(pair(c, attribKey(dst)), newAt);
        }

        private int valueSize(int off, int[] d, int al) {
            if ((d[4] & 1) != 0) {
                int cap = u16(bin.a, off + 2), el = u16(bin.a, off + 4);
                return align(8, al) + cap * el;
            }
            return d[2];
        }

        // where a field's value lives in this node; no place when only its parent has it
        Place fieldPlace(String cls, String node, String field, int index) {
            int c = attribKey(cls), fk = attribKey(field);
            int[] d = classes.get(c).fields.get(fk);
            Place out = new Place();
            out.def = d;
            if (d == null) return out;
            int at = colls.get(pair(c, attribKey(node)));
            byte[] h = read(at + 20, 8);
            int ne = i32(h, 0), nt = i32(h, 4) & 0xFFFF;
            int ents = at + 32 + nt * 4;
            Integer alObj = aligns.get(pair(c, fk));
            int al = alObj == null ? 4 : alObj;
            if ((d[4] & 2) != 0) {
                int[] f = allFix(at + 0x1C);
                if (f == null) return out;
                if ((d[4] & 1) == 0) {
                    out.bin = f[3] + d[1];
                    return out;
                }
                // an array in the layout has its count, room and element size there too
                out.bin = arrayItem(f[3] + d[1], index, al);
                return out;
            }
            for (int e = 0; e < ne; e++) {
                byte[] ent = read(ents + e * 12, 12);
                if (i32(ent, 0) != fk) continue;
                if ((u16(ent, 10) & 0x20) != 0) {
                    out.inline = ents + e * 12 + 4;
                    return out;
                }
                int[] f = allFix(ents + e * 12 + 4);
                if (f == null) return out;
                out.bin = (d[4] & 1) != 0 ? arrayItem(f[3], index, al) : f[3];
                return out;
            }
            return out;
        }

        private int arrayItem(int base, int index, int al) {
            int count = u16(bin.a, base), cap = u16(bin.a, base + 2), el = u16(bin.a, base + 4);
            if (index >= cap) return -1;
            if (index >= count) put16(bin.a, base, index + 1);
            return base + align(8, al) + index * el;
        }

        // null, or why not
        String setValue(String cls, String node, String field, int index, String member, String text) {
            Place p = fieldPlace(cls, node, field, index);
            if (p.def == null) return "no such field";
            if (p.bin < 0 && p.inline < 0) return "not in this node";
            int at = 0;
            byte[] raw;
            if (member != null) {
                Integer m = MEMBERS.get(member);
                if (m == null) return "unknown member " + member;
                at = m;
                raw = member.equals("Collection") || member.equals("Class") ? ints(attribKey(text)) : packFloat(text);
            } else {
                String type = typeNames.get(p.def[0]);
                raw = packScalar(text, type == null ? "" : type, p.def[2]);
            }
            if (raw == null) return "value " + text;
            if (p.inline >= 0) write(p.inline + at, raw);
            else System.arraycopy(raw, 0, bin.a, p.bin + at, raw.length);
            return null;
        }

        // one name for the model and its collision: a StringKey, its two hashes and its text
        void setModel(String node, String name) {
            int place = fieldPlace("pvehicle", node, "MODEL", 0).bin;
            long k = stringKey64(name);
            put32(bin.a, place, (int) k);
            put32(bin.a, place + 4, (int) (k >>> 32));
            put32(bin.a, place + 8, attribKey(name));
            setText(place + 12, name);
        }

        void setLayoutText(String cls, String node, int offset, String text) {
            setText(allFix(colls.get(pair(attribKey(cls), attribKey(node))) + 0x1C)[3] + offset, text);
        }

        // the frontend's make, a byte (0x134250's table, 0 none): it, not the car type's, names the car's CARNAME_%s_%s
        // (0x140C60) and its logo. A make the game has not keeps the node's, whose name is the one returned
        String setMake(String node, String manufacturer) {
            int at = allFix(colls.get(pair(attribKey("frontend"), attribKey(node))) + 0x1C)[3] + MAKE_OFFSET;
            int make = makeIndex(manufacturer);
            if (make <= 0) make = bin.a[at] & 255;
            if (make <= 0 || make >= MAKES.length) make = makeIndex("LAMBORGHINI");
            bin.a[at] = (byte) make;
            return MAKES[make];
        }

        // the wheels' places in a car's own copy of the Gallardo's; z stays, and each y keeps its side
        void setWheels(String node, WheelPlaces w) {
            for (int i = 0; i < 4; i++) {
                int at = fieldPlace("ecar", node, "TireOffsets", i).bin;
                if (at < 0) continue;
                boolean isFront = i < 2;
                float y = f32(bin.a, at + 4);
                putF(bin.a, at, (float) (isFront ? w.frontX : w.rearX));
                putF(bin.a, at + 4, (float) (y < 0 ? -(isFront ? w.frontY : w.rearY) : (isFront ? w.frontY : w.rearY)));
                putF(bin.a, at + 12, (float) w.radius);
                int skid = fieldPlace("ecar", node, "TireSkidWidth", i).bin;
                if (skid >= 0) putF(bin.a, skid, (float) w.width);
            }
        }

        private void setText(int ptr, String text) {
            int at = bin.n;
            bin.add(latin(text + "\0"));
            newFixBin.removeIf(f -> f[0] == ptr);
            int[] f = {ptr, 3, 1, at};
            newFixBin.add(f);
            binFix.put(ptr, f);
        }

        // the bin's end moves, the fixups' groups are 1 for the bin, 0 for the vlt, in that order
        byte[] write() {
            Buf v = new Buf();
            for (int[] ch : order) {
                if (ch[0] == fourcc("NtaD")) {
                    int bodyN = datSize - 8 + newVlt.n, n = align(8 + bodyN, 16);
                    v.add(ints(ch[0], n)).add(vlt, datAt + 8, datSize - 8).add(newVlt.a, 0, newVlt.n).zeros(n - 8 - bodyN);
                } else if (ch[0] == fourcc("NpxE")) {
                    Buf body = new Buf();
                    body.add(ints(rows.size() + newRows.size()));
                    for (int[] r : rows) body.add(ints(r));
                    for (int[] r : newRows) body.add(ints(r));
                    int padFrom = ch[1] + 12 + rows.size() * 20, padN = ch[1] + ch[2] - padFrom;
                    int n = align(8 + body.n + padN, 16);
                    v.add(ints(ch[0], n)).add(body.a, 0, body.n).add(vlt, padFrom, padN).zeros(n - 8 - body.n - padN);
                } else if (ch[0] == fourcc("NrtP")) {
                    Buf out = new Buf();
                    int cur = -1;
                    for (int[] p : ptrs) {
                        if (p[1] == 2) {
                            if (cur >= 0) putFixes(out, cur);
                            cur = p[2];
                        } else if (p[1] == 0) {
                            if (cur >= 0) putFixes(out, cur);
                            cur = -1;
                        }
                        putFix(out, p);
                    }
                    int n = align(8 + out.n, 16);
                    v.add(ints(ch[0], n)).add(out.a, 0, out.n).zeros(n - 8 - out.n);
                } else {
                    v.add(vlt, ch[1], ch[2]);
                }
            }
            int vltOff = align(0x80 + bin.n, 0x80);
            byte[] h = head.clone();
            put32(h, 0x14, bin.n);
            put32(h, 0x18, v.n);
            put32(h, 0x1C, 0x80);
            put32(h, 0x20, vltOff);
            Buf out = new Buf();
            out.add(h).add(bin.a, 0, bin.n).zeros(vltOff - 0x80 - bin.n).add(v.a, 0, v.n);
            out.zeros(align(out.n, 0x80) - out.n);
            return out.bytes();
        }

        private void putFixes(Buf out, int group) {
            for (int[] f : group == 0 ? newFixVlt : newFixBin) putFix(out, f);
        }

        private static void putFix(Buf out, int[] f) {
            byte[] r = new byte[12];
            put32(r, 0, f[0]);
            put16(r, 4, f[1]);
            put16(r, 6, f[2]);
            put32(r, 8, f[3]);
            out.add(r);
        }
    }

    private static final Map<String, Integer> MEMBERS = new HashMap<>();
    static {
        String[] names = {"Front", "Rear", "X", "Y", "Z", "W", "DegPerG", "DegPerSec", "MaxG", "Class", "Collection"};
        int[] at = {0, 4, 0, 4, 8, 12, 0, 4, 8, 0, 4};
        for (int i = 0; i < names.length; i++) MEMBERS.put(names[i], at[i]);
    }

    private static byte[] packFloat(String text) {
        try {
            return ints(Float.floatToRawIntBits((float) Double.parseDouble(text)));
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private static byte[] packScalar(String text, String type, int size) {
        try {
            if (type.contains("Float")) return packFloat(text);
            if (type.contains("Bool")) {
                String t = text.toLowerCase(Locale.ROOT);
                return new byte[] {(byte) (t.equals("true") || t.equals("1") ? 1 : 0)};
            }
            String t = text.toLowerCase(Locale.ROOT);
            if (type.contains("Int") || type.contains("Enum") || t.startsWith("0x")) {
                boolean neg = t.startsWith("-");
                String digits = neg ? t.substring(1) : t;
                BigInteger v = digits.startsWith("0x") ? new BigInteger(digits.substring(2), 16) : new BigInteger(digits);
                if (neg) v = v.negate();
                byte[] out = new byte[size];
                for (int i = 0; i < size; i++) out[i] = v.shiftRight(i * 8).byteValue();
                return out;
            }
        } catch (NumberFormatException e) {
            // no number
        }
        return null;
    }

    private static final Pattern FIELD = Pattern.compile("(\\w+)(?:\\[(\\d+)\\])?");

    // VltEd's script, the parts of it the Xbox's database holds: nodes and their values. FE_ATTRIB holds the frontend
    // class's nodes, whose definition is in ATTRIBUTES. Smoke with its place moved is not worth new nodes
    private static void applyScript(Vault main, Vault fe, byte[] script, Consumer<String> log) {
        Map<String, Integer> skipped = new TreeMap<>();
        int done = 0;
        for (String line : latin(script, 0, script.length).split("\n")) {
            String t = line.trim();
            if (t.isEmpty()) continue;
            String[] w = t.split("\\s+");
            String op = w[0], cls = w.length > 1 ? w[1] : "";
            if (cls.equals("emitterdata") || cls.equals("emittergroup")) continue;
            Vault v = cls.equals("frontend") ? fe : main;
            if (op.equals("copy_node") && w.length >= 5) {
                if (!v.has(cls, w[2])) {
                    skipped.merge("copy " + cls + " " + w[2] + ": source missing", 1, Integer::sum);
                    continue;
                }
                if (v.has(cls, w[4])) continue;  // another add-on's script made it: names are the mods' to clash
                v.cloneNode(cls, w[2], w[4], w[3]);
                done++;
            } else if (op.equals("update_field") && w.length >= 5) {
                String node = w[2];
                Matcher m = FIELD.matcher(w[3]);
                if (!m.matches()) continue;
                String name = m.group(1);
                int index = m.group(2) == null ? 0 : Integer.parseInt(m.group(2));
                String member = w.length > 5 ? w[4] : null, text = w[w.length - 1];
                if (!v.has(cls, node)) {
                    skipped.merge("update of " + cls + " " + node, 1, Integer::sum);
                    continue;
                }
                if (name.equals("MODEL")) {
                    v.setModel(node, text);
                    continue;
                }
                String why = v.setValue(cls, node, name, index, member, text);
                if (why != null) skipped.merge(cls + "." + name + ": " + why, 1, Integer::sum);
                else done++;
            } else if ((op.equals("add_field") || op.equals("resize_field")) && w.length > 3) {
                skipped.merge(op + " " + cls + "." + w[3], 1, Integer::sum);
            }
        }
        log.accept("vault: " + done + " script lines applied");
        for (Map.Entry<String, Integer> e : skipped.entrySet()) log.accept("  skipped " + e.getKey() + " (" + e.getValue() + ")");
    }

    // GLOBAL\GLOBALB: the car's record, its parts' table entry and collision, all in place: the size stays and with it the
    // alignment of the textures after them

    private static List<String> carNames(byte[] g) throws Failure {
        for (int[] c : chunks(g, 0, g.length)) {
            if (c[0] != 0x34600) continue;
            List<String> names = new ArrayList<>();
            for (int i = 0; i < 84; i++) names.add(text(g, c[2] + 8 + i * 0xD0, c[2] + 8 + i * 0xD0 + 16));
            return names;
        }
        throw new Failure(Failure.Reason.UNSUPPORTED, "no car table in the game's files");
    }

    private static byte[] insert(byte[] g, int at, byte[] what) {
        byte[] out = new byte[g.length + what.length];
        System.arraycopy(g, 0, out, 0, at);
        System.arraycopy(what, 0, out, at, what.length);
        System.arraycopy(g, at, out, at + what.length, g.length - at);
        return out;
    }

    // what is added to a chunk is a multiple of 0x80, to keep the alignment of what follows it (textures, on 0x80). From the
    // file's end to its start, so the offsets stay good
    private static byte[] patchGlobal(byte[] gIn, List<Kept> cars, Consumer<String> log) throws Failure {
        byte[] g = gIn;
        Map<Integer, int[]> sub = new HashMap<>();
        int[] types = null, partsChunk = null, collisions = null;
        for (int[] c : chunks(g, 0, g.length)) {
            if (c[0] == 0x34600) types = c;
            else if (c[0] == 0x80034602) {
                partsChunk = c;
                for (int[] c2 : chunks(g, c[2], c[2] + c[1])) sub.put(c2[0], c2);
            } else if (c[0] == 0x8003B900) collisions = c;
        }
        List<String> names = carNames(g);
        int gallardo = names.indexOf("GALLARDO");
        if (types == null || partsChunk == null || collisions == null || gallardo < 0 || !sub.containsKey(0x3460B)
                || !sub.containsKey(0x34604) || !sub.containsKey(0x34603))
            throw new Failure(Failure.Reason.UNSUPPORTED, "not the game's car tables");

        // a collision of its own for each, the Gallardo's
        int s = collisions[1], o = collisions[2], cid = 0;
        byte[] item = null;
        for (int[] c2 : chunks(g, o, o + s)) {
            int p = payload(g, c2[2], c2[1]);
            if (i32(g, p) == attribKey("GALLARDO")) {
                item = Arrays.copyOfRange(g, p, c2[2] + c2[1]);
                cid = c2[0];
            }
        }
        if (item == null) throw new Failure(Failure.Reason.UNSUPPORTED, "no Gallardo collision to copy");
        Buf added = new Buf();
        int end = o + s;
        for (Kept car : cars) {
            int fillN = Math.floorMod(16 - (end + added.n + 8) % 16, 16);
            byte[] body = cat(fill(fillN, 0x11), item);
            put32(body, fillN, attribKey(car.car));
            int grow = align(8 + body.length, 0x80);
            added.add(ints(cid, grow - 8)).add(body).zeros(grow - 8 - body.length);
        }
        g = insert(g, end, added.bytes());
        put32(g, o - 4, s + added.n);

        // the template's parts again for each, under a new index in the parts' car table
        int[] tab = sub.get(0x3460B), parts = sub.get(0x34604), head = sub.get(0x34603);
        int carCount = tab[1] / 4, rows = parts[1] / 14, template = -1;
        for (int i = 0; i < carCount; i++)
            if (i32(g, tab[2] + i * 4) == bhash(TEMPLATE_PARTS)) template = i;
        if (template < 0) throw new Failure(Failure.Reason.UNSUPPORTED, "no " + TEMPLATE_PARTS + " in the parts' car table");
        if (carCount + cars.size() > 255) throw new Failure(Failure.Reason.UNSUPPORTED, "too many cars for the parts' table");
        Buf run = new Buf();
        for (int i = 0; i < rows; i++)
            if ((g[parts[2] + i * 14 + 7] & 255) == template) run.add(g, parts[2] + i * 14, 14);
        byte[] one = run.bytes();
        hideExtraParts(g, sub, one, log);
        Buf newRows = new Buf(), newHashes = new Buf();
        for (int i = 0; i < cars.size(); i++) {
            byte[] mine = one.clone();
            for (int r = 0; r < mine.length; r += 14) mine[r + 7] = (byte) (carCount + i);
            newRows.add(mine);
            newHashes.add(ints(bhash(cars.get(i).car)));
        }
        int grown = newRows.n + newHashes.n, pad = align(grown, 0x80) - grown;
        newRows.zeros(pad);
        g = insert(g, parts[2] + rows * 14, newRows.bytes());
        put32(g, parts[2] - 4, parts[1] + newRows.n);
        g = insert(g, tab[2] + tab[1], newHashes.bytes());
        put32(g, tab[2] - 4, tab[1] + newHashes.n);
        put32(g, partsChunk[2] - 4, partsChunk[1] + newRows.n + newHashes.n);
        put32(g, head[2] + 0x28, carCount + cars.size());
        put32(g, head[2] + 0x38, rows + cars.size() * (one.length / 14));

        // the car types: an empty one for 84, the "no car" answer, then the add-ons
        int rowsAt = types[2] + 8;
        byte[] rec = Arrays.copyOfRange(g, rowsAt + gallardo * 0xD0, rowsAt + (gallardo + 1) * 0xD0);
        Buf newTypes = new Buf();
        byte[] none = new byte[0xD0];
        put32(none, 0x90, STOCK_TYPES);
        newTypes.add(none);
        for (int i = 0; i < cars.size(); i++) {
            Kept car = cars.get(i);
            byte[] r = rec.clone();
            System.arraycopy(Arrays.copyOf(latin(car.car), 16), 0, r, 0, 16);
            System.arraycopy(Arrays.copyOf(latin(car.car), 16), 0, r, 0x10, 16);
            System.arraycopy(Arrays.copyOf(latin("CARS\\" + car.car + "\\GEOMETRY.BIN"), 32), 0, r, 0x20, 32);
            System.arraycopy(Arrays.copyOf(latin(car.manufacturer), 16), 0, r, 0x40, 16);
            put32(r, 0x50, bhash(car.car));
            put32(r, 0x90, FIRST_ADDON_TYPE + i);
            newTypes.add(r);
        }
        newTypes.zeros(align(newTypes.n, 0x80) - newTypes.n);
        g = insert(g, types[2] + types[1], newTypes.bytes());
        put32(g, types[2] - 4, types[1] + newTypes.n);
        return g;
    }

    private static final Pattern EXTRA_PART = Pattern.compile("_KIT0[1-9]|_STYLE\\d\\d");

    // the mod has one body: other kits' and hoods' parts name models it does not have. Slot 0xFF is no slot: the records
    // stay in their place in the table, which is searched by car
    private static void hideExtraParts(byte[] g, Map<Integer, int[]> sub, byte[] rows, Consumer<String> log) {
        int[] strings = sub.get(0x34606), uses = sub.get(0x3460A);
        int hidden = 0;
        for (int at = 0; at + 14 <= rows.length; at += 14) {
            int um = u16(rows, at + 0xC);
            if (um * 24 >= uses[1]) continue;
            int flag = g[uses[2] + um * 24] & 255, base = u16(g, uses[2] + um * 24 + 2);
            if (flag == 0 || base == 0xFFFF) continue;
            String name = text(g, strings[2] + base * 4, strings[2] + strings[1]);
            if (EXTRA_PART.matcher(name).lookingAt()) {
                rows[at + 4] = (byte) 0xFF;
                hidden++;
            }
        }
        log.accept("parts: " + hidden + " of " + rows.length / 14 + " other kits' hidden");
    }

    // the boot's bundle of small files (0x1C7B10), which the game takes before the disc: hash, place, two sizes, an
    // address it keeps. Each on 16 bytes
    private static byte[] rebuildMemoryFile(byte[] mf, Map<Integer, byte[]> changed) {
        int n = i32(mf, 12);
        Buf out = new Buf();
        out.add(mf, 0, 16 + n * 20);
        out.zeros(align(out.n, 16) - out.n);
        for (int i = 0; i < n; i++) {
            int row = 16 + i * 20;
            int h = i32(mf, row), place = i32(mf, row + 4), size = i32(mf, row + 12);
            byte[] d = changed.get(h);
            if (d != null) {
                put32(out.a, row + 8, d.length);
                put32(out.a, row + 12, d.length);
            }
            put32(out.a, row + 4, out.n);
            if (d != null) out.add(d);
            else out.add(mf, place, size);
            out.zeros(align(out.n, 16) - out.n);
        }
        return out.bytes();
    }

    // the disc's file list: the given files in a new archive, the disc's last one with nothing in it, written to out
    private static void buildZdir(Disc disc, Map<String, Object> files, Map<String, String> alias, File dir, Built built) throws IOException {
        int last = 0;
        for (int[] r : disc.recs.values()) last = Math.max(last, r[1]);
        if (disc.archiveSize(last) != 0) last++;
        long absBase = 0;
        for (int[] r : disc.recs.values())
            if (r[4] != 0) absBase = Math.max(absBase, (r[3] & 0xFFFFFFFFL) + (r[4] + 2047) / 2048);
        Map<Integer, int[]> recs = new TreeMap<>(Integer::compareUnsigned);
        recs.putAll(disc.recs);
        String archive = "NFS/ZZDATA" + last + ".BIN";
        long written = 0;
        File outFile = new File(dir, archive.replace('/', '_'));
        try (BufferedOutputStream out = new BufferedOutputStream(new FileOutputStream(outFile), 1 << 16)) {
            byte[] chunkBuf = new byte[1 << 16];
            for (Map.Entry<String, Object> e : files.entrySet()) {
                int h = bhash(e.getKey());
                long size;
                if (e.getValue() instanceof byte[]) {
                    byte[] b = (byte[]) e.getValue();
                    out.write(b);
                    size = b.length;
                } else {
                    size = 0;
                    try (FileInputStream in = new FileInputStream((File) e.getValue())) {
                        for (int r; (r = in.read(chunkBuf)) > 0; size += r) out.write(chunkBuf, 0, r);
                    }
                }
                int sector = (int) (written / 2048);
                recs.put(h, new int[] {h, last, sector, (int) (absBase + sector), (int) size, -1});
                long padded = (size + 2047) / 2048 * 2048;
                for (long p = size; p < padded; p++) out.write(0);
                written += padded;
            }
        }
        for (Map.Entry<String, String> e : alias.entrySet()) {
            int[] r = disc.recs.get(bhash(e.getValue())).clone();
            r[0] = bhash(e.getKey());
            recs.put(r[0], r);
        }
        Buf z = new Buf();
        for (int[] r : recs.values()) z.add(ints(r));
        built.files.put("NFS/ZDIR.BIN", z.bytes());
        built.files.put(archive, null);  // already in dir
    }

    // the path's file under the mod, by its last names in any case
    private static String findFile(Map<String, byte[]> mod, String nameRegex) {
        Pattern p = Pattern.compile("(.*/)?" + nameRegex, Pattern.CASE_INSENSITIVE);
        return mod.keySet().stream().sorted().filter(k -> p.matcher(k).matches()).findFirst().orElse(null);
    }

    static String titleCase(String car) {
        return car.isEmpty() ? car : car.charAt(0) + car.substring(1).toLowerCase(Locale.ROOT);
    }

    // Binary's installers keep a car in CARS, or in a pack in Car, beside its collections and strings
    private static final Pattern GEO_NAME = Pattern.compile("(.*/)?cars?(?:_replace)?/([^/]+)/geometry\\.bin", Pattern.CASE_INSENSITIVE);
    // some repacks have the car's folder loose beside their .end scripts, no CARS above it (EVO8 U2/LANCEREVO8/GEOMETRY.BIN)
    private static final Pattern LOOSE_GEO = Pattern.compile("(.*/)?([^/]+)/geometry\\.bin", Pattern.CASE_INSENSITIVE);
    private static final Pattern BINARY_ROOT = Pattern.compile("(.*/)binary/.*", Pattern.CASE_INSENSITIVE);

    // the cars in an archive, each by the path of its GEOMETRY.BIN. Under CARS when there are any; else any folder that has
    // a TEXTURES.BIN beside its GEOMETRY.BIN
    static List<String> carPaths(Collection<String> paths) {
        List<String> geos = new ArrayList<>();
        for (String k : paths)
            if (GEO_NAME.matcher(k).matches()) geos.add(k);
        if (geos.isEmpty()) {
            Set<String> lower = new HashSet<>();
            for (String k : paths) lower.add(k.toLowerCase(Locale.ROOT));
            for (String k : paths)
                if (LOOSE_GEO.matcher(k).matches()
                        && lower.contains(k.substring(0, k.length() - "GEOMETRY.BIN".length()).toLowerCase(Locale.ROOT) + "textures.bin"))
                    geos.add(k);
        }
        geos.sort(null);
        return geos;
    }

    private static Matcher geoMatch(String geoPath) {
        Matcher m = GEO_NAME.matcher(geoPath);
        if (m.matches()) return m;
        m = LOOSE_GEO.matcher(geoPath);
        return m.matches() ? m : null;
    }

    static String carName(String geoPath) {
        Matcher m = geoMatch(geoPath);
        return m != null ? m.group(2).toUpperCase(Locale.ROOT) : geoPath;
    }

    // what in a pack belongs to one car, read in: all under the folder that holds its Binary, or all under its CARS' parent,
    // but the other cars' models and paint
    static Map<String, byte[]> carFiles(Map<String, File> files, String geoPath) throws IOException {
        Matcher binary = BINARY_ROOT.matcher(geoPath);
        Matcher geo = geoMatch(geoPath);
        String root = binary.matches() ? binary.group(1) : geo == null || geo.group(1) == null ? "" : geo.group(1);
        List<String> folders = new ArrayList<>();
        for (String other : carPaths(files.keySet()))
            if (!other.equals(geoPath)) folders.add(other.substring(0, other.length() - "GEOMETRY.BIN".length()));
        Map<String, byte[]> out = new HashMap<>();
        for (Map.Entry<String, File> e : files.entrySet()) {
            String k = e.getKey();
            if (!k.startsWith(root) || folders.stream().anyMatch(k::startsWith)) continue;
            out.put(k, Files.readAllBytes(e.getValue().toPath()));
        }
        return out;
    }

    // the VltEd script for a car: named for it, else the install script, else the only one there is
    private static String findScript(Map<String, byte[]> mod, String car) {
        Pattern p = Pattern.compile("(?:.*/)?([^/]*\\.nfsms)", Pattern.CASE_INSENSITIVE);
        List<String> scripts = new ArrayList<>();
        for (String k : mod.keySet()) {
            Matcher m = p.matcher(k);
            if (m.matches() && !m.group(1).toLowerCase(Locale.ROOT).contains("uninstall")) scripts.add(k);
        }
        scripts.sort(null);
        String named = car.toLowerCase(Locale.ROOT);
        for (String k : scripts)
            if (k.substring(k.lastIndexOf('/') + 1).toLowerCase(Locale.ROOT).contains(named)) return k;
        for (String k : scripts)
            if (k.substring(k.lastIndexOf('/') + 1).toLowerCase(Locale.ROOT).contains("install")) return k;
        return scripts.size() == 1 ? scripts.get(0) : null;
    }

    // mod: one car's files by their paths in the archive, with / in them (carFiles)
    static Package convert(Disc disc, Map<String, byte[]> mod, String geoPath, int maxTexturesKb, Consumer<String> log) throws Failure, IOException {
        String car = carName(geoPath);
        if (car.length() > 15 || ("CARS\\" + car + "\\GEOMETRY.BIN").length() > 31)
            throw new Failure(Failure.Reason.NAME_TOO_LONG, car);
        if (car.equals(TEMPLATE_PARTS)) throw new Failure(Failure.Reason.UNSUPPORTED, "a car named " + car);
        Package pack = new Package();
        pack.car = car;
        byte[] mg = mod.get(geoPath), mt = null;
        String want = geoPath.substring(0, geoPath.length() - "GEOMETRY.BIN".length()) + "TEXTURES.BIN";
        for (String k : mod.keySet())
            if (k.equalsIgnoreCase(want)) mt = mod.get(k);
        if (mt == null) throw new Failure(Failure.Reason.NO_CAR, "no TEXTURES.BIN beside " + geoPath);

        List<Solid> solids = new ArrayList<>();
        for (byte[] d : readModSolids(mg)) {
            Solid s = convertSolid(d, car);
            if (s != null) solids.add(s);
        }
        if (solids.isEmpty()) throw new Failure(Failure.Reason.UNSUPPORTED, "no solid the converter knows in GEOMETRY.BIN");
        log.accept("geometry: " + solids.size() + " solids");
        mergeAttachments(solids, log);
        reduceLods(solids, log);
        pack.geometry = buildGeometry(solids, disc.load("CARS\\GALLARDO\\GEOMETRY.BIN"));

        List<Texture> tex = readModTextures(mt);
        shrinkTextures(tex, maxTexturesKb * 1024, log);
        pack.textures = buildTextures(tex, disc.load("CARS\\GALLARDO\\TEXTURES.BIN"), car);

        pack.manufacturer = "LAMBORGHINI";
        String ci = findFile(mod, "cartypeinfo[^/]*\\.bin");
        if (ci != null) {
            byte[] raw = mod.get(ci);
            Matcher m = Pattern.compile(Pattern.quote(car) + "\0([A-Z0-9 ]+)\0").matcher(latin(raw, 0, raw.length));
            if (m.find()) pack.manufacturer = m.group(1);
        }
        String script = findScript(mod, car);
        if (script != null) pack.script = mod.get(script);
        pack.title = titleCase(car);
        // the name from the strings an installer adds: english.end's first, then any other .end (a repack's own script).
        // With no CarTypeInfo, the string's make is the car's too
        List<String> ends = new ArrayList<>();
        for (String k : mod.keySet()) if (k.toLowerCase(Locale.ROOT).endsWith(".end")) ends.add(k);
        ends.sort(Comparator.comparing((String k) -> !k.toLowerCase(Locale.ROOT).endsWith("english.end")).thenComparing(k -> k));
        Pattern named = Pattern.compile("CARNAME_([A-Z0-9]+)_" + Pattern.quote(car) + "\\s+\"([^\"]+)\"");
        String[] found = null, any = null;
        for (String k : ends) {
            byte[] raw = mod.get(k);
            Matcher m = named.matcher(latin(raw, 0, raw.length));
            while (found == null && m.find()) {
                if (m.group(1).equals(pack.manufacturer)) found = new String[]{m.group(1), m.group(2)};
                else if (any == null) any = new String[]{m.group(1), m.group(2)};
            }
            if (found != null) break;
        }
        if (found == null && ci == null) found = any;
        if (found != null) {
            pack.manufacturer = found[0];
            pack.title = found[1];
        }
        // the car select's name is a texture in the front end's pack, SECONDARY_LOGO_%s; an add-on brings its own
        String logo = findFile(mod, Pattern.quote("SECONDARY_LOGO_" + car + ".dds"));
        if (logo != null) {
            byte[] dds = mod.get(logo);
            if (dds.length >= 128) {
                String fmt = latin(dds, 84, 4);
                int lh = i32(dds, 12), lw = i32(dds, 16);
                if (xboxFormat(fmt) >= 0) {
                    pack.logo = sub(dds, 128, 128 + ((lw + 3) / 4) * ((lh + 3) / 4) * blockSize(fmt));
                    pack.logoWidth = lw;
                    pack.logoHeight = lh;
                    pack.logoFormat = xboxFormat(fmt);
                }
            }
        }
        return pack;
    }

    static boolean isStockCar(Disc disc, String car) throws Failure, IOException {
        return carNames(jdlzDecode(disc.load("GLOBAL\\GLOBALB.LZC"))).contains(car);
    }

    // the name texture's file for src/game/car_mods.cpp, under the mods folder
    // a name texture for a car that brings none: 4 bit alpha from the drawn text, all white, in DXT3. alpha is width x height
    static void setLogo(Package pack, int[] argb, int width, int height) {
        byte[] out = new byte[width / 4 * (height / 4) * 16];
        int at = 0;
        for (int y = 0; y + 4 <= height; y += 4) {
            for (int x = 0; x + 4 <= width; x += 4, at += 16) {
                for (int r = 0; r < 4; r++) {
                    int row = 0;
                    for (int c = 0; c < 4; c++) row |= (argb[(y + r) * width + x + c] >>> 28) << (c * 4);
                    put16(out, at + r * 2, row);
                }
                put16(out, at + 8, 0xFFFF);
                put16(out, at + 10, 0xFFFF);
            }
        }
        pack.logo = out;
        pack.logoWidth = width;
        pack.logoHeight = height;
        pack.logoFormat = xboxFormat("DXT3");
    }

    static String logoFile(String car) {
        return String.format("name_%08x.dxt", bhash("SECONDARY_LOGO_" + car));
    }

    // all the kept cars as the game's files. The archive goes straight to dir, as NFS_ZZDATAn.BIN: it may be large
    static Built build(Disc disc, List<Kept> kept, File dir, Consumer<String> log) throws Failure, IOException {
        Built built = new Built();
        byte[] g = jdlzDecode(disc.load("GLOBAL\\GLOBALB.LZC"));
        List<String> stock = carNames(g);
        List<Kept> addons = new ArrayList<>();
        Map<String, Object> files = new LinkedHashMap<>();
        Map<String, String> alias = new LinkedHashMap<>();
        for (Kept k : kept) {
            if (stock.contains(k.car)) {
                // named as one of the game's: its model and paint in that car's place, the rest stays the game's
                built.replaced.add(k.car);
            } else {
                addons.add(k);
                built.addons.add(k.car);
            }
            files.put("CARS\\" + k.car + "\\GEOMETRY.BIN", k.geometry);
            files.put("CARS\\" + k.car + "\\TEXTURES.BIN", k.textures);
        }
        if (addons.size() > MAX_ADDONS) throw new Failure(Failure.Reason.UNSUPPORTED, "more than " + MAX_ADDONS + " add-on cars");
        if (!addons.isEmpty()) {
            g = patchGlobal(g, addons, log);
            byte[] lzc = jdlzEncode(g);
            if (!Arrays.equals(jdlzDecode(lzc), g)) throw new IllegalStateException("JDLZ encoder failed its round trip");

            Vault v = new Vault(disc.load("GLOBAL\\ATTRIBUTES.BIN"));
            Vault fe = new Vault(disc.load("GLOBAL\\FE_ATTRIB.BIN"));
            fe.classes = v.classes;
            fe.typeNames = v.typeNames;
            StringBuilder list = new StringBuilder();
            for (Kept k : addons) {
                String node = k.car.toLowerCase(Locale.ROOT);
                if (k.script != null) applyScript(v, fe, k.script, log);
                // a script of another name for the car, or none: the Gallardo's handling
                if (!v.has("ecar", node)) {
                    v.cloneNode("ecar", "gallardo", node, null);
                    WheelPlaces w = measureWheels(readBuiltSolids(Files.readAllBytes(k.geometry.toPath())));
                    if (w != null) {
                        v.setWheels(node, w);
                        log.accept(String.format(Locale.ROOT, "%s: wheels at x %.3f, %.3f, y %.3f, %.3f, radius %.3f", k.car, w.frontX, w.rearX, w.frontY, w.rearY, w.radius));
                    }
                }
                if (!v.has("pvehicle", node)) v.cloneNode("pvehicle", "gallardo", node, null);
                if (!fe.has("frontend", node)) fe.cloneNode("frontend", "gallardo", node, null);
                // a copied pvehicle still names the Gallardo's: the car select then took this car for it
                v.setValue("pvehicle", node, "frontend", 0, "Collection", node);
                v.setModel(node, k.car);
                fe.setLayoutText("frontend", node, 0x18, k.car);  // the name in CARNAME_%s_%s (0x140CC1)
                String make = fe.setMake(node, k.manufacturer);
                alias.put("CARS\\" + k.car + "\\VINYLS.BIN", "CARS\\GALLARDO\\VINYLS.BIN");
                alias.put("CARS\\" + k.car + "\\PREVINYL.BIN", "CARS\\GALLARDO\\PREVINYL.BIN");
                // for src/game/car_mods.cpp: the profile's list and the car's name
                String logoInfo = "0 0 0 0";
                if (k.logo != null) {
                    built.files.put(logoFile(k.car), k.logo);
                    logoInfo = String.format("%08x %d %d %d", bhash("SECONDARY_LOGO_" + k.car), k.logoWidth, k.logoHeight, k.logoFormat);
                }
                list.append(String.format("%08x %08x %08x %s %s\n", attribKey(node), attribKey(node),
                        bhash("CARNAME_" + make + "_" + k.car), logoInfo, k.title));
            }
            byte[] attributes = v.write();
            Map<Integer, byte[]> memoryFiles = new HashMap<>();
            memoryFiles.put(bhash("GLOBAL\\ATTRIBUTES.BIN"), attributes);
            memoryFiles.put(bhash("GLOBAL\\GLOBALB.LZC"), lzc);
            byte[] memory = rebuildMemoryFile(disc.load("GLOBAL\\GLOBALMEMORYFILE.BIN"), memoryFiles);
            Map<String, Object> first = new LinkedHashMap<>();
            first.put("GLOBAL\\GLOBALB.LZC", lzc);
            first.put("GLOBAL\\GLOBALB.BUN", g);
            first.put("GLOBAL\\ATTRIBUTES.BIN", attributes);
            first.put("GLOBAL\\GLOBALMEMORYFILE.BIN", memory);
            first.put("GLOBAL\\FE_ATTRIB.BIN", fe.write());
            first.putAll(files);
            files = first;
            built.files.put("cars.txt", latin(list.toString()));
            built.files.put("car_types.txt", latin((FIRST_ADDON_TYPE + addons.size()) + "\n"));
        }
        buildZdir(disc, files, alias, dir, built);
        return built;
    }
}
