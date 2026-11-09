package com.racas.mostwanted;

import android.app.ActivityManager;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Environment;
import android.os.Process;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;

// The cars in the mods folder (MostWanted/mods). Each is kept converted in garage/<car>; the files the kernel reads over
// the disc (sdk/runtime/kernel/files.cpp) are made again from all of them whenever one comes or goes. ZDIR.BIN is what
// makes the rest count: it goes first and comes back last, so a stop halfway leaves the plain disc. A car once installed
// stays in the profiles that saved it: cars_removed.txt tells src/game/car_mods.cpp to take it out of them.
final class ModInstaller {
    private static final String ZDIR = "NFS/ZDIR.BIN", LIST = "cars.txt", TYPES = "car_types.txt", REMOVED = "cars_removed.txt";
    private static final String GARAGE = "garage", INFO = "info.txt", GEOMETRY = "geometry.bin", TEXTURES = "textures.bin";
    private static final String SCRIPT = "script.nfsms", LOGO = "logo.dxt";

    private ModInstaller() {}

    static File modsDir() {
        return new File(new File(Environment.getExternalStorageDirectory(), MostWantedActivity.SHARED), "mods");
    }

    // where the game is, in the order native/game_files.cpp looks: the extracted files before an image in each folder
    static File findGame(Context context) {
        List<File> folders = new ArrayList<>();
        File shared = new File(Environment.getExternalStorageDirectory(), MostWantedActivity.SHARED);
        folders.add(shared);
        folders.add(new File(shared, "assets"));
        for (File card : MostWantedActivity.cardFolders(context)) {
            folders.add(card);
            folders.add(new File(card, "assets"));
        }
        File own = context.getExternalFilesDir(null);
        if (own != null) folders.add(new File(own, "assets"));
        for (File dir : folders) {
            if (new File(dir, "default.xbe").exists()) return dir;
            File[] files = dir.listFiles();
            if (files == null) continue;
            Arrays.sort(files);
            for (File f : files)
                if (f.isFile() && f.getName().toLowerCase(Locale.ROOT).endsWith(".iso")) return f;
        }
        return null;
    }

    // the kept cars, in the order they came: their car types go by it
    static List<CarConverter.Kept> kept() {
        List<CarConverter.Kept> out = new ArrayList<>();
        File[] dirs = new File(modsDir(), GARAGE).listFiles(File::isDirectory);
        if (dirs == null) return out;
        Arrays.sort(dirs, (a, b) -> {
            int c = Long.compare(new File(a, INFO).lastModified(), new File(b, INFO).lastModified());
            return c != 0 ? c : a.getName().compareTo(b.getName());
        });
        for (File d : dirs) {
            try {
                List<String> l = Files.readAllLines(new File(d, INFO).toPath(), StandardCharsets.UTF_8);
                CarConverter.Kept k = new CarConverter.Kept();
                k.car = l.get(0);
                k.title = l.get(1);
                k.manufacturer = l.get(2);
                k.logoWidth = Integer.parseInt(l.get(3));
                k.logoHeight = Integer.parseInt(l.get(4));
                k.logoFormat = Integer.parseInt(l.get(5));
                k.geometry = new File(d, GEOMETRY);
                k.textures = new File(d, TEXTURES);
                File script = new File(d, SCRIPT), logo = new File(d, LOGO);
                if (script.exists()) k.script = Files.readAllBytes(script.toPath());
                if (logo.exists()) k.logo = Files.readAllBytes(logo.toPath());
                if (k.geometry.exists() && k.textures.exists()) out.add(k);
            } catch (IOException | RuntimeException e) {
                // half written: it is not a car, and the next install of it writes it again
            }
        }
        return out;
    }

    // what an archive came to: the cars that went in, and the ones that would not, each with why
    static final class Installed {
        final List<String> done = new ArrayList<>();
        final List<String> failed = new ArrayList<>();
    }

    // from the archive to the garage, every car in it, then the game's files again, once. A car that will not go in leaves the
    // others in; the first's failure is thrown only if none went. The same car a second time takes its old place's files
    static Installed install(Context context, File archive, String pageTitle, Consumer<String> log) throws Exception {
        File game = findGame(context);
        if (game == null) throw new IOException("no game");
        File spool = new File(new File(context.getCacheDir(), "mods"), "spool");
        try {
            return install(context, game, archive, spool, pageTitle, log);
        } finally {
            ModArchive.clear(spool);
        }
    }

