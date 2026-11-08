package com.racas.mostwanted;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.function.Consumer;

// The PC's NextGenGraphics texture packs: a TexturePackInfo.json maps a game texture (gameId, its name or 0x and its hash)
// to a .dds under rootDirectory. Each pack is kept in mods/textures/<pack>, an image a file named by the hash of its game
// texture's name (bStringHash, as GetTextureInfo is asked), which native/vulkan.cpp draws in its place
// (src/common/texture_packs.h). The images are made at most max texels across, with all their mips: a pack's 8192 texel
// textures are 85 MB each, more than a phone's GPU holds a city of. A file with mips is cut to the mip of that size; one
// without (or still too big) is decoded, shrunk by a box filter, given its mips and encoded again in its own format.
final class TexturePacks {
    // the largest an image is kept, chosen as a pack goes in (ModsActivity): the game's TEXTURE RESOLUTION picks up to it
    static final int[] SIZES = {512, 1024, 2048, 4096};
    static volatile int max = 1024;
    private static final String DIR = "textures", INFO = "info.txt", JSON = "texturepackinfo.json";

    private TexturePacks() {}

    static File dir() {
        return new File(ModInstaller.modsDir(), DIR);
    }

    // the files of an archive kept for a pack: its json, and every .dds, shrunk as it is read (ModArchive)
    static boolean wanted(String path) {
        String p = path.toLowerCase(Locale.ROOT);
        return p.endsWith("/" + JSON) || p.equals(JSON) || p.endsWith(".dds");
    }

    static boolean isImage(String path) {
        return path.toLowerCase(Locale.ROOT).endsWith(".dds");
    }

    static List<String> packPaths(Iterable<String> paths) {
        List<String> out = new ArrayList<>();
        for (String p : paths) {
            String l = p.toLowerCase(Locale.ROOT);
            if (l.endsWith("/" + JSON) || l.equals(JSON)) out.add(p);
        }
        return out;
    }

    // ---- what is installed

    static final class Kept {
        String id, name, author;
        int count;
        int size;  // the largest its images were kept, 0 if an older install did not say (1024)
    }

