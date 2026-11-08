package com.racas.mostwanted;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.text.TextUtils;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

// Mods Loader: add-on cars, an icon of its own in a process of its own (the game's libraries stay out of it). The player
// downloads a car from nfsaddons.com in their own browser and installs the archive here, through the file picker or by
// opening it with this app; nothing here fetches anything.
public class ModsActivity extends Activity {
    private static final String TAG = "MostWantedMods";
    private static final String SITE = "https://www.nfsaddons.com/downloads/nfsmw/cars/";
    private static final int PICK = 0x4D4F;
    private static final long MAX_FILE = 4L << 30;  // a texture pack's archive is gigabytes
    private static final String LOADER_VERSION = "1.0.0";

    // flat: a dark ground, one orange for what acts
    private static final int BG = 0xFF111317, SURFACE = 0xFF1A1D23, LINE = 0xFF2A2E36;
    private static final int TEXT = 0xFFEDEEF0, MUTED = 0xFF8D939E, ACCENT = 0xFFFF7A00, ON_ACCENT = 0xFF111317;
    private static final String PREFS = "mods", LANG = "lang";
    private static final String[][] LANGS = {{"en", "EN"}, {"in", "ID"}};

    private TextView count;
    private LinearLayout carList;
    private AlertDialog dialog;
    private volatile boolean cancelled;
    private ProgressBar progressBar;
    private TextView progressText;
    private volatile long expected;

