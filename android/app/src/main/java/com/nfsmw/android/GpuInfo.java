package com.nfsmw.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.util.Log;

import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Detects the phone's GPU so the launcher can pick lighter defaults on weak GPUs.
 *
 * The renderer runs on Vulkan, but the only name the launcher can read cheaply, before the game starts and
 * in any process, is the OpenGL ES {@code GL_RENDERER} string (for example "Mali-G52 MC2" or "Adreno (TM)
 * 830"). It names the same GPU as Vulkan, which is all the tier needs. The string is read once through a
 * short-lived off-screen EGL context and cached in {@link #PREFS}; later launches read the cache.
 *
 * The tier is only ever used to LOWER defaults on GPUs known to struggle with this game (budget Mali such as
 * the Helio G80's Mali-G52, old Mali, Adreno below 600, PowerVR). Everything else -- including the Adreno 830
 * and Xclipse the port was tested on -- is {@link #TIER_FULL} and keeps today's defaults unchanged. Anything
 * unknown or any detection failure also stays {@link #TIER_FULL}, so a wrong guess never degrades a phone it
 * was not meant to.
 */
final class GpuInfo {
    private static final String TAG = "nfsmw.gpu";
    private static final String PREFS = "nfsmw_gpu";
    private static final String KEY_RENDERER = "renderer";

    /** Capable GPU: keep the launcher's normal defaults. */
    static final int TIER_FULL = 0;
    /** Weak GPU (budget/old Mali, old Adreno, PowerVR): use the lighter defaults. */
    static final int TIER_LOW = 1;

    private static String cachedRenderer;   // "" means detected-but-empty; null means not yet detected
    private static int cachedTier = -1;

    private GpuInfo() {
    }

    /** The GPU name ("Mali-G52 MC2", ...), or "" if it could not be read. Cached across launches. */
    static synchronized String renderer(Context context) {
        if (cachedRenderer != null) {
            return cachedRenderer;
        }
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String stored = prefs.getString(KEY_RENDERER, null);
        if (stored != null) {
            cachedRenderer = stored;
            return cachedRenderer;
        }
        String detected = queryRenderer();
        cachedRenderer = detected != null ? detected : "";
        prefs.edit().putString(KEY_RENDERER, cachedRenderer).apply();
        Log.i(TAG, "GPU detectada: " + (cachedRenderer.isEmpty() ? "(desconocida)" : cachedRenderer));
        return cachedRenderer;
    }

    /** {@link #TIER_FULL} or {@link #TIER_LOW}. */
    static synchronized int tier(Context context) {
        if (cachedTier < 0) {
            cachedTier = classify(renderer(context));
        }
        return cachedTier;
    }

    static boolean isLowEnd(Context context) {
        return tier(context) == TIER_LOW;
    }

    /** A short label for the launcher, e.g. "Mali-G52 MC2". Empty string if unknown. */
    static String label(Context context) {
        return renderer(context);
    }

    // Visible for the classification rules; package-private so a future test could call it directly.
    static int classify(String rendererName) {
        if (rendererName == null || rendererName.isEmpty()) {
            return TIER_FULL;  // unknown: never lower settings on a guess
        }
        String r = rendererName.toLowerCase(Locale.US);

        if (r.contains("mali")) {
            // Valhall/Bifrost "Mali-G<number>": the small models (G31/G51/G52/G57) are the budget tier.
            Matcher m = Pattern.compile("mali-g(\\d+)").matcher(r);
            if (m.find()) {
                int model = parseInt(m.group(1), 0);
                return (model == 31 || model == 51 || model == 52 || model == 57) ? TIER_LOW : TIER_FULL;
            }
            // Older Midgard/Utgard families (Mali-T###, Mali-4##/6##/8##) are all weaker than any Mali-G.
            if (r.matches(".*mali-(t\\d|[4568]\\d).*")) {
                return TIER_LOW;
            }
            return TIER_FULL;
        }

        if (r.contains("adreno")) {
            Matcher m = Pattern.compile("adreno[^0-9]*(\\d+)").matcher(r);
            if (m.find()) {
                int model = parseInt(m.group(1), 0);
                return model > 0 && model < 600 ? TIER_LOW : TIER_FULL;
            }
            return TIER_FULL;
        }

        if (r.contains("powervr")) {
            return TIER_LOW;  // budget MediaTek/older parts
        }

        return TIER_FULL;
    }

    private static int parseInt(String s, int fallback) {
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    /**
     * Brings up a 1x1 pbuffer EGL context just long enough to read GL_RENDERER, then tears it all down.
     * Returns null on any failure; the caller treats that as an unknown (capable) GPU.
     */
    private static String queryRenderer() {
        EGLDisplay display = EGL14.EGL_NO_DISPLAY;
        EGLContext context = EGL14.EGL_NO_CONTEXT;
        EGLSurface surface = EGL14.EGL_NO_SURFACE;
        try {
            display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            if (display == EGL14.EGL_NO_DISPLAY) {
                return null;
            }
            int[] version = new int[2];
            if (!EGL14.eglInitialize(display, version, 0, version, 1)) {
                display = EGL14.EGL_NO_DISPLAY;
                return null;
            }
            int[] cfgAttribs = {
                    EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                    EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                    EGL14.EGL_RED_SIZE, 8,
                    EGL14.EGL_GREEN_SIZE, 8,
                    EGL14.EGL_BLUE_SIZE, 8,
                    EGL14.EGL_NONE
            };
            EGLConfig[] configs = new EGLConfig[1];
            int[] numConfigs = new int[1];
            if (!EGL14.eglChooseConfig(display, cfgAttribs, 0, configs, 0, 1, numConfigs, 0)
                    || numConfigs[0] == 0 || configs[0] == null) {
                return null;
            }
            int[] ctxAttribs = {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE};
            context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT, ctxAttribs, 0);
            if (context == EGL14.EGL_NO_CONTEXT) {
                return null;
            }
            int[] surfAttribs = {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE};
            surface = EGL14.eglCreatePbufferSurface(display, configs[0], surfAttribs, 0);
            if (surface == EGL14.EGL_NO_SURFACE) {
                return null;
            }
            if (!EGL14.eglMakeCurrent(display, surface, surface, context)) {
                return null;
            }
            String renderer = GLES20.glGetString(GLES20.GL_RENDERER);
            return renderer != null ? renderer.trim() : null;
        } catch (RuntimeException e) {
            Log.w(TAG, "no se pudo leer GL_RENDERER", e);
            return null;
        } finally {
            if (display != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE,
                        EGL14.EGL_NO_CONTEXT);
                if (surface != EGL14.EGL_NO_SURFACE) {
                    EGL14.eglDestroySurface(display, surface);
                }
                if (context != EGL14.EGL_NO_CONTEXT) {
                    EGL14.eglDestroyContext(display, context);
                }
                EGL14.eglTerminate(display);
            }
        }
    }
}
