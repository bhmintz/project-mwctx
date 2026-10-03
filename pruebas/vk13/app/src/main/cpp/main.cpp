// Vulkan 1.3 test with PanVK (Mesa) on the Mali-G52, loaded the same way as the game: libadrenotools makes a
// private copy of the system's libvulkan.so open PanVK instead of the vendor driver.
//
// It draws a turning RGB triangle over a background that changes color, using only Vulkan 1.3 core paths:
// dynamic rendering (vkCmdBeginRendering), synchronization2 (vkCmdPipelineBarrier2, vkQueueSubmit2). It logs
// what the driver reports (tag VK13) and the FPS. If PanVK cannot be opened it falls back to the system driver
// and says so; with that one (Vulkan 1.1) the 1.3 drawing path is not available: the log says PRUEBA FALLIDA
// and nothing is drawn.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <adrenotools/driver.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "shaders.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VK13", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VK13", __VA_ARGS__)

namespace {

constexpr char kDriver[] = "libvulkan_panfrost.so";

// ---- Function pointers ---------------------------------------------------------------------------------------

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define FUNCIONES_GLOBALES(X) \
  X(vkCreateInstance)         \
  X(vkEnumerateInstanceVersion)
#define FUNCIONES_INSTANCIA(X)                    \
  X(vkDestroyInstance)                            \
  X(vkEnumeratePhysicalDevices)                   \
  X(vkGetPhysicalDeviceProperties2)               \
  X(vkGetPhysicalDeviceFeatures2)                 \
  X(vkGetPhysicalDeviceQueueFamilyProperties)     \
  X(vkEnumerateDeviceExtensionProperties)         \
  X(vkCreateDevice)                               \
  X(vkGetDeviceProcAddr)                          \
  X(vkCreateAndroidSurfaceKHR)                    \
  X(vkDestroySurfaceKHR)                          \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)         \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)    \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)
#define FUNCIONES_DISPOSITIVO(X)  \
  X(vkDestroyDevice)              \
  X(vkGetDeviceQueue)             \
  X(vkDeviceWaitIdle)             \
  X(vkCreateSwapchainKHR)         \
  X(vkDestroySwapchainKHR)        \
  X(vkGetSwapchainImagesKHR)      \
  X(vkAcquireNextImageKHR)        \
  X(vkQueuePresentKHR)            \
  X(vkCreateImageView)            \
  X(vkDestroyImageView)           \
  X(vkCreateShaderModule)         \
  X(vkDestroyShaderModule)        \
  X(vkCreatePipelineLayout)       \
  X(vkDestroyPipelineLayout)      \
  X(vkCreateGraphicsPipelines)    \
  X(vkDestroyPipeline)            \
  X(vkCreateCommandPool)          \
  X(vkDestroyCommandPool)         \
  X(vkAllocateCommandBuffers)     \
  X(vkResetCommandBuffer)         \
  X(vkBeginCommandBuffer)         \
  X(vkEndCommandBuffer)           \
  X(vkCreateSemaphore)            \
  X(vkDestroySemaphore)           \
  X(vkCreateFence)                \
  X(vkDestroyFence)               \
  X(vkWaitForFences)              \
  X(vkResetFences)                \
  X(vkCreateQueryPool)            \
  X(vkDestroyQueryPool)           \
  X(vkGetQueryPoolResults)        \
  X(vkCmdResetQueryPool)          \
  X(vkCmdWriteTimestamp2)         \
  X(vkCmdPipelineBarrier2)        \
  X(vkCmdBeginRendering)          \
  X(vkCmdEndRendering)            \
  X(vkCmdBindPipeline)            \
  X(vkCmdSetViewport)             \
  X(vkCmdSetScissor)              \
  X(vkCmdPushConstants)           \
  X(vkCmdDraw)                    \
  X(vkQueueSubmit2)

#define DECLARAR(nombre) PFN_##nombre nombre;
FUNCIONES_GLOBALES(DECLARAR)
FUNCIONES_INSTANCIA(DECLARAR)
FUNCIONES_DISPOSITIVO(DECLARAR)
#undef DECLARAR

