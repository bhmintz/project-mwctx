# Plan: probar PanVK (Mesa) en lugar del driver Vulkan de Samsung

Plan para cargar el driver PanVK de Mesa desde la app en el Galaxy A32 4G y compararlo con el driver de ARM.
Escrito el 2026-10-03 para seguir en otra conversación. Contexto del backend: [backend-mali.md](backend-mali.md) y
[plan-optimizacion-mali.md](plan-optimizacion-mali.md).

## El teléfono y el punto de partida

- Samsung Galaxy A32 4G (SM-A325M), Android 13, 4 GB de RAM, Helio G80, **Mali-G52 MC2** (Bifrost).
- Driver actual: el de ARM/Samsung, **Vulkan 1.1.131**. Le faltan: estado dinámico extendido, renderizado
  dinámico, `push_descriptor`, `bufferDeviceAddress`, `shaderInt64`, indexado de descriptores y marcas de
  tiempo de GPU (`timestampValidBits 0`). Por eso existe el "modo Mali" (`modo_mali_`, `nfsmw_nativo_mali`).
- Estado al 2026-10-03 (commits locales `3d5b8cf`, `1353b94`, `dee36a3`, `14efb1c`, sin push): la **GPU** es
  el límite (98 % ocupada; Android la topa de 950 a 586 MHz por temperatura). El hilo del anillo va a
  ~9 µs por dibujo y pasa 5-15 ms por fotograma esperando a la GPU. 896×504 sin tope y bilineal: 35-48 FPS
  en carrera.

## El driver

