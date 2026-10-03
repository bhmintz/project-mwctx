// Capture of what the vendor Mali driver (Samsung's libGLES_mali.so, through which the system Vulkan runs) sends
// to the GPU, to reverse engineer how it lays out each draw around ARM's shader binaries: see
// docs/plan-arm-hibrido.md, stage 1. Off by default (nfsmw_captura_mali = 0). With a custom driver
// (vulkan_icd_android, e.g. PanVK) it hooks that library instead, to compare what both drivers build.
//
// libGLES_mali.so imports ioctl/mmap/mmap64/munmap/read/__read_chk from libc and is linked BIND_NOW, so its GOT is resolved: the
// entries are rewritten to point here. The kbase memory it maps from /dev/mali0 is SAME_VA on 64 bits (the CPU
// address is the GPU address), so every region can be read in this process. On each KBASE_IOCTL_JOB_SUBMIT during
// the chosen frame, the atoms and the small regions whose contents changed are written to a file, before the GPU
// runs them: the descriptors the CPU wrote. The job completion events the driver reads from the same fd trigger a
// second copy of what changed (what the GPU wrote: the driver's compute jobs, the pilots), until the frame after.
// Read with tools/mali_arm/leer_captura and tools/mali_arm/captura.py.
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

#include <csetjmp>
#include <csignal>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>

REXCVAR_DEFINE_INT32(nfsmw_captura_mali, 0, "NFSMW",
                     "Diagnostico (Mali; driver del sistema o el de vulkan_icd_android): captura lo que el driver envia a la GPU "
                     "durante el fotograma N (descriptores y cadenas de trabajos) a captura_mali_N.bin; 0 = apagado");
REXCVAR_DEFINE_STRING(nfsmw_captura_mali_carpeta, "/storage/emulated/0/nsfmw-androidevolved", "NFSMW",
                      "Carpeta donde se escribe la captura de nfsmw_captura_mali");
REXCVAR_DECLARE(std::string, vulkan_icd_android);

namespace nfsmw::nativo {
extern std::atomic<uint64_t> g_fotogramas_mostrados;  // nfsmw_nativo_destinos.cpp
}

