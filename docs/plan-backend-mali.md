# Plan: backend Vulkan para Mali-G52 (estilo Vita3K) — roadmap

> Objetivo: un **tercer camino de render** que funcione en Mali-G52 (Helio G80) y, en general, en GPUs
> móviles sin bindless/Int64/BDA. Filosofía tipo Vita3K: descriptores clásicos / acotados, shaders
> **precompilados** (no compilar en runtime), y aprovechar lo que la Mali sí hace bien (FP16, TBDR).
>
> Prerrequisito leído: `docs/diagnostico-crash-mali-g52.md` (por qué el nativo y el xenos están tapiados).
> Fecha: 2025-10-01.

---

## 0. Resumen ejecutivo

Los dos renderizadores actuales no sirven en Mali-G52:
- **Nativo**: exige `shaderInt64` + `bufferDeviceAddress` + bindless (runtime/update-after-bind) → la Mali
  no los tiene (hardware). No prepara dibujos.
- **Xenos (portable Xenia)**: arranca, pero **el driver Mali se cuelga compilando los shaders traducidos
  en runtime**. No llega a presentar un frame.

La vía viable es un **backend derivado del nativo** (que YA precompila shaders a `.nfsp`, evitando el
cuelgue del driver) pero **sustituyendo las 3 cosas que la Mali no soporta** por equivalentes clásicos.
La buena noticia: una ya existe a medias en el código.

### Qué sí tiene la Mali-G52 (se puede explotar)
`fragmentStoresAndAtomics` ✅, `geometryShader` ✅, `tessellationShader` ✅,
**`shaderSampledImageArrayDynamicIndexing` ✅** (clave), MSAA 4x ✅, `samplerAnisotropy` ✅, FP16 nativo (2×).

### Qué NO tiene (hay que evitar)
`shaderInt64` ❌, `bufferDeviceAddress` ❌, `runtimeDescriptorArray` + `descriptorBinding*` (bindless) ❌,
`vertexPipelineStoresAndAtomics` ❌, `fillModeNonSolid` ❌.

---

## 1. Las 3 incompatibilidades del nativo y su sustituto Mali

### (A) Constantes por puntero de 64 bits → UBO clásico  **[YA EXISTE A MEDIAS]**
- Dónde: `shaders/shader_common.h:77-84` define `PushConstants { uint64_t VertexShaderConstants;
  PixelShaderConstants; SharedConstants; }` como punteros (`vk::RawBufferLoad<T>(ptr + offset)`), que
  necesitan Int64+BDA.
- **Pero** ya hay vía alterna por spec constant `SPEC_CONSTANT_CONSTANTES_UBO` (`NFSMW_UBO`,
  `shader_common.h:92-113`): las mismas constantes como **UBOs dinámicos** en set 4 (bindings 0/1/2),
  sin Int64/BDA.
- Trabajo: forzar `SPEC_CONSTANT_CONSTANTES_UBO` siempre en modo Mali **y** eliminar/condicionar la
  declaración `uint64_t` del push-constant (declarar un uint64 en el SPIR-V puede exigir `shaderInt64`
  aunque no se use). Verificar con el validador que el SPIR-V resultante no pide `Int64`/`PhysicalStorageBuffer`.
- Esfuerzo: **bajo-medio** (la lógica ya está; es sellar la vía 64-bit).

### (B) Texturas bindless ilimitadas → array acotado con indexado dinámico  **[EL TRABAJO REAL]**

> **NO se reescribe el sistema de texturas del X360.** Solo cambia la "última milla": cómo una textura
> YA resuelta se expone al shader (bindless → descriptores clásicos). Detalle abajo.

**Lo que se REUTILIZA tal cual (es agnóstico de GPU y ya funciona en Mali):**
- Destiling/untiling, formatos Xenos (k_8_8_8_8, k_DXT1, k_10_11_11, swizzle del fetch constant, orden de
  bytes): `app/src/nfsmw_nativo_dibujos.cpp:1649+`.
- Resolver **fetch constant del juego → `VkImageView`**, con caché por registro, caché entre fotogramas,
  subida y `en_vuelo_`: `nfsmw_nativo_dibujos.cpp:2744` y alrededores (`VkImageView vista`, línea 2064).
- Fallbacks de formato para Mali: ya vistos funcionando en el run del xenos
  (`VulkanTextureCache: k_16_16 … via fallback format`).
