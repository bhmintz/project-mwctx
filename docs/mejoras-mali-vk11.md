# Mejoras para el modo Mali con Vulkan 1.1 (driver de Samsung)

Mejoras que salen de lo que se aprendió al capturar el driver de Samsung, compilar los shaders con el compilador de
ARM (`malioc`) y correr esos binarios dentro de PanVK (eso último en la rama `vk1.3`, `docs/plan-arm-hibrido.md`).
Todas cambian los shaders del juego o el backend: sirven con el driver del sistema (Vulkan 1.1), que es el de esta
rama, y también con PanVK.

Cómo se obtuvieron los datos y cómo volver a medirlos: [herramientas-mali.md](herramientas-mali.md). Las mejoras ya
anotadas antes (`remapInput` en la pila, skinning) están en [mejoras-shaders-mali.md](mejoras-shaders-mali.md).

## Qué se sabe del driver de Samsung (2026-10-03)

- Cada dibujo es un trabajo IDVS (vértices, varyings y fragmento en un solo trabajo). La tabla de UBO tiene 17
  entradas y hay dos bloques de uniformes empujados, uno por etapa.
- Los shaders que corren en el teléfono son los de `malioc`, instrucción por instrucción. Lo único que cambia entre
  versiones del compilador es la disposición del bloque de uniformes. Por eso `malioc` sirve para medir: lo que
  dice de un shader vale para el teléfono.
- El compilador de ARM divide cada shader en un **principal** (corre en la GPU) y un **piloto** (el cálculo que da
  lo mismo para todo el dibujo). El driver de Samsung corre el piloto **en la CPU**, una vez por dibujo, y escribe
  el resultado como uniformes empujados.
- Antes de cada lote de dibujos el driver lanza dos cómputos propios: uno escanea el rango de índices de cada
  dibujo y otro parchea los dibujos.
- Con PanVK (Mesa) el juego va a 16 FPS en carrera, contra 35–48 con Samsung, y la GPU está al 92%. Aun corriendo
  43 fragmentos con el binario de ARM, la diferencia sigue. El compilador de fragmentos de Mesa no la explica sola;
  todavía no se sabe de dónde viene el resto.

## Datos de los shaders de fragmento

De los 56 fragmentos especializados que usa el juego (lista de pipelines de una partida) y de las estadísticas de
`malioc` de los 89 fragmentos del `.nfsp` (estos últimos sin especializar):

| Dato | Valor | Por qué importa en la G52 |
|---|---|---|
| Aritmética en fp16 | 0% en todos | Bifrost hace dos operaciones fp16 por carril en el tiempo de una fp32 |
| Registros de trabajo | 22 de 56 usan 64 (el resto 32) | con más de 32 registros el núcleo corre la mitad de hilos a la vez |
| Varyings precargados | 37 LD_VAR, todos en fp32, ninguno en fp16 | un varying fp32 ocupa el doble de memoria y se interpola a la mitad de velocidad |
| Precarga de textura (VAR_TEX) | 0 de 56 | el hardware puede leer la primera textura antes de que arranque el shader, y acá nunca lo hace |
| Pueden descartar fragmentos | 21 de 89 (sin especializar) | esos dibujos pierden el early-ZS y el FPK (descarte de píxeles tapados) |
| Tienen cómputo uniforme (piloto) | 77 de 89 | ese trabajo lo hace la CPU en cada dibujo dentro del driver |
| Uniformes empujados | 10 a 128 palabras por dibujo | el piloto los escribe en cada dibujo; 128 es el límite del hardware |
| Varyings por fragmento | hasta 11 vec4 | cada uno se escribe a memoria por vértice y se lee por fragmento |

Los más pesados según `malioc`, en ciclos del camino más largo (aritmética / load-store / varyings / texturas):

| Shader | Ciclos | Límite | Registros | Descarte |
|---|---|---|---|---|
| 097 | 9.3 / 3.0 / 3.1 / 13.5 | texturas | 64 | sí |
| 060, 094, 105 | 11.9 / 0 / 3.3 / 1.5 | aritmética | 32 | no |
| 077 | 3.7 / 0 / 0.3 / 8.0 | texturas | 64 | no |
| 076 | 4.8 / 0 / 3.5 / 7.5 | texturas | 60 | no |
| 098, 130 | ~3 / 0 / 0.3 / 8.0 | texturas | 57, 32 | no |
| 025, 026, 049, 072 | 4.3 / 3.0 / 2.9 / 7.0 | texturas | 52 | sí |

Hay que tomarlos con cuidado: están medidos sin especializar, y la especialización puede sacar código (incluido el
descarte). Para cifras exactas, mirar las variantes (`out/mali_arm/mbs2/NNN_eXXXXXXXX_*`).

## Mejoras propuestas

Ordenadas por lo que se espera ganar en la GPU, que es el cuello en el teléfono. Todas van solo con `NFSMW_MALI`
(o detrás de una cvar apagada) y se verifican con `malioc` antes de regenerar el `.nfsp`.

### 1. Aritmética en fp16 (`min16float`) en los fragmentos

- **Qué:** usar `min16float`/`min16float4` en los cálculos de color, iluminación, niebla y mezcla de los
  fragmentos. DXC, sin `-enable-16bit-types`, lo traduce a `RelaxedPrecision` en el SPIR-V, y el compilador de ARM
  lo respeta (es el `mediump` de GLSL). Las coordenadas de textura y las posiciones quedan en fp32.
