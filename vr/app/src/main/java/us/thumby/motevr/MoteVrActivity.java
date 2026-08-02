package us.thumby.motevr;

import android.app.NativeActivity;
import android.os.Bundle;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Mote VR — the whole app is native (platform/vr). This class exists only to be
 * a NativeActivity that also answers the handful of questions C cannot ask
 * Android directly: where game modules may live, and how to fetch a file over
 * HTTPS for the gallery.
 *
 * It deliberately mirrors us.thumby.mote.MoteActivity rather than inventing its
 * own answers — the storage rules are subtle (see moteInternalGames) and there
 * is no reason for the headset build to learn them differently.
 */
public class MoteVrActivity extends NativeActivity {

    static {
        // Before any native code asks for it. The Khronos loader is a plain
        // shared library in the APK; the runtime it finds is the headset's.
        System.loadLibrary("openxr_loader");
    }

    private static MoteVrActivity sInstance;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sInstance = this;
        super.onCreate(savedInstanceState);
    }

    /**
     * Where the gallery installs and where modules load from.
     *
     * Internal, and it has to be: getExternalFilesDir() looks ideal — visible
     * over USB, easy to drop files into — but that path is on the emulated-SD
     * FUSE mount, which is mounted noexec. A module installed there writes
     * perfectly and then fails at dlopen, every time. So internal is home, and
     * it shadows whatever shipped in the APK, which is what makes an update
     * mean anything.
     */
    public static String moteInternalGames() {
        MoteVrActivity a = sInstance;
        if (a == null) return "";
        File f = new File(a.getFilesDir(), "games");
        //noinspection ResultOfMethodCallIgnored
        f.mkdirs();
        return f.getAbsolutePath();
    }

    /** The side-loading drop zone: the only games dir a file manager can reach. */
    public static String moteExternalGames() {
        MoteVrActivity a = sInstance;
        if (a == null) return "";
        File f = new File(a.getExternalFilesDir(null), "games");
        //noinspection ResultOfMethodCallIgnored
        f.mkdirs();
        return f.getAbsolutePath();
    }

    /** Games bundled with the build. */
    public static String moteNativeLibDir() {
        MoteVrActivity a = sInstance;
        return a == null ? "" : a.getApplicationInfo().nativeLibraryDir;
    }

    /**
     * Blocking HTTPS GET to a file, for the gallery — the one thing the native
     * side cannot do here (no TLS in C). Called from the gallery's worker
     * thread; returns 0 on success.
     */
    public static int moteHttpGet(String url, String dest) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(30000);
            conn.setInstanceFollowRedirects(true);
            conn.setRequestProperty("User-Agent", "mote-vr");
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
            //noinspection ResultOfMethodCallIgnored
            new File(dest).delete();
            if (conn != null) conn.disconnect();   /* a broken one is not worth pooling */
            return -1;
        }
        /* Deliberately NOT disconnect() on success: that evicts the connection
         * from the keep-alive pool, and the gallery fetches a hundred small
         * files — a fresh TLS handshake each would dominate the time. */
    }
}
