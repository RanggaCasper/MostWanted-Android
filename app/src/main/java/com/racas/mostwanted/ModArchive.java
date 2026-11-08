package com.racas.mostwanted;

import me.zhanghai.android.libarchive.Archive;
import me.zhanghai.android.libarchive.ArchiveEntry;

import org.apache.commons.compress.archivers.sevenz.SevenZArchiveEntry;
import org.apache.commons.compress.archivers.sevenz.SevenZFile;
import org.apache.commons.compress.archivers.sevenz.SevenZFileOptions;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

// The cars' files out of a downloaded archive, each to a numbered file in the app's cache (a pack does not fit memory):
// nothing a stranger packed gets a path on the storage, and
// nothing in it is run. .zip and .7z are read here in Java; RAR of any version, which has no decoder in Java, and a .7z the
// Java reader refuses, by libarchive.
final class ModArchive {
    static final class Unsupported extends IOException {
        Unsupported(String format) {
            super(format);
        }
    }

    private static final int MAX_FILE = 256 << 20;
    // what is kept of an archive, spooled to the cache: a texture pack kept at 4096 is over a gigabyte
    private static final long MAX_ALL = 8L << 30, ROOM = 256L << 20;
    private static final int SEVEN_Z_MEMORY_KB = 256 << 10;
    private static java.util.function.Consumer<String> progress = line -> {};

    private ModArchive() {}

    // the wanted files by their paths in the archive, with / in them. spool is emptied first
    static Map<String, File> read(File file, File spool) throws IOException {
        return read(file, spool, line -> {});
    }

    // log: "read <n>" now and then, the files kept so far
    static Map<String, File> read(File file, File spool, java.util.function.Consumer<String> log) throws IOException {
        progress = log;
        clear(spool);
        if (!spool.isDirectory() && !spool.mkdirs()) throw new IOException("cannot write to " + spool);
        byte[] m = new byte[8];
        int got;
        try (FileInputStream in = new FileInputStream(file)) {
            got = in.read(m);
        }
        if (got >= 4 && m[0] == 'P' && m[1] == 'K') return zip(file, spool);
        if (got >= 6 && m[0] == '7' && m[1] == 'z' && (m[2] & 255) == 0xBC && (m[3] & 255) == 0xAF) return sevenZ(file, spool);
        if (got >= 7 && m[0] == 'R' && m[1] == 'a' && m[2] == 'r' && m[3] == '!') return libarchive(file, spool);
        throw new Unsupported(got > 0 && m[0] == '<' ? "HTML" : "?");
    }

    // whether the archive is a texture pack, by its entries' names alone (no data read): the Mods Loader asks its size
    // before it goes in. False for anything that will not list
    static boolean isTexturePack(File file) {
        try {
            for (String n : names(file))
                if (!TexturePacks.packPaths(java.util.Collections.singletonList(n.replace('\\', '/'))).isEmpty()) return true;
        } catch (IOException | RuntimeException | LinkageError e) {
            // read as it goes in
        }
        return false;
    }

