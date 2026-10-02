// nfsmw - persistent cache of BC textures transcoded to ETC2/EAC (native renderer, Mali mode)
//
// Why
//   The Mali-G52 cannot sample BC (DXT). Without this, every BC texture is decoded on the CPU when it is
//   uploaded, and the image holds the decoded texels: 16 or 32 bits per texel instead of 4 or 8. That is
//   CPU on the ring thread for every new texture and four times the GPU memory, on a phone whose stutters
//   were RAM pressure (lmkd).
//
//   ETC2/EAC is what the Mali samples natively, with the same bytes per 4x4 block as the BC it replaces:
//     BC1 (opaque)  -> ETC2 RGB8         8 bytes
//     BC2, BC3      -> ETC2 RGBA8 (EAC)  16 bytes
//     BC4           -> EAC R11           8 bytes
//     BC5           -> EAC RG11          16 bytes
//   So a transcoded texture has exactly the layout of its BC data: same level offsets, same sizes.
//   BC1 with transparent texels is not stored (etcpak has no punch-through ETC2): it keeps the decode path.
//
// How
//   The first time a BC texture is uploaded, the ring decodes it as before and hands a copy of its BC data
//   to this thread, which runs on the little cores at low priority, transcodes it with etcpak and appends it
//   to datos.bin, with a record in indice.bin. From then on (this session or any later one), a texture with
//   that content is created directly as ETC2 and its data comes from the file: no decode, a quarter of the
//   memory.
//
//   The key is the content: the hash of the guest bytes (the same huella_cruda the renderer already uses to
//   know whether a texture changed) seeded with its shape (format, size, levels, tiling, byte order). A
//   texture can only ever get the data of the same bytes read the same way.
//
//   Files: <executable folder>/cache/etc2/ (files/nfsmw/user/cache/etc2 on Android). Each record carries an
//   XXH3 of its data, checked on every read: a record whose data was cut short by a crash is simply not
//   used.

#include "nfsmw_cache_etc2.h"
#include "nfsmw_etc2_codificar.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(nfsmw_nativo_mali_cache_etc2, true, "NFSMW",
                    "Modo Mali: las texturas BC se transcodifican a ETC2/EAC en segundo plano y se guardan en "
                    "cache/etc2. Desde la siguiente vez se cargan ya en ETC2: sin descomprimir en la CPU y con "
                    "la cuarta parte de memoria de GPU")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_mali_cache_etc2_cola_mb, 64, "NFSMW",
                     "Modo Mali: MB de datos BC que pueden esperar a transcodificarse; el resto se deja para la "
                     "proxima vez que aparezca la textura")
    .range(4, 1024);

#if defined(__ANDROID__)

#include <sys/resource.h>
#include <sched.h>
#include <unistd.h>

