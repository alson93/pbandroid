package com.pipibibs.recomp;

import android.app.Activity;
import android.app.AlarmManager;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.ClipData;
import android.content.DialogInterface;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Pipi & Bibis (recompiled arcade game). On first start it asks ONCE for the ROM files (the five tp025-N.bin files, or the
 * MAME zip) and keeps a private copy; later starts go straight into the game. The settings text file can be edited from the
 * game with the TXT button.
 */
public class MainActivity extends Activity {
    private static final int REQ_ROMS = 1;
    private static final String[] ROM_NAMES = { "tp025-1.bin", "tp025-2.bin", "tp025-3.bin", "tp025-4.bin", "tp025-5.bin" };

    private File romDir, settingsFile;
    private GameView game;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        romDir = new File(getFilesDir(), "roms");
        File ext = getExternalFilesDir(null);                       // app-specific folder, no permission needed (visible to file managers on most devices)
        if (ext == null) ext = getFilesDir();
        settingsFile = new File(ext, "pipibibs_settings.txt");
        ensureSettings();
        if (romsComplete()) startGame(); else showImportScreen(null);
    }

    // ---------------------------------------------------------------- settings file
    private void ensureSettings() {
        if (settingsFile.exists()) return;
        try { InputStream in = getAssets().open("pipibibs_settings.txt"); copy(in, new FileOutputStream(settingsFile)); }
        catch (IOException e) { /* the engine writes a default file itself if this one is missing */ }
    }

    /** The TXT button: edit the settings text, save, restart the game so the engine reads it again. */
    void openSettingsEditor() {
        final EditText et = new EditText(this);
        et.setTypeface(Typeface.MONOSPACE);
        et.setTextSize(11);
        et.setGravity(Gravity.TOP | Gravity.START);
        et.setHorizontallyScrolling(true);
        et.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        try { et.setText(new String(readAll(new java.io.FileInputStream(settingsFile)), "UTF-8")); } catch (IOException e) { et.setText(""); }
        AlertDialog dlg = new AlertDialog.Builder(this)
                .setTitle("pipibibs_settings.txt  (the game restarts after saving)")
                .setView(et)
                .setPositiveButton("Save and restart", new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface d, int w) {
                        try { FileOutputStream o = new FileOutputStream(settingsFile); o.write(et.getText().toString().getBytes("UTF-8")); o.close(); }
                        catch (IOException e) { Toast.makeText(MainActivity.this, "Could not save: " + e.getMessage(), Toast.LENGTH_LONG).show(); return; }
                        restartApp();
                    }
                })
                .setNegativeButton("Cancel", null)
                .setNeutralButton("Change ROMs", new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface d, int w) {
                        for (String n : ROM_NAMES) new File(romDir, n).delete();                  // ask for the ROMs again on the next start
                        restartApp();
                    }
                })
                .create();
        dlg.show();
        if (dlg.getWindow() != null) dlg.getWindow().setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
    }

    private void restartApp() {
        Toast.makeText(this, "Restarting...", Toast.LENGTH_SHORT).show();
        Intent i = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (i != null) {
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            int fl = PendingIntent.FLAG_CANCEL_CURRENT | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
            PendingIntent pi = PendingIntent.getActivity(this, 0, i, fl);
            ((AlarmManager) getSystemService(ALARM_SERVICE)).set(AlarmManager.RTC, System.currentTimeMillis() + 400, pi);
        }
        finishAffinity();
        Runtime.getRuntime().exit(0);   // the game engine keeps global state: a fresh process is the clean way to reload the settings
    }

    // ---------------------------------------------------------------- ROM import (only when the ROMs are not stored yet)
    private boolean romsComplete() {
        for (String n : ROM_NAMES) { File f = new File(romDir, n); if (!f.isFile() || f.length() == 0) return false; }
        return true;
    }

    private void showImportScreen(String message) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setBackgroundColor(Color.BLACK);
        box.setPadding(48, 24, 48, 24);
        TextView t = new TextView(this);
        t.setTextColor(Color.WHITE);
        t.setTextSize(16);
        t.setGravity(Gravity.CENTER);
        t.setText((message != null ? message + "\n\n" : "")
                + "Pipi & Bibis needs the arcade ROM files (set \"pipibibs\"):\n"
                + "tp025-1.bin ... tp025-5.bin, or the zip that contains them.\n"
                + "Select them once - they are copied into the app and you are not asked again.");
        Button btn = new Button(this);
        btn.setText("Select ROM files or ZIP");
        btn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                Intent it = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                it.addCategory(Intent.CATEGORY_OPENABLE);
                it.setType("*/*");
                it.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
                startActivityForResult(it, REQ_ROMS);
            }
        });
        box.addView(t);
        box.addView(btn);
        setContentView(box);
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_ROMS) return;
        if (res != RESULT_OK || data == null) { showImportScreen("No files were selected."); return; }
        ArrayList<Uri> uris = new ArrayList<Uri>();
        ClipData cd = data.getClipData();
        if (cd != null) for (int i = 0; i < cd.getItemCount(); ++i) uris.add(cd.getItemAt(i).getUri());
        else if (data.getData() != null) uris.add(data.getData());
        romDir.mkdirs();
        for (Uri u : uris) {
            try { importUri(u); } catch (IOException e) { /* reported below through the list of missing files */ }
        }
        if (romsComplete()) { startGame(); return; }
        StringBuilder miss = new StringBuilder();
        for (String n : ROM_NAMES) if (!new File(romDir, n).isFile()) miss.append(miss.length() > 0 ? ", " : "").append(n);
        showImportScreen("Still missing: " + miss);
    }

    private void importUri(Uri u) throws IOException {
        String name = displayName(u).toLowerCase(Locale.ROOT);
        InputStream in = getContentResolver().openInputStream(u);
        if (in == null) return;
        try {
            if (name.endsWith(".zip")) {
                ZipInputStream zin = new ZipInputStream(in);
                ZipEntry e;
                while ((e = zin.getNextEntry()) != null) {
                    String base = e.getName().toLowerCase(Locale.ROOT);
                    base = base.substring(base.lastIndexOf('/') + 1);
                    if (!e.isDirectory() && isRomName(base)) copyNoClose(zin, new FileOutputStream(new File(romDir, base)));
                }
            } else if (isRomName(name)) {
                copyNoClose(in, new FileOutputStream(new File(romDir, name)));
            }
        } finally { try { in.close(); } catch (IOException ignored) {} }
    }

    private static boolean isRomName(String n) { for (String r : ROM_NAMES) if (r.equals(n)) return true; return false; }

    private String displayName(Uri u) {
        Cursor c = null;
        try {
            c = getContentResolver().query(u, null, null, null, null);
            if (c != null && c.moveToFirst()) { int i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME); if (i >= 0) return c.getString(i); }
        } catch (Exception ignored) {
        } finally { if (c != null) c.close(); }
        String p = u.getLastPathSegment();
        return p == null ? "" : p.substring(p.lastIndexOf('/') + 1);
    }

    // ---------------------------------------------------------------- game
    private void startGame() {
        if (!Native.init(romDir.getAbsolutePath(), settingsFile.getAbsolutePath())) {
            for (String n : ROM_NAMES) new File(romDir, n).delete();
            showImportScreen("The ROM files could not be loaded (wrong or damaged files?).");
            return;
        }
        game = new GameView(this);
        setContentView(game);
        hideSystemUi();
    }

    @Override protected void onPause() { super.onPause(); if (game != null) game.setPaused(true); }
    @Override protected void onResume() { super.onResume(); if (game != null) game.setPaused(false); hideSystemUi(); }
    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); if (f) hideSystemUi(); }

    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    // ---------------------------------------------------------------- small io helpers
    private static void copy(InputStream in, OutputStream out) throws IOException { try { copyNoClose(in, out); } finally { in.close(); } }
    private static void copyNoClose(InputStream in, OutputStream out) throws IOException {
        try { byte[] b = new byte[65536]; int n; while ((n = in.read(b)) > 0) out.write(b, 0, n); } finally { out.close(); }
    }
    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        try { byte[] b = new byte[8192]; int n; while ((n = in.read(b)) > 0) o.write(b, 0, n); } finally { in.close(); }
        return o.toByteArray();
    }
}