- La lista de "qué texturas usa este draw" ya se calcula al resolver por fetch constant → se reaprovecha.

**Lo que CAMBIA (la capa de binding, 2 puntos de contacto):**
1. Shader: `shader_common.h:136-139` y `XenosRecomp/shader_common.h` —
   `Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);` (+ 3D, Cube, Sampler), indexado
   por `resourceDescriptorIndex` (`tfetch2D`, líneas 148+). Array **ilimitado** → `runtimeDescriptorArray`
   + update-after-bind + partially-bound (Mali ❌). Sustituir por slots fijos `g_Textures[slot]` o un array
   **ACOTADO** `g_Textures[N]` indexado dinámicamente (la Mali **sí** tiene
   `shaderSampledImageArrayDynamicIndexing`). Regenerar el `.nfsp`.
2. C++: `app/src/nfsmw_nativo_dibujos.cpp` `CrearDescriptores()` + el write del heap bindless. En vez de
   escribir el `VkImageView` en el array global y pasar índice, asignar un **descriptor set por draw/lote**
   con las N texturas del draw en bindings fijos (sets 2-3, patrón Vita3K §7.4) y vincularlo. Relajar la
   lista `requisitos[]` de `Inicializar()` (~línea 2157). Igual con los samplers
   (`g_SamplerDescriptorHeap[]`).

**Lo tedioso/arriesgado:** mapear `resourceDescriptorIndex` del juego → slot 0..N-1 por draw; gestionar el
**churn de descriptor sets por frame** (con streaming hay muchas texturas → pool con reset/rotación por
frame, patrón Vita3K); regenerar shaders y validar el muestreo.

- Esfuerzo: **medio-alto**. Es el núcleo del backend, pero es UNA capa delimitada (binding), no reescribir
  el decodificado/caché de texturas del X360.

### (C) memexport desde vertex shader → omitir o compute  **[ACOTADO]**
- La Mali no tiene `vertexPipelineStoresAndAtomics`. Los draws cuyo VS escribe memoria (memexport) no
  pueden ejecutarse tal cual.
- Opciones: (1) omitirlos (se pierden efectos puntuales) — como ya probamos en el xenos; (2) portar la
  conversión **memexport→compute** de Xenia upstream (`sdk/src/graphics/pipeline/shader/spirv_translator_memexport.cpp`).
- Esfuerzo: bajo si se omite; alto si se emula en compute. Empezar omitiendo.

### (D) fillModeNonSolid → relleno sólido  **[TRIVIAL]**
- Ya lo manejamos: line/point → solid. Sin impacto visual real.

---

## 2. Mecanismo de selección

Reusar el renderizador **nativo** (precompila shaders → evita el cuelgue del driver) y añadir un **sub-modo
Mali** que active los sustitutos de arriba, sin afectar a GPUs potentes:
- Detección: ya existe `GpuInfo` (Java) que clasifica Mali como "débil" (ver [[android-graphics-settings-flow]]).
  Añadir que, en GPU sin las features, el launcher pase los spec constants / cvars del modo Mali.
- En C++: relajar la lista `requisitos[]` (nfsmw_nativo_dibujos.cpp:2157) en modo Mali y ramificar
  descriptores (acotado vs bindless) y constantes (UBO forzado).