// Same xxHash setup as nfsmw_nativo_dibujos.cpp (reads through memcpy; see the note there).
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace nfsmw::nativo::etc2 {
namespace {

struct Entrada {
  uint64_t offset;
  uint32_t bytes;
  VkFormat formato;
  uint64_t suma;
};

std::mutex g_mutex;  // the map, the queue and the files
std::unordered_map<uint64_t, Entrada> g_entradas;
std::unordered_set<uint64_t> g_en_cola;
std::unordered_set<uint64_t> g_mundo;  // mundo.bin, read once in Iniciar and never written while playing
std::unordered_set<uint64_t> g_sin_etc2;  // sin_etc2.bin, likewise
std::deque<Trabajo> g_cola;
uint64_t g_bytes_cola = 0;
std::condition_variable g_cv;
bool g_salir = false;
bool g_activo = false;
std::thread* g_hilo = nullptr;  // never destroyed at exit: a joinable std::thread there would abort
FILE* g_datos_escritura = nullptr;
FILE* g_indice_escritura = nullptr;
FILE* g_datos_lectura = nullptr;
uint64_t g_tam_datos = 0;

// Counters for the report (relaxed atomics: only for the log).
std::atomic<uint64_t> g_codificadas{0}, g_ns_codificando{0}, g_bytes_codificados{0}, g_sin_guardar{0},
    g_descartadas_cola{0}, g_lecturas_fallidas{0};
std::chrono::steady_clock::time_point g_ultimo_informe{};
uint64_t g_aciertos_previos = 0, g_fallos_previos = 0;  // totals at the previous report

// The little cores (the lowest maximum frequency), so the encoder never competes with the ring or the game.
void IrANucleosChicos() {
  const long n = sysconf(_SC_NPROCESSORS_CONF);
  if (n <= 1 || n > 64) {
    return;
  }
  std::vector<long> frecuencias(size_t(n), 0);
  long minima = 0, maxima = 0;
  for (long i = 0; i < n; ++i) {
    char ruta[96];
    std::snprintf(ruta, sizeof(ruta), "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", i);
    if (FILE* f = std::fopen(ruta, "re")) {
      long v = 0;
      if (std::fscanf(f, "%ld", &v) == 1) {
        frecuencias[size_t(i)] = v;
      }
      std::fclose(f);
    }
    if (frecuencias[size_t(i)] > 0) {
      minima = minima == 0 ? frecuencias[size_t(i)] : std::min(minima, frecuencias[size_t(i)]);
      maxima = std::max(maxima, frecuencias[size_t(i)]);
    }
  }
  if (minima == 0 || minima == maxima) {
    return;  // unknown or all equal: leave it to the scheduler
  }
  cpu_set_t conjunto;
  CPU_ZERO(&conjunto);
  for (long i = 0; i < n; ++i) {
    if (frecuencias[size_t(i)] == minima) {
      CPU_SET(int(i), &conjunto);
    }
  }
  sched_setaffinity(0, sizeof(conjunto), &conjunto);
}

void Bucle() {
  setpriority(PRIO_PROCESS, 0, 10);
  IrANucleosChicos();
  std::vector<uint8_t> salida;
  for (;;) {
    Trabajo t;
    {
      std::unique_lock<std::mutex> l(g_mutex);
      g_cv.wait(l, [] { return g_salir || !g_cola.empty(); });
      if (g_salir) {
        return;
      }
      t = std::move(g_cola.front());
      g_cola.pop_front();
      g_bytes_cola -= std::min<uint64_t>(g_bytes_cola, t.datos.size());
    }
    const auto antes = std::chrono::steady_clock::now();
    VkFormat formato = VK_FORMAT_UNDEFINED;
    const bool ok = Codificar(t, formato, salida);
    g_ns_codificando.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - antes)
                                            .count()),
                               std::memory_order_relaxed);
    if (!ok) {
      std::lock_guard<std::mutex> l(g_mutex);
      g_en_cola.erase(t.clave);
      g_sin_guardar.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    // The files are written outside the lock (only this thread writes them), so a Buscar from the ring never
    // waits for the disk. The entry becomes visible once its data and record are flushed.
    const uint64_t suma = XXH3_64bits(salida.data(), salida.size());
    const uint64_t offset = g_tam_datos;
    if (std::fwrite(salida.data(), 1, salida.size(), g_datos_escritura) != salida.size() ||
        std::fflush(g_datos_escritura) != 0) {
      REXLOG_WARN("[nativo] C3 cache ETC2: no se pudo escribir datos.bin; deja de guardar");
      std::lock_guard<std::mutex> l(g_mutex);
      g_en_cola.erase(t.clave);
      return;
    }
    const Registro r{t.clave, offset, uint32_t(salida.size()), uint32_t(formato), suma};
    std::fwrite(&r, sizeof(r), 1, g_indice_escritura);
    std::fflush(g_indice_escritura);
    {
      std::lock_guard<std::mutex> l(g_mutex);
      g_en_cola.erase(t.clave);
      g_entradas[t.clave] = {offset, uint32_t(salida.size()), formato, suma};
      g_tam_datos = offset + salida.size();
    }
    g_codificadas.fetch_add(1, std::memory_order_relaxed);
    g_bytes_codificados.fetch_add(salida.size(), std::memory_order_relaxed);
  }
}

}  // namespace