    private static Installed install(Context context, File game, File archive, File spool, String pageTitle, Consumer<String> log) throws Exception {
        Map<String, File> files = ModArchive.read(archive, spool, log);
        List<String> paths = CarConverter.carPaths(files.keySet());
        Installed result = new Installed();
        // texture packs (TexturePacks), each a folder of its own; the cars' files are not touched by them
        if (!TexturePacks.packPaths(files.keySet()).isEmpty()) {
            result.done.addAll(TexturePacks.install(context, files, log));
            if (paths.isEmpty()) {
                if (result.done.isEmpty()) throw new CarConverter.Failure(CarConverter.Failure.Reason.NO_CAR, "a texture pack whose images are not in it");
                return result;
            }
        }
        if (paths.isEmpty()) throw new CarConverter.Failure(CarConverter.Failure.Reason.NO_CAR, "no <name>/GEOMETRY.BIN with a TEXTURES.BIN beside it");
        int packs = result.done.size();
        CarConverter.Failure first = null;
        try (CarConverter.Disc disc = new CarConverter.Disc(game)) {
            for (String path : paths) {
                String car = CarConverter.carName(path);
                log.accept("car " + (paths.indexOf(path) + 1) + "/" + paths.size() + ": " + car);
                try {
                    CarConverter.Package pack = CarConverter.convert(disc, CarConverter.carFiles(files, path), path, CarConverter.MAX_TEXTURES_KB, log);
                    // one car with no strings of its own: the site's name for it (a pack's page names the pack)
                    if (paths.size() == 1 && pack.title.equals(CarConverter.titleCase(pack.car)) && pageTitle != null && !pageTitle.isEmpty())
                        pack.title = pageTitle;
                    keep(pack);
                    result.done.add(pack.title);
                } catch (CarConverter.Failure e) {
                    log.accept(car + ": " + e.getMessage());
                    if (first == null) first = e;
                    result.failed.add(car);
                } catch (OutOfMemoryError e) {
                    log.accept(car + ": out of memory");
                    result.failed.add(car);
                    if (first == null) first = new CarConverter.Failure(CarConverter.Failure.Reason.UNSUPPORTED, car + ": out of memory");
                }
            }
        }
        if (result.done.size() == packs) {  // no car went in
            if (packs == 0) throw first;
            return result;
        }
        rebuild(context, game, log);
        return result;
    }

    private static void keep(CarConverter.Package pack) throws Exception {
        if (pack.logo == null) drawLogo(pack);
        File dir = new File(new File(modsDir(), GARAGE), pack.car);
        if (!dir.exists() && kept().size() >= CarConverter.MAX_ADDONS)
            throw new CarConverter.Failure(CarConverter.Failure.Reason.UNSUPPORTED, "more than " + CarConverter.MAX_ADDONS + " cars");
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot write to " + dir);
        new File(dir, INFO).delete();  // not a car until it is whole
        new File(dir, SCRIPT).delete();
        new File(dir, LOGO).delete();
        write(new File(dir, GEOMETRY), pack.geometry);
        write(new File(dir, TEXTURES), pack.textures);
        if (pack.script != null) write(new File(dir, SCRIPT), pack.script);
        if (pack.logo != null) write(new File(dir, LOGO), pack.logo);
        write(new File(dir, INFO), (pack.car + "\n" + pack.title + "\n" + pack.manufacturer + "\n" + pack.logoWidth + "\n"
                + pack.logoHeight + "\n" + pack.logoFormat + "\n").getBytes(StandardCharsets.UTF_8));
    }

