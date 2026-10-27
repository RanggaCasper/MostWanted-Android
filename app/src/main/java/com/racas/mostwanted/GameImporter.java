package com.racas.mostwanted;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.util.Arrays;
import java.util.function.Consumer;

// Copies a disc image (.iso) the player picks with Android's file picker (Downloads, a cloud drive, a USB stick)
// into the folder the game is read from. The copy is written as .part and renamed at the end: the game looks for
// any .iso there every second and would open a half written one.
final class GameImporter {
    static final int REQUEST = 0x8A11;
    private static final String TAG = "MostWantedImport";
    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes();
    // where the volume descriptor sits: the game partition alone, redump, XGD3, XGD1 video offsets
    private static final long[] PARTITIONS = {0, 0x18300000L, 0xFD90000L, 0x2080000L};
    private static final int BUFFER = 4 << 20;

    private final Activity activity;
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile boolean cancelled;

    GameImporter(Activity activity) {
        this.activity = activity;
    }

    // the picker; any file type, since phones rarely know a type for .iso
    boolean pick() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            activity.startActivityForResult(i, REQUEST);
            return true;
        } catch (ActivityNotFoundException e) {
            message(R.string.import_game, activity.getString(R.string.no_file_picker), null);
            return false;
        }
    }

    // the picked file into dir; done runs on the main thread with true once the image is in place
    void copy(Uri uri, File dir, Consumer<Boolean> done) {
        String name = displayName(uri);
        long size = size(uri);
        if (name == null || !name.toLowerCase().endsWith(".iso")) {
            message(R.string.import_game, activity.getString(R.string.choose_iso), () -> done.accept(false));
            return;
        }
        dir.mkdirs();
        if (size > 0 && dir.getUsableSpace() < size + (64L << 20)) {
            message(R.string.not_enough_space, activity.getString(R.string.space_needed, size / 1e9,
                    dir.getUsableSpace() / 1e9), () -> done.accept(false));
            return;
        }

        ProgressBar bar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setIndeterminate(size <= 0);
        TextView text = new TextView(activity);
        text.setText(R.string.import_starting);
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        box.addView(bar);
        box.addView(text);
        cancelled = false;
        AlertDialog progress = new AlertDialog.Builder(activity)
                .setTitle(activity.getString(R.string.importing, name))
                .setView(box)
                .setCancelable(false)
                .setNegativeButton(android.R.string.cancel, (d, w) -> cancelled = true)
                .show();
        progress.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        File part = new File(dir, name + ".part");
        File iso = new File(dir, name);
        new Thread(() -> {
            String error = null;
            try (ParcelFileDescriptor pfd = activity.getContentResolver().openFileDescriptor(uri, "r");
                 FileInputStream in = new FileInputStream(pfd.getFileDescriptor());
                 FileChannel src = in.getChannel()) {
                if (isXboxImage(src) == Boolean.FALSE) {
                    error = activity.getString(R.string.not_xbox_image, name);
                } else {
                    try (FileOutputStream out = new FileOutputStream(part); FileChannel dst = out.getChannel()) {
                        ByteBuffer buf = ByteBuffer.allocateDirect(BUFFER);
                        long copied = 0, shown = 0;
                        while (!cancelled && src.read(buf) > 0) {
                            buf.flip();
                            while (buf.hasRemaining()) copied += dst.write(buf);
                            buf.clear();
                            if (copied - shown >= (16L << 20)) {
                                shown = copied;
                                long c = copied;
                                main.post(() -> show(bar, text, c, size));
                            }
                        }
                        out.getFD().sync();
                        if (!cancelled && size > 0 && copied != size) error = activity.getString(R.string.copy_stopped);
                    }
                }
            } catch (Exception e) {
                Log.w(TAG, "import failed", e);
                error = activity.getString(R.string.copy_failed, e.getMessage());
            }
            boolean ok = error == null && !cancelled && part.renameTo(iso);
            if (!ok) {
                part.delete();
                if (error == null && !cancelled) error = activity.getString(R.string.not_in_place);
            }
            String why = error;
            main.post(() -> {
                progress.dismiss();
                if (ok) {
                    Log.i(TAG, "imported " + iso);
                    message(R.string.game_imported, activity.getString(R.string.game_imported_text, name),
                            () -> done.accept(true));
                } else if (why != null) {
                    message(R.string.import_failed, why, () -> done.accept(false));
                } else {
                    done.accept(false);  // cancelled
                }
            });
        }, "GameImport").start();
    }

    private void show(ProgressBar bar, TextView text, long copied, long size) {
        if (size > 0) {
            bar.setProgress((int) (copied * 1000 / size));
            text.setText(activity.getString(R.string.import_progress, copied / 1e9, size / 1e9));
        } else {
            text.setText(String.format("%.2f GB", copied / 1e9));
        }
    }

    // null when the file cannot be read out of order (some cloud drives stream it): copied unchecked then, the game
    // tells of a wrong image itself. Positional reads leave the channel at the start for the copy.
    private static Boolean isXboxImage(FileChannel ch) {
        ByteBuffer b = ByteBuffer.allocate(MAGIC.length);
        try {
            for (long part : PARTITIONS) {
                b.clear();
                if (ch.read(b, part + 32 * 2048) == MAGIC.length && Arrays.equals(b.array(), MAGIC)) return true;
            }
            return false;
        } catch (Exception e) {
            Log.i(TAG, "image not checked, no seeking: " + e);
            return null;
        }
    }

    private String displayName(Uri uri) {
        try (Cursor c = activity.getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return new File(c.getString(0)).getName();
        } catch (Exception e) {
            // fall back on the path
        }
        String last = uri.getLastPathSegment();
        return last == null ? null : new File(last).getName();
    }

    private long size(Uri uri) {
        try (Cursor c = activity.getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (Exception e) {
            // unknown: the progress has no end then
        }
        return -1;
    }

    private void message(int title, String text, Runnable after) {
        new AlertDialog.Builder(activity)
                .setTitle(title)
                .setMessage(text)
                .setPositiveButton(android.R.string.ok, null)
                .setOnDismissListener(d -> {
                    if (after != null) after.run();
                })
                .show();
    }
}
