package us.thumby.mote;

import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.View;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

import org.libsdl.app.SDLActivity;

/**
 * Mote for Android — the whole app is the native shell (platform/android). This
 * class only supplies the three things that have to come from Java:
 *
 *   · where game modules live, handed to main() as argv (the APK's native-library
 *     dir for the bundled gallery, then an external dir for side-loaded ones)
 *   · the vibrator, for the engine's rumble hook
 *   · an immersive, always-on landscape window
 */
public class MoteActivity extends SDLActivity {

    private static MoteActivity sInstance;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sInstance = this;
        MoteUsb.init(this);          /* before the native side starts asking */
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) goImmersive();
    }

    /** Hide the status and navigation bars: the chassis is the whole screen. */
    private void goImmersive() {
        View v = getWindow().getDecorView();
        v.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    /**
     * Module search path, highest priority first — read by
     * mote_android_os_add_dir() in argv order. The writable games dir comes FIRST
     * so a gallery install (or a side-loaded module) shadows the copy baked into
     * the APK, which is what makes updates possible; it is also where the gallery
     * writes.
     */
    @Override
    protected String[] getArguments() {
        File games = new File(getExternalFilesDir(null), "games");
        //noinspection ResultOfMethodCallIgnored
        games.mkdirs();
        return new String[] { games.getAbsolutePath(), getApplicationInfo().nativeLibraryDir };
    }

    /**
     * Blocking HTTPS GET to a file, for the gallery — the one thing the native
     * side can't do here (no TLS in C). Called from the gallery's worker thread;
     * returns 0 on success. Writes to a fresh file so a failed download can't be
     * mistaken for a good one.
     */
    public static int moteHttpGet(String url, String dest) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(30000);
            conn.setInstanceFollowRedirects(true);
            conn.setRequestProperty("User-Agent", "mote-android");
            int code = conn.getResponseCode();
            if (code / 100 != 2) return -1;
            try (InputStream in = conn.getInputStream();
                 FileOutputStream out = new FileOutputStream(dest)) {
                byte[] buf = new byte[16384];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                out.flush();
            }
            return 0;
        } catch (Exception e) {
            new File(dest).delete();
            return -1;
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    /** Called from the native rumble hook (any thread). ms<=0 stops nothing — it just returns. */
    public static void moteVibrate(int ms, int amplitude) {
        MoteActivity a = sInstance;
        if (a == null || ms <= 0) return;
        Vibrator v = (Vibrator) a.getSystemService(Context.VIBRATOR_SERVICE);
        if (v == null || !v.hasVibrator()) return;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                int amp = Math.max(1, Math.min(255, amplitude));
                v.vibrate(VibrationEffect.createOneShot(ms, amp));
            } else {
                v.vibrate(ms);
            }
        } catch (Exception ignored) {
            // A device that refuses to buzz is not a reason to lose the frame.
        }
    }
}
