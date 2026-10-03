package com.nfsmw.android;

import android.content.Context;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * A Vulkan driver other than the system's, e.g. Mesa's PanVK for Mali Bifrost (github.com/JimVulkan/mali-panvk).
 * It comes as the zip emulators use (meta.json + the .so) in the game folder's drivers/; external storage is
 * noexec, so the library is copied to app storage and the native side opens it (vulkan_icd_android).
 */
final class VulkanDriver {
    static final String OPTION = "vulkan_driver";
    static final String FOLDER = "drivers";
    private static final String TAG = "NFSMW";

    private VulkanDriver() {
    }

    /** The absolute path of the driver ready to dlopen, or null to stay on the system driver. */
    static String prepare(Context context, File folder) {
        File[] files = folder.listFiles();
        if (files == null) {
            Log.w(TAG, "Driver Vulkan: no existe " + folder);
            return null;
        }
        File target = new File(context.getFilesDir(), "drivers");
        target.mkdirs();
        for (File f : files) {
            if (!f.getName().toLowerCase().endsWith(".zip")) {
                continue;
            }
            try (ZipFile zip = new ZipFile(f)) {
                ZipEntry meta = zip.getEntry("meta.json");
                if (meta == null) {
                    continue;
                }
                String library = new JSONObject(new String(read(zip.getInputStream(meta)), "UTF-8"))
                        .getString("libraryName");
                ZipEntry entry = zip.getEntry(library);
                if (entry == null || library.contains("/")) {
                    continue;
                }
                File out = new File(target, library);
                // The zip's size and date stand for its version: copy again only when it changes.
                File stamp = new File(target, library + ".origen");
                String origin = f.getName() + " " + f.length() + " " + f.lastModified();
                if (!out.isFile() || !stamp.isFile() || !new String(read(new java.io.FileInputStream(stamp)),
                        "UTF-8").equals(origin)) {
                    out.delete();
                    try (InputStream in = zip.getInputStream(entry); OutputStream os = new FileOutputStream(out)) {
                        copy(in, os);
                    }
                    try (OutputStream os = new FileOutputStream(stamp)) {
                        os.write(origin.getBytes("UTF-8"));
                    }
                    Log.i(TAG, "Driver Vulkan: " + library + " copiado desde " + f.getName());
                }
                out.setReadOnly();
                if (!copyHooks(context, target)) {
                    return null;
                }
                return out.getAbsolutePath();
            } catch (Exception e) {
                Log.w(TAG, "Driver Vulkan: " + f + ": " + e);
            }
        }
        Log.w(TAG, "Driver Vulkan: ningún zip con meta.json en " + folder);
        return null;
    }

    /**
     * libadrenotools' hooks next to the driver. They are in the APK uncompressed and are not extracted (no
     * useLegacyPackaging: that would deflate the 42 MB game library on every build and inflate it on every
     * install), so they are copied out of it, with the libc++ they need: their linker namespace only searches
     * that folder.
     */
    private static boolean copyHooks(Context context, File target) {
        String abi = android.os.Build.SUPPORTED_ABIS[0];
        try (ZipFile apk = new ZipFile(context.getApplicationInfo().sourceDir)) {
            for (String name : new String[] {"libhook_impl.so", "libmain_hook.so", "libc++_shared.so"}) {
                ZipEntry entry = apk.getEntry("lib/" + abi + "/" + name);
                if (entry == null) {
                    Log.w(TAG, "Driver Vulkan: el APK no trae " + name);
                    return false;
                }
                File out = new File(target, name);
                File stamp = new File(target, name + ".origen");
                String origin = Long.toHexString(entry.getCrc()) + " " + entry.getSize();
                if (out.isFile() && stamp.isFile() && new String(read(new java.io.FileInputStream(stamp)),
                        "UTF-8").equals(origin)) {
                    continue;
                }
                out.delete();
                try (InputStream in = apk.getInputStream(entry); OutputStream os = new FileOutputStream(out)) {
                    copy(in, os);
                }
                try (OutputStream os = new FileOutputStream(stamp)) {
                    os.write(origin.getBytes("UTF-8"));
                }
                out.setReadOnly();
            }
            return true;
        } catch (IOException e) {
            Log.w(TAG, "Driver Vulkan: hooks de adrenotools: " + e);
            return false;
        }
    }

    private static byte[] read(InputStream in) throws IOException {
        try (InputStream i = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            copy(i, out);
            return out.toByteArray();
        }
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[1 << 16];
        int n;
        while ((n = in.read(buffer)) > 0) {
            out.write(buffer, 0, n);
        }
    }
}