namespace {
constexpr char kTag[] = "NFSMW";
// The vendor driver, or the custom one (vulkan_icd_android, e.g. PanVK) to capture what it builds and compare.
std::string g_biblioteca = "libGLES_mali.so";

// kbase UAPI (mali_kbase_ioctl.h): type 0x80.
struct IoctlJobSubmit {
  uint64_t addr;
  uint32_t nr_atoms;
  uint32_t stride;
};
struct IoctlGetGpuprops {
  uint64_t buffer;
  uint32_t size;
  uint32_t flags;
};
constexpr unsigned long kJobSubmit = _IOW(0x80, 2, IoctlJobSubmit);
constexpr unsigned long kGetGpuprops = _IOW(0x80, 3, IoctlGetGpuprops);

// Only small regions are copied (descriptors, shaders, uniforms); textures and render targets are left out.
constexpr uint64_t kRegionMax = 4ull << 20;
constexpr uint64_t kPresupuesto = 160ull << 20;
// Bigger regions (the game's dynamic uniform buffers, but also textures) are copied once, when the capture starts.
constexpr uint64_t kRegionGrandeMax = 64ull << 20;
constexpr uint64_t kPresupuestoGrandes = 384ull << 20;

// File: "NFCM", version, then records {u32 type, u32 bytes, data}.
// A region's u32 submit has kDespues set when it was copied on a completion event (after the GPU ran) instead of
// before a submit. A completion record is {u32 submits so far, raw base_jd_event_v2 read}.
enum : uint32_t {
  kRegistroRegion = 1,
  kRegistroEnvio = 2,
  kRegistroNota = 3,
  kRegistroGpuprops = 4,
  kRegistroEventos = 5,
};
constexpr uint32_t kDespues = 0x80000000u;

using FnIoctl = int (*)(int, int, ...);
using FnMmap = void* (*)(void*, size_t, int, int, int, off_t);
using FnMmap64 = void* (*)(void*, size_t, int, int, int, off64_t);
using FnMunmap = int (*)(void*, size_t);
using FnRead = ssize_t (*)(int, void*, size_t);
using FnReadChk = ssize_t (*)(int, void*, size_t, size_t);
FnIoctl g_ioctl_real = nullptr;
FnMmap g_mmap_real = nullptr;
FnMmap64 g_mmap64_real = nullptr;
FnMunmap g_munmap_real = nullptr;
FnRead g_read_real = nullptr;
FnReadChk g_read_chk_real = nullptr;

struct Region {
  uint64_t tam;
  uint64_t va_gpu = 0;  // equal to the CPU address in SAME_VA; otherwise the mmap offset
  int prot = 0;
  uint64_t huella = 0;  // of the last copy written, to write it again only if it changed
};

std::mutex g_mutex;
std::map<uint64_t, Region> g_regiones;  // by CPU address
std::map<int, bool> g_fd_mali;          // fd -> is /dev/mali0
// The driver maps the executable memory, writes the shaders and unmaps it: the last contents of each small region
// it unmaps, by GPU address, to be written when the capture starts.
std::map<uint64_t, std::vector<uint8_t>> g_desmapadas;
uint64_t g_tam_desmapadas = 0;
constexpr uint64_t kDesmapadasMax = 64ull << 20;
std::vector<uint8_t> g_gpuprops;
FILE* g_archivo = nullptr;
uint64_t g_escrito = 0;
uint64_t g_escrito_grandes = 0;  // part of g_escrito: the big regions, outside kPresupuesto
uint32_t g_envios = 0;
bool g_terminada = false;

bool EsMali(int fd) {
  auto it = g_fd_mali.find(fd);
  if (it != g_fd_mali.end()) {
    return it->second;
  }
  char enlace[64], destino[128] = {};
  snprintf(enlace, sizeof(enlace), "/proc/self/fd/%d", fd);
  const ssize_t n = readlink(enlace, destino, sizeof(destino) - 1);
  const bool mali = n > 0 && strstr(destino, "mali") != nullptr;
  g_fd_mali[fd] = mali;
  return mali;
}

// Reads without crashing on pages the CPU cannot read. process_vm_readv does not work on kbase mappings (special
// pages), so the copy is direct, with a SIGSEGV/SIGBUS handler that jumps back here only for this thread.
thread_local sigjmp_buf* t_salto = nullptr;
struct sigaction g_segv_viejo, g_bus_viejo;

void AlFallar(int senal, siginfo_t* info, void* contexto) {
  if (t_salto) {
    siglongjmp(*t_salto, 1);
  }
  const struct sigaction& viejo = senal == SIGBUS ? g_bus_viejo : g_segv_viejo;
  if (viejo.sa_flags & SA_SIGINFO) {
    viejo.sa_sigaction(senal, info, contexto);
  } else if (viejo.sa_handler != SIG_IGN && viejo.sa_handler != SIG_DFL) {
    viejo.sa_handler(senal);
  } else {
    signal(senal, SIG_DFL);
    raise(senal);
  }
}

bool LeerSeguro(uint64_t direccion, void* destino, size_t tam) {
  sigjmp_buf salto;
  if (sigsetjmp(salto, 1)) {
    t_salto = nullptr;
    return false;
  }
  t_salto = &salto;
  memcpy(destino, reinterpret_cast<const void*>(direccion), tam);
  t_salto = nullptr;
  return true;
}

void ProtegerLecturas(bool poner) {
  if (poner) {
    struct sigaction nuevo {};
    nuevo.sa_sigaction = AlFallar;
    nuevo.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&nuevo.sa_mask);
    sigaction(SIGSEGV, &nuevo, &g_segv_viejo);
    sigaction(SIGBUS, &nuevo, &g_bus_viejo);
  } else {
    sigaction(SIGSEGV, &g_segv_viejo, nullptr);
    sigaction(SIGBUS, &g_bus_viejo, nullptr);
  }
}

void Escribir(uint32_t tipo, const void* a, uint32_t tam_a, const void* b = nullptr, uint32_t tam_b = 0) {
  const uint32_t cab[2] = {tipo, tam_a + tam_b};
  fwrite(cab, sizeof(cab), 1, g_archivo);
  fwrite(a, 1, tam_a, g_archivo);
  if (b) {
    fwrite(b, 1, tam_b, g_archivo);
  }
  g_escrito += 8 + tam_a + tam_b;
}

void Nota(const std::string& texto) { Escribir(kRegistroNota, texto.data(), uint32_t(texto.size())); }

uint64_t Fnv(const uint8_t* d, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i += 8) {  // every 8th byte: enough to notice a change in descriptors
    h = (h ^ d[i]) * 1099511628211ull;
  }
  return h;
}

uint32_t CopiarRegiones(uint32_t indice, uint32_t& ilegibles, uint32_t& sin_lectura);

