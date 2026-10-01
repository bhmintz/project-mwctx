# Diagnóstico del "crash" al Jugar en Mali-G52 (Helio G80) — sesión 2025-10-01

> Continuación directa de `docs/NOTAS-compilacion-y-shaders.md` §0 ("problema abierto").
> Dispositivo: **Samsung Galaxy A32 (SM-A325M)**, SoC **MediaTek Helio G80**, GPU **Mali-G52 MC2**.
> Driver Vulkan: **ArmProprietary v1.r26p0-01eac0** (viejo), `vkGetPhysicalDeviceProperties` reporta
> **Vulkan 1.1.131**, conformance 1.2.2.0, vendor 0x13B5.
> Método: sesión ADB por USB + `adb logcat -b all`. (La captura inalámbrica no hizo falta.)

---

## 0. TL;DR — qué sabemos ahora

1. **Nunca fue un crash.** El proceso sale **limpio (`System.exit(0)`)**, sin SIGSEGV, sin tombstone,
   sin excepción Java. El juego decide salir solo cuando no puede montar Vulkan.
2. **Causa original del cierre:** el selector de dispositivo Vulkan **rechazaba** la Mali-G52 por no
   exponer `vertexPipelineStoresAndAtomics` (y luego `fillModeNonSolid`). Ambos requisitos se relajaron
   con parches (ver §2). **Con eso la app ya arranca y no se cierra.**
3. **Estado tras los parches:** la app abre, **se oye el sonido inicial**, se ve el **overlay de
   controles táctiles**, pero la **pantalla está en negro: no se dibuja nada**.
4. **Causa del negro (el muro real):** el **renderizador nativo ("C6")** exige un conjunto de
   capacidades Vulkan modernas que la Mali-G52 (driver r26p0) **no tiene**. Aborta en la primera que
   falta, `shaderInt64`, con el log explícito:
   ```
   E NFSMW-rex: [nativo] C6: el dispositivo Vulkan no tiene shaderInt64: no se dibuja
   E NFSMW-rex: [nativo] C6: no se pudieron preparar los dibujos nativos
   ```
   El audio y el overlay funcionan porque no pasan por ese camino de GPU.

**Conclusión honesta:** esto ya no es un filtro cosmético. El renderizador nativo está construido sobre
*bindless* + punteros de GPU (buffer device address) + enteros de 64 bits en shaders. Sin soporte del
driver, sus pipelines no se pueden crear. **No se arregla con un flag.**

---

## 1. Qué se hizo en esta sesión (en orden)

1. **Se inicializó git** en la carpeta del proyecto (antes no era repo):
   - `sdk/thirdparty/` añadido a `.gitignore` (381 MB / ~18.7k archivos, reproducibles con
     `tools/fetch_thirdparty.py`; no tiene sentido versionarlos).
   - Commit baseline `4c1c0a2` con todo el fuente antes de tocar nada.
2. **Parche 1** (commit `ee505b1`): no rechazar la GPU por `vertexPipelineStoresAndAtomics`.
3. **Parche 2** (commit `81286d1`): no rechazar la GPU por `fillModeNonSolid`.
4. Recompilado el APK con `build_android.ps1` (solo cambió C++ del SDK → no hizo falta re-ejecutar el
   codegen; `app/generated/` de la sesión anterior se reutilizó), reinstalado por `adb install -r`,
   relanzado y capturado logcat. Dos iteraciones (una por parche).

### Cómo revertir los parches
```bash
cd D:/users/projects/x360/extract-xiso-Win64_Release/nfsmw-android-main
git revert 81286d1 ee505b1      # deshace solo los parches, conserva el historial
# o volver al estado previo exacto:
git checkout 4c1c0a2 -- sdk/src/ui/vulkan/vulkan_device.cpp
```

---

## 2. Los tres filtros de features del dispositivo (en `CreateIfSupported`)

Archivo: `sdk/src/ui/vulkan/vulkan_device.cpp`, función `VulkanDevice::CreateIfSupported`, bloque
`if (with_gpu_emulation)`. Se evalúan en este orden y cada uno hacía `return nullptr`:

