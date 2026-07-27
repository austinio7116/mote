package us.thumby.mote;

import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.View;
import android.view.WindowManager;

import java.io.File;

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
     */
    @Override
    protected String[] getArguments() {
        String libDir = getApplicationInfo().nativeLibraryDir;
        File games = new File(getExternalFilesDir(null), "games");
        //noinspection ResultOfMethodCallIgnored
        games.mkdirs();
        return new String[] { libDir, games.getAbsolutePath() };
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