// Under g_mutex.
void Cerrar() {
  if (g_archivo) {
    Nota("fin");
    fclose(g_archivo);
    g_archivo = nullptr;
    __android_log_print(ANDROID_LOG_INFO, kTag, "captura Mali: terminada, %u envios, %llu MB", g_envios,
                        (unsigned long long)(g_escrito >> 20));
  }
  g_terminada = true;
}

// Under g_mutex. Called before the real JOB_SUBMIT.
void CapturarEnvio(const IoctlJobSubmit& envio) {
  const uint64_t fotograma = nfsmw::nativo::g_fotogramas_mostrados.load(std::memory_order_relaxed);
  const int32_t objetivo = REXCVAR_GET(nfsmw_captura_mali);
  if (g_terminada || objetivo <= 0 || fotograma < uint64_t(objetivo)) {
    return;
  }
  if (fotograma > uint64_t(objetivo)) {
    // The submits of the next frame are not captured; the file stays open for the completion events of this one.
    if (fotograma > uint64_t(objetivo) + 1 || !g_archivo) {
      Cerrar();
    }
    return;
  }
  if (!g_archivo) {
    const std::string ruta = REXCVAR_GET(nfsmw_captura_mali_carpeta) + "/captura_mali_" +
                             std::to_string(objetivo) + ".bin";
    g_archivo = fopen(ruta.c_str(), "wb");
    if (!g_archivo) {
      __android_log_print(ANDROID_LOG_ERROR, kTag, "captura Mali: no se pudo abrir %s", ruta.c_str());
      g_terminada = true;
      return;
    }
    // The game's pipeline lists (internal storage): their keys carry the specialization constant of each
    // pipeline, to compile ARM's binaries of the same variants with malioc.
    std::error_code error;
    for (const auto& e : std::filesystem::directory_iterator(rex::filesystem::GetExecutableFolder() / "cache", error)) {
      if (e.path().filename().string().rfind("nfsmw_nativo_pipelines", 0) == 0) {
        std::filesystem::copy_file(e.path(),
                                   std::filesystem::path(REXCVAR_GET(nfsmw_captura_mali_carpeta)) / e.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing, error);
      }
    }
    const uint32_t cab[2] = {0x4D43464Eu /* "NFCM" */, 1};
    fwrite(cab, sizeof(cab), 1, g_archivo);
    if (!g_gpuprops.empty()) {
      Escribir(kRegistroGpuprops, g_gpuprops.data(), uint32_t(g_gpuprops.size()));
    }
    Nota("fotograma " + std::to_string(fotograma) + ", " + std::to_string(g_regiones.size()) + " regiones, " +
         std::to_string(g_desmapadas.size()) + " desmapadas");
    uint64_t grandes = 0;
    std::string lista;
    std::vector<uint8_t> copia;
    ProtegerLecturas(true);
    for (const auto& [va, r] : g_regiones) {
      if (r.tam <= kRegionMax) {
        continue;
      }
      char linea[96];
      snprintf(linea, sizeof(linea), " %llx:%llx", (unsigned long long)r.va_gpu, (unsigned long long)r.tam);
      lista += linea;
      if (!(r.prot & PROT_READ) || r.tam > kRegionGrandeMax || grandes + r.tam > kPresupuestoGrandes) {
        continue;
      }
      copia.resize(r.tam);
      if (!LeerSeguro(va, copia.data(), copia.size())) {
        continue;
      }
      const uint64_t cab[2] = {r.va_gpu, r.tam};
      uint8_t pre[sizeof(cab) + 4] = {};
      memcpy(pre, cab, sizeof(cab));
      Escribir(kRegistroRegion, pre, sizeof(pre), copia.data(), uint32_t(copia.size()));
      grandes += r.tam;
    }
    g_escrito_grandes = g_escrito;
    ProtegerLecturas(false);
    Nota("regiones grandes (va:tam):" + lista + "; copiadas " + std::to_string(grandes >> 20) + " MB");
    for (const auto& [va_gpu, bytes] : g_desmapadas) {
      const uint64_t cab[2] = {va_gpu, bytes.size()};
      uint8_t pre[sizeof(cab) + 4] = {};
      memcpy(pre, cab, sizeof(cab));
      Escribir(kRegistroRegion, pre, sizeof(pre), bytes.data(), uint32_t(bytes.size()));
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "captura Mali: empieza en el fotograma %llu (%s)",
                        (unsigned long long)fotograma, ruta.c_str());
  }
  // Regions first (what the atoms point to), then the submit.
  uint32_t ilegibles = 0, sin_lectura = 0;
  ProtegerLecturas(true);
  const uint32_t escritas = CopiarRegiones(g_envios, ilegibles, sin_lectura);
  std::vector<uint8_t> atomos(size_t(envio.nr_atoms) * envio.stride);
  LeerSeguro(envio.addr, atomos.data(), atomos.size());
  ProtegerLecturas(false);
  const uint32_t cab[3] = {g_envios, envio.nr_atoms, envio.stride};
  Escribir(kRegistroEnvio, cab, sizeof(cab), atomos.data(), uint32_t(atomos.size()));
  Nota("envio " + std::to_string(g_envios) + ": " + std::to_string(envio.nr_atoms) + " atomos, " +
       std::to_string(escritas) + " regiones nuevas o cambiadas, " + std::to_string(ilegibles) + " ilegibles, " + std::to_string(sin_lectura) + " sin PROT_READ");
  ++g_envios;
  fflush(g_archivo);
}

