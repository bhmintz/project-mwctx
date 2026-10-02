package com.nfsmw.android;

import android.os.Handler;
import android.os.Looper;

import java.io.File;

/**
 * "Preparar texturas": fills the native renderer's ETC2 cache from the game files before playing, so a Mali
 * (which cannot sample the game's BC textures) never decodes or transcodes a world texture while driving.
 *
 * The work is in libnfsmw_precarga.so (app/src/nfsmw_precarga_etc2.cpp): it reads every texture pack of
 * NFS/ZZDATA*.BIN, transcodes what the cache does not have with all cores and appends it to the same files the
 * game writes (files/nfsmw/user/cache/etc2). It also writes mundo.bin, the list of world textures: the texture
 * quality option lowers only those. It takes a few minutes the first time and only checks afterwards.
 */
final class TexturePrep {
    interface Listener {
        void onProgress(float fraction, String text);

        void onDone(String summary, boolean ok);
    }

    private static boolean loaded;

    private static native boolean nativeIniciar(String nfsFolder, String cacheFolder);

    private static native void nativeCancelar();

    private static native long[] nativeEstado();

    private final Handler main = new Handler(Looper.getMainLooper());
    private final Listener listener;
    private final long startMs = System.currentTimeMillis();

    private TexturePrep(Listener listener) {
        this.listener = listener;
    }

    static File cacheFolder(File filesDir) {
        return new File(new File(new File(new File(filesDir, "nfsmw"), "user"), "cache"), "etc2");
    }

    /** Starts it; null if the library cannot be loaded or it is already running. */
    static TexturePrep start(File gameRoot, File filesDir, Listener listener) {
        try {
            if (!loaded) {
                System.loadLibrary("nfsmw_precarga");
                loaded = true;
            }
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
        File cache = cacheFolder(filesDir);
        cache.mkdirs();
        if (!nativeIniciar(new File(gameRoot, "NFS").getAbsolutePath(), cache.getAbsolutePath())) {
            return null;
        }
        TexturePrep prep = new TexturePrep(listener);
        prep.main.post(prep::poll);
        return prep;
    }

    void cancel() {
        nativeCancelar();
    }

    private void poll() {
        long[] s = nativeEstado();
        long state = s[0];
        float fraction = s[2] > 0 ? Math.min(1f, (float) s[1] / s[2]) : 0f;
        long seconds = (System.currentTimeMillis() - startMs) / 1000;
        if (state == 1) {
            listener.onProgress(fraction, String.format(java.util.Locale.ROOT,
                    "Preparando texturas para la GPU (%d%%, %d:%02d) · %d nuevas, %d ya estaban",
                    Math.round(fraction * 100), seconds / 60, seconds % 60, s[4], s[5]));
            main.postDelayed(this::poll, 250);
            return;
        }
        String summary;
        boolean ok = state == 2 && s[8] == 0;
        if (state == 3) {
            summary = "No se pudieron leer los archivos del juego o la caché de texturas.";
        } else {
            summary = String.format(java.util.Locale.ROOT,
                    "%s: %d texturas nuevas, %d ya estaban, %d del mundo (las únicas que baja la calidad)%s",
                    state == 4 ? "Cancelado" : "Texturas listas", s[4], s[5], s[9],
                    s[8] > 0 ? String.format(java.util.Locale.ROOT,
                            " · ATENCIÓN: %d de %d comprobadas no coinciden", s[8], s[7]) : "");
        }
        listener.onDone(summary, ok);
    }
}