    // English unless the player picked another here: the phone's language does not decide it. An override, so it
    // outlives the rotations the activity handles itself
    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        Configuration c = new Configuration();
        c.setLocale(new Locale(language(base)));
        applyOverrideConfiguration(c);
    }

    private static String language(Context c) {
        return c.getSharedPreferences(PREFS, MODE_PRIVATE).getString(LANG, "en");
    }

    private void setLanguage(String lang) {
        if (lang.equals(language(this))) return;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(LANG, lang).commit();
        recreate();
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        buildUi();
        if (saved == null) takeIntent(getIntent());
    }

    // the activity keeps itself across rotations: laid out again for the new shape
    @Override
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        buildUi();
        showInstalled();
    }

    private int dp(float v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private boolean twoColumns() {
        Configuration c = getResources().getConfiguration();
        return c.screenWidthDp >= 720 || (c.orientation == Configuration.ORIENTATION_LANDSCAPE && c.screenWidthDp >= 560);
    }

    // upright phone: one scrolling column. Tablet or landscape: the actions stay put on the left and the cars scroll
    // on the right under their own header
    private void buildUi() {
        boolean two = twoColumns();
        int edge = dp(two ? 28 : 16);

        LinearLayout main = column();
        main.addView(header(), spaced(dp(24)));
        main.addView(action(R.drawable.mods_ic_open, R.string.mods_open_site, R.string.mods_open_site_sub, v -> openSite()), spaced(dp(8)));
        main.addView(action(R.drawable.mods_ic_file, R.string.mods_from_file, R.string.mods_from_file_sub, v -> pickFile()), spaced(dp(16)));
        main.addView(playButton(), spaced(dp(28)));
        main.addView(steps(), spaced(dp(28)));
        main.addView(credits(), spaced(dp(28)));
        main.addView(versions());

        carList = column();
        View carsHead = carsHeader();

        if (two) {
            LinearLayout root = new LinearLayout(this);
            root.setBackgroundColor(BG);

            ScrollView left = new ScrollView(this);  // fits a tablet as is; scrolls only on a short landscape phone
            left.setOverScrollMode(View.OVER_SCROLL_NEVER);
            main.setPadding(edge, edge, edge, edge);
            left.addView(main);
            root.addView(left, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1));

            View rule = new View(this);
            rule.setBackgroundColor(LINE);
            root.addView(rule, new LinearLayout.LayoutParams(dp(1), ViewGroup.LayoutParams.MATCH_PARENT));

            LinearLayout right = column();
            carsHead.setPadding(edge, edge, edge, dp(8));
            right.addView(carsHead);
            ScrollView list = new ScrollView(this);
            carList.setPadding(edge, 0, edge, edge);
            list.addView(carList);
            right.addView(list, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
            root.addView(right, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1));
            setContentView(root);
        } else {
            LinearLayout col = column();
            col.setPadding(edge, dp(20), edge, dp(24));
            col.addView(main, spaced(dp(28)));
            col.addView(carsHead, spaced(dp(8)));
            col.addView(carList);
            ScrollView scroll = new ScrollView(this);
            scroll.setBackgroundColor(BG);
            scroll.addView(col);
            setContentView(scroll);
        }
    }

    private LinearLayout column() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    private LinearLayout.LayoutParams spaced(int bottom) {
        LinearLayout.LayoutParams l = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        l.bottomMargin = bottom;
        return l;
    }

    private GradientDrawable shape(int color, float radius) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(radius));
        return g;
    }

    private Drawable pressable(Drawable bg, float radius) {
        return new RippleDrawable(ColorStateList.valueOf(0x26FFFFFF), bg, shape(Color.WHITE, radius));
    }

    private TextView text(CharSequence s, float size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(size);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        return t;
    }

    private ImageView icon(int res, int tint, int size) {
        ImageView i = new ImageView(this);
        i.setImageResource(res);
        i.setImageTintList(ColorStateList.valueOf(tint));
        i.setLayoutParams(new LinearLayout.LayoutParams(dp(size), dp(size)));
        return i;
    }

    // the icon, the name, and the language switch
    private View header() {
        LinearLayout box = new LinearLayout(this);
        box.setGravity(Gravity.CENTER_VERTICAL);

        ImageView logo = new ImageView(this);
        logo.setImageResource(R.mipmap.ic_mods);
        logo.setBackground(shape(Color.TRANSPARENT, 10));
        logo.setClipToOutline(true);
        LinearLayout.LayoutParams ll = new LinearLayout.LayoutParams(dp(48), dp(48));
        ll.setMarginEnd(dp(14));
        box.addView(logo, ll);

        LinearLayout words = column();
        words.addView(text(getString(R.string.mods_title), 20, TEXT, true));
        words.addView(text(getString(R.string.mods_subtitle), 13, MUTED, false));
        box.addView(words, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        box.addView(languageSwitch());
        return box;
    }

    private View languageSwitch() {
        LinearLayout box = new LinearLayout(this);
        GradientDrawable frame = shape(Color.TRANSPARENT, 8);
        frame.setStroke(dp(1), LINE);
        box.setBackground(frame);
        box.setPadding(dp(3), dp(3), dp(3), dp(3));
        box.setContentDescription(getString(R.string.mods_language));
        String now = language(this);
        for (String[] l : LANGS) {
            boolean on = l[0].equals(now);
            TextView t = text(l[1], 13, on ? ON_ACCENT : MUTED, true);
            t.setGravity(Gravity.CENTER);
            t.setPadding(dp(12), dp(6), dp(12), dp(6));
            t.setBackground(on ? shape(ACCENT, 6) : pressable(shape(Color.TRANSPARENT, 6), 6));
            t.setOnClickListener(v -> setLanguage(l[0]));
            box.addView(t);
        }
        return box;
    }

    private View action(int iconRes, int title, int sub, View.OnClickListener click) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setBackground(pressable(shape(SURFACE, 8), 8));
        row.setPadding(dp(16), dp(14), dp(12), dp(14));
        row.setOnClickListener(click);
        ImageView i = icon(iconRes, ACCENT, 24);
        ((LinearLayout.LayoutParams) i.getLayoutParams()).setMarginEnd(dp(16));
        row.addView(i);
        LinearLayout words = column();
        words.addView(text(getString(title), 16, TEXT, true));
        words.addView(text(getString(sub), 13, MUTED, false));
        row.addView(words, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        row.addView(icon(R.drawable.mods_ic_chevron, MUTED, 22));
        return row;
    }

    private View playButton() {
        LinearLayout b = new LinearLayout(this);
        b.setGravity(Gravity.CENTER);
        b.setBackground(pressable(shape(ACCENT, 8), 8));
        b.setMinimumHeight(dp(52));
        b.setOnClickListener(v -> play());
        b.addView(icon(R.drawable.mods_ic_play, ON_ACCENT, 24));
        TextView t = text(getString(R.string.mods_play), 16, ON_ACCENT, true);
        t.setPadding(dp(6), 0, 0, 0);
        b.addView(t);
        return b;
    }

    private View steps() {
        LinearLayout box = column();
        box.addView(sectionTitle(R.string.mods_steps), spaced(dp(10)));
        String[] lines = getString(R.string.mods_how).split("\n");
        for (int n = 0; n < lines.length; n++) {
            LinearLayout row = new LinearLayout(this);
            TextView num = text((n + 1) + ".", 14, TEXT, true);
            num.setMinWidth(dp(22));
            row.addView(num);
            row.addView(text(lines[n], 14, MUTED, false), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            box.addView(row, spaced(dp(6)));
        }
        return box;
    }

    // who made it, and where to follow or support it: the same links as the game's credits
    private View credits() {
        LinearLayout box = column();
        box.addView(sectionTitle(R.string.credits), spaced(dp(10)));
        box.addView(text(getString(R.string.port_credit), 14, TEXT, false), spaced(dp(2)));
        box.addView(text(getString(R.string.mods_credit_cars), 13, MUTED, false), spaced(dp(12)));
        LinearLayout links = new LinearLayout(this);
        links.addView(link(R.string.donate, R.string.donate_url, true));
        links.addView(link(R.string.github, R.string.github_url, false));
        links.addView(link(R.string.youtube, R.string.youtube_url, false));
        box.addView(links);
        return box;
    }

    // the app's version is the APK's (the release workflow sets it from the tag); the loader counts its own
    private View versions() {
        LinearLayout box = column();
        box.addView(sectionTitle(R.string.mods_version), spaced(dp(10)));
        String app = "?";
        try {
            android.content.pm.PackageInfo p = getPackageManager().getPackageInfo(getPackageName(), 0);
            app = p.versionName + " (" + p.getLongVersionCode() + ")";
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            // our own package: always there
        }
        box.addView(versionRow("Most Wanted Android", app), spaced(dp(6)));
        box.addView(versionRow("Mods Loader", LOADER_VERSION), spaced(0));
        return box;
    }

    private View versionRow(String name, String version) {
        LinearLayout row = new LinearLayout(this);
        row.addView(text(name, 14, MUTED, false), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        row.addView(text(version, 14, TEXT, false));
        return row;
    }

    private View link(int label, int url, boolean first) {
        TextView t = text(getString(label), 14, first ? ACCENT : TEXT, true);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(16), dp(10), dp(16), dp(10));
        GradientDrawable frame = shape(Color.TRANSPARENT, 8);
        frame.setStroke(dp(1), first ? ACCENT : LINE);
        t.setBackground(pressable(frame, 8));
        t.setOnClickListener(v -> openLink(getString(url)));
        LinearLayout.LayoutParams l = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        if (!first) l.setMarginStart(dp(8));
        t.setLayoutParams(l);
        return t;
    }

    private TextView sectionTitle(int res) {
        TextView t = text(getString(res).toUpperCase(Locale.getDefault()), 12, MUTED, true);
        t.setLetterSpacing(0.08f);
        return t;
    }

    private View carsHeader() {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(sectionTitle(R.string.mods_manage), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        count = text("0", 12, MUTED, true);
        row.addView(count);
        return row;
    }

    // a car a row, ruled off from the next, its remove button at the end
    private View carRow(CarConverter.Kept car) {
        return modRow(car.title, car.car, () -> askRemove(getString(R.string.mods_remove_text, car.title), () -> remove(car)));
    }

    private View packRow(TexturePacks.Kept pack) {
        String sub = getString(R.string.mods_texture_pack, pack.count) + " · " + pack.size + " px"
                + (pack.author.isEmpty() ? "" : " · " + pack.author);
        return modRow(pack.name, sub, () -> askRemove(getString(R.string.mods_remove_pack_text, pack.name), () -> removePack(pack)));
    }

    private View modRow(String title, String sub, Runnable onRemove) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(10), 0, dp(10));
        LinearLayout words = column();
        TextView name = text(title, 15, TEXT, false);
        name.setSingleLine(true);
        name.setEllipsize(TextUtils.TruncateAt.END);
        words.addView(name);
        TextView id = text(sub, 12, MUTED, false);
        id.setSingleLine(true);
        words.addView(id);
        row.addView(words, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        ImageView del = icon(R.drawable.mods_ic_delete, MUTED, 40);
        del.setPadding(dp(10), dp(10), dp(10), dp(10));
        del.setBackground(new RippleDrawable(ColorStateList.valueOf(0x26FFFFFF), null, null));
        del.setContentDescription(getString(R.string.mods_remove) + " " + title);
        del.setOnClickListener(v -> onRemove.run());
        row.addView(del);
        return row;
    }

    private View rule() {
        View v = new View(this);
        v.setBackgroundColor(LINE);
        v.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1)));
        return v;
    }

    @Override
    protected void onResume() {
        super.onResume();
        showInstalled();
        offerUpdate();
    }

    private boolean updateOffered;

    // cars kept from an older converter (ModInstaller.BUILD): built again once, on the player's word
    private void offerUpdate() {
        if (updateOffered || !hasAccess() || !ModInstaller.outdated() || (dialog != null && dialog.isShowing())) return;
        updateOffered = true;
        StringBuilder msg = new StringBuilder(getString(R.string.mods_update_text));
        if (ModInstaller.gameRunning(this)) msg.append("\n\n").append(getString(R.string.mods_game_closes));
        dialog = new AlertDialog.Builder(this)
                .setTitle(R.string.mods_update_title)
                .setMessage(msg)
                .setPositiveButton(R.string.mods_update, (d, w) -> refresh())
                .setNegativeButton(R.string.mods_later, null)
                .show();
    }

    private void refresh() {
        dialog = progress(getString(R.string.mods_update_title), R.string.mods_converting, false);
        new Thread(() -> {
            String error = null;
            try {
                ModInstaller.refresh(this, line -> Log.i(TAG, line));
            } catch (Exception e) {
                Log.w(TAG, "refresh", e);
                error = getString(R.string.mods_error, String.valueOf(e.getMessage()));
            }
            String why = error;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                dialog.dismiss();
                showInstalled();
                if (why != null) tell(R.string.mods_failed, why);
            });
        }, "ModRefresh").start();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        takeIntent(intent);
    }

    @Override
    protected void onDestroy() {
        if (dialog != null) dialog.dismiss();
        super.onDestroy();
    }

    private void showInstalled() {
        List<CarConverter.Kept> kept = hasAccess() ? ModInstaller.kept() : new ArrayList<>();
        List<TexturePacks.Kept> packs = hasAccess() ? TexturePacks.kept() : new ArrayList<>();
        count.setText(String.valueOf(kept.size() + packs.size()));
        carList.removeAllViews();
        for (int i = 0; i < packs.size(); i++) {
            if (i > 0) carList.addView(rule());
            carList.addView(packRow(packs.get(i)));
        }
        if (!packs.isEmpty() && !kept.isEmpty()) carList.addView(rule());
        if (kept.isEmpty() && packs.isEmpty()) {
            TextView none = text(getString(R.string.mods_none), 14, MUTED, false);
            none.setPadding(0, dp(12), 0, dp(12));
            carList.addView(none);
            return;
        }
        for (int i = 0; i < kept.size(); i++) {
            if (i > 0) carList.addView(rule());
            carList.addView(carRow(kept.get(i)));
        }
    }

    // the player's own browser: the site's download, its wait and its ads stay there
    private void openSite() {
        openLink(SITE);
    }

    private void openLink(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)).addCategory(Intent.CATEGORY_BROWSABLE));
        } catch (ActivityNotFoundException e) {
            tell(R.string.mods_failed, getString(R.string.mods_no_browser));
        }
    }

    // an archive opened with or shared to this app, from the browser's downloads or a file manager
    private void takeIntent(Intent intent) {
        if (intent == null) return;
        Uri uri = null;
        if (Intent.ACTION_VIEW.equals(intent.getAction())) uri = intent.getData();
        else if (Intent.ACTION_SEND.equals(intent.getAction())) uri = intent.getParcelableExtra(Intent.EXTRA_STREAM);
        if (uri != null) installFrom(uri);
    }

    private boolean hasAccess() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    // what installing needs, told if it is missing
    private boolean ready() {
        if (!hasAccess()) {
            dialog = new AlertDialog.Builder(this)
                    .setTitle(R.string.storage_access)
                    .setMessage(R.string.mods_need_access)
                    .setPositiveButton(R.string.mods_play, (d, w) -> play())
                    .setNegativeButton(android.R.string.cancel, null)
                    .show();
            return false;
        }
        if (ModInstaller.findGame(this) == null) {
            tell(R.string.mods_failed, getString(R.string.mods_no_game, "/storage/emulated/0/" + MostWantedActivity.SHARED));
            return false;
        }
        return true;
    }

    private AlertDialog progress(String title, int message, boolean canCancel) {
        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setIndeterminate(true);
        progressText = new TextView(this);
        progressText.setText(message);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(20 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        box.addView(progressBar);
        box.addView(progressText);
        AlertDialog.Builder b = new AlertDialog.Builder(this).setTitle(title).setView(box).setCancelable(false);
        if (canCancel) b.setNegativeButton(android.R.string.cancel, (d, w) -> cancelled = true);
        AlertDialog shown = b.show();
        shown.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        return shown;
    }

    private void pickFile() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            startActivityForResult(i, PICK);
        } catch (ActivityNotFoundException e) {
            tell(R.string.mods_failed, getString(R.string.mods_no_picker));
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        Uri uri = request == PICK && result == RESULT_OK && data != null ? data.getData() : null;
        if (uri != null) installFrom(uri);
    }

    // the file's own name and size where its provider gives them
    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                if (!c.isNull(1)) expected = c.getLong(1);
                String n = c.getString(0);
                if (n != null && !n.isEmpty()) return n;
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "file name", e);
        }
        String last = uri.getLastPathSegment();
        return last == null ? "?" : new File(last).getName();
    }

    // the file copied to the app's cache, then the question
    private void installFrom(Uri uri) {
        if (!ready()) return;
        cancelled = false;
        expected = -1;
        String name = displayName(uri);
        dialog = progress(name, R.string.mods_reading, true);
        new Thread(() -> {
            File dir = new File(getCacheDir(), "mods");
            dir.mkdirs();
            File file = new File(dir, "download.bin");
            String error = null;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) throw new IOException("the file cannot be opened");
                save(in, file);
            } catch (Exception e) {
                Log.w(TAG, "read", e);
                error = getString(R.string.mods_error, String.valueOf(e.getMessage()));
            }
            String why = error;
            boolean pack = why == null && !cancelled && ModArchive.isTexturePack(file);
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                dialog.dismiss();
                if (why != null || cancelled) {
                    file.delete();
                    if (why != null) tell(R.string.mods_failed, why);
                    return;
                }
                if (pack) chooseResolution(name, file);
                else confirm(name, file);
            });
        }, "ModRead").start();
    }

    // a texture pack's images are kept at most this size; the game's TEXTURE RESOLUTION picks up to it. Bigger is
    // sharper, and more storage and GPU memory
    private void chooseResolution(String name, File file) {
        int[] sizes = TexturePacks.SIZES;
        String[] labels = new String[sizes.length];
        int[] notes = {R.string.mods_res_small, R.string.mods_res_recommended, R.string.mods_res_large, R.string.mods_res_huge};
        int chosen = 1;
        for (int i = 0; i < sizes.length; i++) {
            labels[i] = sizes[i] + " px — " + getString(notes[i]);
            if (sizes[i] == TexturePacks.max) chosen = i;
        }
        int[] pick = {chosen};
        dialog = new AlertDialog.Builder(this)
                .setTitle(R.string.mods_res_title)
                .setSingleChoiceItems(labels, chosen, (d, w) -> pick[0] = w)
                .setCancelable(false)
                .setPositiveButton(R.string.mods_next, (d, w) -> {
                    TexturePacks.max = sizes[pick[0]];
                    confirm(name, file);
                })
                .setNegativeButton(android.R.string.cancel, (d, w) -> file.delete())
                .show();
    }

    // what the install is about to do, said once before anything changes
    private void confirm(String name, File file) {
        StringBuilder msg = new StringBuilder(getString(R.string.mods_confirm, name));
        if (ModInstaller.gameRunning(this)) msg.append("\n\n").append(getString(R.string.mods_game_closes));
        dialog = new AlertDialog.Builder(this)
                .setTitle(R.string.mods_install)
                .setMessage(msg)
                .setCancelable(false)
                .setPositiveButton(R.string.mods_install, (d, w) -> install(name, file))
                .setNegativeButton(android.R.string.cancel, (d, w) -> file.delete())
                .show();
    }

    // the last archive that would not go in, in the logs folder, to see why
    private void keepFailed(File file, String name) {
        File logs = new File(new File(Environment.getExternalStorageDirectory(), MostWantedActivity.SHARED), "logs");
        File[] old = logs.listFiles((d, n) -> n.startsWith("failed_"));
        if (old != null) for (File f : old) f.delete();
        try {
            logs.mkdirs();
            Files.copy(file.toPath(), new File(logs, "failed_" + new File(name).getName()).toPath());
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "keep failed", e);
        }
    }

    private void install(String name, File file) {
        dialog = progress(name, R.string.mods_converting, false);  // nothing stops halfway from here
        new Thread(() -> {
            ModInstaller.Installed result = null;
            String error = null;
            try {
                result = ModInstaller.install(this, file, null, line -> {
                    Log.i(TAG, line);
                    // a pack takes minutes: which car it has got to
                    if (line.startsWith("car ") || line.startsWith("pack "))
                        runOnUiThread(() -> progressText.setText(line.substring(line.indexOf(' ') + 1)));
                    else if (line.startsWith("read "))
                        runOnUiThread(() -> progressText.setText(getString(R.string.mods_reading_files, line.substring(5))));
                });
            } catch (CarConverter.Failure e) {
                Log.w(TAG, "convert", e);
                error = explain(e);
            } catch (ModArchive.Unsupported e) {
                error = getString(R.string.mods_bad_archive, e.getMessage());
            } catch (OutOfMemoryError e) {
                error = getString(R.string.mods_too_big);
            } catch (Exception e) {
                Log.w(TAG, "install", e);
                error = getString(R.string.mods_error, String.valueOf(e.getMessage()));
            } finally {
                if (result == null) keepFailed(file, name);
                file.delete();
            }
            ModInstaller.Installed done = result;
            String why = error;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                dialog.dismiss();
                showInstalled();
                if (done != null) tellDone(done);
                else if (why != null) tell(R.string.mods_failed, why);
            });
        }, "ModInstall").start();
    }

    private String explain(CarConverter.Failure e) {
        switch (e.reason) {
            case NO_CAR: return getString(R.string.mods_no_car);
            case NAME_TOO_LONG: return getString(R.string.mods_name_long, e.getMessage());
            default: return getString(R.string.mods_unsupported, e.getMessage());
        }
    }

    private void tellDone(ModInstaller.Installed r) {
        StringBuilder msg = new StringBuilder(r.done.size() == 1 ? getString(R.string.mods_done_addon, r.done.get(0))
                : getString(R.string.mods_done_pack, r.done.size()));
        if (!r.failed.isEmpty()) msg.append("\n\n").append(getString(R.string.mods_pack_failed, android.text.TextUtils.join(", ", r.failed)));
        dialog = new AlertDialog.Builder(this)
                .setTitle(R.string.mods_done)
                .setMessage(msg)
                .setPositiveButton(R.string.mods_play, (d, w) -> play())
                .setNegativeButton(R.string.close, null)
                .show();
    }

    private void tell(int title, String text) {
        dialog = new AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage(text)
                .setPositiveButton(android.R.string.ok, null)
                .show();
    }

    private void askRemove(String text, Runnable remove) {
        StringBuilder msg = new StringBuilder(text);
        if (ModInstaller.gameRunning(this)) msg.append("\n\n").append(getString(R.string.mods_game_closes));
        dialog = new AlertDialog.Builder(this)
                .setTitle(R.string.mods_remove)
                .setMessage(msg)
                .setPositiveButton(R.string.mods_remove, (d, w) -> remove.run())
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    // the game's files are made again from the rest
    private void remove(CarConverter.Kept car) {
        dialog = progress(car.title, R.string.mods_converting, false);
        new Thread(() -> {
            String error = null;
            try {
                ModInstaller.uninstall(this, car.car, line -> Log.i(TAG, line));
            } catch (Exception e) {
                Log.w(TAG, "remove", e);
                error = getString(R.string.mods_error, String.valueOf(e.getMessage()));
            }
            String why = error;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                dialog.dismiss();
                showInstalled();
                if (why != null) tell(R.string.mods_failed, why);
            });
        }, "ModRemove").start();
    }

    // a pack is a folder of its own: nothing else is made again
    private void removePack(TexturePacks.Kept pack) {
        dialog = progress(pack.name, R.string.mods_converting, false);
        new Thread(() -> {
            TexturePacks.uninstall(this, pack.id);
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                dialog.dismiss();
                showInstalled();
            });
        }, "ModRemove").start();
    }

    private void play() {
        startActivity(new Intent(this, MostWantedActivity.class));
    }

    private void save(InputStream in, File target) throws IOException {
        byte[] b = new byte[1 << 16];
        long got = 0, shown = 0;
        try (FileOutputStream out = new FileOutputStream(target)) {
            for (int r; !cancelled && (r = in.read(b)) > 0; ) {
                got += r;
                if (got > MAX_FILE) throw new IOException("over " + (MAX_FILE >> 20) + " MB");
                out.write(b, 0, r);
                if (got - shown >= (1 << 19)) {
                    shown = got;
                    long g = got, all = expected;
                    runOnUiThread(() -> {
                        if (all > 0) {
                            progressBar.setIndeterminate(false);
                            progressBar.setMax(1000);
                            progressBar.setProgress((int) (g * 1000 / all));
                            progressText.setText(String.format(Locale.ROOT, "%.1f / %.1f MB", g / 1048576.0, all / 1048576.0));
                        } else {
                            progressText.setText(String.format(Locale.ROOT, "%.1f MB", g / 1048576.0));
                        }
                    });
                }
            }
        }
    }
}