// Under g_mutex, with ProtegerLecturas. Writes the small readable regions whose contents changed since their last
// copy, tagged with `indice`; returns how many.
uint32_t CopiarRegiones(uint32_t indice, uint32_t& ilegibles, uint32_t& sin_lectura) {
  std::vector<uint8_t> copia;
  uint32_t escritas = 0;
  for (auto& [va, r] : g_regiones) {
    if (r.tam > kRegionMax || g_escrito - g_escrito_grandes + r.tam > kPresupuesto) {
      continue;
    }
    if (!(r.prot & PROT_READ)) {
      ++sin_lectura;
      continue;
    }
    copia.resize(r.tam);
    if (!LeerSeguro(va, copia.data(), r.tam)) {
      ++ilegibles;
      continue;
    }
    const uint64_t huella = Fnv(copia.data(), copia.size()) ^ r.tam;
    if (huella == r.huella) {
      continue;
    }
    r.huella = huella;
    const uint64_t cab[2] = {r.va_gpu, r.tam};
    std::vector<uint8_t> pre(sizeof(cab) + 4);
    memcpy(pre.data(), cab, sizeof(cab));
    memcpy(pre.data() + sizeof(cab), &indice, 4);
    Escribir(kRegistroRegion, pre.data(), uint32_t(pre.size()), copia.data(), uint32_t(copia.size()));
    ++escritas;
  }
  return escritas;
}

// Under g_mutex. Called after the driver read completion events from the Mali fd.
void CapturarEventos(const void* eventos, size_t tam) {
  if (!g_archivo) {
    return;
  }
  const uint64_t fotograma = nfsmw::nativo::g_fotogramas_mostrados.load(std::memory_order_relaxed);
  if (fotograma > uint64_t(REXCVAR_GET(nfsmw_captura_mali)) + 1) {
    Cerrar();
    return;
  }
  const uint32_t envios = g_envios;
  Escribir(kRegistroEventos, &envios, 4, eventos, uint32_t(tam));
  uint32_t ilegibles = 0, sin_lectura = 0;
  ProtegerLecturas(true);
  CopiarRegiones(kDespues | (g_envios ? g_envios - 1 : 0), ilegibles, sin_lectura);
  ProtegerLecturas(false);
  fflush(g_archivo);
}

void AlLeer(int fd, void* destino, ssize_t r) {
  if (r > 0) {
    std::lock_guard<std::mutex> cerrojo(g_mutex);
    if (g_archivo && EsMali(fd)) {
      CapturarEventos(destino, size_t(r));
    }
  }
}

ssize_t MiRead(int fd, void* destino, size_t tam) {
  const ssize_t r = g_read_real(fd, destino, tam);
  AlLeer(fd, destino, r);
  return r;
}

// The driver is built with FORTIFY: its reads of the events go through __read_chk.
ssize_t MiReadChk(int fd, void* destino, size_t tam, size_t tam_destino) {
  const ssize_t r = g_read_chk_real(fd, destino, tam, tam_destino);
  AlLeer(fd, destino, r);
  return r;
}

