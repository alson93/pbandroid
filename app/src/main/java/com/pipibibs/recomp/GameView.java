package com.pipibibs.recomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * Draws the game (320x240, kept 4:3) and the touch controls, runs the audio thread that paces the sound board.
 *
 * Controls: stick (left), SHOOT = button 1, A = button 2, CREDIT = coin, START = start (all held buttons, multi-touch).
 * Top right: the menu button opens a row with TXT (edit settings), LOAD, SAVE, P2 and P1 (which player the touch controls play).
 */
public class GameView extends SurfaceView implements SurfaceHolder.Callback {
    private static final int W = 320, H = 240;
    private static final int R_NONE = -1, R_STICK = 0, R_A = 1, R_SHOOT = 2, R_CREDIT = 3, R_START = 4;

    private final MainActivity act;
    private final float dp;
    private final Bitmap fb = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888);
    private final int[] px = new int[W * H];
    private final Paint gamePaint = new Paint();                         // no filtering: crisp pixels
    private final Paint ui = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Bitmap bBase, bKnob, bA, bShoot, bCredit, bStart, bMenu, bP1, bP2, bSave, bLoad, bTxt;

    private volatile boolean running = true, paused = false;
    private Thread renderThread, audioThread;

    // layout (pixels), computed in onSizeChanged
    private float stickX, stickY, stickR, aX, aY, shootX, shootY, btnR, creditX, creditY, startX, startY, smallR, menuX, menuY;
    private boolean panelOpen = false;
    private volatile boolean p2mode;

    // touch state
    private final int[] role = new int[32];
    private float knobDx, knobDy;                       // stick offset in pixels (clamped), for drawing and direction bits
    private boolean aDown, shootDown, creditDown, startDown;

    private String flash = null; private long flashUntil = 0;
    private String info = ""; private long infoAt = 0; private long audioFrames = 0;

    public GameView(MainActivity a) {
        super(a);
        act = a;
        dp = a.getResources().getDisplayMetrics().density;
        java.util.Arrays.fill(role, R_NONE);
        text.setColor(Color.WHITE); text.setTextAlign(Paint.Align.CENTER); text.setShadowLayer(3, 0, 0, Color.BLACK);
        BitmapFactory.Options o = new BitmapFactory.Options(); o.inScaled = false;
        bBase = load(R.drawable.joystickback, o); bKnob = load(R.drawable.joystick, o); bA = load(R.drawable.abutton, o);
        bShoot = load(R.drawable.shoot, o); bCredit = load(R.drawable.credit, o); bStart = load(R.drawable.start, o);
        bMenu = load(R.drawable.menu, o); bP1 = load(R.drawable.p1, o); bP2 = load(R.drawable.p2, o);
        bSave = load(R.drawable.save, o); bLoad = load(R.drawable.load, o); bTxt = load(R.drawable.txt, o);
        p2mode = Native.swapSetting();
        getHolder().addCallback(this);
        setFocusable(true);
        startAudio();
    }

    private Bitmap load(int id, BitmapFactory.Options o) { return BitmapFactory.decodeResource(getResources(), id, o); }

    void setPaused(boolean p) { paused = p; }

    // ------------------------------------------------------------------ threads
    @Override public void surfaceCreated(SurfaceHolder h) {
        running = true;
        renderThread = new Thread(new Runnable() { @Override public void run() { renderLoop(); } }, "render");
        renderThread.start();
    }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {}
    @Override public void surfaceDestroyed(SurfaceHolder h) {
        running = false;
        try { if (renderThread != null) renderThread.join(500); } catch (InterruptedException ignored) {}
    }

    private void startAudio() {
        audioThread = new Thread(new Runnable() {
            @Override public void run() {
                final int rate = 48000;
                int min = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
                AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT,
                        Math.max(min, 8192 * 2), AudioTrack.MODE_STREAM);
                short[] buf = new short[512 * 2];
                boolean playing = false;
                while (true) {
                    if (paused) { if (playing) { t.pause(); playing = false; } SystemClock.sleep(20); continue; }
                    if (!playing) { t.play(); playing = true; }
                    Native.audio(buf, 512);          // the sound board runs exactly as fast as the audio device consumes samples
                    t.write(buf, 0, buf.length);
                    audioFrames += 512;
                }
            }
        }, "audio");
        audioThread.setPriority(Thread.MAX_PRIORITY);
        audioThread.setDaemon(true);
        audioThread.start();
    }

    private void renderLoop() {
        long last = -1, lastDraw = 0;
        final SurfaceHolder holder = getHolder();
        final RectF dst = new RectF();
        while (running) {
            if (paused) { SystemClock.sleep(30); continue; }
            long fc = Native.frameCount(), now = SystemClock.uptimeMillis();
            String m = Native.message();
            if (m != null) { flash = m; flashUntil = now + 2500; }
            if (!((fc != last && now - lastDraw >= 16) || now - lastDraw > 50)) { SystemClock.sleep(1); continue; }   // at most ~60 draws a second, whatever the game speed
            last = fc; lastDraw = now;
            Native.render(px);
            fb.setPixels(px, 0, W, 0, 0, W, H);
            Canvas c = holder.lockCanvas();
            if (c == null) { SystemClock.sleep(10); continue; }
            try {
                c.drawColor(Color.BLACK);
                float vw = getWidth(), vh = getHeight(), s = Math.min(vw / W, vh / H);
                dst.set((vw - W * s) / 2, (vh - H * s) / 2, (vw + W * s) / 2, (vh + H * s) / 2);
                c.drawBitmap(fb, null, dst, gamePaint);
                drawControls(c);
                if (now - infoAt > 500) { infoAt = now; info = Native.status() + "  audio=" + (audioFrames / 48000) + "s"; }
                text.setTextSize(9 * dp); text.setTextAlign(Paint.Align.LEFT); c.drawText(info, 6 * dp, 12 * dp, text); text.setTextAlign(Paint.Align.CENTER);
                if (flash != null && now < flashUntil) { text.setTextSize(14 * dp); c.drawText(flash, vw / 2, vh * 0.12f, text); }
            } finally { holder.unlockCanvasAndPost(c); }
        }
    }

    // ------------------------------------------------------------------ drawing
    private void drawBmp(Canvas c, Bitmap b, float cx, float cy, float r, int alpha) {
        ui.setAlpha(alpha);
        c.drawBitmap(b, null, new Rect((int) (cx - r), (int) (cy - r), (int) (cx + r), (int) (cy + r)), ui);
    }

    private void drawControls(Canvas c) {
        drawBmp(c, bBase, stickX, stickY, stickR, 150);
        drawBmp(c, bKnob, stickX + knobDx, stickY + knobDy, stickR * 0.5f, 220);
        drawBmp(c, bA, aX, aY, btnR, aDown ? 255 : 170);
        drawBmp(c, bShoot, shootX, shootY, btnR, shootDown ? 255 : 170);
        drawBmp(c, bCredit, creditX, creditY, smallR * 1.15f, creditDown ? 255 : 170);
        drawBmp(c, bStart, startX, startY, smallR * 1.15f, startDown ? 255 : 170);
        drawBmp(c, bMenu, menuX, menuY, smallR, panelOpen ? 255 : 150);
        if (panelOpen) {
            drawBmp(c, bP1, panelX(0), menuY, smallR, p2mode ? 110 : 255);
            drawBmp(c, bP2, panelX(1), menuY, smallR, p2mode ? 255 : 110);
            drawBmp(c, bSave, panelX(2), menuY, smallR, 220);
            drawBmp(c, bLoad, panelX(3), menuY, smallR, 220);
            drawBmp(c, bTxt, panelX(4), menuY, smallR, 220);
        }
    }

    private float panelX(int i) { return menuX - (i + 1) * smallR * 2.5f; }

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
        stickR = 62 * dp; stickX = stickR * 1.25f + 8 * dp; stickY = h - stickR * 1.25f - 8 * dp;
        btnR = 36 * dp; smallR = 22 * dp;
        aX = w - btnR * 1.4f - 8 * dp;   aY = h - btnR * 3.0f - 8 * dp;       // A: upper right
        shootX = w - btnR * 3.7f - 8 * dp; shootY = h - btnR * 1.4f - 8 * dp; // SHOOT: lower, left of A
        creditX = w / 2f - smallR * 2.2f; creditY = h - smallR * 1.5f;
        startX = w / 2f + smallR * 2.2f;  startY = h - smallR * 1.5f;
        menuX = w - smallR * 1.5f; menuY = smallR * 1.5f;
    }

    // ------------------------------------------------------------------ touch
    private static boolean in(float x, float y, float cx, float cy, float r) { float dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= r * r; }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            int i = e.getActionIndex(), id = e.getPointerId(i);
            if (id < role.length) down(id, e.getX(i), e.getY(i));
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            int id = e.getPointerId(e.getActionIndex());
            if (id < role.length) role[id] = R_NONE;
        } else if (action == MotionEvent.ACTION_CANCEL) {
            java.util.Arrays.fill(role, R_NONE);
        }
        // stick follows its finger on every event
        for (int i = 0; i < e.getPointerCount(); ++i) {
            int id = e.getPointerId(i);
            if (id < role.length && role[id] == R_STICK) {
                float dx = e.getX(i) - stickX, dy = e.getY(i) - stickY, d = (float) Math.sqrt(dx * dx + dy * dy), lim = stickR * 0.7f;
                if (d > lim) { dx *= lim / d; dy *= lim / d; }
                knobDx = dx; knobDy = dy;
            }
        }
        recompute();
        return true;
    }

    private void down(int id, float x, float y) {
        if (in(x, y, menuX, menuY, smallR * 1.3f)) { panelOpen = !panelOpen; return; }
        if (panelOpen) {
            for (int k = 0; k < 5; ++k) if (in(x, y, panelX(k), menuY, smallR * 1.3f)) { panelButton(k); return; }
        }
        if (in(x, y, stickX, stickY, stickR * 1.5f)) { role[id] = R_STICK; return; }
        if (in(x, y, aX, aY, btnR * 1.3f)) { role[id] = R_A; return; }
        if (in(x, y, shootX, shootY, btnR * 1.3f)) { role[id] = R_SHOOT; return; }
        if (in(x, y, creditX, creditY, smallR * 1.5f)) { role[id] = R_CREDIT; return; }
        if (in(x, y, startX, startY, smallR * 1.5f)) { role[id] = R_START; return; }
    }

    private void panelButton(int k) {
        if (k == 0) p2mode = false;                      // P1: the touch controls play player 1
        else if (k == 1) p2mode = true;                  // P2: ... player 2 (coin / start become coin 2 / start 2)
        else if (k == 2) Native.requestSave();
        else if (k == 3) Native.requestLoad();
        else if (k == 4) act.runOnUiThread(new Runnable() { @Override public void run() { act.openSettingsEditor(); } });
    }

    private void recompute() {
        boolean a = false, sh = false, cr = false, st = false, stick = false;
        for (int id = 0; id < role.length; ++id) {
            int r = role[id];
            if (r == R_A) a = true; else if (r == R_SHOOT) sh = true; else if (r == R_CREDIT) cr = true; else if (r == R_START) st = true; else if (r == R_STICK) stick = true;
        }
        if (!stick) { knobDx = 0; knobDy = 0; }
        aDown = a; shootDown = sh; creditDown = cr; startDown = st;
        int dir = 0, th = Math.max(1, (int) (stickR * 0.22f));
        if (knobDy < -th) dir |= 0x01;
        if (knobDy > th)  dir |= 0x02;
        if (knobDx < -th) dir |= 0x04;
        if (knobDx > th)  dir |= 0x08;
        int pad = dir | (sh ? 0x10 : 0) | (a ? 0x20 : 0);                // button 1 = SHOOT, button 2 = A
        int p1 = 0, p2 = 0, sys = 0x18;                                    // coin lines idle high (active low)
        if (!p2mode) { p1 = pad; if (cr) sys &= ~0x08; if (st) sys |= 0x20; }
        else         { p2 = pad; if (cr) sys &= ~0x10; if (st) sys |= 0x40; }
        Native.input(p1, p2, sys);
    }
}