- [JimVulkan/mali-panvk](https://github.com/JimVulkan/mali-panvk): port de PanVK para Bifrost que habla con
  el driver de kernel que ya trae el teléfono (`mali_kbase`, `/dev/mali0`). **Sin root ni kernel propio.**
  Expone Vulkan 1.3 en Bifrost. Probado en un Galaxy A31 (Mali-G52 MC2, Helio P65), muy parecido al A32.
- Se distribuye como zip con `meta.json`, `libvulkan_panfrost.so` y `NOTICE.txt`, el formato de los
  emuladores que cargan drivers propios desde un zip.
- Limitaciones conocidas: **sin texturas BCn** (el modo Mali ya las descomprime en CPU o usa la caché ETC2),
  y lo que vkd3d-proton necesita para D3D12 (no nos afecta).
- Licencia: MIT (los archivos de Mesa conservan la suya).
- Alternativas parecidas: [mesa-unified-drivers](https://github.com/JimVulkan/mesa-unified-drivers),
  [JICA98/panvk-kbase-android](https://github.com/JICA98/panvk-kbase-android),
  [TSZ-Jin/panvk-mtk-driver](https://github.com/TSZ-Jin/panvk-mtk-driver) (MediaTek).

## Qué esperar

| | Driver de ARM | PanVK |
|---|---|---|
| Vulkan | 1.1 | 1.3 |
| Medir cada pase en la GPU | No (solo desglose por vallas, que infla) | Probablemente sí, con marcas de tiempo (verificar) |
| CPU por dibujo | Modo Mali, ~9 µs | Podría usar el camino de PC/Switch (BDA, bindless) |
| Texturas BC | Descompresión en CPU / ETC2 | Igual |
| Velocidad de la GPU | Compilador de ARM | Compilador de Mesa: sin comparaciones publicadas |

Estimación honesta: de **−20 % a +20 %** en FPS. Como manda la GPU, decide la calidad del compilador de
shaders de Mesa para Bifrost frente al de ARM, y eso solo se sabe probando. Lo más seguro es ganar una
herramienta de medición mucho mejor y menos tirones de CPU.

## Pasos

1. **Conseguir el driver** (descargar necesita permiso explícito del usuario): el zip de la última versión
   de `JimVulkan/mali-panvk`. Revisar el tamaño antes y no meterlo en git (ignorarlo como los headers
   SPIR-V derivados).
2. **Cargarlo desde la app**, opción del launcher "Driver Vulkan: del sistema / PanVK":
   - Copiar `libvulkan_panfrost.so` a la carpeta de la app y `dlopen` desde ahí.
   - Tomar `vk_icdGetInstanceProcAddr` (ICD) y pasárselo al cargador de funciones del SDK en vez del
     `vkGetInstanceProcAddr` del sistema. Revisar dónde lo obtiene el SDK
     (`sdk/src/ui/vulkan/`, `sdk/include/rex/ui/vulkan/functions/*.inc`) y si hace falta la negociación de
     interfaz del ICD (`vk_icdNegotiateLoaderICDInterfaceVersion`).
   - La WSI de Android (superficie de `ANativeWindow`) la implementa el driver.
   - Si falla, alternativa: el método de los emuladores (gancho del linker estilo `libadrenotools`).
3. **Arrancar con el mismo modo Mali** (el `vendorID` sigue siendo ARM `0x13B5`) para comparar igual con igual.
4. **Medir en la misma carrera** con el contador de FPS y el log, leyendo la frecuencia de la GPU:
   `adb shell "cat /sys/kernel/gpu/gpu_busy /sys/kernel/gpu/gpu_clock /sys/kernel/gpu/gpu_max_clock"`.
   Comparar FPS por log con el script que cuenta los Swaps de la línea "frontal perezoso" cada 10 s.
5. **Si anda bien:** comprobar las marcas de tiempo de GPU (log "C2: marcas de tiempo") para medir cada pase
   de verdad; después ir sacando partes del modo Mali según lo que exponga el driver (sets persistentes,
   barreras, conversión de índices, etc.).
6. **Si va peor:** dejarlo como opción experimental (o quitarlo) y volver al plan de GPU: A/B por shader de
   la escena (`nfsmw_nativo_mali_ab_omitir_ps = "0x<huella>"`) y variante de la pasada final `19C0C358`.

## Avance (2026-10-03)

- Paso 1 hecho: `mali-panvk-26.3.0-devel-r4.zip` (v1.3.0, 3,4 MB) en `out/panvk/` (ignorado) y en el teléfono en
  `nsfmw-androidevolved/drivers/`. La `.so` exporta solo `vk_icdGetInstanceProcAddr`,
  `vk_icdGetPhysicalDeviceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion` y `HMI` (módulo HAL de
  Vulkan). Necesita `libhardware.so` (solo `hw_get_module`), `libsync.so`, `libnativewindow.so`, `liblog.so`.
- Paso 2, primer intento (carga directa como ICD, `vk_icdGetInstanceProcAddr`): carga, pero sin
  `VK_KHR_android_surface` (8 extensiones de instancia). En Android la superficie y el swapchain los pone el
  cargador del sistema (`libvulkan.so`) sobre `VK_ANDROID_native_buffer` del driver; por eso el zip exporta `HMI`.
- Paso 2 hecho con **libadrenotools** (BSD-2, clonado en `out/libadrenotools`, ignorado): carga otra copia del
  `libvulkan.so` del sistema y redirige su `dlopen` de `vulkan.mt6768.so` a PanVK. No usa nada de Adreno.
  - SDK: cvar `vulkan_icd_android` + `SetAndroidVulkanLoaderOpener` (`sdk/include/rex/ui/vulkan/instance.h`,
    `vulkan_instance.cpp`); si falla, vuelve al driver del sistema.
  - App: `android/app/src/main/cpp/android_driver_vulkan.cpp`, CMake (`NFSMW_ADRENOTOOLS_DIR`),
    `useLegacyPackaging true` en `build.gradle` (los ganchos se cargan desde `nativeLibraryDir`; el APK pasó de
    65 a 30 MB), y en el launcher "DRIVER GRÁFICO → Driver Vulkan" (`VulkanDriver.java` copia la `.so` del zip
    de `drivers/` a `files/drivers/`, porque el almacenamiento externo es noexec).
  - Resultado en el A32: **Vulkan 1.3.358**, `driverID MesaPanvk`, "Mali-G52 r1 MC2", Mesa 26.3.0-devel
    (avisa "panvk is not well-tested on v7"). El juego corre (videos, menú, ~30 FPS contados), pero **la pantalla
    queda negra**. Los búferes que recibe SurfaceFlinger son RGBA8888 lineales sin compresión: el problema es
    el contenido, no el formato. Tampoco hay marcas de tiempo de GPU (`timestampValidBits 0`, igual que ARM).
- **App de prueba** `pruebas/vk13/` (proyecto Gradle aparte, NativeActivity en C++, paquete
  `com.nfsmw.pruebavk13`; lleva PanVK como asset desde `out/panvk` y libadrenotools desde `out/libadrenotools`):
  dibuja un triángulo solo con caminos de 1.3 (`vkCmdBeginRendering`, `vkCmdPipelineBarrier2`,
  `vkQueueSubmit2`). En el A32 **funciona**: se ve el triángulo, ~81 FPS con FIFO, tubería en 4,6 ms.
  Reporta 1.3 completo (dynamicRendering, synchronization2, maintenance4…), `bufferDeviceAddress`,
  `shaderInt64`, `vertexPipelineStoresAndAtomics`, `fillModeNonSolid`, `push_descriptor`,
  `extended_dynamic_state` 1-3, `graphics_pipeline_library`, `timelineSemaphore`; **no** tiene
  `descriptorIndexing`, `descriptorBindingPartiallyBound`, `shaderFloat16` ni BCn; marcas de tiempo
  `timestampValidBits 0` (pero ofrece `VK_KHR_calibrated_timestamps` y `VK_KHR_shader_clock`). 153
  extensiones de dispositivo. Mesa usa "fallback gralloc".
  Para compilar: `cd pruebas/vk13 && ./gradlew assembleRelease -q` (copiar `local.properties` de `android/`).
- Conclusión: el driver y la presentación funcionan; la pantalla negra del juego es algo del camino del juego.
- **Causa del negro:** `DecidirModoMali` (`nfsmw_nativo_dibujos.cpp`) apaga el modo Mali si hay `shaderInt64`,
  `bufferDeviceAddress` y `runtimeDescriptorArray`, y PanVK los tiene: el juego tomaba el camino de PC, que pide
  bindless completo (PanVK no tiene `descriptorIndexing`/`PartiallyBound`) y BC, con la biblioteca de shaders Mali.
  Con `--nfsmw_nativo_mali=1` en `nfsmw_args.txt` **el juego se ve bien con PanVK** (2026-10-03).
- Pendiente (ya resuelto, ver la línea anterior): averiguar por qué sale negro (¿render del juego o copia final al swapchain?). Empezar dibujando
  un color fijo en el presentador; los errores de gralloc4 del log (formato 0x38, sondeos de 4×4) parecen
  inofensivos.

## Modo Mali 1.3 (rama `vk1.3`)

Idea: no copiar el renderizador (~31.000 líneas), sino partir del modo Mali y cambiar solo lo que existe por
las limitaciones de 1.1, usando lo que 1.3 ya resuelve. El modo Mali 1.1 queda igual para el driver de Samsung.

- Hecho: con PanVK el modo Mali se elige solo. `DecidirModoMali` en Android también exige
  `descriptorBindingPartiallyBound` y update-after-bind para tomar el camino de PC.
- Límites de PanVK medidos con la app de prueba: `maxPushDescriptors` **32**, `maxBoundDescriptorSets` 4,
  `maxPerStageDescriptorSampledImages` 256, samplers 128, push constants 256 bytes, UBO dinámicos por set 16,
  `maxUniformBufferRange` 1 MB, `maxInlineUniformBlockSize` 64 KB, update-after-bind 0.
- **Descartado por ahora: `push_descriptor`.** El set por dibujo del modo Mali tiene 96 descriptores
  (32 2D, 32 3D, 16 cubos, 16 samplers) y el tope es 32. Haría falta otra variante de shaders con menos
  ranuras; y los sets persistentes ya dejaron la creación de sets cerca de cero.
- Ya existe, de la Switch (NVK, 1.3), y se puede probar sin código: estado dinámico EDS1/EDS2
  (`nfsmw_nativo_estado_dinamico`) y EDS3 de mezcla (`nfsmw_nativo_estado_dinamico3`). Con PanVK los dos salen
  "disponible". En la Switch fueron una pérdida neta (bind más caro), por eso están apagados: medir aquí.
- Pendiente, en orden:
  1. Medición base en la misma carrera y las mismas opciones: driver de Samsung contra PanVK, ambos en modo Mali.
  2. (Otro día, decisión del usuario) A/B del estado dinámico (las dos cvars) con PanVK.
  3. **Hecho (2026-10-03):** renderizado dinámico en los pases del anillo y en los borrados por pase
     (`app/src/vk13/nfsmw_vk13_pases.h`, cvar `nfsmw_nativo_mali_vk13`, prendida por defecto). Sin `VkRenderPass`
     ni `VkFramebuffer`; los pipelines se crean con `VkPipelineRenderingCreateInfo`; las dependencias externas de
     `DependenciasImagenes` pasan a barreras de `synchronization2`. El SDK habilita `dynamicRendering` y
     `synchronization2` solo en Android con un dispositivo 1.3. Probado con PanVK: se ve igual que con render
     passes y los FPS son iguales (16 con opciones altas en el mismo lugar; el límite es la GPU).
     La salida a la pantalla (`PintarSalida` en `nfsmw_nativo_destinos.cpp`) también abre con renderizado
     dinámico: sin framebuffers por imagen del presentador, con las transiciones de layout que hacía su render pass
     (UNDEFINED → attachment → `kGuestOutputInternalLayout`) en barreras de imagen. Sus pipelines (normal y de
     rampa) se rehacen después de que los dibujos deciden el modo. Queda con render pass solo `LimpiarSalida`
     (borrado de emergencia de `nfsmw_nativo_sistema.cpp`).
     Aviso de PanVK en el log, inofensivo: `VK_ERROR_OUT_OF_POOL_MEMORY` cuando un pool de sets se llena y el
     modo Mali pasa al siguiente.
  4. **Hecho (2026-10-03):** `synchronization2` donde gana algo:
     - La barrera completa al empezar cada trabajo (ALL_COMMANDS → ALL_COMMANDS) pasa a las etapas y escrituras
       que de verdad dejan los envíos anteriores (imágenes: la GPU no escribe buffers) y, con
       `nfsmw_nativo_mali_sin_burbuja`, sin terminar en la etapa de vértices. La geometría del primer pase ya no
       espera a que el trabajo anterior termine de sombrear.
     - La copia de vuelta para resolver (`CopiarDeVueltaParaResolver`) igual, sin vértices.
     - Las barreras TRANSFER → TRANSFER entre copias y la de `Preparar` quedan en 1.1: en sync2 serían idénticas.
     - **Descartado:** semáforos timeline y `vkQueueSubmit2`. Hay una sola cola y las vallas por ranura ya esperan
       solo cuando hace falta; cambiarlas no saca trabajo a la GPU ni a la CPU.
     - Probado con PanVK: el log dice "C2 modo Mali 1.3: la salida también con renderizado dinámico", se ve bien
       y no hay errores de validación. A/B de corrección: `--nfsmw_nativo_mali_vk13=false` en `nfsmw_args.txt`.
     - Lo mide el usuario (FPS, tirones).
  5. Candidatos que quedan, de 1.3/extensiones que PanVK tiene: `VK_EXT_host_image_copy` (subir texturas sin
     buffer de subida ni comando de copia; menos RAM de staging) y `VK_EXT_memory_budget` (ajustar la caché de
     texturas al presupuesto real, contra los tirones de lmkd).
  6. **Volcado del compilador de Mesa (2026-10-03).** Cvars `vulkan_driver_env` (variables de entorno para el
     driver, `NOMBRE=valor;...`) y `vulkan_driver_stderr` (archivo que recibe stderr/stdout), por
     `nfsmw_args.txt`:
     `--vulkan_driver_env=BIFROST_MESA_DEBUG=shaderdb;MESA_SHADER_CACHE_DISABLE=true` y
     `--vulkan_driver_stderr=/storage/emulated/0/nsfmw-androidevolved/panvk_stderr.txt`.
     Resultado con la lista de precalentado (79 shaders distintos):
     - Todo está limitado por load/store, no por aritmética. Vértices: hasta 100 ciclos de ldst contra 22 de
       arith. Fragmentos: el peor tiene 73 de ldst contra 5 de arith.
     - Son las constantes: el modo Mali las pasa por un UBO indexado. Mesa solo sube a registros uniformes
       (FAU) lo que se indexa de forma estática y entra en el límite; el resto es una carga por acceso.
     - 14 de 30 shaders de vértices y 8 de 49 de fragmentos usan más de 32 registros, así que corren 1 hilo
       en lugar de 2. Hay 48 spills en los de fragmentos.
     - Próximo paso posible: compactar por shader las constantes que usa con índice fijo, para que Mesa las
       empuje a FAU. Para comparar con ARM haría falta `malioc` (Arm Mobile Studio).
  6b. **Comparación con el compilador de ARM (2026-10-03).** `malioc` de Arm Performance Studio 2026.5 (instalado en
     `D:/arm/mali_offline_compiler/malioc.exe`, driver r51p0, Mali-G52 r1p0). Los 152 módulos SPIR-V de
     `out/nfsmw_shaders_mali.nfsp` se extraen buscando la firma de SPIR-V y se compilan con
     `--core Mali-G52 --vertex|--fragment x.spv --format json`.
     - Fragmentos: ARM tiene como máximo 3 ciclos de load/store; Mesa llega a 73. La mediana del ciclo limitante
       es 1 contra 8. ARM sube el UBO de constantes a registros uniformes (58 a 128 en uso); PanVK no lo hace.
     - Vértices: ARM los parte en variante de posición (mediana 3.3 ciclos) y de varyings (mediana 49, limitada por
       load/store); Mesa hace un solo shader con mediana 77.5. En ARM también los varyings quedan limitados por
       load/store: lecturas de constantes con índice dinámico.
     - Conclusión: la diferencia de rendimiento entre los drivers está en el compilador, sobre todo en los
       fragmentos. El empuje de UBO a FAU en PanVK (`pan_nir_opt_push_ubo`, que existe pero PanVK no activa) apunta
       justo ahí.
     - Mejoras de shaders encontradas de paso (sirven en los dos drivers): `docs/mejoras-shaders-mali.md` de la rama `master`.
     - **Binarios de ARM (camino 1, en la PC): funciona.** `tools/mali_arm/volcar_mbs2.py` extrae los 152 SPIR-V
       del `.nfsp`, corre malioc para el Mali-G52 (DLL `graphics/Mali-Gxx_r51p0-00rel0.dll`, que exporta
       `cmpbe_v2_*` por nombre) y con Frida (`pip install frida-tools`) guarda el MBS2 de cada shader y su volcado
       a C. Ese volcado usa las estructuras de leegao/mali-msb2-disassembler (`cmpbe_chunks.h`).
       - Salida en `out/mali_arm/` (ignorado por git): `spv/`, `mbs2/NNN_0.mbs2` + `.h`, y `stats/NNN.json`.
       - Hallazgos en Windows x64: `cmpbe_v2_compile_multiple_shaders(ctx, n, ...)`, salida en el argumento 10
         (puntero a un array de elementos de 72 bytes; en +16 el puntero al MBS2 y en +24 su tamaño). Hay que
         enganchar `LdrLoadDll`, porque malioc carga la DLL tarde. `Module.load` falla con 0x7e.
       - Contenido: fragmentos con 2 binarios (`EBIN`/`OBJC`); vértices con 3 (`CVER`, probablemente posición,
         varyings y completo). Además `FCST` (constantes fijas en el área de uniformes), `UBUF` (tamaños de los
         UBO: 0xe00 = `g_UboPixel`, 0x170 = `g_UboCompartidas`), `SYMB`/`SSYM` (símbolos) y `PDSC`/`VLKN`.
       - El driver de Samsung del teléfono (`/vendor/lib64/egl/libGLES_mali.so`) exporta la misma API
         `cmpbe_v2_*`, en ARM64 (camino 2, en el teléfono, sin hacer).
       - **(a) Hecho:** `tools/mali_arm/compilar_bidis.sh` arma el desensamblador de Bifrost de Mesa en WSL
         (`out/bidis/bidis`), y `tools/mali_arm/mbs2.py` lee los bloques del MBS2 y saca los `OBJC`. El código de
         ARM se desensambla limpio como Bifrost: cláusulas con `FMA`, `TEXC`, `LD_VAR_IMM`, `BRANCH` y lecturas
         de registros de uniformes `uN.wM`.
       - **Hallazgo:** en el fragmento 097 el binario principal casi no lee memoria: 3 loads, y usa los registros
         de uniformes u0 a u40. El segundo binario (3648 bytes) es un **shader piloto**: puras cargas (`LOAD`) y
         escrituras (`STORE`). Lee UBO y descriptores y arma, por dibujo, el bloque que después el shader
         principal recibe en registros. Así resuelve ARM las constantes: corre un trabajo chico antes de cada
         dibujo. Para usar los binarios en PanVK habría que correr también ese piloto con las entradas que
         espera. También aparece un uniforme interno de ARM, `gl_mali_TextureSizesFragment`. Los `FCST` coinciden
         con inmediatos del código, como el `0xf478700f` del `TEXC`.
       - **Piloto, leído (fragmento 097):**
         - `r2:r3` es un puntero a la tabla de descriptores de UBO del hardware (8 bytes cada uno: dirección y
           tamaño empaquetados, con chequeo de límites en `CSEL`). Lee las ranuras 3, 18 y 19. La 18 es
           `g_UboPixel`, porque su símbolo tiene ubicación 0x12; la 19 es probablemente `g_UboCompartidas`; la 3
           es interna de ARM.
         - `r0:r1` es el bloque de salida: escribe de +0x10 a +0x147, que son 82 palabras de 32 bits,
           exactamente los "82 uniform registers" de malioc. Los `FCST` son palabras fijas de ese mismo bloque.
         - `r0` a `r3` no se definen dentro del piloto: llegan precargados. Falta saber cómo los pone el driver
           de ARM, o anteponerle una cláusula propia que los cargue.
       - **En los 152:** 77 fragmentos con 2 binarios (principal y piloto), 12 fragmentos con 1 (sin piloto) y
         63 vértices con 3. Hay 140 pilotos; 73 hacen cuentas de punto flotante (la "uniform computation"), así
         que el piloto tiene que correr en la GPU, como trabajo de cómputo antes de cada dibujo. No se puede
         reemplazar por una copia en CPU.
       - Próximos pasos: (a) desensamblar un `OBJC` con el desensamblador de Bifrost de Mesa para confirmar que es
         código Bifrost listo para correr; (b) descifrar cómo ARM llena los registros de uniformes desde los UBO
         (símbolos de `g_UboPixel`, `FCST`) y qué piden los campos de `EBIN`/`PDSC`; (c) el adaptador en PanVK. leegao/mali-msb2-disassembler extrae con Frida el
       binario MSB2 que genera malioc.
  7. Cache de pipelines por driver: con `vulkan_icd_android` el archivo es
     `nfsmw_nativo_pipelines_<driver>.bin`, y la lista de precalentado se toma del archivo del driver del
     sistema la primera vez. Antes cada cambio de driver recompilaba todo y pisaba la cache del otro.
  8. Ya no se usa `useLegacyPackaging`: `VulkanDriver.java` copia los hooks de adrenotools (y su libc++) del
     APK a la carpeta del driver. El build ya no comprime los 42 MB de `libmain.so` en cada pasada.
  9. Lo que muestre la medición (por ejemplo, el compilador de Mesa frente al de ARM en los shaders caros).

## Reglas del proyecto que siguen valiendo

- Cambios en C++ solo detrás de `modo_mali_`, Android o cvars apagadas por defecto: no cambiar PC, Adreno ni Switch.
- Commit solo cuando el usuario lo pide, sin `.gitignore`, `android/app/build.gradle` ni
  `android/app/src/main/AndroidManifest.xml`.
- Pruebas cortas (30-40 s), saltear los videos; al compilar, instalar y abrir directo:
  `cd android && ./gradlew assembleRelease -q`,
  `adb install -r android/app/build/outputs/apk/release/app-release.apk`,
  `adb shell am start -n com.nfsmw.android/.MainActivity`.
- Hay dos entradas de adb del mismo teléfono: `export ANDROID_SERIAL=192.168.1.14:<puerto>`.
- Log: `adb logcat -v time -s NFSMW-rex:I NFSMW:I`.
