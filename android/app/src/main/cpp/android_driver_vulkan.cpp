// Vulkan with a driver from app storage (the vulkan_icd_android cvar), e.g. Mesa's PanVK for the Mali-G52, through
// libadrenotools: a private copy of the system's libvulkan.so whose HAL dlopen loads that driver instead of the
// vendor's. The system loader keeps providing the Android surface and swapchain.
#include <adrenotools/driver.h>
#include <android/log.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/ui/vulkan/instance.h>

// Diagnostics of that driver (Mesa reads its debug options from the environment and prints to stderr, which an
// Android app does not have): variables set before opening it, and a file that receives stderr and stdout.
REXCVAR_DEFINE_STRING(vulkan_driver_env, "", "UI/Vulkan",
                      "Android: variables de entorno para el driver de vulkan_icd_android, NOMBRE=valor separadas "
                      "por ';' (p. ej. BIFROST_MESA_DEBUG=shaderdb;MESA_SHADER_CACHE_DISABLE=true)");
REXCVAR_DEFINE_STRING(vulkan_driver_stderr, "", "UI/Vulkan",
                      "Android: archivo que recibe stderr y stdout del proceso (lo que imprime el driver de "
                      "vulkan_icd_android); vacio = nada");

namespace {
constexpr char kTag[] = "NFSMW";

void PrepararDiagnostico() {
  std::string variables = REXCVAR_GET(vulkan_driver_env);
  size_t inicio = 0;
  while (inicio < variables.size()) {
    size_t fin = variables.find(';', inicio);
    if (fin == std::string::npos) {
      fin = variables.size();
    }
    const std::string par = variables.substr(inicio, fin - inicio);
    const size_t igual = par.find('=');
    if (igual != std::string::npos && igual > 0) {
      setenv(par.substr(0, igual).c_str(), par.substr(igual + 1).c_str(), 1);
      __android_log_print(ANDROID_LOG_INFO, kTag, "driver Vulkan: %s", par.c_str());
    }
    inicio = fin + 1;
  }
  const std::string& salida = REXCVAR_GET(vulkan_driver_stderr);
  if (!salida.empty()) {
    const int fd = open(salida.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
      fflush(stdout);
      fflush(stderr);
      dup2(fd, STDERR_FILENO);
      dup2(fd, STDOUT_FILENO);
      close(fd);
      setvbuf(stderr, nullptr, _IONBF, 0);
      setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    __android_log_print(fd >= 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag, "driver Vulkan: stderr a %s: %s",
                        salida.c_str(), fd >= 0 ? "si" : "no se pudo abrir");
  }
}

void* AbrirLibvulkan(const char* driver_path) {
  // The hooks (libmain_hook.so, libhook_impl.so and their libc++) are next to the driver: VulkanDriver.java
  // copies them out of the APK.
  std::string driver = driver_path;
  size_t barra_driver = driver.rfind('/');
  if (barra_driver == std::string::npos) {
    return nullptr;
  }
  std::string carpeta_driver = driver.substr(0, barra_driver + 1);
  const std::string& carpeta_hooks = carpeta_driver;
  PrepararDiagnostico();
  std::string nombre_driver = driver.substr(barra_driver + 1);
  void* libvulkan = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, carpeta_hooks.c_str(),
                                               carpeta_driver.c_str(), nombre_driver.c_str(), nullptr, nullptr);
  __android_log_print(libvulkan ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                      "adrenotools: libvulkan con %s%s (hooks en %s): %s", carpeta_driver.c_str(),
                      nombre_driver.c_str(), carpeta_hooks.c_str(), libvulkan ? "abierto" : "fallo");
  return libvulkan;
}

struct Registro {
  Registro() { rex::ui::vulkan::SetAndroidVulkanLoaderOpener(AbrirLibvulkan); }
} registro;
}  // namespace
