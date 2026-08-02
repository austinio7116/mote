package us.thumby.cuevr;

import android.app.NativeActivity;

/**
 * CueVR — the whole game is native (games/cuevr). This class exists only to be
 * a NativeActivity that loads the OpenXR loader before anything asks for it.
 *
 * Unlike the Mote console app there is nothing else for Java to answer here:
 * no game modules to find, no storage to hand over, no HTTPS for a gallery.
 */
public class CueVrActivity extends NativeActivity {
    static {
        System.loadLibrary("openxr_loader");
    }
}