// ---- State ---------------------------------------------------------------------------------------------------

struct Estado {
  android_app* app = nullptr;
  bool panvk = false;
  bool listo_13 = false;  // Device created with dynamic rendering and synchronization2.
  VkInstance instancia = VK_NULL_HANDLE;
  VkPhysicalDevice fisico = VK_NULL_HANDLE;
  VkDevice dispositivo = VK_NULL_HANDLE;
  uint32_t familia = 0;
  VkQueue cola = VK_NULL_HANDLE;
  uint32_t bits_marcas = 0;
  float periodo_marcas = 0.0f;

  VkSurfaceKHR superficie = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat formato = VK_FORMAT_UNDEFINED;
  VkExtent2D extension{};
  std::vector<VkImage> imagenes;
  std::vector<VkImageView> vistas;

  VkPipelineLayout disposicion = VK_NULL_HANDLE;
  VkPipeline tuberia = VK_NULL_HANDLE;
  VkFormat formato_tuberia = VK_FORMAT_UNDEFINED;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer comandos = VK_NULL_HANDLE;
  VkSemaphore adquirida = VK_NULL_HANDLE;
  std::vector<VkSemaphore> terminada;  // One per swapchain image.
  VkFence valla = VK_NULL_HANDLE;
  VkQueryPool consultas = VK_NULL_HANDLE;
  bool consultas_escritas = false;

  double inicio = 0.0;
  double ultimo_informe = 0.0;
  uint32_t fotogramas = 0;
  double gpu_ms_suma = 0.0;
  uint32_t gpu_muestras = 0;
};

double Ahora() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

const char* Resultado(VkResult r) {
  static char texto[32];
  snprintf(texto, sizeof(texto), "%d", r);
  return texto;
}

#define VERIFICAR(llamada)                                       \
  do {                                                           \
    VkResult r_ = (llamada);                                     \
    if (r_ != VK_SUCCESS) {                                      \
      LOGE("%s fallo: %s (linea %d)", #llamada, Resultado(r_), __LINE__); \
      return false;                                              \
    }                                                            \
  } while (0)

// ---- Loading the driver --------------------------------------------------------------------------------------

std::string CarpetaPropia() {
  Dl_info info{};
  dladdr(reinterpret_cast<void*>(&CarpetaPropia), &info);
  std::string ruta = info.dli_fname ? info.dli_fname : "";
  return ruta.substr(0, ruta.rfind('/') + 1);
}

// Copies the driver from the APK's assets to internal storage (external storage and the APK are not dlopen-able
// for a library that is not in nativeLibraryDir).
bool CopiarDriver(android_app* app, const std::string& destino) {
  AAsset* asset = AAssetManager_open(app->activity->assetManager, kDriver, AASSET_MODE_STREAMING);
  if (!asset) {
    LOGE("no esta el asset %s", kDriver);
    return false;
  }
  FILE* f = fopen(destino.c_str(), "wb");
  if (!f) {
    AAsset_close(asset);
    LOGE("no se pudo escribir %s", destino.c_str());
    return false;
  }
  char buffer[1 << 16];
  int n;
  size_t total = 0;
  while ((n = AAsset_read(asset, buffer, sizeof(buffer))) > 0) {
    fwrite(buffer, 1, n, f);
    total += n;
  }
  fclose(f);
  AAsset_close(asset);
  LOGI("driver copiado: %s (%zu bytes)", destino.c_str(), total);
  return true;
}

bool CargarLibvulkan(Estado& e) {
  std::string carpeta_driver = std::string(e.app->activity->internalDataPath) + "/";
  void* libvulkan = nullptr;
  if (CopiarDriver(e.app, carpeta_driver + kDriver)) {
    std::string hooks = CarpetaPropia();
    libvulkan = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hooks.c_str(),
                                           carpeta_driver.c_str(), kDriver, nullptr, nullptr);
    LOGI("adrenotools (hooks en %s): %s", hooks.c_str(), libvulkan ? "libvulkan abierto con PanVK" : "FALLO");
  }
  e.panvk = libvulkan != nullptr;
  if (!libvulkan) {
    LOGE("se usa el driver del sistema");
    libvulkan = dlopen("libvulkan.so", RTLD_NOW);
  }
  if (!libvulkan) {
    return false;
  }
  vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(libvulkan, "vkGetInstanceProcAddr"));
  if (!vkGetInstanceProcAddr) {
    return false;
  }