bool Iniciar(const std::filesystem::path& carpeta) {
  if (g_activo || !REXCVAR_GET(nfsmw_nativo_mali_cache_etc2)) {
    return g_activo;
  }
  const std::string ruta_datos = (carpeta / "datos.bin").string();
  const std::string ruta_indice = (carpeta / "indice.bin").string();
  std::unordered_map<uint64_t, Registro> registros;
  uint64_t descartados = 0;
  g_tam_datos = 0;
  AbrirIndice(carpeta, registros, g_tam_datos, descartados);
  LeerMundo(carpeta, g_mundo);
  LeerClaves(carpeta, "sin_etc2.bin", g_sin_etc2);
  g_entradas.clear();
  for (const auto& [clave, r] : registros) {
    g_entradas[clave] = {r.offset, r.bytes, VkFormat(r.formato), r.suma};
  }
  g_datos_escritura = std::fopen(ruta_datos.c_str(), "ab");
  g_indice_escritura = std::fopen(ruta_indice.c_str(), "ab");
  g_datos_lectura = std::fopen(ruta_datos.c_str(), "rb");
  if (!g_datos_escritura || !g_indice_escritura || !g_datos_lectura) {
    REXLOG_WARN("[nativo] C3 cache ETC2: no se pudo abrir {}; sigue sin cache", carpeta.string());
    Terminar();
    return false;
  }
  // Appending continues where the valid data ends (a torn tail is overwritten logically: its records
  // were dropped and new offsets start at the end of the file).
  std::fseek(g_datos_escritura, 0, SEEK_END);
  g_tam_datos = uint64_t(std::ftell(g_datos_escritura));
  g_activo = true;
  g_salir = false;
  g_hilo = new std::thread(Bucle);
  REXLOG_INFO("[nativo] C3 cache ETC2 en {}: {} texturas ({} MB), {} del mundo, {} del mapa sin ETC2{}",
              carpeta.string(), g_entradas.size(), g_tam_datos >> 20, g_mundo.size(), g_sin_etc2.size(),
              descartados ? fmt::format(", {} registros incompletos descartados", descartados) : std::string());
  return true;
}

void Terminar() {
  {
    std::lock_guard<std::mutex> l(g_mutex);
    g_salir = true;
  }
  g_cv.notify_all();
  if (g_hilo) {
    g_hilo->join();
    delete g_hilo;
    g_hilo = nullptr;
  }
  std::lock_guard<std::mutex> l(g_mutex);
  for (FILE** f : {&g_datos_escritura, &g_indice_escritura, &g_datos_lectura}) {
    if (*f) {
      std::fclose(*f);
      *f = nullptr;
    }
  }
  g_activo = false;
}

bool Activo() {
  return g_activo;
}

bool EsDelMundo(uint64_t clave) {
  return g_activo && g_mundo.count(clave) != 0;
}

bool SinEtc2(uint64_t clave) {
  return g_activo && g_sin_etc2.count(clave) != 0;
}

bool Buscar(uint64_t clave, VkFormat& formato, uint32_t& bytes) {
  if (!g_activo) {
    return false;
  }
  std::lock_guard<std::mutex> l(g_mutex);
  const auto it = g_entradas.find(clave);
  if (it == g_entradas.end()) {
    return false;
  }
  formato = it->second.formato;
  bytes = it->second.bytes;
  return true;
}