| Feature                          | Mali-G52 | Acción tomada                                              |
|----------------------------------|----------|-----------------------------------------------------------|
| `independentBlend`               | ✅ sí    | pasa                                                       |
| `fragmentStoresAndAtomics`       | ✅ sí    | pasa                                                       |
| `vertexPipelineStoresAndAtomics` | ❌ no    | **Parche 1**: warn y continúa (antes: rechazo)            |
| `geometryShader`                 | ✅ sí    | pasa (sorpresa: esta Mali sí lo expone)                   |
| `fillModeNonSolid`               | ❌ no    | **Parche 2**: warn y continúa (el motor cae a relleno sólido) |

Detalle importante: el cvar `vulkan_require_vertex_pipeline_stores_and_atomics` está **deprecated e
ignorado** en el código (el requisito es forzoso), por eso `vertexPipelineStoresAndAtomics` **tuvo** que
parchearse en fuente y no se podía apagar con un flag. `fillModeNonSolid` sí tiene cvar
(`vulkan_require_fill_mode_non_solid`) pero su default es `true`; se parcheó igual por consistencia.

**Por qué los parches son seguros para crear el dispositivo:** el macro `XE_UI_VULKAN_FEATURE(name)` hace
`enabled = supported`, así que una feature no soportada **no se solicita** en `vkCreateDevice` → no falla
con `FEATURE_NOT_PRESENT`. Solo se deja de rechazar el dispositivo.

Tras estos dos parches el dispositivo lógico se crea correctamente (se ve la lista completa de
"Vulkan device properties and enabled features" en el log).

---

## 3. El muro real: el renderizador nativo "C6" exige features que la Mali-G52 no tiene

Archivo: `app/src/nfsmw_nativo_dibujos.cpp`, `DibujosVulkanImpl::Inicializar()` (~línea 2157).
Lista de requisitos (aborta en el **primero** que falte):

```cpp
{propiedades.shaderInt64,                                   "shaderInt64"},                 // ❌ falta en Mali-G52
{propiedades.bufferDeviceAddress,                          "bufferDeviceAddress"},         // ❌ casi seguro falta
{propiedades.runtimeDescriptorArray,                       "runtimeDescriptorArray"},      // ❌ casi seguro falta
{propiedades.shaderSampledImageArrayDynamicIndexing,       "shaderSampledImageArray..."},
{propiedades.descriptorBindingPartiallyBound,              "descriptorBindingPartiallyBound"},
{propiedades.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImage..."},
{propiedades.descriptorBindingUpdateUnusedWhilePending,    "descriptorBindingUpdate..."},
```

En el log solo vemos el primero (`shaderInt64`) porque corta ahí, pero el resto (bufferDeviceAddress,
runtimeDescriptorArray y el stack de descriptor-indexing / *bindless*) son features de **Vulkan 1.2**
que un driver Mali **r26p0 / Vulkan 1.1** del 2020 prácticamente seguro tampoco expone.

Estas features solo se habilitan si el cvar `vulkan_native_shader_features` está activo
(`vulkan_device.cpp:674`), y aun así solo si el hardware las soporta. En la Mali-G52 no están, así que el
camino de dibujo nativo no puede inicializarse.

`app/src/nfsmw_video_present.cpp:25` tiene el mismo patrón para el vídeo: pide `shaderInt64`,
`bufferDeviceAddress`, `runtimeDescriptorArray`, `scalarBlockLayout`; si faltan, desactiva el vídeo.

### Por qué esto NO es un simple bypass
El renderizador nativo traduce el GPU Xenos usando punteros de GPU de 64 bits y descriptores *bindless*.
Los shaders generados asumen `Int64` y `buffer_reference`. Si se "quita" el requisito como hicimos con
los otros dos, el dispositivo pasaría la comprobación pero **las pipelines no compilarían / el driver
rechazaría los SPIR-V** (usan tipos de 64 bits y extensiones que la Mali no implementa). El resultado
sería, en el mejor caso, el mismo negro; en el peor, errores de validación o cuelgue del GPU.

---

## 4. Lo que SÍ funciona ya (progreso de esta sesión)

