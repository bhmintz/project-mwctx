// nfsmw - who takes the big cores from the ring thread (Android)
//
// Measured on the Samsung A32 (2026-10-02, "[tiron] hilo del anillo"): in most race stutters the ring had its
// usual ~1750 draws and took 100-200 ms of wall time, but only 15-40 ms of CPU, with 10-200 forced context
// switches, on cpu6/cpu7 (the two A75). Same work, a fifth of the CPU: something else ran on its core.
//
// Two things here:
//   - nfsmw_android_nice_anillo: the ring thread's nice value (Mali mode). CFS shares a core by weight: at
//     nice -10 the ring weighs ~9x a normal thread, so a competing thread of the process (or a normal
//     priority one from elsewhere) can no longer take four fifths of the core from it.
//   - nfsmw_android_hilos_tiron: a 100 ms sampler of /proc/self/task. When the ring reports a stutter, the
//     threads that used CPU in that window are logged with their milliseconds and the core they last ran on.
//   - nfsmw_android_adpf: an ADPF performance hint session (Android 13+) for the ring thread. Sampled during
//     a race, the A75s sometimes sat at 1.3-1.4 GHz with a 1.71 GHz cap: the frequency governor lowered the
//     clock in the middle of the ring's work. Each Swap reports how long the ring's frame took against the
//     target (nfsmw_android_adpf_objetivo_us), and the system keeps the clock up for it.

#include "nfsmw_hilos_android.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(nfsmw_android_nice_anillo, -10, "NFSMW",
                     "Android, modo Mali: valor nice del hilo del anillo (el que graba los dibujos). Mas negativo "
                     "= el planificador le quita menos el nucleo. 0 = no se toca")
    .range(-20, 0);
REXCVAR_DEFINE_BOOL(nfsmw_android_hilos_tiron, true, "NFSMW",
                    "Android: en cada tiron, anota que hilos del proceso usaron CPU en esa ventana y en que nucleo");
REXCVAR_DEFINE_BOOL(nfsmw_android_adpf, true, "NFSMW",
                    "Android 13+, modo Mali: sesion ADPF (performance hint) del hilo del anillo, que informa la "
                    "duracion de cada fotograma para que el sistema no le baje el reloj")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_android_adpf_objetivo_us, 16666, "NFSMW",
                     "Duracion objetivo del fotograma del anillo que se le pide a ADPF, en microsegundos")
    .range(1000, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__ANDROID__)

#include <dirent.h>
#include <dlfcn.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nfsmw::nativo {
extern std::atomic<bool> g_nativo_modo_mali;
}

namespace nfsmw::hilos {
namespace {

struct Hilo {
  std::string nombre;
  uint64_t ticks = 0;  // utime + stime, in clock ticks
  int cpu = -1;        // the core it last ran on
};
using Foto = std::unordered_map<int, Hilo>;

bool LeerHilo(int tid, Hilo& h) {
  char ruta[64];
  std::snprintf(ruta, sizeof(ruta), "/proc/self/task/%d/stat", tid);
  FILE* f = std::fopen(ruta, "re");
  if (!f) {
    return false;
  }
  char buf[1024];
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = 0;
  // "tid (comm) state ppid ..." : comm can hold spaces and parentheses, so the last ')' closes it.
  char* abre = std::strchr(buf, '(');
  char* cierra = std::strrchr(buf, ')');
  if (!abre || !cierra || cierra < abre) {
    return false;
  }
  h.nombre.assign(abre + 1, cierra);
  // Fields after ')' start at field 3 (state). utime = 14, stime = 15, processor = 39.
  int campo = 3;
  uint64_t utime = 0, stime = 0;
  for (char* p = cierra + 2; *p; ++campo) {
    char* fin = nullptr;
    const unsigned long long v = std::strtoull(p, &fin, 10);
    if (campo == 14) utime = v;
    if (campo == 15) stime = v;
    if (campo == 39) {
      h.cpu = int(v);
      break;
    }
    p = std::strchr(p, ' ');
    if (!p) break;
    ++p;
  }
  h.ticks = utime + stime;
  return true;
}

void Fotografiar(Foto& foto) {
  foto.clear();
  DIR* d = opendir("/proc/self/task");
  if (!d) {
    return;
  }
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') {
      continue;
    }
    const int tid = std::atoi(e->d_name);
    Hilo h;
    if (LeerHilo(tid, h)) {
      foto.emplace(tid, std::move(h));
    }
  }
  closedir(d);
}

using Reloj = std::chrono::steady_clock;

std::mutex g_mutex;
std::condition_variable g_cv;
std::vector<std::pair<Reloj::time_point, double>> g_pedidos;  // (end of the stutter, its ms)
std::atomic<bool> g_arrancado{false};
std::atomic<uint32_t> g_informes{0};

