#include <jni.h>

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/input/sdl/sdl_input_driver.h>

extern "C" JNIEXPORT void JNICALL
Java_com_nfsmw_android_TouchControlsView_nativeSetTouchState(JNIEnv*, jclass, jint buttons, jint left_x,
                                                             jint left_y, jint right_x, jint right_y,
                                                             jint left_trigger, jint right_trigger) {
  rex_sdl_set_touch_gamepad_state(static_cast<uint16_t>(buttons), static_cast<int16_t>(left_x),
                                  static_cast<int16_t>(left_y), static_cast<int16_t>(right_x),
                                  static_cast<int16_t>(right_y), static_cast<uint8_t>(left_trigger),
                                  static_cast<uint8_t>(right_trigger));
}

// Screen mode, live: the presenter reads these cvars on every frame (presenter.cpp).
//   stretch = true:  the game image fills the whole phone screen.
//   stretch = false: 16:9 with bars at the sides, as the game was made.
// The launcher's "Mostrar FPS": the counter is drawn by GameActivity; the cvar only exists so the option can
// travel on the command line like the others.
REXCVAR_DEFINE_BOOL(nfsmw_mostrar_fps, false, "NFSMW", "Android: contador de FPS en pantalla (lo dibuja el launcher)");

namespace nfsmw::nativo {
extern std::atomic<uint64_t> g_fotogramas_mostrados;  // nfsmw_nativo_destinos.cpp
}

// The FPS counter (GameActivity): the running total of game frames shown. The Java side divides the
// difference between two readings by the time between them.
extern "C" JNIEXPORT jlong JNICALL Java_com_nfsmw_android_GameActivity_nativeFotogramas(JNIEnv*, jclass) {
  return jlong(nfsmw::nativo::g_fotogramas_mostrados.load(std::memory_order_relaxed));
}

extern "C" JNIEXPORT void JNICALL
Java_com_nfsmw_android_GameActivity_nativeSetStretch(JNIEnv*, jclass, jboolean stretch) {
  rex::cvar::SetFlagByName("present_letterbox", stretch ? "false" : "true");
  if (stretch) {
    // No overscan crop: all of the image, stretched to the screen.
    rex::cvar::SetFlagByName("present_safe_area_x", "100");
    rex::cvar::SetFlagByName("present_safe_area_y", "100");
  }
}