#define CARGAR(nombre) nombre = reinterpret_cast<PFN_##nombre>(vkGetInstanceProcAddr(nullptr, #nombre));
  FUNCIONES_GLOBALES(CARGAR)
#undef CARGAR
  return vkCreateInstance != nullptr;
}

// ---- Instance and device -------------------------------------------------------------------------------------

bool CrearInstancia(Estado& e) {
  uint32_t version = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion) {
    vkEnumerateInstanceVersion(&version);
  }
  LOGI("version de instancia: %u.%u.%u", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
       VK_API_VERSION_PATCH(version));
  VkApplicationInfo aplicacion{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  aplicacion.pApplicationName = "prueba-vk13";
  aplicacion.apiVersion = VK_API_VERSION_1_3;
  const char* extensiones[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &aplicacion;
  info.enabledExtensionCount = 2;
  info.ppEnabledExtensionNames = extensiones;
  VERIFICAR(vkCreateInstance(&info, nullptr, &e.instancia));
#define CARGAR(nombre) nombre = reinterpret_cast<PFN_##nombre>(vkGetInstanceProcAddr(e.instancia, #nombre));
  FUNCIONES_INSTANCIA(CARGAR)
#undef CARGAR
  return true;
}

const char* Si(VkBool32 b) { return b ? "si" : "NO"; }

bool CrearDispositivo(Estado& e) {
  uint32_t cuenta = 0;
  VERIFICAR(vkEnumeratePhysicalDevices(e.instancia, &cuenta, nullptr));
  if (!cuenta) {
    LOGE("ningun dispositivo fisico");
    return false;
  }
  std::vector<VkPhysicalDevice> fisicos(cuenta);
  VERIFICAR(vkEnumeratePhysicalDevices(e.instancia, &cuenta, fisicos.data()));
  e.fisico = fisicos[0];

  VkPhysicalDevicePushDescriptorPropertiesKHR pushp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
  VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
  VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
  VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p13.pNext = &pushp;
  p12.pNext = &p13;
  p.pNext = &p12;
  vkGetPhysicalDeviceProperties2(e.fisico, &p);
  uint32_t api = p.properties.apiVersion;
  LOGI("dispositivo: %s, API %u.%u.%u, driver %s (%s), vendor 0x%X", p.properties.deviceName,
       VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api), p12.driverName,
       p12.driverInfo, p.properties.vendorID);
  e.periodo_marcas = p.properties.limits.timestampPeriod;
  const VkPhysicalDeviceLimits& l = p.properties.limits;
  LOGI("limites: maxPushDescriptors %u, maxBoundDescriptorSets %u, maxPerStageDescriptorSampledImages %u, "
       "maxPerStageDescriptorSamplers %u, maxPerStageResources %u, maxPushConstantsSize %u, "
       "maxDescriptorSetUniformBuffersDynamic %u, maxUniformBufferRange %u, maxInlineUniformBlockSize %u",
       pushp.maxPushDescriptors, l.maxBoundDescriptorSets, l.maxPerStageDescriptorSampledImages,
       l.maxPerStageDescriptorSamplers, l.maxPerStageResources, l.maxPushConstantsSize,
       l.maxDescriptorSetUniformBuffersDynamic, l.maxUniformBufferRange, p13.maxInlineUniformBlockSize);
  LOGI("1.2 indexado: maxUpdateAfterBindDescriptorsInAllPools %u, shaderSampledImageArrayNonUniformIndexingNative %u",
       p12.maxUpdateAfterBindDescriptorsInAllPools, p12.shaderSampledImageArrayNonUniformIndexingNative);

  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  bool es_13 = api >= VK_API_VERSION_1_3;
  if (es_13) {
    f12.pNext = &f13;
    f11.pNext = &f12;
    f.pNext = &f11;
  }
  vkGetPhysicalDeviceFeatures2(e.fisico, &f);
  if (es_13) {
    LOGI("1.3: dynamicRendering %s, synchronization2 %s, maintenance4 %s, inlineUniformBlock %s, "
         "shaderDemoteToHelperInvocation %s, subgroupSizeControl %s, pipelineCreationCacheControl %s",
         Si(f13.dynamicRendering), Si(f13.synchronization2), Si(f13.maintenance4), Si(f13.inlineUniformBlock),
         Si(f13.shaderDemoteToHelperInvocation), Si(f13.subgroupSizeControl),
         Si(f13.pipelineCreationCacheControl));
    LOGI("1.2: bufferDeviceAddress %s, descriptorIndexing %s, runtimeDescriptorArray %s, "
         "descriptorBindingPartiallyBound %s, timelineSemaphore %s, shaderFloat16 %s, shaderInt8 %s, "
         "scalarBlockLayout %s, imagelessFramebuffer %s, hostQueryReset %s",
         Si(f12.bufferDeviceAddress), Si(f12.descriptorIndexing), Si(f12.runtimeDescriptorArray),
         Si(f12.descriptorBindingPartiallyBound), Si(f12.timelineSemaphore), Si(f12.shaderFloat16),
         Si(f12.shaderInt8), Si(f12.scalarBlockLayout), Si(f12.imagelessFramebuffer), Si(f12.hostQueryReset));
    LOGI("1.0: shaderInt64 %s, geometryShader %s, tessellationShader %s, fillModeNonSolid %s, "
         "vertexPipelineStoresAndAtomics %s, textureCompressionBC %s, textureCompressionETC2 %s, "
         "samplerAnisotropy %s, depthClamp %s, independentBlend %s",
         Si(f.features.shaderInt64), Si(f.features.geometryShader), Si(f.features.tessellationShader),
         Si(f.features.fillModeNonSolid), Si(f.features.vertexPipelineStoresAndAtomics),
         Si(f.features.textureCompressionBC), Si(f.features.textureCompressionETC2),
         Si(f.features.samplerAnisotropy), Si(f.features.depthClamp), Si(f.features.independentBlend));
  }

  uint32_t n_ext = 0;
  vkEnumerateDeviceExtensionProperties(e.fisico, nullptr, &n_ext, nullptr);
  std::vector<VkExtensionProperties> ext(n_ext);
  vkEnumerateDeviceExtensionProperties(e.fisico, nullptr, &n_ext, ext.data());
  std::string lista;
  for (const auto& x : ext) {
    if (!lista.empty()) lista += ' ';
    lista += x.extensionName;
    if (lista.size() > 700) {
      LOGI("extensiones: %s", lista.c_str());
      lista.clear();
    }
  }
  LOGI("extensiones (%u en total): %s", n_ext, lista.c_str());

  uint32_t n_familias = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(e.fisico, &n_familias, nullptr);
  std::vector<VkQueueFamilyProperties> familias(n_familias);
  vkGetPhysicalDeviceQueueFamilyProperties(e.fisico, &n_familias, familias.data());
  for (uint32_t i = 0; i < n_familias; ++i) {
    if (familias[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      e.familia = i;
      e.bits_marcas = familias[i].timestampValidBits;
      break;
    }
  }
  LOGI("cola %u: timestampValidBits %u, timestampPeriod %.2f ns", e.familia, e.bits_marcas, e.periodo_marcas);

  e.listo_13 = es_13 && f13.dynamicRendering && f13.synchronization2;
  if (!e.listo_13) {
    LOGE("sin Vulkan 1.3 con dynamicRendering y synchronization2: no se puede dibujar el camino 1.3");
    return false;
  }

  float prioridad = 1.0f;
  VkDeviceQueueCreateInfo cola{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  cola.queueFamilyIndex = e.familia;
  cola.queueCount = 1;
  cola.pQueuePriorities = &prioridad;
  VkPhysicalDeviceVulkan13Features pedir13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  pedir13.dynamicRendering = VK_TRUE;
  pedir13.synchronization2 = VK_TRUE;
  const char* extensiones[] = {"VK_KHR_swapchain"};
  VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  info.pNext = &pedir13;
  info.queueCreateInfoCount = 1;
  info.pQueueCreateInfos = &cola;
  info.enabledExtensionCount = 1;
  info.ppEnabledExtensionNames = extensiones;
  VERIFICAR(vkCreateDevice(e.fisico, &info, nullptr, &e.dispositivo));
#define CARGAR(nombre)                                                                          \
  nombre = reinterpret_cast<PFN_##nombre>(vkGetDeviceProcAddr(e.dispositivo, #nombre));         \
  if (!nombre) {                                                                                \
    LOGE("falta %s", #nombre);                                                                  \
    return false;                                                                               \
  }
  FUNCIONES_DISPOSITIVO(CARGAR)
#undef CARGAR
  vkGetDeviceQueue(e.dispositivo, e.familia, 0, &e.cola);

  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = e.familia;
  VERIFICAR(vkCreateCommandPool(e.dispositivo, &pool, nullptr, &e.pool));
  VkCommandBufferAllocateInfo asignar{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  asignar.commandPool = e.pool;
  asignar.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  asignar.commandBufferCount = 1;
  VERIFICAR(vkAllocateCommandBuffers(e.dispositivo, &asignar, &e.comandos));
  VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VERIFICAR(vkCreateSemaphore(e.dispositivo, &sem, nullptr, &e.adquirida));
  VkFenceCreateInfo valla{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  valla.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VERIFICAR(vkCreateFence(e.dispositivo, &valla, nullptr, &e.valla));
  if (e.bits_marcas) {
    VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    q.queryType = VK_QUERY_TYPE_TIMESTAMP;
    q.queryCount = 2;
    VERIFICAR(vkCreateQueryPool(e.dispositivo, &q, nullptr, &e.consultas));
  }
  return true;
}

// ---- Swapchain and pipeline ----------------------------------------------------------------------------------

bool CrearTuberia(Estado& e) {
  if (e.tuberia && e.formato_tuberia == e.formato) {
    return true;
  }
  if (e.tuberia) {
    vkDestroyPipeline(e.dispositivo, e.tuberia, nullptr);
    e.tuberia = VK_NULL_HANDLE;
  }
  if (!e.disposicion) {
    VkPushConstantRange rango{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &rango;
    VERIFICAR(vkCreatePipelineLayout(e.dispositivo, &info, nullptr, &e.disposicion));
  }
  VkShaderModule modulos[2]{};
  const uint32_t* codigos[2] = {kVert, kFrag};
  size_t tamanos[2] = {sizeof(kVert), sizeof(kFrag)};
  for (int i = 0; i < 2; ++i) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = tamanos[i];
    info.pCode = codigos[i];
    VERIFICAR(vkCreateShaderModule(e.dispositivo, &info, nullptr, &modulos[i]));
  }
  VkPipelineShaderStageCreateInfo etapas[2]{};
  etapas[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
               modulos[0], "main"};
  etapas[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
               modulos[1], "main"};
  VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ensamblado{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ensamblado.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vista{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vista.viewportCount = 1;
  vista.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo muestreo{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  muestreo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState mezcla_adjunto{};
  mezcla_adjunto.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo mezcla{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  mezcla.attachmentCount = 1;
  mezcla.pAttachments = &mezcla_adjunto;
  VkDynamicState dinamicos[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dinamico{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dinamico.dynamicStateCount = 2;
  dinamico.pDynamicStates = dinamicos;
  // Dynamic rendering: no VkRenderPass, the formats go here.
  VkPipelineRenderingCreateInfo renderizado{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  renderizado.colorAttachmentCount = 1;
  renderizado.pColorAttachmentFormats = &e.formato;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &renderizado;
  info.stageCount = 2;
  info.pStages = etapas;
  info.pVertexInputState = &vertices;
  info.pInputAssemblyState = &ensamblado;
  info.pViewportState = &vista;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &muestreo;
  info.pColorBlendState = &mezcla;
  info.pDynamicState = &dinamico;
  info.layout = e.disposicion;
  double t0 = Ahora();
  VkResult r = vkCreateGraphicsPipelines(e.dispositivo, VK_NULL_HANDLE, 1, &info, nullptr, &e.tuberia);
  LOGI("tuberia creada en %.1f ms: %s", (Ahora() - t0) * 1000.0, Resultado(r));
  vkDestroyShaderModule(e.dispositivo, modulos[0], nullptr);
  vkDestroyShaderModule(e.dispositivo, modulos[1], nullptr);
  e.formato_tuberia = e.formato;
  return r == VK_SUCCESS;
}

void DestruirSwapchain(Estado& e) {
  if (!e.dispositivo) return;
  vkDeviceWaitIdle(e.dispositivo);
  for (VkImageView v : e.vistas) vkDestroyImageView(e.dispositivo, v, nullptr);
  for (VkSemaphore s : e.terminada) vkDestroySemaphore(e.dispositivo, s, nullptr);
  e.vistas.clear();
  e.terminada.clear();
  e.imagenes.clear();
  if (e.swapchain) vkDestroySwapchainKHR(e.dispositivo, e.swapchain, nullptr);
  e.swapchain = VK_NULL_HANDLE;
}

bool CrearSwapchain(Estado& e) {
  VkSurfaceCapabilitiesKHR capacidades{};
  VERIFICAR(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(e.fisico, e.superficie, &capacidades));
  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(e.fisico, e.superficie, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formatos(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(e.fisico, e.superficie, &n, formatos.data());
  VkSurfaceFormatKHR elegido = formatos[0];
  for (const auto& f : formatos) {
    if (f.format == VK_FORMAT_R8G8B8A8_UNORM) {
      elegido = f;
      break;
    }
  }
  e.formato = elegido.format;
  e.extension = capacidades.currentExtent;
  if (e.extension.width == 0xFFFFFFFFu) {
    e.extension = {(uint32_t)ANativeWindow_getWidth(e.app->window), (uint32_t)ANativeWindow_getHeight(e.app->window)};
  }
  VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = e.superficie;
  info.minImageCount = std::max(3u, capacidades.minImageCount);
  if (capacidades.maxImageCount) info.minImageCount = std::min(info.minImageCount, capacidades.maxImageCount);
  info.imageFormat = elegido.format;
  info.imageColorSpace = elegido.colorSpace;
  info.imageExtent = e.extension;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = capacidades.currentTransform;
  info.compositeAlpha = (capacidades.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                            : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  info.clipped = VK_TRUE;
  VERIFICAR(vkCreateSwapchainKHR(e.dispositivo, &info, nullptr, &e.swapchain));
  uint32_t cuenta = 0;
  vkGetSwapchainImagesKHR(e.dispositivo, e.swapchain, &cuenta, nullptr);
  e.imagenes.resize(cuenta);
  vkGetSwapchainImagesKHR(e.dispositivo, e.swapchain, &cuenta, e.imagenes.data());
  for (VkImage imagen : e.imagenes) {
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = imagen;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = e.formato;
    v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView vista;
    VERIFICAR(vkCreateImageView(e.dispositivo, &v, nullptr, &vista));
    e.vistas.push_back(vista);
    VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore sem;
    VERIFICAR(vkCreateSemaphore(e.dispositivo, &s, nullptr, &sem));
    e.terminada.push_back(sem);
  }
  LOGI("swapchain %ux%u, formato %d, %u imagenes, transformacion %u", e.extension.width, e.extension.height,
       e.formato, cuenta, capacidades.currentTransform);
  return CrearTuberia(e);
}

bool IniciarVentana(Estado& e) {
  if (!e.instancia) {
    if (!CargarLibvulkan(e) || !CrearInstancia(e)) {
      return false;
    }
    if (!CrearDispositivo(e)) {
      return false;
    }
    e.inicio = e.ultimo_informe = Ahora();
  }
  VkAndroidSurfaceCreateInfoKHR s{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
  s.window = e.app->window;
  VERIFICAR(vkCreateAndroidSurfaceKHR(e.instancia, &s, nullptr, &e.superficie));
  VkBool32 soporta = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(e.fisico, e.familia, e.superficie, &soporta);
  LOGI("la cola %u %s presentar en la superficie", e.familia, soporta ? "puede" : "NO puede");
  return CrearSwapchain(e);
}

void CerrarVentana(Estado& e) {
  DestruirSwapchain(e);
  if (e.superficie) vkDestroySurfaceKHR(e.instancia, e.superficie, nullptr);
  e.superficie = VK_NULL_HANDLE;
}

// ---- Frame ---------------------------------------------------------------------------------------------------

void Barrera(VkCommandBuffer cb, VkImage imagen, VkPipelineStageFlags2 desde_etapa, VkAccessFlags2 desde_acceso,
             VkPipelineStageFlags2 hacia_etapa, VkAccessFlags2 hacia_acceso, VkImageLayout de, VkImageLayout a) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = desde_etapa;
  b.srcAccessMask = desde_acceso;
  b.dstStageMask = hacia_etapa;
  b.dstAccessMask = hacia_acceso;
  b.oldLayout = de;
  b.newLayout = a;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = imagen;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  d.imageMemoryBarrierCount = 1;
  d.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &d);
}

bool Dibujar(Estado& e) {
  VERIFICAR(vkWaitForFences(e.dispositivo, 1, &e.valla, VK_TRUE, UINT64_MAX));

  if (e.consultas && e.consultas_escritas) {
    uint64_t marcas[2] = {};
    if (vkGetQueryPoolResults(e.dispositivo, e.consultas, 0, 2, sizeof(marcas), marcas, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      e.gpu_ms_suma += (marcas[1] - marcas[0]) * e.periodo_marcas * 1e-6;
      ++e.gpu_muestras;
    }
  }

  uint32_t indice = 0;
  VkResult r = vkAcquireNextImageKHR(e.dispositivo, e.swapchain, UINT64_MAX, e.adquirida, VK_NULL_HANDLE, &indice);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    DestruirSwapchain(e);
    return CrearSwapchain(e);
  }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
    LOGE("vkAcquireNextImageKHR: %s", Resultado(r));
    return false;
  }
  VERIFICAR(vkResetFences(e.dispositivo, 1, &e.valla));

  float t = float(Ahora() - e.inicio);
  VkCommandBuffer cb = e.comandos;
  vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo comienzo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  comienzo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &comienzo);
  if (e.consultas) {
    vkCmdResetQueryPool(cb, e.consultas, 0, 2);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, e.consultas, 0);
  }
  Barrera(cb, e.imagenes[indice], VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkRenderingAttachmentInfo adjunto{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  adjunto.imageView = e.vistas[indice];
  adjunto.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  adjunto.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  adjunto.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  adjunto.clearValue.color = {{0.15f + 0.1f * std::sin(t * 0.7f), 0.15f + 0.1f * std::sin(t * 0.9f + 2.0f),
                               0.25f + 0.1f * std::sin(t * 1.1f + 4.0f), 1.0f}};
  VkRenderingInfo renderizado{VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderizado.renderArea = {{0, 0}, e.extension};
  renderizado.layerCount = 1;
  renderizado.colorAttachmentCount = 1;
  renderizado.pColorAttachments = &adjunto;
  vkCmdBeginRendering(cb, &renderizado);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, e.tuberia);
  VkViewport viewport{0, 0, float(e.extension.width), float(e.extension.height), 0, 1};
  VkRect2D tijera{{0, 0}, e.extension};
  vkCmdSetViewport(cb, 0, 1, &viewport);
  vkCmdSetScissor(cb, 0, 1, &tijera);
  vkCmdPushConstants(cb, e.disposicion, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &t);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRendering(cb);

  Barrera(cb, e.imagenes[indice], VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0,
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  if (e.consultas) {
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, e.consultas, 1);
    e.consultas_escritas = true;
  }
  vkEndCommandBuffer(cb);

  VkSemaphoreSubmitInfo espera{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  espera.semaphore = e.adquirida;
  espera.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphoreSubmitInfo senal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  senal.semaphore = e.terminada[indice];
  senal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo comando{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  comando.commandBuffer = cb;
  VkSubmitInfo2 envio{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  envio.waitSemaphoreInfoCount = 1;
  envio.pWaitSemaphoreInfos = &espera;
  envio.commandBufferInfoCount = 1;
  envio.pCommandBufferInfos = &comando;
  envio.signalSemaphoreInfoCount = 1;
  envio.pSignalSemaphoreInfos = &senal;
  VERIFICAR(vkQueueSubmit2(e.cola, 1, &envio, e.valla));

  VkPresentInfoKHR presentar{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  presentar.waitSemaphoreCount = 1;
  presentar.pWaitSemaphores = &e.terminada[indice];
  presentar.swapchainCount = 1;
  presentar.pSwapchains = &e.swapchain;
  presentar.pImageIndices = &indice;
  r = vkQueuePresentKHR(e.cola, &presentar);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
    DestruirSwapchain(e);
    return CrearSwapchain(e);
  }
  if (r != VK_SUCCESS) {
    LOGE("vkQueuePresentKHR: %s", Resultado(r));
    return false;
  }

  ++e.fotogramas;
  double ahora = Ahora();
  if (ahora - e.ultimo_informe >= 2.0) {
    double fps = e.fotogramas / (ahora - e.ultimo_informe);
    if (e.gpu_muestras) {
      LOGI("%s: %.1f FPS, GPU %.3f ms por fotograma", e.panvk ? "PanVK" : "sistema", fps,
           e.gpu_ms_suma / e.gpu_muestras);
    } else {
      LOGI("%s: %.1f FPS (sin marcas de tiempo de GPU)", e.panvk ? "PanVK" : "sistema", fps);
    }
    e.fotogramas = 0;
    e.gpu_ms_suma = 0.0;
    e.gpu_muestras = 0;
    e.ultimo_informe = ahora;
  }
  return true;
}

// ---- Main loop -----------------------------------------------------------------------------------------------

Estado estado;
bool activo = false;
bool fallo = false;

void AlComando(android_app* app, int32_t comando) {
  switch (comando) {
    case APP_CMD_INIT_WINDOW:
      if (app->window && !fallo) {
        activo = IniciarVentana(estado);
        if (!activo) {
          fallo = true;
          LOGE("PRUEBA FALLIDA: no se pudo iniciar el camino de Vulkan 1.3");
        }
      }
      break;
    case APP_CMD_TERM_WINDOW:
      activo = false;
      CerrarVentana(estado);
      break;
    default:
      break;
  }
}

}  // namespace

void android_main(android_app* app) {
  estado.app = app;
  app->onAppCmd = AlComando;
  LOGI("prueba de Vulkan 1.3 en marcha");
  while (!app->destroyRequested) {
    int eventos;
    android_poll_source* fuente;
    while (ALooper_pollOnce(activo ? 0 : -1, nullptr, &eventos, reinterpret_cast<void**>(&fuente)) >= 0) {
      if (fuente) fuente->process(app, fuente);
      if (app->destroyRequested) break;
    }
    if (activo && !Dibujar(estado)) {
      activo = false;
      fallo = true;
      LOGE("PRUEBA FALLIDA al dibujar");
    }
  }
  CerrarVentana(estado);
}
