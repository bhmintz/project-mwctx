package com.nfsmw.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.List;

/**
 * The graphics options of the launcher. Each one is a game cvar: GameActivity passes them on the command
 * line, which wins over nfsmw.toml. Values must be ones the cvar allows (nfsmw_ajustes_graficos.cpp and
 * friends), or the game ignores them.
 */
final class GameOptions {
    static final class Option {
        final String key;
        final String title;
        final String cvar;
        final String[] values;
        final String[] labels;
        final String defaultValue;
        /** Default used instead of {@link #defaultValue} on a weak GPU, or null to keep the normal one. */
        final String lowEndDefault;

        Option(String key, String title, String cvar, String defaultValue, String[] values, String[] labels) {
            this(key, title, cvar, defaultValue, null, values, labels);
        }

        Option(String key, String title, String cvar, String defaultValue, String lowEndDefault,
               String[] values, String[] labels) {
            this.key = key;
            this.title = title;
            this.cvar = cvar;
            this.defaultValue = defaultValue;
            this.lowEndDefault = lowEndDefault;
            this.values = values;
            this.labels = labels;
        }

        String label(String value) {
            for (int i = 0; i < values.length; i++) {
                if (values[i].equals(value)) {
                    return labels[i];
                }
            }
            return value;
        }
    }

    static final String RESOLUTION = "resolution";
    static final String FPS = "fps";

    // The fifth argument (lowEndDefault), where present, is the default used on a weak GPU (GpuInfo.TIER_LOW,
    // e.g. the Helio G80's Mali-G52) when the player has not chosen a value. It only ever lowers quality, and
    // only on GPUs the port expects to struggle; a chosen value always wins.
    static final Option[] ALL = {
            new Option(RESOLUTION, "Resolución interna", "nfsmw_resolucion_interna", "1280x720", "1024x576",
                    new String[] {"640x360", "1024x576", "1280x720", "1920x1080"},
                    new String[] {"640x360 · máximo rendimiento", "1024x576 · rendimiento", "1280x720 · equilibrado",
                            "1920x1080 · máxima calidad"}),
            new Option(FPS, "Límite de FPS", "nfsmw_limite_fps", "60",
                    new String[] {"30", "60", "90", "120"},
                    new String[] {"30 FPS · ahorra batería", "60 FPS", "90 FPS · experimental",
                            "120 FPS · experimental"}),
            new Option("aa", "Antialiasing", "nfsmw_antialiasing", "apagado",
                    new String[] {"apagado", "fxaa"},
                    new String[] {"Desactivado", "FXAA"}),
            new Option("shadows", "Sombras", "nfsmw_sombras_cada", "1", "-1",
                    new String[] {"1", "2", "-1"},
                    new String[] {"Cada fotograma", "Cada 2 fotogramas · más rendimiento",
                            "Desactivadas · máximo rendimiento"}),
            new Option("car_reflections", "Reflejos del coche", "nfsmw_cubemap_caras_max", "6", "1",
                    new String[] {"6", "2", "1", "0"},
                    new String[] {"Altos", "Medios", "Bajos · más rendimiento",
                            "Desactivados (reflejo fijo) · máximo rendimiento"}),
            new Option("rear_mirror", "Retrovisor", "nfsmw_retrovisor_cada", "1", "2",
                    new String[] {"1", "2", "3", "-1"},
                    new String[] {"Nativo · cada fotograma", "Cada 2 fotogramas", "Cada 3 fotogramas · más rendimiento",
                            "Desactivado · máximo rendimiento"}),
            new Option("road_reflection", "Reflejo del asfalto mojado", "nfsmw_reflejo_carretera", "true", "false",
                    new String[] {"true", "false"},
                    new String[] {"Activado", "Desactivado · más rendimiento"}),
            new Option("sky", "Resplandor del cielo", "nfsmw_resplandor_cielo", "natural",
                    new String[] {"original", "natural", "suave"},
                    new String[] {"Original (Xbox 360)", "Natural", "Suave"}),
            new Option("volume", "Volumen del juego", "audio_ganancia_pct", "100",
                    new String[] {"100", "125", "150", "200"},
                    new String[] {"Normal", "Alto", "Muy alto", "Máximo · puede saturar"}),
            new Option("filter", "Filtro de imagen", "nfsmw_posproceso", "apagado",
                    new String[] {"apagado", "cine", "vivo", "calido", "frio", "sepia", "noir", "crt"},
                    new String[] {"Sin filtro", "Cine", "Vivo", "Cálido", "Frío", "Sepia", "Blanco y negro",
                            "CRT"}),
    };

    private static final String PREFS = "nfsmw_game";

    private GameOptions() {
    }

    static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static Option find(String key) {
        for (Option o : ALL) {
            if (o.key.equals(key)) {
                return o;
            }
        }
        throw new IllegalArgumentException(key);
    }

    /** The default for this option on this phone: the weak-GPU one when it applies, otherwise the normal one. */
    static String effectiveDefault(Context context, Option o) {
        if (o.lowEndDefault != null && GpuInfo.isLowEnd(context)) {
            return o.lowEndDefault;
        }
        return o.defaultValue;
    }

    static String get(Context context, String key) {
        Option o = find(key);
        String value = prefs(context).getString(key, effectiveDefault(context, o));
        for (String allowed : o.values) {
            if (allowed.equals(value)) {
                return value;
            }
        }
        return effectiveDefault(context, o);
    }

    static void set(Context context, String key, String value) {
        prefs(context).edit().putString(key, value).apply();
    }

    /**
     * True when the weak-GPU profile is in effect (so the UI can say so). On a weak GPU it always is: even if
     * the player has raised every menu option, the shadow-map scale in {@link #profileArguments} stays lowered.
     */
    static boolean lowEndProfileActive(Context context) {
        return GpuInfo.isLowEnd(context);
    }

    static List<String> arguments(Context context) {
        List<String> args = new ArrayList<>();
        for (Option o : ALL) {
            args.add("--" + o.cvar + "=" + get(context, o.key));
        }
        args.addAll(profileArguments(context));
        return args;
    }

    /**
     * Automatic tuning that is not exposed as a menu option, applied only on a weak GPU. The game's two
     * 1600x1600 shadow maps become 1024x1024 (the same size this game's PC version uses), which the native
     * default (100) does not do. Capable GPUs get no extra argument and keep the native default.
     */
    static List<String> profileArguments(Context context) {
        List<String> args = new ArrayList<>();
        if (GpuInfo.isLowEnd(context)) {
            args.add("--nfsmw_nativo_sombras_escala=64");
            // Present on the presenter's own thread. On the Mali-G52 the driver waits for the GPU inside
            // vkQueuePresentKHR; on the render thread that serialized CPU and GPU (100+ ms frames dropped
            // from ~17.6 to ~4.7 a minute in free roam with it, and the game never waited for the presenter).
            args.add("--present_hilo_propio=true");
        }
        return args;
    }
}