- ✅ La app **arranca y no se cierra** (antes salía al instante).
- ✅ **Audio inicial** suena.
- ✅ **Overlay de controles táctiles** visible.
- ✅ Dispositivo Vulkan creado; se enumeran límites y features reales de la Mali-G52.
- ✅ El juego recompilado **ejecuta lógica** (llega a cargar frontend, idioma español, sonido, etc.).
- ❌ **No se dibuja** (pantalla negra) por lo de §3.

### Observación secundaria (a verificar, probablemente benigna)
Hay muchos `W NFSMW-rex: [NtCreateFile] FAILED: path='D:\...' -> 0xc000000f` (STATUS_NO_SUCH_FILE):
`D:\fx\compiled\*.fx.o`, `D:\GLOBAL\GLOBALA.BUN`, `D:\FRONTEND\FRONTB.LZC`, sonidos, etc. Como el juego
igual progresa (suena audio, carga lógica), lo más probable es que sea el patrón normal de "probar la
ruta del disco original y luego caer al VFS/archivos extraídos". **No confundir con la causa del negro**,
que es la de §3. Si en el futuro se investiga carga de assets, empezar por confirmar si estos son
*probes* esperados o fallos reales de mapeo de rutas.

---

## 5. Opciones a futuro (sin prisa — el usuario pidió parar aquí)

Ordenadas de menos a más esfuerzo / realismo:

1. **Aceptar que esta GPU/driver no da para el renderizador nativo tal cual.** La Mali-G52 con driver
   r26p0 carece de bindless + bufferDeviceAddress + Int64, que son la base del renderizador. Es un muro
   de hardware/driver, no de configuración.
2. **Probar un driver más nuevo.** No hay Turnip para Mali (eso es solo Adreno). Dependería de que
   Samsung/Arm publicaran un driver actualizado para el A32, lo cual es improbable. Verificar si algún
   driver Mali más reciente expone `shaderInt64`/`bufferDeviceAddress` en G52 (dudoso en Bifrost de
   gama de entrada).
3. **Escribir un camino de render alternativo "sin bindless"** para GPUs antiguas: descriptores clásicos
   (sets fijos) en vez de descriptor-indexing, offsets de 32 bits en vez de buffer device address, y
   evitar `Int64` en shaders. Es **reescribir el núcleo del renderizador nativo**; trabajo mayor y puede
   que ni sea viable manteniendo la paridad con el Xenos.
4. **Revisar qué pasa con `vertexPipelineStoresAndAtomics` y `fillModeNonSolid`** (lo que apuntó el
   usuario): ahora mismo solo se "ignora" su ausencia. Si en el futuro se intenta dibujar de verdad en
   Mali, habrá que ver qué partes del render dependían de ellas (memexport desde vertex stage, modos de
   polígono línea/punto) y si el fallback a relleno sólido es correcto o hay que emular esas rutas.

**Realista:** la vía 1 es la conclusión práctica hoy. La vía 3 es la única que haría correr el juego en
esta GPU, y es un proyecto en sí mismo.

---

## 5bis. Prior art / referencias externas (investigado 2025-10-01)

### Dato de hardware exacto (Vulkan GPU DB, Mali-G52)
Separar límite de **hardware** (ningún driver lo dará) de **dependiente de driver**:

| Feature | Mali-G52 | Tipo |
|---|---|---|
| `shaderInt64` | ❌ | hardware (Bifrost sin int64 en shaders) |
| `vertexPipelineStoresAndAtomics` | ❌ | hardware (sin stores desde vertex stage) |
| `descriptorIndexing` / `runtimeDescriptorArray` (bindless) | ❌ | hardware |
| `fillModeNonSolid` | ❌ | hardware |
| `bufferDeviceAddress` | ⚠️ | driver: false en r26p0/Vk1.1 (este tel), true en 25.x/Vk1.2 |
| `geometryShader`, `fragmentStoresAndAtomics`, `shaderSampledImageArrayDynamicIndexing` | ✅ | — |

→ Implica: el **renderizador nativo** (Int64+bindless+BDA) es **inviable** en Mali-G52, no por
config sino por silicio. Actualizar driver no lo salva (sigue sin Int64/bindless/vertex-stores).

