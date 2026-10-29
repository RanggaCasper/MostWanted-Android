package com.racas.mostwanted;

import android.Manifest;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.Settings;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Toast;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

// SDL's activity running libmain.so. The game goes in the MostWanted folder on the phone's storage
// (/storage/emulated/0/MostWanted) or at the top of an SD card. Reading it needs all files access, a special access Android grants on a settings page
// of its own, never among the app's ordinary permissions. This asks for it and says where the game goes, each time
// the app comes back without them; the game itself waits and starts as soon as it finds the files.
// At launch it also asks GitHub for a newer release (UpdateChecker) and tells of it once nothing else is asked.
public class MostWantedActivity extends SDLActivity {
    static final String SHARED = "MostWanted";

    private AlertDialog dialog;  // the one up, at most one at a time
    private boolean notNow;      // the player turned the access down this run
    private boolean resumed;
    private UpdateChecker.Release update;  // a newer release not shown yet
    private final GameImporter importer = new GameImporter(this);
    private boolean importing;  // the file picker or the copy is up: nothing else is asked meanwhile
    private boolean checking;   // a check asked from the menu is under way

    // SDL may request an orientation from its window hint. Keep this game in sensor-driven landscape,
    // allowing either horizontal side while rejecting portrait requests.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        if (w > 1 && h > 1) setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        for (View v : new View[] {mSurface, mLayout})
            if (v != null) v.setOnCapturedPointerListener(MostWantedActivity::onCapturedPointer);
        File own = getExternalFilesDir(null);
        if (own != null) new File(own, "assets").mkdirs();
        // every launch: a newer release is shown once nothing else is asked
        UpdateChecker.check(this, false, (r, failed) -> {
            if (r == null) return;
            update = r;
            tellOfUpdate();
        });
    }

    // A controller some phones' game services pass on as a virtual device (MIUI's took a DualShock 4 on a Redmi Note
    // 8 Pro): SDL sees no controller in it and drops its input. Its buttons and sticks go to the game as port 0's
    // here (native/input.cpp); a controller SDL has goes its own way as before.
    private static native void nativeLooseKey(int keyCode, boolean down);
    private static native void nativeLooseAxes(float lx, float ly, float rx, float ry, float lt, float rt, float hx,
                                               float hy);
    private static native void nativeLooseRelease();

    private static boolean loose(InputEvent e, int sources) {
        return !mBrokenLibraries && (e.getSource() & sources) != 0
                && !SDLControllerManager.isDeviceSDLJoystick(e.getDeviceId());
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        boolean pad = KeyEvent.isGamepadButton(e.getKeyCode()) || (e.getKeyCode() >= KeyEvent.KEYCODE_DPAD_UP
                && e.getKeyCode() <= KeyEvent.KEYCODE_DPAD_RIGHT);
        if (pad && loose(e, InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) {
            if (e.getRepeatCount() == 0) nativeLooseKey(e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN);
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (loose(e, InputDevice.SOURCE_JOYSTICK) && e.getActionMasked() == MotionEvent.ACTION_MOVE) {
            // the right stick on Z and RZ, the triggers on LTRIGGER and RTRIGGER or BRAKE and GAS, as Android has them
            nativeLooseAxes(e.getAxisValue(MotionEvent.AXIS_X), e.getAxisValue(MotionEvent.AXIS_Y),
                    e.getAxisValue(MotionEvent.AXIS_Z), e.getAxisValue(MotionEvent.AXIS_RZ),
                    Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE)),
                    Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS)),
                    e.getAxisValue(MotionEvent.AXIS_HAT_X), e.getAxisValue(MotionEvent.AXIS_HAT_Y));
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    // The mouse while the game holds it (SDL's relative mode, Android's pointer capture). SDL's own handler passes on
    // only moves with no button held and the wheel: clicks and the moves while firing were lost. Each sample of a
    // batch is a move of its own, as SDL's C side takes them.
    private static boolean onCapturedPointer(View view, MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_MOVE:
            case MotionEvent.ACTION_HOVER_MOVE:
                for (int h = 0; h < e.getHistorySize(); h++)
                    onNativeMouse(0, MotionEvent.ACTION_MOVE, e.getHistoricalX(h), e.getHistoricalY(h), true);
                onNativeMouse(0, MotionEvent.ACTION_MOVE, e.getX(), e.getY(), true);
                return true;
            case MotionEvent.ACTION_BUTTON_PRESS:
                onNativeMouse(e.getButtonState(), MotionEvent.ACTION_DOWN, 0, 0, true);
                return true;
            case MotionEvent.ACTION_BUTTON_RELEASE:
                onNativeMouse(e.getButtonState(), MotionEvent.ACTION_UP, 0, 0, true);
                return true;
            case MotionEvent.ACTION_SCROLL:
                onNativeMouse(0, MotionEvent.ACTION_SCROLL, e.getAxisValue(MotionEvent.AXIS_HSCROLL),
                        e.getAxisValue(MotionEvent.AXIS_VSCROLL), false);
                return true;
            default:
                // the down and up a press brings with it: the button events above carry it
                return true;
        }
    }

    // CHECK FOR UPDATES in the options menu, from the game's thread: the answer always shows, up to date or offline
    public void checkForUpdates() {
        runOnUiThread(() -> {
            if (checking) return;
            checking = true;
            Toast.makeText(this, R.string.checking_updates, Toast.LENGTH_SHORT).show();
            UpdateChecker.check(this, true, (r, failed) -> {
                checking = false;
                if (r != null) {
                    update = r;
                    tellOfUpdate();
                    return;
                }
                if (isFinishing() || (dialog != null && dialog.isShowing())) return;
                String msg = failed ? getString(R.string.update_unreachable)
                                    : getString(R.string.latest_version, UpdateChecker.installedVersion(this));
                dialog = new AlertDialog.Builder(this)
                        .setTitle(failed ? R.string.update_check_failed : R.string.no_update)
                        .setMessage(msg)
                        .setPositiveButton(android.R.string.ok, null)
                        .show();
            });
        });
    }

    // SHOW CREDITS from the options menu, from the game's thread
    public void showCredits() {
        runOnUiThread(() -> {
            if (isFinishing() || (dialog != null && dialog.isShowing())) return;
            
            // Create a custom view with social links
            android.view.LayoutInflater inflater = android.view.LayoutInflater.from(this);
            android.view.View view = inflater.inflate(android.R.layout.list_content, null);
            
            // Create a simple LinearLayout if layout doesn't exist
            android.widget.LinearLayout layout = new android.widget.LinearLayout(this);
            layout.setOrientation(android.widget.LinearLayout.VERTICAL);
            layout.setPadding(16, 16, 16, 16);
            
            // Credit text
            android.widget.TextView creditText = new android.widget.TextView(this);
            creditText.setText(getString(R.string.port_credit));
            creditText.setTextSize(18);
            creditText.setGravity(android.view.Gravity.CENTER);
            android.widget.LinearLayout.LayoutParams creditParams = new android.widget.LinearLayout.LayoutParams(
                    android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                    android.widget.LinearLayout.LayoutParams.WRAP_CONTENT);
            creditParams.setMargins(0, 0, 0, 24);
            layout.addView(creditText, creditParams);
            
            // Donate button
            android.widget.Button donateBtn = new android.widget.Button(this);
            donateBtn.setText(R.string.donate);
            android.widget.LinearLayout.LayoutParams btnParams = new android.widget.LinearLayout.LayoutParams(
                    android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                    android.widget.LinearLayout.LayoutParams.WRAP_CONTENT);
            btnParams.setMargins(0, 0, 0, 8);
            donateBtn.setOnClickListener(v -> open(getString(R.string.donate_url)));
            layout.addView(donateBtn, btnParams);
            
            // GitHub button
            android.widget.Button githubBtn = new android.widget.Button(this);
            githubBtn.setText(R.string.github);
            android.widget.LinearLayout.LayoutParams gitParams = new android.widget.LinearLayout.LayoutParams(
                    android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                    android.widget.LinearLayout.LayoutParams.WRAP_CONTENT);
            gitParams.setMargins(0, 0, 0, 8);
            githubBtn.setOnClickListener(v -> open(getString(R.string.github_url)));
            layout.addView(githubBtn, gitParams);
            
            // YouTube button
            android.widget.Button youtubeBtn = new android.widget.Button(this);
            youtubeBtn.setText(R.string.youtube);
            android.widget.LinearLayout.LayoutParams ytParams = new android.widget.LinearLayout.LayoutParams(
                    android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                    android.widget.LinearLayout.LayoutParams.WRAP_CONTENT);
            youtubeBtn.setOnClickListener(v -> open(getString(R.string.youtube_url)));
            layout.addView(youtubeBtn, ytParams);
            
            dialog = new AlertDialog.Builder(this)
                    .setTitle(R.string.credits)
                    .setView(layout)
                    .setPositiveButton(R.string.close, null)
                    .show();
        });
    }

    // OPEN DONATION LINK from the options menu, from the game's thread
    public void openDonation() {
        runOnUiThread(() -> {
            if (isFinishing()) return;
            open(getString(R.string.donate_url));
        });
    }

    // the performance overlay's fallback temperature, in tenths of a degree, -1 unknown: an app may not read the
    // thermal zones, the battery's sticky broadcast it always may
    public int batteryTemperature() {
        Intent i = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        return i != null ? i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -1) : -1;
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        askForGame();
        tellOfUpdate();
    }

    @Override
    protected void onPause() {
        resumed = false;
        super.onPause();
    }

    // SDL's older immersive flags don't always come back after a dialog (MIUI on Android 11 kept the status bar):
    // the bars are hidden again each time the game has the focus, a swipe shows them for a moment
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
        else if (!mBrokenLibraries) nativeLooseRelease();  // its ups go elsewhere while the game is not in front
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT < 30) {
            // Android 10: the older immersive flags
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
            return;
        }
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    // Android 11 on has all files access, a page of its own in the settings; Android 10 the storage permission, with
    // the legacy storage the manifest asks for
    private static final boolean ALL_FILES_ACCESS = Build.VERSION.SDK_INT >= 30;
    private static final int STORAGE_REQUEST = 0x426C;
    private boolean storageAsked;  // the permission was asked for this run

    private boolean hasStorageAccess() {
        if (ALL_FILES_ACCESS) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void askForGame() {
        if (importing || (dialog != null && dialog.isShowing())) return;
        File own = getExternalFilesDir(null);
        if (own != null && hasGame(new File(own, "assets"))) return;
        if (!hasStorageAccess()) {
            if (!notNow) askAllFilesAccess();
            return;
        }
        File shared = new File(Environment.getExternalStorageDirectory(), SHARED);
        shared.mkdirs();
        if (hasGame(shared) || hasGame(new File(shared, "assets"))) return;
        for (File card : cardFolders())
            if (hasGame(card) || hasGame(new File(card, "assets"))) return;
        tellWhereTheGameGoes();
    }

    // the MostWanted folder on each SD card or USB stick mounted, as native/game_files.cpp looks in them
    private List<File> cardFolders() {
        List<File> out = new ArrayList<>();
        if (!ALL_FILES_ACCESS) {
            // Android 10: each card's root above the app's own folder on it, <root>/Android/data/<package>/files
            File[] own = getExternalFilesDirs(null);
            for (int i = 1; i < own.length; i++) {
                File root = own[i];
                for (int up = 0; root != null && up < 4; up++) root = root.getParentFile();
                if (root != null) out.add(new File(root, SHARED));
            }
            return out;
        }
        StorageManager sm = getSystemService(StorageManager.class);
        if (sm == null) return out;
        for (StorageVolume v : sm.getStorageVolumes()) {
            File root = v.getDirectory();
            if (v.isPrimary() || root == null || !Environment.MEDIA_MOUNTED.equals(v.getState())) continue;
            out.add(new File(root, SHARED));
        }
        return out;
    }

    // the same folders for native/game_files.cpp, from the game's thread: some phones (a Sony on Android 11) don't let
    // the app list /storage, though each card's folder opens
    public String[] cardFolderPaths() {
        List<File> cards = cardFolders();
        String[] out = new String[cards.size()];
        for (int i = 0; i < out.length; i++) out[i] = cards.get(i).getPath();
        return out;
    }

    // the newer release, once the app is in front with no other dialog up; shown once a run
    private void tellOfUpdate() {
        if (update == null || importing || !resumed || isFinishing() || (dialog != null && dialog.isShowing())) return;
        UpdateChecker.Release r = update;
        update = null;
        String installed = "";
        try {
            installed = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            // only for the message
        }
        StringBuilder msg = new StringBuilder(getString(R.string.update_is_available, r.name));
        if (!installed.isEmpty()) msg.append(' ').append(getString(R.string.update_installed, installed));
        msg.append('.');
        if (!r.changes.isEmpty()) msg.append("\n\n").append(getString(R.string.update_whats_new)).append('\n').append(r.changes);
        msg.append("\n\n").append(getString(R.string.update_install_over));
        dialog = new AlertDialog.Builder(this)
                .setTitle(R.string.update_available)
                .setMessage(msg)
                .setPositiveButton(R.string.download, (d, w) -> open(r.apk != null ? r.apk : r.page))
                .setNeutralButton(R.string.skip_version, (d, w) -> UpdateChecker.skip(this, r.tag))
                .setNegativeButton(R.string.later, null)
                .show();
    }

    private void open(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException e) {
            // no browser: nothing to open it with
        }
    }

    // default.xbe or a disc image in the folder
    private static boolean hasGame(File dir) {
        if (new File(dir, "default.xbe").exists()) return true;
        File[] files = dir.listFiles();
        if (files == null) return false;
        for (File f : files)
            if (f.getName().toLowerCase().endsWith(".iso")) return true;
        return false;
    }

    private void askAllFilesAccess() {
        dialog = new AlertDialog.Builder(this)
                .setTitle(R.string.storage_access)
                .setMessage(getString(R.string.storage_text, "/storage/emulated/0/" + SHARED,
                        getString(ALL_FILES_ACCESS ? R.string.storage_all_files : R.string.storage_permission)))
                .setCancelable(false)
                .setPositiveButton(R.string.allow_access, (d, w) -> openAccessSettings())
                .setNegativeButton(R.string.not_now, (d, w) -> notNow = true)
                .setNeutralButton(R.string.import_game, (d, w) -> startImport())
                .setOnDismissListener(d -> tellOfUpdate())
                .show();
    }

    private void tellWhereTheGameGoes() {
        dialog = new AlertDialog.Builder(this)
                .setTitle(R.string.game_files)
                .setMessage(getString(R.string.game_files_text, "/storage/emulated/0/" + SHARED))
                .setPositiveButton(android.R.string.ok, null)
                .setNeutralButton(R.string.import_game, (d, w) -> startImport())
                .setOnDismissListener(d -> tellOfUpdate())
                .show();
    }

    // the disc image the player picks, copied where the game looks: the MostWanted folder with all files access, else the
    // app's own assets folder, which needs none
    private void startImport() {
        importing = importer.pick();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != GameImporter.REQUEST) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        Uri uri = resultCode == RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) {
            importing = false;  // the picker was closed: onResume asks again
            return;
        }
        File dir = hasStorageAccess()
                ? new File(Environment.getExternalStorageDirectory(), SHARED)
                : new File(getExternalFilesDir(null), "assets");
        importer.copy(uri, dir, ok -> {
            importing = false;
            if (!ok) askForGame();
            tellOfUpdate();
        });
    }

    // the app's own page of the access, or the list of every app's where a phone has no such page. Android 10 asks
    // for the storage permission, or opens the app's details once the player has turned it down for good
    private void openAccessSettings() {
        if (!ALL_FILES_ACCESS) {
            boolean ask = !storageAsked || shouldShowRequestPermissionRationale(Manifest.permission.READ_EXTERNAL_STORAGE);
            if (ask) {
                storageAsked = true;
                requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                        Manifest.permission.WRITE_EXTERNAL_STORAGE}, STORAGE_REQUEST);
            } else {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
            }
            return;
        }
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (ActivityNotFoundException e2) {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
            }
        }
    }
}