    private static java.util.List<String> names(File file) throws IOException {
        java.util.List<String> out = new java.util.ArrayList<>();
        byte[] m = new byte[8];
        int got;
        try (FileInputStream in = new FileInputStream(file)) {
            got = in.read(m);
        }
        if (got >= 4 && m[0] == 'P' && m[1] == 'K') {
            try (ZipFile z = new ZipFile(file)) {
                for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) out.add(en.nextElement().getName());
            }
            return out;
        }
        if (got >= 6 && m[0] == '7' && m[1] == 'z') {
            try (SevenZFile z = new SevenZFile(file, SevenZFileOptions.builder().withMaxMemoryLimitInKb(SEVEN_Z_MEMORY_KB).build())) {
                for (SevenZArchiveEntry e : z.getEntries()) out.add(e.getName());
                return out;
            } catch (IOException e) {
                // libarchive below
            }
        }
        if (got < 4) return out;
        long a = Archive.readNew();
        try {
            Archive.readSupportFilterAll(a);
            Archive.readSupportFormatAll(a);
            Archive.readOpenFileName(a, file.getPath().getBytes(StandardCharsets.UTF_8), 1 << 16);
            for (long e; (e = Archive.readNextHeader(a)) != 0; ) {
                byte[] raw = ArchiveEntry.pathname(e);
                String name = raw == null ? ArchiveEntry.pathnameUtf8(e) : new String(raw, StandardCharsets.UTF_8);
                if (name != null) out.add(name);
                Archive.readDataSkip(a);
            }
        } finally {
            Archive.readFree(a);
        }
        return out;
    }

    static void clear(File spool) {
        File[] old = spool.listFiles();
        if (old != null) for (File f : old) f.delete();
        spool.delete();
    }

    // a car's files, and a texture pack's (TexturePacks): its json, and its images shrunk as they come, an archive of
    // them often more than the cache would hold as they are
    private static boolean wanted(String name) {
        return CarConverter.wanted(name) || TexturePacks.wanted(name);
    }

    private static void save(Map<String, File> out, File spool, String name, byte[] data, long[] total) throws IOException {
        if (TexturePacks.isImage(name) && !CarConverter.wanted(name)) {
            data = TexturePacks.shrink(data);
            if (data == null) return;  // a format the game's draws cannot take
        }
        total[0] += data.length;
        if (total[0] > MAX_ALL) throw new IOException("the archive's files are too large");
        if (spool.getUsableSpace() < data.length + ROOM) throw new IOException("not enough storage left on the phone");
        if (out.size() % 20 == 19) progress.accept("read " + (out.size() + 1));
        File f = new File(spool, String.valueOf(out.size()));
        try (FileOutputStream o = new FileOutputStream(f)) {
            o.write(data);
        }
        out.put(clean(name), f);
    }

    private interface Chunk {
        int read(byte[] b, int off, int len) throws IOException;
    }

    private static byte[] all(Chunk in, long size, long[] total) throws IOException {
        if (size > MAX_FILE) throw new IOException("a file in the archive is too large");
        byte[] b = new byte[size > 0 ? (int) size : 1 << 16];
        int n = 0;
        for (;;) {
            if (n == b.length) {
                if (n >= MAX_FILE) throw new IOException("a file in the archive is too large");
                b = java.util.Arrays.copyOf(b, Math.min(MAX_FILE, n * 2));
            }
            int r = in.read(b, n, b.length - n);
            if (r < 0) break;
            n += r;
        }
        return n == b.length ? b : java.util.Arrays.copyOf(b, n);
    }

    private static String clean(String name) {
        return name.replace('\\', '/');
    }

    private static Map<String, File> zip(File file, File spool) throws IOException {
        Map<String, File> out = new LinkedHashMap<>();
        long[] total = {0};
        try (ZipFile z = new ZipFile(file)) {
            Enumeration<? extends ZipEntry> en = z.entries();
            while (en.hasMoreElements()) {
                ZipEntry e = en.nextElement();
                if (e.isDirectory() || !wanted(e.getName())) continue;
                try (InputStream in = z.getInputStream(e)) {
                    save(out, spool, e.getName(), all(in::read, e.getSize(), total), total);
                }
            }
        }
        return out;
    }

    private static Map<String, File> sevenZ(File file, File spool) throws IOException {
        try {
            return sevenZJava(file, spool);
        } catch (IOException e) {
            return libarchive(file, spool);
        }
    }

    private static Map<String, File> sevenZJava(File file, File spool) throws IOException {
        Map<String, File> out = new LinkedHashMap<>();
        long[] total = {0};
        SevenZFileOptions options = SevenZFileOptions.builder().withMaxMemoryLimitInKb(SEVEN_Z_MEMORY_KB).build();
        try (SevenZFile z = new SevenZFile(file, options)) {
            SevenZArchiveEntry e;
            while ((e = z.getNextEntry()) != null) {
                if (e.isDirectory() || !wanted(e.getName())) continue;
                save(out, spool, e.getName(), all(z::read, e.getSize(), total), total);
            }
        }
        return out;
    }

    private static Map<String, File> libarchive(File file, File spool) throws IOException {
        Map<String, File> out = new LinkedHashMap<>();
        long[] total = {0};
        long a;
        try {
            a = Archive.readNew();
        } catch (LinkageError e) {
            throw new Unsupported("libarchive");  // its library would not load on this phone
        }
        try {
            Archive.readSupportFilterAll(a);
            Archive.readSupportFormatAll(a);
            Archive.readOpenFileName(a, file.getPath().getBytes(StandardCharsets.UTF_8), 1 << 16);
            ByteBuffer buf = ByteBuffer.allocateDirect(1 << 16);
            byte[] copy = new byte[1 << 16];
            for (long e; (e = Archive.readNextHeader(a)) != 0; ) {
                byte[] raw = ArchiveEntry.pathname(e);
                String name = raw == null ? ArchiveEntry.pathnameUtf8(e) : new String(raw, StandardCharsets.UTF_8);
                if (name == null || (ArchiveEntry.filetype(e) & ArchiveEntry.AE_IFMT) != ArchiveEntry.AE_IFREG
                        || !wanted(name))
                    continue;
                long size = ArchiveEntry.sizeIsSet(e) ? ArchiveEntry.size(e) : 0;
                save(out, spool, name, all((b, off, len) -> {
                    buf.clear();
                    buf.limit(Math.min(len, buf.capacity()));
                    Archive.readData(a, buf);
                    buf.flip();
                    int n = buf.remaining();
                    if (n == 0) return -1;
                    buf.get(b, off, n);
                    return n;
                }, size, total), total);
            }
        } finally {
            Archive.readFree(a);
        }
        return out;
    }
}
