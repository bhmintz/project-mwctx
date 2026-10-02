# Mali-G52 MC2: lo que expone Vulkan

Capacidades reales del teléfono de referencia del [modo Mali](backend-mali.md), sacadas el 2026-10-02 con:

```bash
adb shell cmd gpu vkjson
```

(no necesita el APK depurable). Lo que importa para el port está en negrita, y al final, qué significa para el
[plan de optimización](plan-optimizacion-mali.md).

## Teléfono

| | |
|---|---|
| Modelo | Samsung Galaxy A32 4G (2021), **SM-A325M** |
| Android | **13** (API 33), parche de seguridad 2025-03-01 |
| SoC | MediaTek Helio G80 (`mt6768`) |
| CPU | 2 × Cortex-A75 (`0xd0a`) a 2,0 GHz + 6 × Cortex-A55 (`0xd05`) a 1,8 GHz |
| RAM | **4 GB** (3,73 GB visibles para el sistema) |
| OpenGL ES | 3.2 |

## Dispositivo Vulkan

| | |
|---|---|
| GPU | Mali-G52 MC2 (vendor `0x13B5` Arm, device `0x74021000`) |
| Driver | `v1.r26p0-01eac0` (Arm propietario, `driverID` 9) |
| API del dispositivo | **Vulkan 1.1.131** (el loader de Android es 1.3, pero el driver es 1.1) |
| Conformidad | CTS 1.2.0.2 |
| Colas | **1 familia con 2 colas**, cada una gráficos + cómputo + transferencia; `timestampValidBits = 0` |
| Subgrupos | Tamaño 8; en fragment y compute |

## Memoria

Un solo heap de **3,36 GB** (memoria unificada con la CPU) y tres tipos:

| Tipo | Propiedades | Uso |
|---|---|---|
| 0 | DEVICE_LOCAL + HOST_VISIBLE + HOST_COHERENT | Todo lo normal: texturas, búferes de subida |
| 1 | DEVICE_LOCAL + HOST_VISIBLE + **HOST_CACHED** | Lecturas de la CPU (lecturas de vuelta de texturas resueltas) |
| 2 | DEVICE_LOCAL + **LAZILY_ALLOCATED** | **Adjuntos transitorios**: la memoria solo existe si el tile la necesita |

`maxMemoryAllocationSize` = 3,36 GB; sin tope práctico en número de asignaciones.

## Features (Vulkan 1.0)

**Tiene:** `shaderSampledImageArrayDynamicIndexing`, `shaderUniformBufferArrayDynamicIndexing`,
`shaderStorageBufferArrayDynamicIndexing`, `shaderStorageImageArrayDynamicIndexing`,
**`textureCompressionETC2`**, **`textureCompressionASTC_LDR`**, `fragmentStoresAndAtomics`, `independentBlend`,
`samplerAnisotropy`, `geometryShader`, `tessellationShader`, `imageCubeArray`, `sampleRateShading`,
`shaderInt16`, `depthBiasClamp`, `fullDrawIndexUint32`, `drawIndirectFirstInstance`, `occlusionQueryPrecise`,
`shaderImageGatherExtended`, `shaderStorageImageExtendedFormats`, `shaderStorageImageReadWithoutFormat`,
`shaderStorageImageWriteWithoutFormat`, `largePoints`, `robustBufferAccess`.

**No tiene:** **`textureCompressionBC`**, **`shaderInt64`**, `shaderFloat64`, `vertexPipelineStoresAndAtomics`,
`fillModeNonSolid`, `pipelineStatisticsQuery`, `multiDrawIndirect`, `multiViewport`, `depthClamp`,
`depthBounds`, `dualSrcBlend`, `logicOp`, `wideLines`, `shaderClipDistance`, `shaderCullDistance`,
`shaderResourceMinLod`, `inheritedQueries`, `alphaToOne`, `variableMultisampleRate`, sparse (nada).

