// JNI of libnfsmw_precarga.so, the launcher's "Preparar texturas" (TexturePrep.java). The prefill runs on its
// own thread; Java polls its progress with nativeEstado() a few times per second, so no native thread ever
// calls into Java.

#include <jni.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "nfsmw_precarga_etc2.h"

namespace {

using nfsmw::nativo::precarga::Progreso;

// 0 = idle, 1 = running, 2 = done, 3 = could not open the files, 4 = cancelled.
std::atomic<int> g_estado{0};
std::atomic<bool> g_cancelar{false};
std::unique_ptr<Progreso> g_progreso;
std::thread g_hilo;

std::string Cadena(JNIEnv* env, jstring s) {
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string r = c ? c : "";
  if (c) {
    env->ReleaseStringUTFChars(s, c);
  }
  return r;
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL Java_com_nfsmw_android_TexturePrep_nativeIniciar(JNIEnv* env, jclass,
                                                                                       jstring carpeta_nfs,
                                                                                       jstring carpeta_cache) {
  if (g_estado.load() == 1) {
    return JNI_FALSE;
  }
  if (g_hilo.joinable()) {
    g_hilo.join();
  }
  const std::string nfs = Cadena(env, carpeta_nfs);
  const std::string cache = Cadena(env, carpeta_cache);
  g_progreso = std::make_unique<Progreso>();
  g_cancelar = false;
  g_estado = 1;
  g_hilo = std::thread([nfs, cache]() {
    const bool ok = nfsmw::nativo::precarga::Precargar(nfs, cache, *g_progreso, g_cancelar);
    g_estado = !ok ? 3 : g_cancelar.load() ? 4 : 2;
  });
  return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL Java_com_nfsmw_android_TexturePrep_nativeCancelar(JNIEnv*, jclass) {
  g_cancelar = true;
}

// {state, bytes done, bytes total, BC textures, new, already cached, not stored, checked, mismatched, world}
extern "C" JNIEXPORT jlongArray JNICALL Java_com_nfsmw_android_TexturePrep_nativeEstado(JNIEnv* env, jclass) {
  jlong v[10] = {g_estado.load()};
  if (const Progreso* p = g_progreso.get()) {
    v[1] = jlong(p->bytes_hechos.load());
    v[2] = jlong(p->bytes_total.load());
    v[3] = jlong(p->texturas.load());
    v[4] = jlong(p->nuevas.load());
    v[5] = jlong(p->ya_estaban.load());
    v[6] = jlong(p->sin_guardar.load());
    v[7] = jlong(p->comprobadas.load());
    v[8] = jlong(p->distintas.load());
    v[9] = jlong(p->del_mundo.load());
  }
  jlongArray r = env->NewLongArray(10);
  env->SetLongArrayRegion(r, 0, 10, v);
  return r;
}