int MiIoctl(int fd, int pedido, ...) {
  va_list args;
  va_start(args, pedido);
  void* arg = va_arg(args, void*);
  va_end(args);
  const unsigned long p = static_cast<unsigned int>(pedido);
  if (p == kJobSubmit && arg) {
    std::lock_guard<std::mutex> cerrojo(g_mutex);
    CapturarEnvio(*static_cast<const IoctlJobSubmit*>(arg));
  }
  const int r = g_ioctl_real(fd, pedido, arg);
  if (p == kGetGpuprops && arg && r > 0) {
    const auto* props = static_cast<const IoctlGetGpuprops*>(arg);
    if (props->buffer && props->size) {
      std::lock_guard<std::mutex> cerrojo(g_mutex);
      g_gpuprops.assign(reinterpret_cast<const uint8_t*>(props->buffer),
                        reinterpret_cast<const uint8_t*>(props->buffer) + props->size);
    }
  }
  return r;
}

// kbase mmap offsets: below BASE_MEM_FIRST_FREE_ADDRESS (0x80000) they are cookies of SAME_VA allocations (the
// GPU address is the CPU address the mmap returns) or special handles; above it, the GPU address itself (the
// allocations outside SAME_VA, like the executable zone where the shaders are).
void AnotarMapa(void* r, size_t tam, int prot, int fd, uint64_t desplazamiento) {
  if (r == MAP_FAILED || fd < 0) {
    return;
  }
  std::lock_guard<std::mutex> cerrojo(g_mutex);
  if (EsMali(fd)) {
    Region region;
    region.tam = tam;
    region.prot = prot;
    region.va_gpu = desplazamiento >= 0x80000 ? desplazamiento : reinterpret_cast<uint64_t>(r);
    g_regiones[reinterpret_cast<uint64_t>(r)] = region;
  }
}

void* MiMmap(void* dir, size_t tam, int prot, int flags, int fd, off_t off) {
  void* r = g_mmap_real(dir, tam, prot, flags, fd, off);
  AnotarMapa(r, tam, prot, fd, uint64_t(off));
  return r;
}

void* MiMmap64(void* dir, size_t tam, int prot, int flags, int fd, off64_t off) {
  void* r = g_mmap64_real(dir, tam, prot, flags, fd, off);
  AnotarMapa(r, tam, prot, fd, uint64_t(off));
  return r;
}

int MiMunmap(void* dir, size_t tam) {
  {
    std::lock_guard<std::mutex> cerrojo(g_mutex);
    auto it = g_regiones.find(reinterpret_cast<uint64_t>(dir));
    if (it != g_regiones.end()) {
      const Region& region = it->second;
      if ((region.prot & PROT_READ) && region.tam <= kRegionMax) {
        std::vector<uint8_t> copia(region.tam);
        ProtegerLecturas(true);
        const bool leida = LeerSeguro(it->first, copia.data(), copia.size());
        ProtegerLecturas(false);
        auto vieja = g_desmapadas.find(region.va_gpu);
        const uint64_t tam_viejo = vieja != g_desmapadas.end() ? vieja->second.size() : 0;
        if (leida && g_tam_desmapadas - tam_viejo + copia.size() <= kDesmapadasMax) {
          g_tam_desmapadas += copia.size() - tam_viejo;
          g_desmapadas[region.va_gpu] = std::move(copia);
        }
      }
      g_regiones.erase(it);
    }
  }
  return g_munmap_real(dir, tam);
}

// Rewrites the GOT entries of the imports with these names in the library loaded at `base`.
struct Busqueda {
  ElfW(Addr) base = 0;
  const ElfW(Phdr) * phdr = nullptr;
  ElfW(Half) phnum = 0;
};

int BuscarBiblioteca(dl_phdr_info* info, size_t, void* dato) {
  if (info->dlpi_name && strstr(info->dlpi_name, g_biblioteca.c_str())) {
    auto* b = static_cast<Busqueda*>(dato);
    b->base = info->dlpi_addr;
    b->phdr = info->dlpi_phdr;
    b->phnum = info->dlpi_phnum;
    return 1;
  }
  return 0;
}

