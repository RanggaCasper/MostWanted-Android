package com.racas.mostwanted;

import android.content.Context;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import java.io.File;
import java.util.ArrayList;
import java.util.List;

/** Shared storage locations used by both the game and the standalone MODS app. */
final class ModsStorage {
    static final String SHARED = "MostWanted";
    private ModsStorage() {}
    static List<File> cardFolders(Context context) {
        List<File> out = new ArrayList<>();
        if (BuildCompat.isLegacyStorage()) {
            File[] own = context.getExternalFilesDirs(null);
            for (int i = 1; i < own.length; i++) {
                File root = own[i];
                for (int up = 0; root != null && up < 4; up++) root = root.getParentFile();
                if (root != null) out.add(new File(root, SHARED));
            }
            return out;
        }
        StorageManager sm = context.getSystemService(StorageManager.class);
        if (sm == null) return out;
        for (StorageVolume v : sm.getStorageVolumes()) {
            File root = v.getDirectory();
            if (v.isPrimary() || root == null || !Environment.MEDIA_MOUNTED.equals(v.getState())) continue;
            out.add(new File(root, SHARED));
        }
        return out;
    }
    private static final class BuildCompat {
        static boolean isLegacyStorage() { return android.os.Build.VERSION.SDK_INT <= 29; }
    }
}