**De extensiones o 1.1:** `shaderFloat16` y `shaderInt8` sí; almacenamiento de 16 bits en todos los tipos de
búfer, push constants y entradas/salidas; `variablePointers`, `shaderDrawParameters`, `multiview` (sin geometry ni
tessellation) y `samplerYcbcrConversion` sí; `protectedMemory` no.

## Extensiones del dispositivo

| Extensión | Para el port |
|---|---|
| `VK_KHR_spirv_1_4` | **La que permite SPIR-V 1.4** (los shaders Mali se compilan con `vulkan1.1spirv1.4`) |
| `VK_KHR_timeline_semaphore` | **Sí**: sincronización CPU–GPU con un contador |
| `VK_KHR_descriptor_update_template` | **Sí**: escribir un set entero en una llamada, más barato que `vkUpdateDescriptorSets` |
| `VK_EXT_inline_uniform_block` | **Sí**: datos pequeños dentro del propio descriptor set |
| `VK_EXT_index_type_uint8` | **Sí**: índices de 8 bits sin convertir |
| `VK_KHR_shader_float16_int8`, `VK_KHR_16bit_storage`, `VK_KHR_8bit_storage` | FP16 y tipos chicos explícitos |
| `VK_EXT_global_priority` | Prioridad de la cola (alta puede requerir permisos) |
| `VK_KHR_imageless_framebuffer` | Framebuffers sin fijar las vistas al crearlos |
| `VK_KHR_create_renderpass2`, `VK_KHR_depth_stencil_resolve`, `VK_KHR_separate_depth_stencil_layouts` | Render passes más detallados y resolve de profundidad en el pase |
| `VK_KHR_buffer_device_address` | Está como extensión, pero sin `shaderInt64` no sirve para el puntero de constantes del nativo |
| `VK_EXT_texture_compression_astc_hdr`, `VK_EXT_astc_decode_mode` | ASTC HDR y decodificación ASTC a menor precisión |
| `VK_EXT_scalar_block_layout`, `VK_KHR_uniform_buffer_standard_layout`, `VK_KHR_relaxed_block_layout` | Layouts de UBO más flexibles |
| `VK_EXT_line_rasterization`, `VK_EXT_transform_feedback` | No se usan |
| `VK_GOOGLE_display_timing`, `VK_KHR_incremental_present`, `VK_KHR_shared_presentable_image` | Ritmo de presentación (frame pacing) |
| `VK_EXT_host_query_reset`, `VK_KHR_maintenance1-3`, `VK_KHR_dedicated_allocation`, `VK_KHR_bind_memory2`, `VK_KHR_get_memory_requirements2`, `VK_KHR_image_format_list`, `VK_KHR_vulkan_memory_model`, `VK_KHR_shader_float_controls`, `VK_KHR_variable_pointers`, `VK_KHR_storage_buffer_storage_class`, `VK_KHR_sampler_mirror_clamp_to_edge`, `VK_KHR_sampler_ycbcr_conversion`, `VK_KHR_multiview`, `VK_KHR_device_group`, `VK_KHR_shader_draw_parameters`, `VK_KHR_shader_subgroup_extended_types`, `VK_EXT_shader_subgroup_ballot`, `VK_EXT_shader_subgroup_vote`, `VK_EXT_separate_stencil_usage`, `VK_KHR_driver_properties` | Utilidades |
| `VK_KHR_external_*`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`, `VK_EXT_queue_family_foreign`, `VK_ANDROID_external_memory_android_hardware_buffer` | Compartir memoria con otros procesos; no se usan |

**No están:** `VK_KHR_push_descriptor`, `VK_EXT_extended_dynamic_state` (ninguna versión),
`VK_KHR_dynamic_rendering`, `VK_EXT_memory_budget`, `VK_EXT_descriptor_indexing`,
`VK_ARM_rasterization_order_attachment_access`, `VK_EXT_pipeline_creation_cache_control`.

## Límites

| Límite | Valor | Nota |
|---|---|---|
| `maxBoundDescriptorSets` | **4** | El nativo usaba 5 |
| `maxPerStageDescriptorSampledImages` | **256** | |
| `maxPerStageDescriptorSamplers` | **128** | |
| `maxPerStageDescriptorUniformBuffers` | 36 | |
| `maxPerStageDescriptorStorageBuffers` | 35 | |
| `maxPerStageResources` | 365 | |
| `maxDescriptorSetSampledImages` / `Samplers` | 1536 / 768 | |
| `maxDescriptorSetUniformBuffersDynamic` | 32 | |
| `maxPerSetDescriptors` | 65536 | |
| `maxUniformBufferRange` | **64 KB** | |
| `minUniformBufferOffsetAlignment` | **16 bytes** | Muy bajo: los offsets dinámicos casi no desperdician |
| `maxPushConstantsSize` | **256 bytes** | |
| `maxStorageBufferRange` | 256 MB | |
| `nonCoherentAtomSize`, `optimalBufferCopyOffsetAlignment`, `optimalBufferCopyRowPitchAlignment` | 64 bytes | |
| `bufferImageGranularity` | 4 KB | |
| `maxImageDimension2D` / `Cube` | 16383 | |
| `maxImageArrayLayers` | 1024 | |
| `maxColorAttachments` | 8 | |
| `maxVertexInputAttributes` / `Bindings` | 32 / 32 | |
| `maxSamplerAnisotropy` | 16 | |
| Muestras de MSAA | 1 y 4 | |
| `maxDrawIndirectCount` | 1 | Sin multi-draw indirect |
| `maxViewports` | 1 | |
| `maxComputeSharedMemorySize` / `WorkGroupInvocations` | 32 KB / 384 | |
| `timestampPeriod` | 0 | Sin timestamps |

## Formatos

| Formato | Muestreo | Color/profundidad | Mezcla | Filtro lineal |
|---|---|---|---|---|
| R8G8B8A8, B8G8R8A8, R8, R8G8 | sí | sí | sí | sí |
| **A1R5G5B5**, **R5G6B5** (BC1 a 16 bits) | sí | sí | sí | sí |
| A2B10G10R10, B10G11R11_UFLOAT, R16G16B16A16_SFLOAT | sí | sí | sí | sí |
| R16G16B16A16_UNORM | sí | sí | sí | **no** |
| R32_SFLOAT, R32G32B32A32_SFLOAT | sí | sí | **no** | sí |
| D16, X8_D24, D32, **D24_S8**, D32_S8 | sí | profundidad | — | sí |
| **ETC2** RGB/RGBA, **EAC** R11/RG11 | sí | no | — | sí |
| **ASTC** (4×4 a 12×12) | sí | no | — | sí |
| **BC1-BC7** | **no está** | | | |

## Qué cambia en el plan

- **Push descriptors: no.** La alternativa para bajar el costo de armar un set por dibujo es
  **`VK_KHR_descriptor_update_template`**, que escribe el set entero en una llamada. También está
  `VK_EXT_inline_uniform_block` para datos chicos.
- **Adjuntos transitorios: sí.** Existe el tipo de memoria `LAZILY_ALLOCATED`, así que la profundidad que no se
  lee después puede vivir solo en el tile.
- **Timeline semaphores: sí**, como extensión. Ganancia chica, como se explicó.
- **Segunda cola**: hay 2 colas en la misma familia. Las subidas de texturas podrían ir por una cola propia en
  paralelo con el trabajo, sincronizadas con un semáforo.
- **ASTC: sí** (LDR y HDR). Es una alternativa a ETC2 para el mundo, con mejor calidad por bit.
- **Lecturas de vuelta**: hay memoria `HOST_CACHED`; leerla desde la CPU es mucho más rápido que de memoria sin
  caché.
- **Índices de 8 bits**: `VK_EXT_index_type_uint8` evita convertirlos si el juego los usa.
- **No hay** dynamic state extendido, dynamic rendering ni memory budget: cada combinación de estado sigue
  necesitando su pipeline, y la RAM disponible hay que medirla por fuera de Vulkan.
- `pipelineStatisticsQuery` **no** está: el contador de fragmentos y vértices por pase no funciona en este
  teléfono.
