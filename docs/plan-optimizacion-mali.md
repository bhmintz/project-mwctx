# Plan de optimización del modo Mali

Lista de trabajo para seguir acelerando el juego en la Mali-G52 MC2 (Samsung Galaxy A32 4G), ordenada por
impacto esperado. Cómo funciona hoy el backend: [backend-mali.md](backend-mali.md).

El punto de partida que guía el orden: **en carrera el cuello de botella es la CPU del hilo del anillo**
(~20 µs por dibujo: ~6 µs elegir la pipeline, ~5 µs llamadas al driver), no la GPU. Lo que baja el costo por
dibujo va primero; lo que solo alivia a la GPU, después. Cada punto se mide antes y después en **la misma
carrera**, porque contra tres rivales rinde menos que contra uno solo.

## Plan de acción, en orden

Las pruebas son cortas: 30-40 s de juego en el mismo lugar alcanzan para 3-4 informes de 10 s.

1. [ ] **Medición base** (30-40 s, por logcat, sin APK depurable): FPS, `[nativo] tiempos` (ocupación del anillo
       y µs por dibujo), `C2: presentar` (candado de la cola y `vkQueueSubmit`), `C2: esperas del anillo a la
       GPU`, `C6 subetapas` y tirones.
2. [ ] **Candado de la cola entre trabajo y presentación.** En un log de hoy, cada envío de trabajo esperaba
       ~7,5 ms el candado de la cola: la presentación lo retiene mientras `vkQueuePresentKHR` espera a la GPU.
       El teléfono tiene **2 colas** en la misma familia: presentar por la segunda, con un semáforo, libera al
       anillo.
3. [ ] **Filtrar estado redundante**: no repetir `vkCmdBind*`, viewport ni scissor si no cambiaron.
4. [ ] **Búsqueda de pipeline más barata**: recordar la última y armar la clave de forma incremental.
5. [ ] **Plantillas de descriptores** (`VK_KHR_descriptor_update_template`) para el set por dibujo.
6. [ ] **Constantes**: subir solo los rangos que cambiaron.
7. [ ] **Adjuntos transitorios** (`LAZILY_ALLOCATED`) para la profundidad que no se lee después: menos RAM.
8. [ ] **`storeOp DONT_CARE`** en lo que nadie lee después del pase.
9. [ ] **Memoria `HOST_CACHED`** para las lecturas de vuelta de texturas resueltas.
10. [ ] **Afinar barreras**: la completa al empezar cada trabajo y las TRANSFER→TRANSFER tras cada copia.
11. [ ] **2 o 3 fotogramas en vuelo**, según cuánto espere la CPU a la GPU.
12. [ ] **Subidas por la segunda cola**, en paralelo con el trabajo (si el punto 2 no la usa ya).
13. [ ] Cubemaps fijos por zona.
14. [ ] ASTC para las texturas grandes del mundo.
15. [ ] Estabilidad: crash a 640×360, shaders del resplandor, manejador de excepciones.
16. [ ] *Evaluar al final:* agrupar dibujos, grabar en dos hilos, subpasses para el posprocesado.

Cada punto se mide antes y después con la misma prueba corta; si la medición cambia las prioridades, se
reordena.

## Verificado en el teléfono

Detalle completo en [mali-g52-vulkan.md](mali-g52-vulkan.md) (`adb shell cmd gpu vkjson`, 2026-10-02).
Teléfono: SM-A325M, Android 13, 4 GB de RAM.

| Duda | Respuesta | Qué cambia |
|---|---|---|
| `VK_KHR_push_descriptor` | **No** | Usar `VK_KHR_descriptor_update_template` (**sí**) para escribir el set por dibujo en una llamada |
| `VK_KHR_timeline_semaphore` | **Sí** | Opcional; ganancia chica |
| `VK_EXT_extended_dynamic_state` (1/2/3) | No | Cada combinación de estado sigue siendo una pipeline |
| `VK_KHR_dynamic_rendering` | No | Se siguen usando `VkRenderPass` y `VkFramebuffer` |
| `VK_EXT_memory_budget` | No | La RAM disponible se mide por fuera de Vulkan |
| FP16 / 16 bits | **Sí** | Disponible si se retoma `--fp16` |
| `VK_EXT_index_type_uint8` | **Sí** | Índices de 8 bits sin convertir |
| `VK_ARM_rasterization_order_attachment_access` | No | |
| Memoria `LAZILY_ALLOCATED` | **Sí** (tipo 2) | El punto 7 es posible |
| Memoria `HOST_CACHED` | **Sí** (tipo 1) | Usarla para las lecturas de vuelta |
| ASTC | **Sí** (LDR, HDR y todos los tamaños de bloque) | El punto 14 es viable |
| Profundidad | D16, X8_D24, D32, D24_S8, D32_S8 | |
| A1R5G5B5 / R5G6B5 | Muestreo, color, mezcla y filtro lineal | |
| Límites de UBO | 64 KB por rango, offsets alineados a 16 bytes, 32 dinámicos por set, 256 bytes de push constants | |
| Colas | **2 en la misma familia** | Nueva idea: subir texturas por una cola propia, en paralelo |

