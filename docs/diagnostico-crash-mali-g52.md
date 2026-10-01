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