    static List<Kept> kept() {
        List<Kept> out = new ArrayList<>();
        File[] dirs = dir().listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));
        if (dirs == null) return out;
        Arrays.sort(dirs, (a, b) -> Long.compare(new File(a, INFO).lastModified(), new File(b, INFO).lastModified()));
        for (File d : dirs) {
            try {
                List<String> l = Files.readAllLines(new File(d, INFO).toPath(), StandardCharsets.UTF_8);
                Kept k = new Kept();
                k.id = d.getName();
                k.name = l.get(0);
                k.author = l.size() > 1 ? l.get(1) : "";
                k.count = l.size() > 2 ? Integer.parseInt(l.get(2).trim()) : 0;
                k.size = l.size() > 3 ? Integer.parseInt(l.get(3).trim()) : 1024;
                out.add(k);
            } catch (IOException | RuntimeException e) {
                // half written: not a pack
            }
        }
        return out;
    }

    static void uninstall(Context context, String id) {
        ModInstaller.stopGame(context);  // it reads the images as the track streams
        deleteTree(new File(dir(), id));
    }

    // ---- installing

    // every pack in the archive, each its own folder; the names of those that went in
    static List<String> install(Context context, Map<String, File> files, Consumer<String> log) throws Exception {
        Map<String, String> lower = new HashMap<>();
        for (String k : files.keySet()) lower.put(normal(k).toLowerCase(Locale.ROOT), k);
        List<String> jsons = packPaths(files.keySet());
        List<String> done = new ArrayList<>();
        File root = dir();
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("cannot write to " + root);
        for (int n = 0; n < jsons.size(); n++) {
            String json = jsons.get(n);
            JSONObject o = new JSONObject(lenient(new String(Files.readAllBytes(files.get(json).toPath()), StandardCharsets.UTF_8)));
            JSONObject desc = o.optJSONObject("description");
            String base = normal(json);
            base = base.contains("/") ? base.substring(0, base.lastIndexOf('/')) : "";
            String folder = base.contains("/") ? base.substring(base.lastIndexOf('/') + 1) : base;
            String name = desc != null ? desc.optString("name", folder) : folder;
            if (name.isEmpty()) name = folder.isEmpty() ? "Texture pack" : folder;
            String author = desc != null ? desc.optString("author", "") : "";
            String rootDir = o.optString("rootDirectory", "");
            JSONArray maps = o.optJSONArray("textureMappings");
            if (maps == null) continue;
            log.accept("pack " + (n + 1) + "/" + jsons.size() + ": " + name);
            String id = name.replaceAll("[^A-Za-z0-9._ -]", "_").trim();
            File work = new File(root, ".new_" + id);
            deleteTree(work);
            if (!work.mkdirs()) throw new IOException("cannot write to " + work);
            java.util.Set<Integer> made = new java.util.HashSet<>();  // two mappings may name one game texture: the later's
            for (int i = 0; i < maps.length(); i++) {
                JSONObject m = maps.optJSONObject(i);
                if (m == null) continue;
                String game = m.optString("gameId", "").trim(), path = m.optString("texturePath", "");
                if (game.isEmpty() || path.isEmpty()) continue;
                String full = normal(join(base, join(rootDir, path)));
                String key = lower.get(full.toLowerCase(Locale.ROOT));
                if (key == null) continue;
                int hash;
                try {
                    hash = game.regionMatches(true, 0, "0x", 0, 2) ? (int) Long.parseLong(game.substring(2), 16) : hash(game);
                } catch (NumberFormatException e) {
                    continue;
                }
                File img = new File(work, String.format(Locale.ROOT, "%08x.dds", hash));
                Files.copy(files.get(key).toPath(), img.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                made.add(hash);
                if (i % 50 == 0) log.accept("pack " + name + ": " + i + "/" + maps.length());
            }
            int count = made.size();
            if (count == 0) {
                deleteTree(work);
                continue;
            }
            write(new File(work, INFO), (name + "\n" + author + "\n" + count + "\n" + max + "\n").getBytes(StandardCharsets.UTF_8));
            ModInstaller.stopGame(context);
            File dest = new File(root, id);
            deleteTree(dest);
            if (!work.renameTo(dest)) throw new IOException("cannot move " + work + " to " + dest);
            done.add(name);
        }
        return done;
    }

    // bStringHash, of the name as the game has it: upper case
    static int hash(String name) {
        int h = -1;
        for (byte b : name.toUpperCase(Locale.ROOT).getBytes(StandardCharsets.ISO_8859_1)) h = h * 33 + (b & 255);
        return h;
    }

    // the json as people write it: a byte order mark, commas before a closing bracket
    private static String lenient(String s) {
        if (s.startsWith("﻿")) s = s.substring(1);
        return s.replaceAll(",\\s*([\\]}])", "$1");
    }

    private static String join(String a, String b) {
        if (a == null || a.isEmpty()) return b;
        if (b == null || b.isEmpty()) return a;
        return a + "/" + b;
    }

    private static String normal(String p) {
        p = p.replace('\\', '/');
        List<String> parts = new ArrayList<>();
        for (String s : p.split("/")) {
            if (s.isEmpty() || s.equals(".")) continue;
            if (s.equals("..")) {
                if (!parts.isEmpty()) parts.remove(parts.size() - 1);
            } else {
                parts.add(s);
            }
        }
        return String.join("/", parts);
    }

    private static void deleteTree(File f) {
        File[] in = f.listFiles();
        if (in != null) for (File c : in) deleteTree(c);
        f.delete();
    }

    private static void write(File f, byte[] data) throws IOException {
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(data);
            out.getFD().sync();
        }
    }

    // ---- DDS

    private static final int DXT1 = 0x31545844, DXT3 = 0x33545844, DXT5 = 0x35545844;
    private static final int RGBA = 0;  // 32 bit, A8R8G8B8 when written

    private static int i32(byte[] b, int o) {
        return (b[o] & 255) | (b[o + 1] & 255) << 8 | (b[o + 2] & 255) << 16 | (b[o + 3] & 255) << 24;
    }

    private static void put32(byte[] b, int o, int v) {
        b[o] = (byte) v;
        b[o + 1] = (byte) (v >> 8);
        b[o + 2] = (byte) (v >> 16);
        b[o + 3] = (byte) (v >> 24);
    }

    private static int levelSize(int format, int w, int h) {
        if (format == RGBA) return w * h * 4;
        return Math.max(1, (w + 3) / 4) * Math.max(1, (h + 3) / 4) * (format == DXT1 ? 8 : 16);
    }

    // a .dds at most max across with its mips, or null for one this cannot use (cube maps, volumes, other formats)
    static byte[] shrink(byte[] d) {
        if (d.length < 128 || i32(d, 0) != 0x20534444) return null;
        int h = i32(d, 12), w = i32(d, 16), flags = i32(d, 8);
        int mips = (flags & 0x20000) != 0 ? Math.max(1, i32(d, 28)) : 1;
        int pf = i32(d, 80), fourcc = i32(d, 84), bits = i32(d, 88);
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (i32(d, 112) & 0x200) != 0 || (i32(d, 112) & 0x200000) != 0) return null;
        int format;
        if ((pf & 4) != 0) {
            if (fourcc != DXT1 && fourcc != DXT3 && fourcc != DXT5) return null;
            format = fourcc;
        } else if ((pf & 0x40) != 0 && bits == 32) {
            format = RGBA;
        } else {
            return null;
        }
        // the levels the file has
        int[] offs = new int[mips];
        int have = 0, at = 128, lw = w, lh = h;
        for (int l = 0; l < mips; l++) {
            int n = levelSize(format, lw, lh);
            if (at + n > d.length) break;
            offs[have++] = at;
            at += n;
            lw = Math.max(1, lw / 2);
            lh = Math.max(1, lh / 2);
        }
        if (have == 0) return null;
        // the first level at most max across
        int start = 0;
        lw = w;
        lh = h;
        while (start + 1 < have && Math.max(lw, lh) > max) {
            start++;
            lw = Math.max(1, lw / 2);
            lh = Math.max(1, lh / 2);
        }
        if (Math.max(lw, lh) <= max && have - start > 1) {
            // its own mips, from there to the last
            if (start == 0 && have == mips) return d;
            int end = at;
            byte[] out = new byte[128 + end - offs[start]];
            System.arraycopy(d, 0, out, 0, 128);
            System.arraycopy(d, offs[start], out, 128, end - offs[start]);
            put32(out, 8, i32(d, 8) | 0x20000);
            put32(out, 12, lh);
            put32(out, 16, lw);
            put32(out, 28, have - start);
            put32(out, 20, levelSize(format, lw, lh));
            return out;
        }
        // decoded, shrunk, mipped and encoded again, from the smallest level at least max across (or the last there is)
        int[] px = decodeShrunk(d, offs[start], format, lw, lh, (pf & 1) != 0, i32(d, 92) == 0xFF);
        int f = 1;
        while (Math.max(lw, lh) / f > max) f <<= 1;
        int ow = Math.max(1, lw / f), oh = Math.max(1, lh / f);
        // not shrunk: its first level as it was, only the mips made
        byte[] first = f == 1 ? Arrays.copyOfRange(d, offs[start], offs[start] + levelSize(format, lw, lh)) : null;
        if (first != null && format == RGBA) first = rgba(px);  // in the order written
        return encode(px, ow, oh, format, first);
    }

    // a level as ARGB, shrunk by a power of two box to at most max across, a row at a time: an 8192 texel level decoded
    // whole would be a quarter of a gigabyte
    private static int[] decodeShrunk(byte[] d, int at, int format, int w, int h, boolean alpha, boolean rgbaOrder) {
        int f = 1;
        while (Math.max(w, h) / f > max) f <<= 1;
        int ow = Math.max(1, w / f), oh = Math.max(1, h / f);
        int[] out = new int[ow * oh];
        long[] acc = new long[ow * 4];
        int[] rows = new int[w * 4];  // a block row's four rows, ARGB
        int[] blk = new int[16];
        int bw = Math.max(1, (w + 3) / 4), bh = Math.max(1, (h + 3) / 4), bytes = format == DXT1 ? 8 : 16;
        int counted = 0;
        for (int by = 0; by < (format == RGBA ? (h + 3) / 4 : bh); by++) {
            if (format == RGBA) {
                for (int y = 0; y < 4 && by * 4 + y < h; y++)
                    for (int x = 0; x < w; x++) {
                        int o = at + ((by * 4 + y) * w + x) * 4;
                        int b0 = d[o] & 255, b1 = d[o + 1] & 255, b2 = d[o + 2] & 255, b3 = alpha ? d[o + 3] & 255 : 255;
                        rows[y * w + x] = rgbaOrder ? b3 << 24 | b0 << 16 | b1 << 8 | b2 : b3 << 24 | b2 << 16 | b1 << 8 | b0;
                    }
            } else {
                for (int bx = 0; bx < bw; bx++) {
                    decodeBlock(d, at + (by * bw + bx) * bytes, format, blk);
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4 && bx * 4 + x < w; x++) rows[y * w + bx * 4 + x] = blk[y * 4 + x];
                }
            }
            for (int y = 0; y < 4; y++) {
                int sy = by * 4 + y;
                if (sy >= h) break;
                int oy = sy / f;
                if (oy >= oh) break;
                for (int x = 0; x < w; x++) {
                    int ox = x / f;
                    if (ox >= ow) break;
                    int c = rows[y * w + x];
                    acc[ox * 4] += c >>> 24;
                    acc[ox * 4 + 1] += c >> 16 & 255;
                    acc[ox * 4 + 2] += c >> 8 & 255;
                    acc[ox * 4 + 3] += c & 255;
                }
                if (++counted == f || sy == h - 1) {
                    long n = (long) counted * f;
                    for (int ox = 0; ox < ow; ox++) {
                        int a = (int) (acc[ox * 4] / n), r = (int) (acc[ox * 4 + 1] / n), g = (int) (acc[ox * 4 + 2] / n),
                                b = (int) (acc[ox * 4 + 3] / n);
                        out[oy * ow + ox] = a << 24 | r << 16 | g << 8 | b;
                    }
                    Arrays.fill(acc, 0);
                    counted = 0;
                }
            }
        }
        return out;
    }

    private static int expand565(int c) {
        int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
        return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }

    private static void decodeBlock(byte[] d, int o, int format, int[] out) {
        int co = format == DXT1 ? o : o + 8;
        int c0 = (d[co] & 255) | (d[co + 1] & 255) << 8, c1 = (d[co + 2] & 255) | (d[co + 3] & 255) << 8;
        int[] pal = new int[4];
        int e0 = expand565(c0), e1 = expand565(c1);
        pal[0] = 0xFF000000 | e0;
        pal[1] = 0xFF000000 | e1;
        if (c0 > c1 || format != DXT1) {
            pal[2] = 0xFF000000 | mix(e0, e1, 2, 1, 3);
            pal[3] = 0xFF000000 | mix(e0, e1, 1, 2, 3);
        } else {
            pal[2] = 0xFF000000 | mix(e0, e1, 1, 1, 2);
            pal[3] = 0;
        }
        int bits = i32(d, co + 4);
        for (int i = 0; i < 16; i++) out[i] = pal[bits >>> (i * 2) & 3];
        if (format == DXT3) {
            for (int i = 0; i < 16; i++) {
                int a = (d[o + i / 2] & 255) >> ((i & 1) * 4) & 15;
                out[i] = (out[i] & 0xFFFFFF) | (a * 17) << 24;
            }
        } else if (format == DXT5) {
            int a0 = d[o] & 255, a1 = d[o + 1] & 255;
            int[] a = alphaPalette(a0, a1);
            long abits = 0;
            for (int i = 0; i < 6; i++) abits |= (long) (d[o + 2 + i] & 255) << (8 * i);
            for (int i = 0; i < 16; i++) out[i] = (out[i] & 0xFFFFFF) | a[(int) (abits >>> (3 * i) & 7)] << 24;
        }
    }

    private static int[] alphaPalette(int a0, int a1) {
        int[] a = new int[8];
        a[0] = a0;
        a[1] = a1;
        if (a0 > a1) {
            for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        } else {
            for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a0 + i * a1) / 5;
            a[6] = 0;
            a[7] = 255;
        }
        return a;
    }

    private static int mix(int x, int y, int wx, int wy, int div) {
        int r = ((x >> 16 & 255) * wx + (y >> 16 & 255) * wy) / div, g = ((x >> 8 & 255) * wx + (y >> 8 & 255) * wy) / div,
                b = ((x & 255) * wx + (y & 255) * wy) / div;
        return r << 16 | g << 8 | b;
    }

    // the image and its mips down to 1x1, in format, as a .dds
    private static byte[] encode(int[] px, int w, int h, int format, byte[] first) {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        int levels = 0, w0 = w, h0 = h;
        for (;;) {
            byte[] l = levels == 0 && first != null ? first : format == RGBA ? rgba(px) : dxt(px, w, h, format);
            body.write(l, 0, l.length);
            levels++;
            if (w == 1 && h == 1) break;
            int nw = Math.max(1, w / 2), nh = Math.max(1, h / 2);
            int[] next = new int[nw * nh];
            for (int y = 0; y < nh; y++)
                for (int x = 0; x < nw; x++) {
                    int a = 0, r = 0, g = 0, b = 0, n = 0;
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++) {
                            int sx = Math.min(w - 1, x * 2 + dx), sy = Math.min(h - 1, y * 2 + dy);
                            int c = px[sy * w + sx];
                            a += c >>> 24;
                            r += c >> 16 & 255;
                            g += c >> 8 & 255;
                            b += c & 255;
                            n++;
                        }
                    next[y * nw + x] = (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
                }
            px = next;
            w = nw;
            h = nh;
        }
        byte[] head = new byte[128];
        put32(head, 0, 0x20534444);
        put32(head, 4, 124);
        put32(head, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);
        put32(head, 12, h0);
        put32(head, 16, w0);
        put32(head, 20, levelSize(format, w0, h0));
        put32(head, 28, levels);
        put32(head, 76, 32);
        if (format == RGBA) {
            put32(head, 80, 0x41);
            put32(head, 88, 32);
            put32(head, 92, 0xFF0000);
            put32(head, 96, 0xFF00);
            put32(head, 100, 0xFF);
            put32(head, 104, 0xFF000000);
        } else {
            put32(head, 80, 4);
            put32(head, 84, format);
        }
        put32(head, 108, 0x1000 | 0x400000 | 0x8);
        byte[] b = body.toByteArray(), out = new byte[128 + b.length];
        System.arraycopy(head, 0, out, 0, 128);
        System.arraycopy(b, 0, out, 128, b.length);
        return out;
    }

    private static byte[] rgba(int[] px) {
        byte[] out = new byte[px.length * 4];
        for (int i = 0; i < px.length; i++) put32(out, i * 4, px[i]);  // B, G, R, A in memory: A8R8G8B8
        return out;
    }

    private static int to565(int r, int g, int b) {
        return (r * 31 + 127) / 255 << 11 | (g * 63 + 127) / 255 << 5 | (b * 31 + 127) / 255;
    }

    private static int dist(int x, int y) {
        int r = (x >> 16 & 255) - (y >> 16 & 255), g = (x >> 8 & 255) - (y >> 8 & 255), b = (x & 255) - (y & 255);
        return r * r + g * g + b * b;
    }

    // a bounding box encoder, each block's endpoints its colours' corners pulled in a sixteenth
    private static byte[] dxt(int[] px, int w, int h, int format) {
        int bw = Math.max(1, (w + 3) / 4), bh = Math.max(1, (h + 3) / 4), bytes = format == DXT1 ? 8 : 16;
        byte[] out = new byte[bw * bh * bytes];
        int[] blk = new int[16];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        blk[y * 4 + x] = px[Math.min(h - 1, by * 4 + y) * w + Math.min(w - 1, bx * 4 + x)];
                int o = (by * bw + bx) * bytes;
                if (format == DXT3) {
                    for (int i = 0; i < 16; i++) {
                        int a = ((blk[i] >>> 24) * 15 + 127) / 255;
                        out[o + i / 2] |= (byte) (a << ((i & 1) * 4));
                    }
                } else if (format == DXT5) {
                    alphaBlock(blk, out, o);
                }
                colourBlock(blk, out, format == DXT1 ? o : o + 8, format == DXT1);
            }
        return out;
    }

    private static void alphaBlock(int[] blk, byte[] out, int o) {
        int lo = 255, hi = 0;
        for (int c : blk) {
            lo = Math.min(lo, c >>> 24);
            hi = Math.max(hi, c >>> 24);
        }
        out[o] = (byte) hi;
        out[o + 1] = (byte) lo;
        if (hi == lo) return;  // every index 0
        int[] a = alphaPalette(hi, lo);
        long bits = 0;
        for (int i = 0; i < 16; i++) {
            int v = blk[i] >>> 24, best = 0, bd = 1 << 30;
            for (int k = 0; k < 8; k++) {
                int dd = Math.abs(a[k] - v);
                if (dd < bd) {
                    bd = dd;
                    best = k;
                }
            }
            bits |= (long) best << (3 * i);
        }
        for (int i = 0; i < 6; i++) out[o + 2 + i] = (byte) (bits >>> (8 * i));
    }

    private static void colourBlock(int[] blk, byte[] out, int o, boolean dxt1) {
        boolean holes = false;
        int[] min = {255, 255, 255}, max = {0, 0, 0};
        for (int c : blk) {
            if (dxt1 && (c >>> 24) < 128) {
                holes = true;
                continue;
            }
            int[] ch = {c >> 16 & 255, c >> 8 & 255, c & 255};
            for (int k = 0; k < 3; k++) {
                min[k] = Math.min(min[k], ch[k]);
                max[k] = Math.max(max[k], ch[k]);
            }
        }
        if (min[0] > max[0]) {  // all holes
            min = new int[]{0, 0, 0};
            max = new int[]{0, 0, 0};
        }
        for (int k = 0; k < 3; k++) {
            int inset = (max[k] - min[k]) / 16;
            min[k] += inset;
            max[k] -= inset;
        }
        int c0 = to565(max[0], max[1], max[2]), c1 = to565(min[0], min[1], min[2]);
        int[] pal = new int[4];
        if (holes) {
            // three colours and a hole: c0 <= c1
            if (c0 > c1) {
                int t = c0;
                c0 = c1;
                c1 = t;
            }
            int e0 = expand565(c0), e1 = expand565(c1);
            pal[0] = e0;
            pal[1] = e1;
            pal[2] = mix(e0, e1, 1, 1, 2);
        } else {
            if (c0 < c1) {
                int t = c0;
                c0 = c1;
                c1 = t;
            }
            int e0 = expand565(c0), e1 = expand565(c1);
            pal[0] = e0;
            pal[1] = e1;
            pal[2] = mix(e0, e1, 2, 1, 3);
            pal[3] = mix(e0, e1, 1, 2, 3);
        }
        int bits = 0;
        if (c0 != c1 || holes)
            for (int i = 0; i < 16; i++) {
                int c = blk[i] & 0xFFFFFF, best = 0;
                if (holes && (blk[i] >>> 24) < 128) {
                    best = 3;
                } else {
                    int bd = Integer.MAX_VALUE;
                    for (int k = 0; k < (holes ? 3 : 4); k++) {
                        int dd = dist(c, pal[k]);
                        if (dd < bd) {
                            bd = dd;
                            best = k;
                        }
                    }
                }
                bits |= best << (2 * i);
            }
        out[o] = (byte) c0;
        out[o + 1] = (byte) (c0 >> 8);
        out[o + 2] = (byte) c1;
        out[o + 3] = (byte) (c1 >> 8);
        put32(out, o + 4, bits);
    }
}