- **Por qué:** hoy no hay nada en fp16. Bifrost hace dos operaciones fp16 por carril, y los valores fp16 ocupan
  medio registro. Eso baja la presión de registros: los 22 fragmentos de 64 registros pueden caer a 32 y duplicar
  los hilos en vuelo.
- **Verificar:** en `malioc`, que suba `fp16_arithmetic` y bajen los registros de trabajo y los ciclos de
  aritmética. En pantalla, que no aparezcan bandas en degradados ni en la niebla.
- **Empezar por:** 060/094/105 (limitados por aritmética) y los de 64 registros (097, 101, 142, 143, 090, 092…).

### 2. Varyings en fp16

- **Qué:** declarar con precisión relajada los varyings que la toleran: colores (`COLOR0`–`COLOR2`), niebla,
  normales y factores de mezcla. Las coordenadas de textura de texturas grandes conviene dejarlas en fp32.
- **Por qué:** en IDVS el vértice escribe los varyings a memoria y el fragmento los lee e interpola. En fp16 ocupan
  la mitad (menos ancho de banda, que en la G52 es caro) y se interpolan al doble de velocidad. Hoy los 37 varyings
  precargados son fp32.
- **Verificar:** en el `PDSC` del MBS2 (el `.h` que deja `volcar_mbs2.py`; formato de registro F16 en el message
  preload: bits 9–10 = 1) y en los ciclos de varyings de `malioc`.

### 3. Menos registros en los fragmentos de 64

- **Qué:** además del fp16, partir o simplificar los fragmentos que superan 32 registros: menos valores vivos a la
  vez, sin cargar todas las texturas antes de usarlas, sin arrays locales.
- **Por qué:** con 33 a 64 registros el núcleo corre la mitad de hilos, y eso esconde peor la latencia de texturas
  y memoria. Los más usados en carrera (097, 101, 142) están en ese grupo.
- **Verificar:** `work_registers_used` en `malioc`.

### 4. Índices de textura constantes donde se pueda

- **Qué:** hoy cada fragmento elige su textura y su sampler con un índice que lee del UBO compartido (heap de
  descriptores). Para los dibujos de pocos materiales, o con slots fijos por dibujo, pasar el índice como constante
  (especialización o binding fijo).
- **Por qué:** con índice variable el compilador de ARM usa `TEXC` en modo registro y no puede usar la precarga
  `VAR_TEX`, con la que el hardware lee la primera textura con el varying antes de que arranque el shader. Hoy
  ningún fragmento la usa. Además el piloto tiene que calcular los índices en cada dibujo.
- **Ojo:** es un cambio de diseño del backend (cómo se enlazan las texturas). Primero hay que medir la ganancia:
  compilar con `malioc` una variante del 063 con índice literal y comparar ciclos y precargas.

### 5. Descarte solo donde haga falta

- **Qué:** que los dibujos sin prueba alfa usen una variante sin `discard`. La función alfa ya va en los bits
  16–18 de la constante de especialización, así que hay que confirmar que la variante de "siempre pasa" no deja
  ningún `discard` (o `clip`) en el código.
- **Por qué:** un fragmento que puede descartar pierde el early-ZS y el FPK, y entonces se sombrean píxeles que
  después quedan tapados.
- **Verificar:** la propiedad `modifies_coverage` de `malioc` sobre las variantes especializadas, no sobre el
  `.nfsp` crudo.

### 6. Muestreo de texturas en los shaders limitados por texturas

- **Qué:** en los que limitan por texturas (097, 077, 076, 098, 130, 025/026/049/072), revisar cuántas lecturas
  hacen, con qué filtro y de qué formato. En Bifrost el trilineal cuesta el doble que el bilineal, el anisotrópico
  más, y los formatos de 64 o 128 bits por texel leen a la mitad o a un cuarto de velocidad.
- **Por qué:** son los de más ciclos. Ya existe el filtrado bilineal opcional; esto lo extiende a lo que `malioc`
  marca como limitado por texturas.
- **Verificar:** ciclos de texturas en `malioc` y FPS en el mismo punto de la carrera.

### 7. Menos trabajo del piloto en la CPU

- **Qué:** precalcular en el juego (C++), al llenar los UBO, los valores que hoy calcula el piloto: productos de
  constantes, índices de textura y escalas.
- **Por qué:** el driver de Samsung corre el piloto en la CPU en cada dibujo, y 77 de 89 fragmentos tienen uno.
- **Prioridad baja:** el cuello es la GPU. Solo vale si un perfil de CPU (simpleperf sobre `libGLES_mali.so`)
  muestra al piloto entre lo caro.

### 8. Memoria del proceso

- En carrera se vieron 100 MB libres y 1,4 GB de swap usados (ver la nota de congelones por RAM). Cada mejora de
  arriba que baje memoria de varyings o de texturas también ayuda con eso. `top -H` y `free` muestran el estado
  durante la partida.

## Qué no conviene

- Imitar los cómputos de escaneo de índices del driver: son propios del IDVS de ARM y el juego no los controla.
- Sacar conclusiones de rendimiento de PanVK hacia el driver de Samsung, o al revés: el mismo shader cuesta
  distinto según el driver que arma el resto del trabajo.