### Los dos backends de GPU en ESTE árbol
- **Renderizador nativo** (`app/src/nfsmw_nativo_*`, `sdk/src/ui/vulkan/`): pide Int64+bindless+BDA → muro.
- **Backend portable de Xenia** (`sdk/src/graphics/vulkan/command_processor.cpp`): revisado, solo pide
  `vertexPipelineStoresAndAtomics` + `fillModeNonSolid` (los mismos dos ya parcheados). **NO** pide
  Int64/bindless/BDA. Es el candidato real para Mali. Falta ver si es seleccionable para este juego
  (el port gatea lo nativo con `nfsmw::nativo::Activo()`, `app/src/nfsmw_ajustes_graficos.cpp:319`).

### Proyectos de referencia (no reinventar la rueda)
- **Xenia / xenia-canary** (upstream directo de este port): su backend Vulkan **convierte los vertex
  shaders de memexport a compute shaders** cuando el vertex stage no soporta stores → elimina la
  necesidad de `vertexPipelineStoresAndAtomics`. Equivalente en este árbol:
  `sdk/src/graphics/pipeline/shader/spirv_translator_memexport.cpp`. **Esta es la pieza clave a portar/activar.**
- **Dolphin** (`Source/Core/VideoBackends/Vulkan`): render Vulkan con descriptores clásicos que corre en
  Mali/Adreno. Referencia de gestión de descriptores sin bindless.
- **MoltenVK** (ya en `sdk/thirdparty/moltenvk`): patrones para emular features ausentes (p.ej. fillModeNonSolid→sólido).
- **Mesa PanVK/Panfrost**: driver Vulkan Mali open-source; NO intercambiable en A32 de stock, solo referencia.
- **Vita3K (emu de PS Vita)** — DATO DEL USUARIO: corre MK9 en ESTE mismo Mali-G52 (baja resolución +
  shaders precompilados). **Prueba de existencia** de que un emulador Vulkan dibuja bien en esta GPU.
  Transferible: su modelo de **descriptores clásicos + pipeline cache + precompilado de shaders**.
  NO transferible: la PS Vita (PowerVR SGX543) no tiene memexport tipo Xenos, así que Vita3K **no**
  resuelve el problema de `vertexPipelineStoresAndAtomics` — para eso, Xenia. Repo: github.com/Vita3K/Vita3K.

### Plan con prior art (si se retoma, sin prisa)
1. Ver si el juego puede ir por el **backend portable** en vez del nativo (leer el gate `nativo::Activo()`).
2. Traer el **memexport→compute** de Xenia para quitar el requisito de `vertexPipelineStoresAndAtomics`.
3. `fillModeNonSolid` → fallback a relleno sólido (patrón MoltenVK; ya trivial).
> Caveat: el nativo probablemente existe porque el portable era lento/incompleto para NFSMW. El portable
> podría ir lento en un G80 o no estar del todo cableado en este fork. Verificar, no es garantía.

---

## 5ter. Resultado del experimento con el backend portable (xenos) — 2025-10-01

Se probó forzar el backend portable de Xenia (`nfsmw_renderizador = "xenos"` en
`android/app/src/main/assets/nfsmw.toml`) + 3 parches en `sdk/src/graphics/vulkan/command_processor.cpp`
para que **degrade en vez de abortar** (commit `0698a6b`):
- init: no abortar por `vertexPipelineStoresAndAtomics` (línea ~940) ni por `fillModeNonSolid` (~958).
- IssueDraw: omitir (return true) los draws cuyo vertex shader hace memexport (~3674), en vez de abortar.

**Resultado: llegó mucho más lejos que el nativo, pero se cuelga.** Secuencia (run4):
- ✅ Swapchain creado (`VulkanPresenter: Created 2194x1017 / 2400x1080 swapchain`).
- ✅ VulkanTextureCache con fallbacks de formato para Mali (k_16_16, DXT*, etc.).
- ✅ Empieza la 1ª pipeline: `Creating graphics pipeline state with VS 6DD9DD04… , PS 2B4F1D2C…` (12:07:36).
- ❌ **Se cuelga ahí.** El hilo de render no registra nada más en ~80 s; solo sigue el audio, que a los
  ~80 s también se atasca ("[audio] … 250 ms sin avanzar; entra en rescate").
