# Handoff: backend Mali — Fase 1→4 (guía para continuar en local)

> Para retomar el trabajo en local tras el límite de la nube. Es la continuación directa de
> `docs/plan-backend-mali.md` y `docs/diagnostico-crash-mali-g52.md` (§5quater, el hito y las restricciones).
> Rama: `claude/relaxed-ride-zku3z6` (PR #1). Dispositivo objetivo: **Mali-G52 MC2, driver r26p0, Vulkan 1.1.131**.

---

## 0. Estado actual (qué YA está hecho y funciona)

En la rama, confirmado por logcat del dispositivo:
- **Memoria** (`85f74e2`, `sdk/src/system/xmemory.cpp`): resuelto el bucle de access-violation host↔guest.
  El juego **corre lógica y audio**.
- **Sub-modo Mali del nativo** (Fase 0, `nfsmw_nativo_dibujos.cpp`): cvar `nfsmw_nativo_mali` (-1 auto/0/1),
  `DecidirModoMali()`, `modo_mali_`; gate `requisitos[]` relajado; constantes por **UBO forzadas**; **sin BDA**
  (ni `vkGetBufferDeviceAddress`, ni `SHADER_DEVICE_ADDRESS`, ni `subida_direccion_`).
- **Features core-1.0 fuera del guard de 1.2** (`ccd3c70`, `sdk/src/ui/vulkan/vulkan_device.cpp`): en un
  dispositivo 1.1, `shaderInt64` / `shaderSampledImageArrayDynamicIndexing` / `pipelineStatisticsQuery` ya se
  leen. Resultado: **el nativo inicializa en Mali** (`[nativo] C6: sub-modo Mali = SI`).
- **toml** (`android/app/src/main/assets/nfsmw.toml`): `nfsmw_renderizador = "nativo"` + `nfsmw_nativo_mali = -1`.
- Tooling: `tools/mw.py` (build/install/run/log/codegen) y `tools/recompilar_mali.py` (lo mismo apuntando a Mali:
  deja el toml y `local.properties` listos).

### Capacidades reales del dispositivo (del logcat, NO asumir)
- ✅ `shaderSampledImageArrayDynamicIndexing` (¡clave! permite indexar un array **acotado** dinámicamente).
- ✅ `fragmentStoresAndAtomics`, `geometryShader`, `tessellationShader`, `samplerAnisotropy`, MSAA 4x.
- ❌ `shaderInt64`, `bufferDeviceAddress`, `runtimeDescriptorArray`, `descriptorBindingPartiallyBound`,
  `descriptorBindingSampledImageUpdateAfterBind`, `descriptorBindingUpdateUnusedWhilePending`,
  `vertexPipelineStoresAndAtomics`, `fillModeNonSolid`.
- ⚠️ **`maxPerStageDescriptorSampledImages = 256`** (límite duro sin update-after-bind).
- API **Vulkan 1.1.131** (no 1.2): afecta `-fspv-target-env` y qué extensiones hay.

## 0bis. CORRECCIÓN (sesión local 2026-10-01): el muro NO era la compilación de pipelines

Con una sonda (`[mali-sonda]`, `nfsmw_nativo_dibujos.cpp`: vigilante que informa cada 5 s en qué llamada al
driver está el anillo, y un log por paso de `Inicializar`) se vio que **nunca se llega a crear una pipeline**:
la Mali se colgaba dentro de **`vkCreatePipelineLayout`** (más de 110 s, sin error ni log del driver). Ese
layout era inválido en este dispositivo por dos razones:

1. **`maxBoundDescriptorSets = 4`**, y el layout usa **5 sets** (0-3 montones + 4 UBO). ← muro real.
2. Montones bindless de 4096+16+64 imágenes y 512 samplers con flags de *descriptor indexing*/UAB, cuando
   los límites son `maxPerStageDescriptorSampledImages = 256` y `maxPerStageDescriptorSamplers = 128`, y la
   extensión ni está habilitada.

Hecho ya (solo en `modo_mali_`): montones acotados `kCapacidadMontonMali = {160,16,64,96}` sin flags ni UAB,
con todos los slots rellenos con la textura/sampler vacíos (no hay PARTIALLY_BOUND); y un chequeo de
`maxBoundDescriptorSets` que hace **fallar limpio** en vez de colgar → el juego ya no se congela y **el
sonido vuelve** (pantalla negra: el nativo no inicializa).

Otros datos medidos: Vulkan 1.1 con solo `VK_KHR_spirv_1_4` → acepta SPIR-V ≤ 1.4, pero el DXC WASM del
instalador (`shaders/wasm/dxc_web.cpp:78`) fija `-fspv-target-env=vulkan1.2` (SPIR-V 1.5). La variante Mali
debe usar `-fspv-target-env=vulkan1.1spirv1.4`. El `dxc.exe` del Windows SDK no trae SPIR-V; hace falta el
del Vulkan SDK.

**Siguiente paso:** variante Mali de los shaders con **4 sets**, p. ej. samplers en el set 0 binding 1
(`[[vk::binding(1,0)]]`), 3D→set 1, cubo→set 2, UBO→set 3 (hoy `[[vk::binding(n,4)]]`, shader_common.h:92-94),
arrays acotados (Fase 2 §4.1) y sin `uint64_t` (Fase 1); y el C++ del modo Mali con el layout equivalente.

### 0ter. Estado al cierre de la sesión local (2026-10-01, noche)

Hecho y probado en el dispositivo:
- **Variante Mali de los shaders** (`NFSMW_MALI` en las 3 copias de `shader_common.h`): 4 sets (2D+samplers
  set 0, 3D set 1, cubo set 2, UBO set 3), montones acotados, sin `uint64_t` (el `1ull` de los texcoords
  incluido), `NFSMW_UBO` fijo. La biblioteca normal sale **byte a byte igual a la oficial** (comprobado).
- **`tools/biblioteca_shaders_mali.mjs`**: arma el `.nfsp` Mali con el pipeline del instalador, DXC nativo
  de WSL (`~/dxc`, release oficial v1.9.2609) con `vulkan1.1spirv1.4` y `spirv-val` del NDK; rechaza
  Int64/RuntimeDescriptorArray/PhysicalStorageBuffer. Lee el juego por `adb:` de a un archivo y deja los
  contenedores en `out/contenedores_cache` (después no hace falta ni el juego ni el celular).
- C++ modo Mali con el layout de 4 sets (`SetsMontones()`/`SetUbo()`).
- **BC (DXT) en CPU**: la Mali-G52 no muestrea BC; crear/copiar una imagen BC crasheaba dentro de
  `vkCmdCopyBufferToImage` (visto con simpleperf: el hilo del anillo girando en el manejador de señales).
  Ahora BC1-3→RGBA8, BC4→R8, BC5→RG8 en `SubirTextura`.
- Resultado: **el nativo corre en la Mali** (1300+ Swaps, 22k dibujos, 33 pipelines, sin cuelgues, con
  sonido), pero la pantalla sale **gris uniforme (#303030)**: siguiente cosa a investigar.

Pendiente / a revisar:
- Imagen gris: ¿qué llega a la salida? (comprobar que los dibujos escriben color, UBOs con datos, índices de
  textura dentro del techo, escrituras de descriptores con el set enlazado: sin UAB es inválido, Fase 2 §4.3).
- `android/app/build.gradle` tiene `debuggable true` TEMPORAL (para simpleperf): revertir antes de publicar.
- El manejador de excepciones (`exception_handler_posix.cpp`) reintenta para siempre un SIGSEGV del host
  dentro del driver en vez de abortar: convendría que crashee con tombstone.
- Los SPIR-V embebidos (`kSpirvResplandor*`) son bindless/vulkan1.2: no valen en Mali si se activan.

## 1. El muro y la meta

**Muro:** la Mali se **atasca dentro de `vkCreateGraphicsPipelines`** compilando la 1ª pipeline, porque el
SPIR-V del `.nfsp` todavía exige `Int64` (puntero de constantes de 64 bits) y `runtimeDescriptorArray` (los
heaps bindless `g_Texture*DescriptorHeap[]`). Síntoma: **0 pipelines creadas**, sin `causa 51/53`, ring sin
avanzar, 17 hilos congelados, audio sin tramas.

**Meta:** que el SPIR-V del `.nfsp` **no pida Int64 ni runtimeDescriptorArray**, y que el C++ del modo Mali
vincule texturas con **descriptores clásicos acotados** (set por draw, ≤256). Entonces la Mali podrá compilar
las pipelines y presentar imagen.

**Regla de oro:** TODO cambio de C++ va **detrás de `modo_mali_`** (o del cvar) para no tocar PC/Adreno/Switch,
que ya funcionan. Los cambios de shader van en una **variante Mali del `.nfsp`** (ver §2.3), no en la única.

---

## 2. Cómo compilar, regenerar el `.nfsp` y probar

### 2.1 APK (cambios de C++ en `app/src/` o `sdk/`)
```bash
python tools/recompilar_mali.py log     # prep (toml+local.properties) + build + install + run + logcat Mali
# o el general:  python tools/mw.py           (build+install+run)
```
Gradle recompila `app/src/*.cpp` y el SDK (incluye `vulkan_device.cpp`) vía `add_subdirectory(app)`.

### 2.2 Regenerar el `.nfsp` (cambios de shader en `shaders/`)
El `.nfsp` NO lo regenera el build del APK. Se hace aparte (necesita Vulkan SDK: DXC + spirv-val, y MinGW g++):
```bash
MESA=/ruta/a/mesa-switch shaders/nfsmw_regenerar_biblioteca_pcf.sh out/lib_mali /ruta/a/contenedores
```
Flujo del script: compila el traductor (`nfsmw_hlsl.cpp` + `XenosRecomp/shader_recompiler.cpp`, `-DNFSMW_RECOMP`)
→ traduce microcódigo a HLSL usando `shaders/XenosRecomp/shader_common.h` → **DXC** a SPIR-V → **spirv-val** →
empaqueta el `.nfsp`. En Android el instalador lo arma en el dispositivo con las herramientas WASM
(`tools/biblioteca_shaders.mjs`); el `.nfsp` resultante va en la carpeta del juego
(`files/nfsmw/user/nfsmw_shaders.nfsp` o junto a `game_root`).

> **Importante:** la línea DXC del script usa `-fspv-target-env=vulkan1.2` y spirv-val `--target-env vulkan1.2`
> (`nfsmw_regenerar_biblioteca_pcf.sh:104,107`). Para la variante Mali conviene **`vulkan1.1`** en ambos: así el
> toolchain rechaza en compilación cualquier capability que la Mali 1.1 no tenga (Int64, runtime arrays), y
> cazás el problema antes del dispositivo.

### 2.3 Mecanismo de la variante Mali del shader
`shader_common.h` lo comparten dos árboles: `shaders/XenosRecomp/shader_common.h` (el que usa el traductor) y
el del SDK. El cambio de shader se condiciona con un **macro de compilación** nuevo, p. ej. `NFSMW_MALI`,
pasado:
- al traductor: añadir `-DNFSMW_MALI` al `g++` de `nfsmw_regenerar_biblioteca_pcf.sh:24-27` cuando se arma la
  variante Mali;
- a DXC si el `#ifdef` está en el HLSL emitido (DXC acepta `-D NFSMW_MALI`).

Así el `.nfsp` normal (PC/Adreno/Switch) queda intacto y hay un **`.nfsp` Mali** aparte. El C++ elige cuál
cargar según `modo_mali_` (o se instala el Mali en ese dispositivo).

### 2.4 Logcat: qué mirar
```bash
grep -aE "sub-modo Mali|causa [0-9]+|pipeline [0-9]+ \(VS|VK_ERROR|no se dibuja|Swap|present" logcat.txt
```
- `pipeline 1 (VS n...)` apareciendo = ¡la Mali ya compila pipelines! (el gran hito siguiente).
- `causa 51` = módulo de shader rechazado (SPIR-V aún inválido). `causa 53` = pipeline no creada (layout/feature).
- Nada + ring parado = sigue atascada compilando (SPIR-V aún pide Int64/bindless).

---

## 3. Fase 1 — sellar el puntero de 64 bits (quitar `Int64` del SPIR-V)

**Dónde:** `shaders/XenosRecomp/shader_common.h` (y el espejo del SDK si aplica), líneas ~77-84:
```hlsl
struct PushConstants {
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
```
Declarar un `uint64_t` (aunque no se use) puede hacer que el SPIR-V pida la capability `Int64`. En modo Mali las
constantes ya van por UBO (`SPEC_CONSTANT_CONSTANTES_UBO`, `NFSMW_UBO`), así que el puntero no se usa.

**Qué hacer:** condicionar con `NFSMW_MALI` para que en la variante Mali **no exista** la declaración `uint64_t`
ni el `g_PushConstants`:
```hlsl
#ifndef NFSMW_MALI
struct PushConstants { uint64_t ...; };
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#endif
```
y que todas las ramas que leen por puntero (`vk::RawBufferLoad<...>(g_PushConstants...)`, macros en
`shader_common.h:99-113`) queden bajo `#ifndef NFSMW_MALI` o seleccionen siempre la rama UBO cuando `NFSMW_MALI`.
Hoy esas macros son ternarios `NFSMW_UBO ? <ubo> : <puntero>`; con `NFSMW_MALI` deben ser solo `<ubo>` para que
DXC no emita el `RawBufferLoad` (que arrastra `PhysicalStorageBuffer`/Int64).

**Validar:** regenerar con `-fspv-target-env=vulkan1.1` y comprobar que ningún `.spv` declara `OpCapability
Int64` ni `PhysicalStorageBufferAddresses` (`spirv-dis` | grep, o el chequeo del packer). spirv-val en 1.1 debe
pasar.

**C++:** nada nuevo (Fase 0 ya fuerza `usar_ubo_` y quita la BDA en `modo_mali_`). Solo asegurar que se carga el
`.nfsp` Mali.

**Esfuerzo:** bajo-medio. **Por sí sola NO da imagen** (falta el bindless de Fase 2), pero quita medio muro.

---

## 4. Fase 2 — texturas bindless → descriptores clásicos acotados (el grueso)

> NO se reescribe el sistema de texturas del X360 (decode/untiling/caché/resolución por fetch constant se
> reutiliza tal cual). Solo cambia la **última milla**: cómo una textura ya resuelta se expone al shader.

### 4.1 Lado shader (`shader_common.h`)
Líneas ~136-139:
```hlsl
Texture2D<float4>   g_Texture2DDescriptorHeap[]   : register(t0, space0);  // array ILIMITADO -> runtimeDescriptorArray
Texture3D<float4>   g_Texture3DDescriptorHeap[]   : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState        g_SamplerDescriptorHeap[]     : register(s0, space3);
```
En la variante Mali, declararlos **acotados** (bound), p. ej.:
```hlsl
#ifdef NFSMW_MALI
  #define NFSMW_MAX_TEX2D 32   // techo por draw; ajustar. Debe caber en maxPerStageDescriptorSampledImages=256
  Texture2D<float4>   g_Texture2DDescriptorHeap[NFSMW_MAX_TEX2D]   : register(t0, space0);
  Texture3D<float4>   g_Texture3DDescriptorHeap[4]                 : register(t0, space1);
  TextureCube<float4> g_TextureCubeDescriptorHeap[8]               : register(t0, space2);
  SamplerState        g_SamplerDescriptorHeap[NFSMW_MAX_SAMP]      : register(s0, space3);
#else
  ... (los [] de siempre)
#endif
```
Las funciones `tfetch2D/3D/Cube/...` siguen indexando por `resourceDescriptorIndex`; el truco es que ese índice
pase a ser **0..N-1 del draw** (ver 4.3). Como la Mali tiene `shaderSampledImageArrayDynamicIndexing`, el índice
puede ser dinámico. Regenerar el `.nfsp` Mali y validar en vulkan1.1 (no debe pedir `runtimeDescriptorArray`).

### 4.2 Lado C++ — layout/pool sin bindless (`nfsmw_nativo_dibujos.cpp`, `CrearDescriptores()` ~8018)
Hoy (bindless): cada set 0-3 es un binding con `descriptorCount = kCapacidadMonton[i]` (`{4096,16,64,512}`) y
flags `PARTIALLY_BOUND | UPDATE_AFTER_BIND | UPDATE_UNUSED_WHILE_PENDING`, layout/pool con
`UPDATE_AFTER_BIND_POOL`.

En `modo_mali_`:
- `descriptorCount` = el techo acotado (p. ej. 32/4/8/16), **no** `kCapacidadMonton`. Suma de SAMPLED_IMAGE de
  los 3 sets ≤ 256.
- **Sin** `banderas_enlace` (quitar los tres `*_BIT`), **sin** `VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT`
  y **sin** `VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT`.
- `maxSets` del pool: ya no 4 fijos; necesitás **muchos sets por frame** (uno por draw/lote). Hacer un pool con
  reset/rotación por frame (patrón Vita3K: pool por ranura de trabajo, `vkResetDescriptorPool` al reciclar el
  frame). `kRanurasDeTrabajo` ya existe.

### 4.3 Lado C++ — escribir/vincular por draw (lo tedioso/arriesgado)
Hoy las texturas se escriben en el heap global con `UPDATE_AFTER_BIND` **durante la grabación** (ver
`RecogerEnlaces`/`AntesDeEnviar`, comentario ~5605 "UPDATE_AFTER_BIND: legal until submission"), y al shader se le
pasa el índice de heap. Sin UAB eso no es legal: hay que **escribir antes de vincular**.

Plan (patrón Vita3K §7.4):
1. Al resolver el draw ya se conoce su lista de texturas (fetch constants → `VkImageView`). Esa lista suele ser
   un puñado.
2. Asignar un **descriptor set del pool del frame**, escribir en slots 0..N-1 las N vistas del draw (y sus
   samplers), **antes** de `vkCmdBindDescriptorSets`.
3. Remapear: donde el draw pasaba `resourceDescriptorIndex` (índice al heap grande), pasar ahora el **slot
   0..N-1**. Ese remapeo es lo que el shader indexa. Guardar un mapa `fetchConstant→slot` por draw.
4. Vincular ese set (sets 2-3, o 0-2 según el layout) y dibujar.

Puntos de contacto en el código (buscar por nombre): la función que escribe el heap y pasa el índice
(alrededor de `montones_`, `CrearDescriptores`, y el write de `VkImageView` en el array global), y donde se
resuelve el fetch constant → `VkImageView` (`nfsmw_nativo_dibujos.cpp` ~2744 y la `VkImageView vista` ~2064,
según el plan original). Todo el decode/caché X360 se reutiliza; solo cambia esta capa de binding.

**Esfuerzo:** medio-alto. Es el núcleo. Riesgo: el churn de sets por frame y el remapeo. Empezá por hacerlo
**correcto** (pool amplio, reset por frame), optimizar después.

---

## 5. Fase 3 — memexport desde vertex shader

La Mali no tiene `vertexPipelineStoresAndAtomics`. Los draws cuyo VS escribe memoria (memexport) no corren.
**Empezar omitiéndolos** (como en el experimento xenos: `sdk/src/graphics/vulkan/command_processor.cpp` ~3674
hacía `return true` en esos draws). En el nativo, detectar el caso y saltar el draw (perdés efectos puntuales,
no la imagen). Emular memexport→compute (Xenia, `spirv_translator_memexport.cpp`) es opcional y mucho mayor.

`fillModeNonSolid` → relleno sólido: ya manejado (parche Mali-G52).

---

## 6. Fase 4 — que dibuje e iterar

1. Regenerar el `.nfsp` Mali (Fase 1+2) y verificar SPIR-V sin Int64/runtimeDescriptorArray (spirv-val 1.1).
2. `python tools/recompilar_mali.py log`, instalar el `.nfsp` Mali en el dispositivo.
3. Logcat: buscar `pipeline 1 (VS n...)` (compila), luego `Swap`/present (dibuja). Iterar sobre `causa 51/53`.
4. Cuando dibuje: Fase 5 del plan (FP16/mediump, pasadas tile-friendly, baja resolución).

---

## 7. Decisiones abiertas / riesgos

- **uint64 (Fase 1):** recomendado condicionar con `NFSMW_MALI` (mantiene el A/B de punteros en PC). Alternativa:
  quitar el puntero del todo (más simple; UBO ya es el default). Sin preferencia del usuario aún.
- **Techo de texturas por draw (NFSMW_MAX_TEX2D):** elegir el menor que no recorte (medir cuántas texturas usa
  el draw más pesado; el remapeo debe fallar ruidoso si se excede). Suma SAMPLED_IMAGE ≤ 256.
- **Variante .nfsp Mali:** confirmar que el builder WASM del dispositivo (`tools/biblioteca_shaders.mjs`) puede
  pasar `-DNFSMW_MALI` y `-fspv-target-env=vulkan1.1`; si no, construir el `.nfsp` Mali en PC y copiarlo.
- **slow vs deadlock:** test gratis (dejar la app quieta 2-3 min) para saber si la 1ª pipeline llega a compilar
  alguna vez; si sí, una compilación asíncrona ayudaría mientras tanto (no hay cvar `async_shader_compilation`
  en este árbol: habría que añadirla).
- Puede haber otros puntos del nativo que asuman BDA/bindless fuera de los 3 identificados; aparecerán al iterar.

---

## 8. Resumen de archivos clave
- `sdk/src/ui/vulkan/vulkan_device.cpp` — gate de features (ya arreglado el guard 1.1/1.2).
- `app/src/nfsmw_nativo_dibujos.cpp` — `DecidirModoMali`/`modo_mali_`, `Inicializar` (~2170), `CrearBuferSubida`
  (~7890), `CrearDescriptores` (~8018), escritura/bind de texturas (heaps, `RecogerEnlaces`).
- `shaders/XenosRecomp/shader_common.h` — push constant uint64 (~77), heaps bindless (~136), `tfetch*`.
- `shaders/nfsmw_regenerar_biblioteca_pcf.sh` — build del `.nfsp` (DXC `-fspv-target-env`, línea ~104).
- `android/app/src/main/assets/nfsmw.toml` — `nfsmw_renderizador="nativo"`, `nfsmw_nativo_mali`.
- `tools/recompilar_mali.py`, `tools/mw.py` — build/install/run/log/codegen.