## Resultados medidos (2026-10-02, carrera a 1024×576, núcleos grandes topados a 1,71 GHz)

| Cambio | Resultado | Estado |
|---|---|---|
| Medición base | ~28 FPS reales en carrera (36 solo con el teléfono frío); anillo al 98 % | Hecho |
| Presentar por la segunda cola | Quitó la espera de 5-9 ms al candado, pero la GPU tardó más (esperas de 14-18 ms): ~30 en vez de 36 FPS | Apagado (`vulkan_cola_presentar_propia`) |
| ADPF (`APerformanceHint`) | El teléfono rechaza la sesión | Sin efecto |
| Nombre de paquete de AnTuTu | El tope de 1,71 GHz sigue igual | Sin efecto |
| Sets de descriptores persistentes | Sets creados: del 28 % de los dibujos a ~0 (1024 en toda la sesión); 8-10 µs por dibujo en vez de 10-15. FPS iguales (~28) | Activo (`nfsmw_nativo_mali_sets_persistentes`) |
| Salida a 1600×720 escalada por hardware (`vulkan_swapchain_alto_max`, opción "Escala de salida") | ~28 → ~31 FPS con la GPU topada a 586 MHz | Activo (720 por defecto en GPU débiles) |
| Copia del mapa de sombras vacío (`nfsmw_nativo_mali_sombra_sin_copia`) | Copias de sombras a 0; FPS dentro del ruido | Activo |
| Bloom optimizado (`nfsmw_bloom`, opción "Resplandor de las luces") | A/B del bloom: 2,3 ms por fotograma (30,6 con, 32,9 sin). Con "optimizado" y el teléfono frío (GPU a 800-950 MHz): ~44 FPS de media, tramos de 60 | Activo ("optimizado" por defecto en GPU débiles) |
| Escena nativa a 1024×576 (`nfsmw_render_escena_nativa`) | El juego dibuja la escena por su modo 1 en vez de 1280×720 encogida: 36 % menos píxeles. ~31 → ~36 FPS con la GPU topada | Activo en GPU débiles |
| Escena nativa a 960×540 y 896×504 | El modo 1 se reescribe a ese tamaño. La salida y el posproceso del juego siguen a 1024×576, así que el resolve de la escena se estira a esa textura (sin eso quedaba en la esquina con el HUD corrido). 896×504 sin tope y bilineal: 35-48 FPS en carrera, casi todo 42-48 | Opciones del launcher |
| Humo optimizado (`nfsmw_humo`) | Variante sin la lectura del mapa de sombras cuando las sombras están apagadas. Saltear partículas alternas parpadeaba: descartado | Activo ("optimizado" por defecto en GPU débiles) |
| Límite de 40/45 FPS | Ritmo fijo por debajo de 60: la GPU descansa entre fotogramas y calienta menos | Opciones del launcher |
| Filtrado bilineal (`nfsmw_filtro_texturas`) | El filtro entre mips pasa a NEAREST: la unidad de texturas hace la mitad. Mejora visible pero no grande | Activo ("bilineal" por defecto en GPU débiles) |

**Desglose por fence, corrección:** los pases sin un solo dibujo (las copias) daban 1,4-2,8 ms cada uno, así que esos "~17 ms de copias" son sobre todo el costo de medir con una valla por pase. Las copias reales son ~10 por fotograma y chicas (~1,2 Mpíxeles). Lo caro es la escena.

**Siguiente:** A/B por shader de la escena (`nfsmw_nativo_mali_ab_omitir_ps`) para encontrar los 2-3 que más pesan y hacerles variantes, y una variante más barata de la pasada final `19C0C358` (1024×576).

**GPU y temperatura:** la GPU está al 98-99 % en carrera. Android la topa en 586 MHz (de 950) con el estado
térmico 2; con el teléfono frío llega a 950 MHz. Medir siempre leyendo `/sys/kernel/gpu/gpu_clock` y
`gpu_max_clock`: la frecuencia cambia más los FPS que muchos de estos cambios. Menos trabajo de GPU también es
menos calor y frecuencias altas por más tiempo.

**Desglose de GPU por pase** (`nfsmw_nativo_desglose_por_fence` vía `nfsmw_args.txt`, serializado): escena ~21 ms,
copias ~17 ms, desenfoque ~8 ms, reflejo ~7 ms, menores ~5 ms. Pasadas caras de un solo dibujo: bloom
`7E1C6EED1AC24341` (hecho), reflejo `19C0C358044A29BF` (lee 1024×576), `687FE25871F1B40D` y
`61BE10993D2721E2` (~4 ms cada una en la escena), `212C84F87E2E3DC1` (~3 ms, lee 1024×1024).

**Opciones de prueba sin APK depurable:** `nfsmw_args.txt` en la carpeta del juego, una línea `--cvar=valor` por
opción; sin el archivo no cambia nada.

**Conclusión (antes de lo de GPU):** con el costo por dibujo ya bajo, el anillo pasa 20-26 ms por fotograma esperando a la GPU dentro
del Swap. El límite ahora es la **GPU**: siguen los puntos 7-10 (adjuntos transitorios, `storeOp`, barreras) y
las copias de resolve que nadie lee.
