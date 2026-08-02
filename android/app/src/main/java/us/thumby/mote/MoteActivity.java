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
     * mote_android_os_add_dir() in argv order.
     *
     * INTERNAL storage comes first, and it has to: it is the only writable place
     * a downloaded .so can actually be run from. getExternalFilesDir() looks
     * ideal — visible over USB, easy to drop files into — but that path is on
     * the emulated-SD FUSE mount, which is mounted noexec. A module installed
     * there writes perfectly and then fails at dlopen, every time, including
     * after a restart. So internal is where the gallery installs and where
     * modules load from, and it shadows the copies baked into the APK, which is
     * what makes an update mean anything.
     *
     * The external dir stays in the list as a drop zone for side-loading, since
     * it is the only one a file manager can reach. Anything found there that
     * will not load in place gets imported into internal — see scan() in
     * os/android/mote_android_os.c.
     */
    @Override
    protected String[] getArguments() {
        File internal = new File(getFilesDir(), "games");
        //noinspection ResultOfMethodCallIgnored
        internal.mkdirs();
        File external = new File(getExternalFilesDir(null), "games");
        //noinspection ResultOfMethodCallIgnored
        external.mkdirs();
        return new String[] { internal.getAbsolutePath(),
                              external.getAbsolutePath(),
                              getApplicationInfo().nativeLibraryDir };
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
            if (conn != null) conn.disconnect();   /* a broken one is not worth pooling */
            return -1;
        }
        /* Deliberately NOT disconnect() on success: that evicts the connection
         * from the keep-alive pool, and the gallery fetches a hundred small
         * files — a fresh TLS handshake each would dominate the time. */
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