bool Leer(uint64_t clave, std::vector<uint8_t>& destino) {
  if (!g_activo) {
    return false;
  }
  Entrada e;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    const auto it = g_entradas.find(clave);
    if (it == g_entradas.end()) {
      return false;
    }
    e = it->second;
  }
  // Only the ring reads (g_datos_lectura is its own handle), outside the lock.
  destino.resize(e.bytes);
  const bool leido = std::fseek(g_datos_lectura, long(e.offset), SEEK_SET) == 0 &&
                     std::fread(destino.data(), 1, e.bytes, g_datos_lectura) == e.bytes;
  if (!leido || XXH3_64bits(destino.data(), destino.size()) != e.suma) {
    std::clearerr(g_datos_lectura);
    g_lecturas_fallidas.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> l(g_mutex);
    g_entradas.erase(clave);
    return false;
  }
  return true;
}

bool Encolar(Trabajo&& trabajo) {
  if (!g_activo || FormatoEtc2De(trabajo.bc) == VK_FORMAT_UNDEFINED) {
    return false;
  }
  {
    std::lock_guard<std::mutex> l(g_mutex);
    if (g_entradas.count(trabajo.clave) || g_en_cola.count(trabajo.clave)) {
      return false;
    }
    if (g_bytes_cola + trabajo.datos.size() > uint64_t(REXCVAR_GET(nfsmw_nativo_mali_cache_etc2_cola_mb)) << 20) {
      g_descartadas_cola.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    g_en_cola.insert(trabajo.clave);
    g_bytes_cola += trabajo.datos.size();
    g_cola.push_back(std::move(trabajo));
  }
  g_cv.notify_one();
  return true;
}

void Informe(uint64_t aciertos, uint64_t fallos) {
  if (!g_activo) {
    return;
  }
  const auto ahora = std::chrono::steady_clock::now();
  if (ahora - g_ultimo_informe < std::chrono::seconds(10)) {
    return;
  }
  g_ultimo_informe = ahora;
  const uint64_t aciertos_10s = aciertos - g_aciertos_previos, fallos_10s = fallos - g_fallos_previos;
  g_aciertos_previos = aciertos;
  g_fallos_previos = fallos;
  size_t entradas = 0, en_cola = 0;
  uint64_t mb = 0;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    entradas = g_entradas.size();
    en_cola = g_cola.size();
    mb = g_tam_datos >> 20;
  }
  const uint64_t codificadas = g_codificadas.exchange(0, std::memory_order_relaxed);
  const uint64_t ns = g_ns_codificando.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[nativo] C3 cache ETC2: {} texturas ({} MB en disco); en 10 s, {} cargadas de la cache y {} nuevas "
              "(desde el arranque {} y {}); el hilo transcodifico {} ({:.1f} ms de media, {:.1f} MB), {} en cola, "
              "{} sin guardar (BC1 con transparencia) y {} sin sitio en la cola; {} lecturas fallidas",
              entradas, mb, aciertos_10s, fallos_10s, aciertos, fallos, codificadas,
              codificadas ? double(ns) / 1e6 / double(codificadas) : 0.0,
              double(g_bytes_codificados.exchange(0, std::memory_order_relaxed)) / 1048576.0, en_cola,
              g_sin_guardar.load(std::memory_order_relaxed), g_descartadas_cola.load(std::memory_order_relaxed),
              g_lecturas_fallidas.load(std::memory_order_relaxed));
}

}  // namespace nfsmw::nativo::etc2

#else  // not Android: the cache does not exist (no GPU there samples ETC2 and not BC)

namespace nfsmw::nativo::etc2 {
bool Iniciar(const std::filesystem::path&) { return false; }
void Terminar() {}
bool Activo() { return false; }
bool EsDelMundo(uint64_t) { return false; }
bool SinEtc2(uint64_t) { return false; }
VkFormat FormatoEtc2De(VkFormat) { return VK_FORMAT_UNDEFINED; }
bool Buscar(uint64_t, VkFormat&, uint32_t&) { return false; }
bool Leer(uint64_t, std::vector<uint8_t>&) { return false; }
bool Encolar(Trabajo&&) { return false; }
void Informe(uint64_t, uint64_t) {}
}  // namespace nfsmw::nativo::etc2

#endif