- En shaders: nuevos spec constants (`SPEC_CONSTANT_CONSTANTES_UBO` ya existe; añadir uno para "texturas
  acotadas" si hace falta diferenciar la declaración del array). Regenerar el `.nfsp` con la variante.
- Alternativa: un valor nuevo `nfsmw_renderizador = "mali"`, pero reusar "nativo" con flags es menos código.

---

## 3. Plan por fases (correctitud primero, luego velocidad)

- **Fase 0 — preparación** ✅ **HECHO** (ver §8): sub-modo del nativo vía cvar `nfsmw_nativo_mali`
  (-1 auto / 0 off / 1 on), gate `requisitos[]` relajado (solo exige `shaderSampledImageArrayDynamicIndexing`),
  BDA opcional y constantes UBO forzadas. El dispositivo ya pasa la inicialización en Mali.
- **Fase 1 — constantes sin 64-bit (A)**: forzar UBO, sellar la vía puntero, validar SPIR-V sin Int64/BDA.
- **Fase 2 — texturas acotadas (B)**: array fijo + indexado dinámico en shader; pool de descriptores
  acotado + rebinding por lote en C++; relajar `requisitos[]`.
- **Fase 3 — memexport (C)**: omitir los draws de memexport-vertex (reusar lo ya hecho en el xenos).
- **Fase 4 — que dibuje**: regenerar `.nfsp`, compilar, instalar, capturar logcat; iterar hasta frame.
- **Fase 5 — velocidad (Mali)**: FP16/mediump en los shaders (la gran palanca, ver
  `docs/NOTAS-compilacion-y-shaders.md §4`), pasadas *tile-friendly* (menos cambios de render target,
  evitar readbacks a mitad de pasada), resolución interna baja. Referencia de diseño: Vita3K.

---

## 4. Riesgos / incógnitas

- La Fase 2 (texturas) toca shader **y** C++ acoplados; es el grueso y donde más fácil romper algo.
- Aunque dibuje, puede ir lento en el G80 (por eso la Fase 5). El objetivo primero es *imagen*, no FPS.
- El `.nfsp` se genera en el dispositivo (XenosRecomp→DXC→packer en WASM); hay que validar que la variante
  Mali de los shaders compila y empaqueta bien.
- Algún otro punto del nativo podría asumir BDA/bindless fuera de los 3 sitios identificados; aparecerán al
  iterar (fase 4).

## 5. Por qué esto evita los dos muros anteriores
- **No** compila shaders en runtime (usa `.nfsp` precompilado) → esquiva el cuelgue del driver Mali (muro del xenos).
- **No** usa Int64/bindless/BDA → esquiva el muro de hardware del nativo.
- Usa solo lo que la Mali soporta (`shaderSampledImageArrayDynamicIndexing`, `fragmentStoresAndAtomics`,
  descriptores acotados, UBOs, FP16).

## 6. Referencias
- Vita3K (github.com/Vita3K/Vita3K): descriptores clásicos + pipeline cache + precompilado; corre en este G52.
- Xenia / xenia-canary: memexport→compute (`spirv_translator_memexport.cpp` equivalente en este árbol).
- Dolphin: backend Vulkan con descriptores clásicos en Mali/Adreno.
- MoltenVK (en `sdk/thirdparty/moltenvk`): patrones de emulación de features ausentes.

---

## 7. Vita3K: lógica concreta a reutilizar (open source)

Fuente estudiada: `vita3k/renderer/src/vulkan/pipeline_cache.cpp` (~44 KB) y la estructura de
`vita3k/renderer/src/vulkan/` (renderer.cpp, creation.cpp, texture.cpp, surface_cache.cpp) y
`vita3k/shader/src/` (spirv_recompiler.cpp).

> **Frontera de reutilización:** el recompilador de shaders de Vita3K (`spirv_recompiler.cpp`,
> USSE/GXM→SPIR-V) **no** se reutiliza — es otro ISA; seguimos con XenosRecomp. Lo que se basa en Vita3K es
> la **infraestructura**: caché en disco, pipeline cache, compilación asíncrona y el modelo de descriptores.

### 7.1 Caché de pipelines en disco (basar nuestro `nfsmw_nativo_pipelines.bin`)
- Vita3K: archivo `pipeline-cache-vk{version}.dat` = magic `0xBEEF4321` + nº de hashes (size_t) +
  array de hashes (uint64) + blob de `VkPipelineCache`. `read_pipeline_cache()` lo carga; se pasa a
  `vkCreateGraphicsPipelines` para que el driver reuse lo compilado entre arranques.
- **Mejora sobre Vita3K:** Vita3K NO valida que el blob sea del mismo GPU/driver. Nosotros debemos guardar y
  comparar `VkPhysicalDeviceProperties::pipelineCacheUUID` (o vendorID/deviceID/driverVersion) en la cabecera,
  e invalidar la caché si no coincide. Evita cuelgues/corrupción al cambiar de driver.

### 7.2 Caché de SPIR-V en disco
- Vita3K: `vk{version}-{sha256}.spv` (SPIR-V crudo) indexado por SHA-256; lista `shaders_cache_hashs`
  (pares vertex/fragment) para saber qué precargar; `precompile_shader()` carga del disco y crea el
  `vk::ShaderModule`. Nuestro `.nfsp` ya cumple este rol (XenosRecomp→DXC→packer); mantener.

### 7.3 Compilación asíncrona (RELEVANTE para el muro del xenos)
- Vita3K: pipelines en un **pool de hilos** (1–6 según CPU, cola lock-free moodycamel); shaders con centinela
  `shader_compiling = ~0ULL` + `std::this_thread::yield()`; `set_async_compilation()` lo activa.
- Por qué importa: en el experimento xenos el hilo de render se **congeló** compilando la 1ª pipeline. Con
  compilación async, la compilación lenta del driver Mali **no congela** el render/audio; se presenta cuando
  está lista (o un placeholder). No arregla un *deadlock* del driver, pero sí la *lentitud*. En el backend
  nativo revisar `async_shader_compilation` (cvar, ya existe) y asegurar que la 1ª pipeline no bloquea.

### 7.4 Descriptores clásicos (el modelo para la incompatibilidad B)
Layout de Vita3K (copiar la forma, adaptar contenido a Xenos):
- set 0: uniforms — UBO dinámico vertex + fragment (+ storage buffers si aplica).
- set 1: attachments — input attachment / storage image (para EDRAM/efectos).
- **sets 2–3: texturas — 0..16 `CombinedImageSampler` con bindings FIJOS** (no heap ilimitado), y una
  matriz de pipeline layouts `[vert_tex_count][frag_tex_count]` (Vita3K usa 17×17).
- Esto sustituye nuestro `g_Texture2DDescriptorHeap[]` (shader_common.h:136). En NFSMW, en vez de un índice a
  heap ilimitado, cada draw vincula sus N texturas en slots fijos del set. La Mali soporta indexado dinámico
  de arrays ACOTADOS (`shaderSampledImageArrayDynamicIndexing`), así que si se prefiere, se puede usar un
  array acotado indexado en vez de slots 1-a-1.

### 7.5 Fallbacks de features (Vita3K `creation.cpp`/`pipeline_cache.cpp`)
- Query por formato (`getFormatProperties`); formatos RGB de 3 componentes → padding a RGBA; atributos
  "scaled" no soportados → convertir en el shader; `wideLines` ausente → quitar el dynamic state.
- Mismo patrón para nuestros fallbacks Mali (fillModeNonSolid→solid, formatos de textura ya con fallback en
  el VulkanTextureCache del xenos, etc.).

---

## 8. Registro de progreso

### Fase 0 — sub-modo Mali del nativo (hecho)
Todo en `app/src/nfsmw_nativo_dibujos.cpp`. No toca el camino del nativo clásico (en una GPU potente
`nfsmw_nativo_mali` auto da `false` y nada cambia).

- **cvar `nfsmw_nativo_mali`** (INT32, default `-1`): -1 auto, 0 off, 1 forzado. Auto se activa cuando el
  dispositivo no tiene `shaderInt64`+`bufferDeviceAddress`+`runtimeDescriptorArray` pero sí
  `shaderSampledImageArrayDynamicIndexing` (perfil Mali-G52). Helper `DecidirModoMali()` + miembro `modo_mali_`.
- **Gate `requisitos[]` relajado** en `Inicializar()`: se parte en `requisitos_siempre[]` (solo
  `shaderSampledImageArrayDynamicIndexing`, único que el camino acotado necesita del hardware) y
  `requisitos_bindless[]` (Int64/BDA/runtimeDescriptorArray/descriptorBinding*), que en modo Mali solo
  emiten `WARN` en vez de abortar.
- **BDA opcional**: no se exige `vkGetBufferDeviceAddress` en modo Mali; `CrearBuferSubida()` no pide
  `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` ni `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` ni calcula
  `subida_direccion_` (queda 0; nadie la lee porque siempre se va por UBO).
- **Constantes UBO forzadas** (incompatibilidad A sellada por C++): `usar_ubo_ = modo_mali_ || cvar`, y el
  test A/B `alternar_ubo_s_` se desactiva en Mali (alternaba a modo puntero, inviable sin Int64/BDA).

> **Pendiente para que esto dibuje** (no es un fallo de la Fase 0, es el orden del plan): la biblioteca de
> shaders sigue declarando el push-constant `uint64_t` y el heap bindless, así que el SPIR-V aún pide
> Int64/bindless. Hasta la Fase 1 (sellar el `uint64_t` en `shader_common.h`) y la Fase 2 (texturas
> acotadas) + regenerar el `.nfsp`, el modo Mali inicializa pero las pipelines no compilarán en la Mali.