- El proceso NO crashea: lo cierra el usuario → `ActivityManager: Killing … (adj 905): remove task`
  (`remove task` = quitado de recientes; no es OOM, ni watchdog, ni SIGSEGV).

**Diagnóstico:** el backend portable se bloquea **dentro del driver Mali compilando la primera
pipeline/shader** (`vkCreateGraphicsPipelines` sobre el SPIR-V traducido por Xenia). El driver r26p0 no
digiere esos shaders en tiempo razonable. Por eso no renderiza y, a diferencia del nativo, **tampoco
suena** (el hilo de render bloqueado acaba congelando el proceso).

**Conclusión (los dos caminos tapiados en Mali-G52):**
- **Nativo**: la Mali no tiene `shaderInt64`/bindless/BDA → ni prepara los dibujos.
- **Xenos portable**: el driver Mali se cuelga compilando los shaders traducidos en runtime.
- Esto explica *por qué* existe el renderizador nativo: precompila shaders a `.nfsp` (XenosRecomp→DXC)
  justo para evitar la compilación en runtime que mata al driver Mali. Catch-22.

**Qué quedaría por intentar (todo mayor, sin garantía):**
1. Confirmar si es *cuelgue* o solo *lentísimo*: relanzar y esperar varios minutos (gratis, sin recompilar)
   a ver si la 1ª pipeline termina alguna vez y aparece un frame.
2. `async_shader_compilation` (cvar, default true): ver por qué no evita el bloqueo del hilo de render en
   la 1ª pipeline (¿el juego espera la pipeline antes de presentar?).
3. Reducir/simplificar los shaders traducidos para que el compilador Mali no se atore (trabajo en el
   traductor SPIR-V de Xenia) — difícil.
4. Driver Mali más nuevo (25.x/Vulkan 1.2) expone más y podría compilar mejor; no disponible en A32 de stock.

Estado del repo tras el experimento: commit `0698a6b` deja el modo xenos activo. Para volver al nativo:
`git checkout 0698a6b~1 -- android/app/src/main/assets/nfsmw.toml` (o revertir el commit). Los parches de
`command_processor.cpp` son inocuos si se vuelve a "nativo".

---

## 6. Datos de referencia (para no recapturar)

- GPU: `Mali-G52 MC2`, vendor `0x13B5`, device `0x74021000`.
- Driver: `ArmProprietary`, `v1.r26p0-01eac0.455662e55e7c7fb95a4b1db7e7af49a8`, Vulkan `1.1.131`.
- Features presentes relevantes: `independentBlend`, `fragmentStoresAndAtomics`, `geometryShader`,
  `tessellationShader`, `samplerAnisotropy`, `fragmentStoresAndAtomics`, MSAA hasta 4x en color/depth.
- Features AUSENTES clave: `vertexPipelineStoresAndAtomics`, `fillModeNonSolid`, `shaderInt64`,
  `bufferDeviceAddress`, `runtimeDescriptorArray`, y el stack de descriptor-indexing (bindless).
- Paquete: `com.nfsmw.android` (actividad `.MainActivity`). Instalado en user 0 (el teléfono tiene un
  perfil secundario user 150 que da `SecurityException` en `pm list --user 150`; ignorar, usar user 0).
- Logs crudos de esta sesión guardados en el scratchpad temporal (no versionados):
  `crash_g80.txt` (run original), `run2.txt`, `run3.txt`.

### Comandos útiles
```bash
# Dispositivo autorizado:
adb devices                                   # debe decir "device", no "unauthorized"
# Reinstalar tras compilar:
adb install -r android/app/build/outputs/apk/release/app-release.apk
# Capturar el arranque:
adb logcat -b all -c && adb logcat -b all -v threadtime > run.txt   # (Ctrl-C tras reproducir)
# Filtro clave del motor:
#   grep "NFSMW-rex" run.txt | grep -iE "no se dibuja|No se pudo|doesn't support"
```