bool ParchearGot(const Busqueda& b) {
  const ElfW(Dyn)* dinamica = nullptr;
  for (ElfW(Half) i = 0; i < b.phnum; ++i) {
    if (b.phdr[i].p_type == PT_DYNAMIC) {
      dinamica = reinterpret_cast<const ElfW(Dyn)*>(b.base + b.phdr[i].p_vaddr);
    }
  }
  if (!dinamica) {
    return false;
  }
  const ElfW(Rela)* jmprel = nullptr;
  size_t tam_jmprel = 0;
  const ElfW(Sym)* simbolos = nullptr;
  const char* cadenas = nullptr;
  for (const ElfW(Dyn)* d = dinamica; d->d_tag != DT_NULL; ++d) {
    switch (d->d_tag) {
      case DT_JMPREL: jmprel = reinterpret_cast<const ElfW(Rela)*>(b.base + d->d_un.d_ptr); break;
      case DT_PLTRELSZ: tam_jmprel = d->d_un.d_val; break;
      case DT_SYMTAB: simbolos = reinterpret_cast<const ElfW(Sym)*>(b.base + d->d_un.d_ptr); break;
      case DT_STRTAB: cadenas = reinterpret_cast<const char*>(b.base + d->d_un.d_ptr); break;
    }
  }
  if (!jmprel || !simbolos || !cadenas) {
    return false;
  }
  struct Gancho {
    const char* nombre;
    void* mio;
    void** real;
  } ganchos[] = {
      {"ioctl", reinterpret_cast<void*>(&MiIoctl), reinterpret_cast<void**>(&g_ioctl_real)},
      {"mmap", reinterpret_cast<void*>(&MiMmap), reinterpret_cast<void**>(&g_mmap_real)},
      {"mmap64", reinterpret_cast<void*>(&MiMmap64), reinterpret_cast<void**>(&g_mmap64_real)},
      {"munmap", reinterpret_cast<void*>(&MiMunmap), reinterpret_cast<void**>(&g_munmap_real)},
      {"read", reinterpret_cast<void*>(&MiRead), reinterpret_cast<void**>(&g_read_real)},
      {"__read_chk", reinterpret_cast<void*>(&MiReadChk), reinterpret_cast<void**>(&g_read_chk_real)},
  };
  // The real functions first, so no hook runs without them.
  g_ioctl_real = reinterpret_cast<FnIoctl>(dlsym(RTLD_DEFAULT, "ioctl"));
  g_mmap_real = reinterpret_cast<FnMmap>(dlsym(RTLD_DEFAULT, "mmap"));
  g_mmap64_real = reinterpret_cast<FnMmap64>(dlsym(RTLD_DEFAULT, "mmap64"));
  g_munmap_real = reinterpret_cast<FnMunmap>(dlsym(RTLD_DEFAULT, "munmap"));
  g_read_real = reinterpret_cast<FnRead>(dlsym(RTLD_DEFAULT, "read"));
  g_read_chk_real = reinterpret_cast<FnReadChk>(dlsym(RTLD_DEFAULT, "__read_chk"));
  int parcheadas = 0;
  const long pagina = sysconf(_SC_PAGESIZE);
  for (size_t i = 0; i < tam_jmprel / sizeof(ElfW(Rela)); ++i) {
    const ElfW(Rela)& r = jmprel[i];
    const char* nombre = cadenas + simbolos[ELF64_R_SYM(r.r_info)].st_name;
    for (const Gancho& g : ganchos) {
      if (strcmp(nombre, g.nombre) != 0) {
        continue;
      }
      void** entrada = reinterpret_cast<void**>(b.base + r.r_offset);
      const uintptr_t inicio = reinterpret_cast<uintptr_t>(entrada) & ~uintptr_t(pagina - 1);
      if (mprotect(reinterpret_cast<void*>(inicio), size_t(pagina), PROT_READ | PROT_WRITE) != 0) {
        continue;
      }
      __atomic_store_n(entrada, g.mio, __ATOMIC_RELEASE);
      ++parcheadas;
    }
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "captura Mali: %d entradas de la GOT de %s enganchadas", parcheadas,
                      g_biblioteca.c_str());
  return parcheadas > 0;
}

// The vendor driver is loaded by the UI (EGL) before the game creates its Vulkan device; the device's memory is
// mapped after that. A thread waits for the cvar and the library and hooks it once.
struct Arranque {
  Arranque() {
    std::thread([] {
      for (int i = 0; i < 6000; ++i) {  // up to 60 s
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (REXCVAR_GET(nfsmw_captura_mali) <= 0) {
          continue;
        }
        const std::string& driver = REXCVAR_GET(vulkan_icd_android);
        if (!driver.empty() && access(driver.c_str(), R_OK) == 0) {
          g_biblioteca = driver.substr(driver.rfind('/') + 1);
        }
        Busqueda b;
        dl_iterate_phdr(BuscarBiblioteca, &b);
        if (b.base) {
          ParchearGot(b);
          return;
        }
      }
    }).detach();
  }
} arranque;
}  // namespace
