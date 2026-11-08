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

    private static final int MAX_FILE = 256 << 20, MAX_ALL = 1 << 30;
    private static final int SEVEN_Z_MEMORY_KB = 256 << 10;

    private ModArchive() {}

    // the wanted files by their paths in the archive, with / in them. spool is emptied first
    static Map<String, File> read(File file, File spool) throws IOException {
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

    static void clear(File spool) {
        File[] old = spool.listFiles();
        if (old != null) for (File f : old) f.delete();
        spool.delete();
    }

    private static void save(Map<String, File> out, File spool, String name, byte[] data) throws IOException {
        File f = new File(spool, String.valueOf(out.size()));
        try (FileOutputStream o = new FileOutputStream(f)) {
            o.write(data);
        }
        out.put(clean(name), f);
    }

    private interface Chunk {
        int read(byte[] b, int off, int len) throws IOException;
    }

    private static byte[] all(Chunk in, long size, int[] total) throws IOException {
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
            if (total[0] + n > MAX_ALL) throw new IOException("the archive's files are too large");
        }
        total[0] += n;
        return n == b.length ? b : java.util.Arrays.copyOf(b, n);
    }

    private static String clean(String name) {
        return name.replace('\\', '/');
    }

    private static Map<String, File> zip(File file, File spool) throws IOException {
        Map<String, File> out = new LinkedHashMap<>();
        int[] total = {0};
        try (ZipFile z = new ZipFile(file)) {
            Enumeration<? extends ZipEntry> en = z.entries();
            while (en.hasMoreElements()) {
                ZipEntry e = en.nextElement();
                if (e.isDirectory() || !CarConverter.wanted(e.getName())) continue;
                try (InputStream in = z.getInputStream(e)) {
                    save(out, spool, e.getName(), all(in::read, e.getSize(), total));
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
        int[] total = {0};
        SevenZFileOptions options = SevenZFileOptions.builder().withMaxMemoryLimitInKb(SEVEN_Z_MEMORY_KB).build();
        try (SevenZFile z = new SevenZFile(file, options)) {
            SevenZArchiveEntry e;
            while ((e = z.getNextEntry()) != null) {
                if (e.isDirectory() || !CarConverter.wanted(e.getName())) continue;
                save(out, spool, e.getName(), all(z::read, e.getSize(), total));
            }
        }
        return out;
    }

    private static Map<String, File> libarchive(File file, File spool) throws IOException {
        Map<String, File> out = new LinkedHashMap<>();
        int[] total = {0};
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
                        || !CarConverter.wanted(name))
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
                }, size, total));
            }
        } finally {
            Archive.readFree(a);
        }
        return out;
    }
}