void Muestreador() {
  setpriority(PRIO_PROCESS, 0, 10);  // a background diagnostic: never in the ring's way
  const double ms_por_tick = 1000.0 / double(sysconf(_SC_CLK_TCK));
  std::deque<std::pair<Reloj::time_point, Foto>> historia;  // one snapshot every 100 ms, the last 3 s
  for (;;) {
    std::vector<std::pair<Reloj::time_point, double>> pedidos;
    {
      std::unique_lock<std::mutex> l(g_mutex);
      g_cv.wait_for(l, std::chrono::milliseconds(100));
      pedidos.swap(g_pedidos);
    }
    Foto ahora;
    Fotografiar(ahora);
    const auto t = Reloj::now();
    for (const auto& [fin, ms] : pedidos) {
      // The last snapshot taken before the stutter began.
      const auto inicio = fin - std::chrono::microseconds(int64_t(ms * 1000.0));
      const Foto* antes = nullptr;
      Reloj::time_point t_antes{};
      for (const auto& [tf, f] : historia) {
        if (tf <= inicio) {
          antes = &f;
          t_antes = tf;
        }
      }
      if (!antes || g_informes.fetch_add(1, std::memory_order_relaxed) >= 400) {
        continue;
      }
      std::vector<std::pair<double, std::string>> filas;
      double total = 0;
      for (const auto& [tid, h] : ahora) {
        const auto it = antes->find(tid);
        const uint64_t t0 = it != antes->end() ? it->second.ticks : 0;
        const double usado = double(h.ticks - std::min(h.ticks, t0)) * ms_por_tick;
        total += usado;
        if (usado > 0) {
          char fila[96];
          std::snprintf(fila, sizeof(fila), " %s(%d) %.0f cpu%d;", h.nombre.c_str(), tid, usado, h.cpu);
          filas.emplace_back(usado, fila);
        }
      }
      std::sort(filas.begin(), filas.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      std::string lista;
      for (size_t i = 0; i < filas.size() && i < 10; ++i) {
        lista += filas[i].second;
      }
      const double ventana =
          double(std::chrono::duration_cast<std::chrono::microseconds>(t - t_antes).count()) / 1000.0;
      REXLOG_INFO("[tiron] hilos (tiron de {:.0f} ms; ventana de {:.0f} ms, {:.0f} ms de CPU del proceso):{}", ms,
                  ventana, total, lista);
    }
    historia.emplace_back(t, std::move(ahora));
    while (historia.size() > 30) {
      historia.pop_front();
    }
  }
}

// ADPF, loaded from libandroid.so by name: the APK still runs on Android 8 (minSdk 26), where it does not exist.
struct Adpf {
  using FnManager = void* (*)();
  using FnCrear = void* (*)(void*, const int32_t*, size_t, int64_t);
  using FnInformar = int (*)(void*, int64_t);
  FnInformar informar = nullptr;
  void* sesion = nullptr;
  Reloj::time_point anterior{};
  uint64_t informes = 0;

  void Crear() {
    void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
      return;
    }
    const auto manager = reinterpret_cast<FnManager>(dlsym(lib, "APerformanceHint_getManager"));
    const auto crear = reinterpret_cast<FnCrear>(dlsym(lib, "APerformanceHint_createSession"));
    informar = reinterpret_cast<FnInformar>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    void* m = manager ? manager() : nullptr;
    if (!m || !crear || !informar) {
      REXLOG_INFO("[hilos] ADPF no disponible en este Android");
      return;
    }
    const int32_t tid = int32_t(gettid());
    const int64_t objetivo = int64_t(REXCVAR_GET(nfsmw_android_adpf_objetivo_us)) * 1000;
    sesion = crear(m, &tid, 1, objetivo);
    REXLOG_INFO("[hilos] ADPF: sesion del hilo del anillo (tid {}), objetivo {:.2f} ms: {}", tid,
                double(objetivo) / 1e6, sesion ? "ok" : "rechazada");
  }

  // Once per Swap: the ring's frame, from the previous Swap to this one.
  void Informar() {
    const auto ahora = Reloj::now();
    if (sesion && anterior.time_since_epoch().count() != 0) {
      const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(ahora - anterior).count();
      if (ns > 0 && ns < 1000000000) {  // a load or a pause is not a frame
        informar(sesion, ns);
        ++informes;
      }
    }
    anterior = ahora;
  }
};

}  // namespace

void AlPresentar() {
  static thread_local bool hecho = false;
  static thread_local Adpf adpf;
  if (hecho) {
    adpf.Informar();
    return;
  }
  hecho = true;
  if (REXCVAR_GET(nfsmw_android_adpf) && nfsmw::nativo::g_nativo_modo_mali.load(std::memory_order_relaxed)) {
    adpf.Crear();
  }
  const int32_t nice = REXCVAR_GET(nfsmw_android_nice_anillo);
  if (nice < 0 && nfsmw::nativo::g_nativo_modo_mali.load(std::memory_order_relaxed)) {
    const int r = setpriority(PRIO_PROCESS, 0, nice);  // 0 = the calling thread on Linux
    REXLOG_INFO("[hilos] hilo del anillo (tid {}): nice {} ({})", gettid(), getpriority(PRIO_PROCESS, 0),
                r == 0 ? "ok" : std::strerror(errno));
  }
  if (REXCVAR_GET(nfsmw_android_hilos_tiron) && !g_arrancado.exchange(true)) {
    std::thread(Muestreador).detach();
  }
}

void AnotarTiron(double ms) {
  if (!g_arrancado.load(std::memory_order_relaxed)) {
    return;
  }
  {
    std::lock_guard<std::mutex> l(g_mutex);
    if (g_pedidos.size() < 8) {
      g_pedidos.emplace_back(Reloj::now(), ms);
    }
  }
  g_cv.notify_one();
}

}  // namespace nfsmw::hilos

#else

namespace nfsmw::hilos {
void AlPresentar() {}
void AnotarTiron(double) {}
}  // namespace nfsmw::hilos

#endif
