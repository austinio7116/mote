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
 * with bulkTransfer.
 *
 * Two details are load-bearing. DTR must be asserted (SET_CONTROL_LINE_STATE on
 * the COMM interface, not the data one): without it TinyUSB treats the port as
 * closed, and tud_cdc_write reports success while dropping the bytes — so the
 * handheld's replies vanish. And a read must ask for a whole packet: request
 * fewer than the endpoint's 64 bytes and the remainder of the packet is
 * discarded, so the native side always reads in 4 KB chunks.
 *
 * Every method here is called from the native dock thread, so this class is
 * deliberately free of any UI or main-thread work.
 */
public final class MoteUsb {

    private static final int VID = 0xCAFE;
    private static final int PID = 0x4D01;

    /** CDC class requests. */
    private static final int CDC_SET_LINE_CODING = 0x20;
    private static final int CDC_SET_CONTROL_LINE_STATE = 0x22;
    private static final int CDC_OUT_REQTYPE = 0x21;   // host->device, class, interface

    private static final String ACTION_PERMISSION = "us.thumby.mote.USB_PERMISSION";

    private static Context sCtx;
    private static UsbManager sMgr;

    private static UsbDeviceConnection sConn;
    private static UsbInterface sIface, sComm;
    private static UsbEndpoint sIn, sOut;
    private static volatile boolean sAsking;
    /* A connection can go stale without the device disappearing from the device
     * list — same VID/PID enumerates again after any re-plug or re-enumeration.
     * bulkTransfer then fails forever while find() still succeeds, so count the
     * consecutive failures and declare the pipe gone; the dock reopens. */
    private static int sFails;
    private static final int FAIL_LIMIT = 40;

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

        /* The bulk endpoints live on the CDC DATA interface (class 0x0A); the
         * COMM interface (class 0x02) carries only the notification endpoint but
         * is the one a line-state request must be addressed to. Find both. */
        UsbInterface pick = null, comm = null;
        UsbEndpoint in = null, out = null;
        for (int i = 0; i < dev.getInterfaceCount(); i++) {
            UsbInterface itf = dev.getInterface(i);
            if (itf.getInterfaceClass() == UsbConstants.USB_CLASS_COMM && comm == null)
                comm = itf;
            UsbEndpoint bin = null, bout = null;
            for (int e = 0; e < itf.getEndpointCount(); e++) {
                UsbEndpoint ep = itf.getEndpoint(e);
                if (ep.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) continue;
                if (ep.getDirection() == UsbConstants.USB_DIR_IN) bin = ep;
                else bout = ep;
            }
            if (pick == null && bin != null && bout != null) { pick = itf; in = bin; out = bout; }
        }
        if (pick == null) return 0;

        UsbDeviceConnection conn = sMgr.openDevice(dev);
        if (conn == null) return 0;
        if (!conn.claimInterface(pick, true)) { conn.close(); return 0; }
        /* Claiming the comm interface too keeps anything else off the port. */
        if (comm != null) conn.claimInterface(comm, true);

        /* Baud is meaningless over USB but TinyUSB still expects a coherent line
         * coding, and DTR must be asserted or the device treats the port as
         * closed and says nothing on its log channel. wIndex is the COMM
         * interface, not the data one — addressing the data interface is a
         * silent no-op, which is why this had to be got right. */
        int ifidx = comm != null ? comm.getId() : 0;
        byte[] coding = { (byte) 0x00, (byte) 0xC2, (byte) 0x01, 0, /* 115200 */
                          0 /* 1 stop */, 0 /* no parity */, 8 /* data bits */ };
        conn.controlTransfer(CDC_OUT_REQTYPE, CDC_SET_LINE_CODING, 0, ifidx,
                             coding, coding.length, 300);
        conn.controlTransfer(CDC_OUT_REQTYPE, CDC_SET_CONTROL_LINE_STATE,
                             0x03, ifidx, null, 0, 300);

        sConn = conn; sIface = pick; sComm = comm; sIn = in; sOut = out;
        return 1;
    }

    public static synchronized void moteClose() {
        if (sConn != null) {
            try {
                if (sIface != null) sConn.releaseInterface(sIface);
                if (sComm != null) sConn.releaseInterface(sComm);
                sConn.close();
            } catch (Exception ignored) { }
        }
        sConn = null; sIface = null; sComm = null; sIn = null; sOut = null;
        sFails = 0;
    }

    /**
     * Read up to max bytes (never more than one transfer's worth). Returns the
     * count, 0 on timeout, -1 if the pipe is gone. A CDC read timing out is
     * normal: the device only speaks when it has something to say.
     *
     * max must be at least the endpoint packet size, or the tail of a packet is
     * discarded by the USB stack — the caller guarantees that.
     */
    public static int moteRead(byte[] buf, int max, int timeoutMs) {
        UsbDeviceConnection c = sConn;
        UsbEndpoint ep = sIn;
        if (c == null || ep == null) return -1;
        try {
            if (max > buf.length) max = buf.length;
            int n = c.bulkTransfer(ep, buf, max, timeoutMs);
            if (n < 0) {
                if (find() == null) return -1;
                return ++sFails >= FAIL_LIMIT ? -1 : 0;  /* timeout, or gone stale */
            }
            sFails = 0;
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
            int n = c.bulkTransfer(ep, buf, len, 3000);
            if (n < 0) return -1;      /* a partial write would desync the stream */
            sFails = 0;
            return n;
        } catch (Exception e) {
            return -1;
        }
    }
}
