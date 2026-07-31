package us.thumby.mote;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;

import java.util.HashMap;

/**
 * USB-host transport to a docked Thumby Color, for the native dock service
 * (os/android/mote_android_dock.c).
 *
 * The handheld is a plain CDC-ACM device (VID:PID CAFE:4D01) with 64-byte bulk
 * endpoints, so no driver is involved: claim the data interface and move bytes
 * with bulkTransfer. The only control request that matters is
 * SET_CONTROL_LINE_STATE with DTR asserted — TinyUSB's tud_cdc_connected() gates
 * the device's log path on it.
 *
 * Every method here is called from the native dock thread, so this class is
 * deliberately free of any UI or main-thread work.
 */
public final class MoteUsb {

    private static final int VID = 0xCAFE;
    private static final int PID = 0x4D01;

    /** CDC: SET_CONTROL_LINE_STATE, DTR|RTS. */
    private static final int CDC_SET_CONTROL_LINE_STATE = 0x22;
    private static final int CDC_OUT_REQTYPE = 0x21;   // host->device, class, interface

    private static final String ACTION_PERMISSION = "us.thumby.mote.USB_PERMISSION";

    private static Context sCtx;
    private static UsbManager sMgr;

    private static UsbDeviceConnection sConn;
    private static UsbInterface sIface;
    private static UsbEndpoint sIn, sOut;
    private static volatile boolean sAsking;

    private MoteUsb() { }

    /** Called once from the activity. */
    static void init(Context ctx) {
        sCtx = ctx.getApplicationContext();
        sMgr = (UsbManager) sCtx.getSystemService(Context.USB_SERVICE);
        IntentFilter f = new IntentFilter(ACTION_PERMISSION);
        if (Build.VERSION.SDK_INT >= 33) {
            sCtx.registerReceiver(sReceiver, f, Context.RECEIVER_NOT_EXPORTED);
        } else {
            sCtx.registerReceiver(sReceiver, f);
        }
    }

    private static final BroadcastReceiver sReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent i) { sAsking = false; }
    };

    private static UsbDevice find() {
        if (sMgr == null) return null;
        HashMap<String, UsbDevice> all = sMgr.getDeviceList();
        if (all == null) return null;
        for (UsbDevice d : all.values())
            if (d.getVendorId() == VID && d.getProductId() == PID) return d;
        return null;
    }

    /** 1 when a Mote handheld is plugged in (whether or not we may open it yet). */
    public static int motePresent() {
        return find() != null ? 1 : 0;
    }

    /**
     * Open the CDC data interface. Returns 1 on success. If permission has not
     * been granted this fires the request and returns 0 — the dock thread simply
     * retries, so the user can grant it whenever the dialog appears. Plugging the
     * device in normally grants it automatically via the manifest's
     * USB_DEVICE_ATTACHED filter.
     */
    public static synchronized int moteOpen() {
        if (sConn != null) return 1;
        UsbDevice dev = find();
        if (dev == null) return 0;

        if (!sMgr.hasPermission(dev)) {
            if (!sAsking) {
                sAsking = true;
                int flags = Build.VERSION.SDK_INT >= 31
                        ? PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_UPDATE_CURRENT
                        : PendingIntent.FLAG_UPDATE_CURRENT;
                try {
                    sMgr.requestPermission(dev, PendingIntent.getBroadcast(
                            sCtx, 0, new Intent(ACTION_PERMISSION).setPackage(sCtx.getPackageName()),
                            flags));
                } catch (Exception e) {
                    sAsking = false;
                }
            }
            return 0;
        }

        /* Prefer the interface that actually carries the bulk endpoints: on a CDC
         * device that is the DATA interface (class 0x0A), not the notification
         * one. Fall back to any interface with a bulk pair. */
        UsbInterface pick = null;
        UsbEndpoint in = null, out = null;
        for (int i = 0; i < dev.getInterfaceCount() && pick == null; i++) {
            UsbInterface itf = dev.getInterface(i);
            UsbEndpoint bin = null, bout = null;
            for (int e = 0; e < itf.getEndpointCount(); e++) {
                UsbEndpoint ep = itf.getEndpoint(e);
                if (ep.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) continue;
                if (ep.getDirection() == UsbConstants.USB_DIR_IN) bin = ep;
                else bout = ep;
            }
            if (bin != null && bout != null) { pick = itf; in = bin; out = bout; }
        }
        if (pick == null) return 0;

        UsbDeviceConnection conn = sMgr.openDevice(dev);
        if (conn == null) return 0;
        if (!conn.claimInterface(pick, true)) { conn.close(); return 0; }

        /* DTR|RTS on. Without it the device considers the port closed and stays
         * quiet on its log channel. Failure is not fatal for bulk traffic. */
        conn.controlTransfer(CDC_OUT_REQTYPE, CDC_SET_CONTROL_LINE_STATE,
                             0x03, pick.getId(), null, 0, 200);

        sConn = conn; sIface = pick; sIn = in; sOut = out;
        return 1;
    }

    public static synchronized void moteClose() {
        if (sConn != null) {
            try {
                if (sIface != null) sConn.releaseInterface(sIface);
                sConn.close();
            } catch (Exception ignored) { }
        }
        sConn = null; sIface = null; sIn = null; sOut = null;
    }

    /**
     * Read up to buf.length bytes. Returns the count, 0 on timeout, -1 if the
     * pipe is gone. A CDC read timing out is normal: the device only speaks when
     * it has something to say.
     */
    public static int moteRead(byte[] buf, int timeoutMs) {
        UsbDeviceConnection c = sConn;
        UsbEndpoint ep = sIn;
        if (c == null || ep == null) return -1;
        try {
            int n = c.bulkTransfer(ep, buf, buf.length, timeoutMs);
            if (n < 0) return find() == null ? -1 : 0;   /* timeout vs unplugged */
            return n;
        } catch (Exception e) {
            return -1;
        }
    }

    /** Write len bytes. Returns the count written, or -1 if the pipe is gone. */
    public static int moteWrite(byte[] buf, int len) {
        UsbDeviceConnection c = sConn;
        UsbEndpoint ep = sOut;
        if (c == null || ep == null) return -1;
        try {
            /* One bulk transfer per endpoint packet run; the endpoint is 64 bytes
             * but bulkTransfer handles the splitting itself. */
            int n = c.bulkTransfer(ep, buf, len, 2000);
            if (n < 0) return find() == null ? -1 : 0;
            return n;
        } catch (Exception e) {
            return -1;
        }
    }
}
