package com.pippy360.mierrorsedgere;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * The game's activity: SDL's, with the native library named and the command line supplied.
 *
 * The command line (the same options as the desktop builds: --chapter, --max-frames,
 * --exit-screenshot, --game-root, ...) comes from, in order:
 *   1. the intent's string extra "args" (the whole command as one adb shell string, or the
 *      device's shell splits the extra's words again):
 *        adb shell 'am start -n com.pippy360.mierrorsedgere/.MirrorsEdgeActivity \
 *            --es args "--chapter 0 --max-frames 600"'
 *   2. the file <external files dir>/me_args.txt
 *        (/sdcard/Android/data/com.pippy360.mierrorsedgere/files/me_args.txt), one command line;
 *   3. nothing.
 * It is split at whitespace; double quotes group words ("a b" is one argument).
 */
public class MirrorsEdgeActivity extends SDLActivity {
    private static final String TAG = "mirrorsedge";

    @Override
    protected String[] getLibraries() {
        // libSDL2.so first (SDLActivity needs its JNI side before anything else), then the game.
        // OpenAL Soft, ogg, vorbis and the renderer are linked into libmain.so.
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        String line = null;
        Intent intent = getIntent();
        if (intent != null) {
            line = intent.getStringExtra("args");
            if (line != null) Log.i(TAG, "arguments from intent: " + line);
        }
        if (line == null) {
            File dir = getExternalFilesDir(null);
            if (dir != null) {
                File f = new File(dir, "me_args.txt");
                if (f.isFile()) {
                    try (BufferedReader r = new BufferedReader(new FileReader(f))) {
                        StringBuilder sb = new StringBuilder();
                        String l;
                        while ((l = r.readLine()) != null) {
                            l = l.trim();
                            if (l.isEmpty() || l.startsWith("#")) continue;
                            if (sb.length() > 0) sb.append(' ');
                            sb.append(l);
                        }
                        line = sb.toString();
                        Log.i(TAG, "arguments from " + f + ": " + line);
                    } catch (IOException e) {
                        Log.w(TAG, "cannot read " + f + ": " + e);
                    }
                }
            }
        }
        if (line == null || line.trim().isEmpty()) return new String[0];
        List<String> out = splitCommandLine(line);
        return out.toArray(new String[0]);
    }

    /** Splits at whitespace; "double quotes" and 'single quotes' group words; backslash escapes. */
    static List<String> splitCommandLine(String line) {
        List<String> out = new ArrayList<>();
        StringBuilder cur = new StringBuilder();
        boolean inWord = false;
        char quote = 0;
        for (int i = 0; i < line.length(); ++i) {
            char c = line.charAt(i);
            if (quote != 0) {
                if (c == quote) {
                    quote = 0;
                } else if (c == '\\' && i + 1 < line.length() && quote == '"') {
                    cur.append(line.charAt(++i));
                } else {
                    cur.append(c);
                }
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                inWord = true;
            } else if (c == '\\' && i + 1 < line.length()) {
                cur.append(line.charAt(++i));
                inWord = true;
            } else if (Character.isWhitespace(c)) {
                if (inWord) {
                    out.add(cur.toString());
                    cur.setLength(0);
                    inWord = false;
                }
            } else {
                cur.append(c);
                inWord = true;
            }
        }
        if (inWord) out.add(cur.toString());
        return out;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // The external files directory exists from the first launch on, so `adb push` has a target
        // (adb cannot create Android/data/<package>/ itself on every device).
        File dir = getExternalFilesDir(null);
        if (dir != null) {
            File root = new File(dir, "mirrorsedge");
            if (!root.isDirectory() && !root.mkdirs()) Log.w(TAG, "cannot create " + root);
            File shots = new File(dir, "screenshots");
            if (!shots.isDirectory() && !shots.mkdirs()) Log.w(TAG, "cannot create " + shots);
            Log.i(TAG, "external files dir: " + dir);
        }
        hideSystemUi();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    /** Immersive sticky full screen: the bars come back with a swipe and go away again. */
    private void hideSystemUi() {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
              | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
              | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
              | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
              | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
              | View.SYSTEM_UI_FLAG_FULLSCREEN);
    }
}
