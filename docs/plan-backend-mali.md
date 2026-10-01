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
- Dónde (shaders): `shader_common.h:136-139` y `XenosRecomp/shader_common.h` —
  `Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);` (+ 3D, Cube, Sampler), indexado
  por `resourceDescriptorIndex` (`tfetch2D`, líneas 148+). Array **ilimitado** → `runtimeDescriptorArray`
  + update-after-bind + partially-bound (Mali ❌).
- Dónde (C++): `app/src/nfsmw_nativo_dibujos.cpp` `CrearDescriptores()` y la gestión del heap bindless;
  la lista de requisitos en `Inicializar()` (~línea 2157).
- Sustituto Mali: **array ACOTADO** (p.ej. `Texture2D g_Textures[256]`) indexado dinámicamente — la Mali
  **sí** soporta `shaderSampledImageArrayDynamicIndexing`. Gestionar los slots por frame (reusar/rotar),
  sin update-after-bind. Es más trabajo de C++ (pool de descriptores acotado, rebinding por lote) que de
  shader (cambiar la declaración del array a tamaño fijo).
- Esfuerzo: **medio-alto**. Es el núcleo del backend.

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

- **Fase 0 — preparación**: decidir mecanismo (sub-modo del nativo), añadir el gate y dejar que el
  dispositivo se cree en Mali (los parches de `vulkan_device.cpp` ya lo permiten).
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