    // the car select's name for a car with no SECONDARY_LOGO: a texture no pack has draws as "temp350"
    private static void drawLogo(CarConverter.Package pack) {
        int width = 512, height = 128;
        Bitmap b = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(Color.WHITE);
        p.setTypeface(Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD_ITALIC));
        p.setTextAlign(Paint.Align.CENTER);
        p.setTextSize(64);
        float room = width - 16, wide = p.measureText(pack.title);
        if (wide > room) p.setTextSize(64 * room / wide);
        new Canvas(b).drawText(pack.title, width / 2f, height / 2f - (p.ascent() + p.descent()) / 2, p);
        int[] pixels = new int[width * height];
        b.getPixels(pixels, 0, width, 0, 0, width, height);
        b.recycle();
        CarConverter.setLogo(pack, pixels, width, height);
    }

    // what the garage's cars were last built with. A converter fix that kept cars need too bumps it (2: reflection flags by
    // shader, fitGeometry), and the Mods Loader offers to build them again once
    static final int BUILD = 2;
    private static final String BUILT = ".built";

    static boolean outdated() {
        File garage = new File(modsDir(), GARAGE);
        if (kept().isEmpty()) return false;
        try {
            return Integer.parseInt(new String(Files.readAllBytes(new File(garage, BUILT).toPath()), StandardCharsets.US_ASCII).trim()) < BUILD;
        } catch (IOException | NumberFormatException e) {
            return true;
        }
    }

    static void refresh(Context context, Consumer<String> log) throws Exception {
        File game = findGame(context);
        if (game == null) throw new IOException("no game");
        rebuild(context, game, log);
    }

    static void uninstall(Context context, String car, Consumer<String> log) throws Exception {
        uninstall(context, java.util.Collections.singletonList(car), log);
    }

    // several at once: the game's files are made again once, from what is left
    static void uninstall(Context context, List<String> cars, Consumer<String> log) throws Exception {
        for (String car : cars) {
            File dir = new File(new File(modsDir(), GARAGE), car);
            File[] files = dir.listFiles();
            if (files != null) for (File f : files) f.delete();
            dir.delete();
        }
        File game = findGame(context);
        if (game == null) throw new IOException("no game");
        rebuild(context, game, log);
    }

    // the game's files for what is in the garage; the game stopped before any of them changes
    private static void rebuild(Context context, File game, Consumer<String> log) throws Exception {
        File dir = modsDir();
        File work = new File(dir, ".new");
        if (!work.isDirectory() && !work.mkdirs()) throw new IOException("cannot write to " + dir);
        try {
            List<CarConverter.Kept> kept = kept();
            CarConverter.Built built = null;
            if (!kept.isEmpty()) {
                try (CarConverter.Disc disc = new CarConverter.Disc(game)) {
                    for (CarConverter.Kept k : kept) CarConverter.fitGeometry(disc, k.geometry, log);
                    built = CarConverter.build(disc, kept, work, log);
                }
                for (Map.Entry<String, byte[]> e : built.files.entrySet())
                    if (e.getValue() != null) write(new File(work, e.getKey().replace('/', '_')), e.getValue());
            }
            stopGame(context);
            Set<String> before = listedKeys();
            removeFiles(dir);
            if (built != null) {
                File nfs = new File(dir, "NFS");
                if (!nfs.isDirectory() && !nfs.mkdirs()) throw new IOException("cannot write to " + nfs);
                for (String name : built.files.keySet()) {
                    if (!name.equals(ZDIR)) move(new File(work, name.replace('/', '_')), new File(dir, name));
                }
            }
            noteRemoved(before, listedKeys());
            if (built != null) move(new File(work, ZDIR.replace('/', '_')), new File(dir, ZDIR));
            write(new File(new File(dir, GARAGE), BUILT), (BUILD + "\n").getBytes(StandardCharsets.US_ASCII));
        } finally {
            File[] left = work.listFiles();
            if (left != null) for (File f : left) f.delete();
            work.delete();
        }
    }

    // only the names this writes: the folder may hold what the player put there
    private static void removeFiles(File dir) {
        new File(dir, ZDIR).delete();
        new File(dir, LIST).delete();
        new File(dir, TYPES).delete();
        new File(dir, "car_name.dxt").delete();  // an older install's
        File[] names = dir.listFiles((d, n) -> n.matches("name_[0-9a-f]{8}\\.dxt"));
        if (names != null) for (File f : names) f.delete();
        File[] archives = new File(dir, "NFS").listFiles((d, n) -> n.toUpperCase(Locale.ROOT).matches("ZZDATA\\d+\\.BIN"));
        if (archives != null) for (File f : archives) f.delete();
    }

    private static Set<String> listedKeys() throws IOException {
        Set<String> keys = new LinkedHashSet<>();
        File list = new File(modsDir(), LIST);
        if (list.exists()) {
            for (String l : Files.readAllLines(list.toPath(), StandardCharsets.ISO_8859_1)) {
                String[] w = l.trim().split("\\s+", 2);
                if (!w[0].isEmpty()) keys.add(w[0]);
            }
        }
        return keys;
    }

    // the add-ons going out on the list, those now in off it
    private static void noteRemoved(Set<String> before, Set<String> now) throws IOException {
        File list = new File(modsDir(), REMOVED);
        Set<String> keys = new LinkedHashSet<>();
        if (list.exists()) {
            for (String l : Files.readAllLines(list.toPath(), StandardCharsets.UTF_8))
                if (!l.trim().isEmpty()) keys.add(l.trim());
        }
        keys.addAll(before);
        keys.removeAll(now);
        if (keys.isEmpty()) {
            list.delete();
            return;
        }
        write(list, (String.join("\n", keys) + "\n").getBytes(StandardCharsets.UTF_8));
    }

    private static void write(File f, byte[] data) throws IOException {
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(data);
            out.getFD().sync();
        }
    }

    private static void move(File from, File to) throws IOException {
        if (!from.renameTo(to)) throw new IOException("cannot move " + from.getName() + " to " + to);
    }

    static boolean gameRunning(Context context) {
        return gamePid(context) != 0;
    }

    private static int gamePid(Context context) {
        ActivityManager am = context.getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> all = am == null ? null : am.getRunningAppProcesses();
        if (all != null)
            for (ActivityManager.RunningAppProcessInfo p : all)
                if (p.processName.equals(context.getPackageName())) return p.pid;
        return 0;
    }

    // it has the disc's list in memory and would read the new archive by it
    static void stopGame(Context context) {
        int pid = gamePid(context);
        if (pid != 0) Process.killProcess(pid);
    }
}
