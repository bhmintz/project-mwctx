# Plan: shaders de ARM sobre PanVK, combinando análisis en frío y captura del driver de Samsung

Objetivo: que PanVK (JimVulkan, Vulkan 1.3) ejecute los shaders compilados por el compilador de ARM en lugar de
los de Mesa. La comparación con `malioc` muestra que la diferencia de rendimiento entre los dos drivers está en
el compilador; ver [plan-panvk.md](plan-panvk.md), "Comparación con el compilador de ARM". Para lograrlo hay que
saber cómo el driver de ARM arma cada dibujo alrededor de sus binarios, e imitarlo en PanVK.

Dos fuentes de ingeniería inversa que se complementan:

| | En frío (malioc + MBS2) | Captura (driver de Samsung en el teléfono) |
|---|---|---|
| Da | **Nombres sin hechos**: símbolos (`g_UboPixel` → ranura 18), `FCST`, `UBUF`, registros, el código del piloto | **Hechos sin nombres**: la cadena de trabajos, los descriptores del hardware, punteros y valores reales de cada dibujo |
| Herramientas | `tools/mali_arm/volcar_mbs2.py`, `mbs2.py`, `compilar_bidis.sh` (ya hechas) | Hook de captura en la APK + decodificador con `pandecode` de Mesa (por hacer) |

## Lo que ya se sabe (en frío)

Detalle en plan-panvk.md:
- Los binarios son Bifrost normal: el desensamblador de Mesa los lee.
- Cada fragmento trae el shader principal y un **piloto**; los vértices traen 3 binarios.
- El piloto lee la tabla de descriptores de UBO del hardware (`r2:r3`, ranuras 3, 18 y 19), escribe el bloque de
  uniformes (`r0:r1`, +0x10 a +0x147 = 82 palabras) y a veces hace cuentas.
- El principal toma las constantes de los registros de uniformes `u0` a `u40`.

Incógnitas: cómo llegan `r0`–`r3` al piloto; qué es la ranura 3; cómo se codifican texturas y samplers en el
inmediato del `TEXC`; qué bits del descriptor del shader salen de `EBIN`/`PDSC`; cómo se encadenan las 3
variantes de vértices.

## Etapas

### 1. Captura en la APK (modo driver de Samsung)

Módulo nativo `android/app/src/main/cpp/android_captura_mali.cpp`, apagado por defecto y activo solo con el
driver del sistema:
- **Hook por GOT** sobre `libGLES_mali.so`, que ya está cargada en el proceso. Se parchean sus importaciones de
  `ioctl`, `mmap` y `munmap`, sin Frida ni root.
- **Memoria de la GPU:** se anotan los `mmap` del fd de `/dev/mali0`. En kbase de 64 bits (SAME_VA) la dirección
  de CPU es la misma que la de GPU, así que cada región se puede leer en el propio proceso.
  `process_vm_readv` no funciona sobre páginas de kbase, así que se copia directo, con un manejador de
  SIGSEGV/SIGBUS que solo atrapa las fallas de esa copia.
- **Trabajos:** en `KBASE_IOCTL_JOB_SUBMIT` se guardan los átomos (`jc` = puntero a la cadena de trabajos y el
  resto de los campos, tal cual).
- **Disparo:** cvar `nfsmw_captura_mali = N`, que captura los envíos entre el swap N y el N+1. Se toma una foto de
  las regiones chicas (≤ 4 MB, presupuesto total ~96 MB, sin texturas grandes) en cada envío de ese fotograma, antes
  de que corra la GPU. Así se ven los descriptores que escribió la CPU. Opcional: otra foto después de que termine,
  para ver lo que escribieron los pilotos.
- Se guarda también el `gpu_id` (sacado del ioctl de propiedades de la GPU, o fijo para el G52).
- **Salida:** `captura_mali_N.bin` en la carpeta del juego, con regiones `(va, tamaño, bytes)`, átomos y
  metadatos.

### 2. Decodificador en la PC

- Compilar `pandecode` de Mesa desde el código de JimVulkan en WSL (`libpanfrost_decode`, genxml v7).
  Herramienta `tools/mali_arm/leer_captura` que carga la captura, hace `pandecode_inject_mmap` de cada región y
  `pandecode_jc` de cada átomo, con el desensamblador de Bifrost como callback.
- Plan B, si compilar `pandecode` se complica: un recorredor propio de la cadena de trabajos con los
  `*_unpack`/`*_print` que genera `gen_pack.py` para v7 (cabecera de trabajo, Draw, Renderer State, Uniform Buffer,
  Texture, Sampler).

### 3. Correlación

`tools/mali_arm/correlacionar.py`:
- Para cada shader de la captura (puntero del Renderer State), sacar su código y buscarlo entre los `OBJC` de
  `out/mali_arm/mbs2/`. Si coincide exacto, ese dibujo usa el shader NNN y hereda sus metadatos. Si no coincide
  por la versión del compilador (malioc r51p0 contra la del teléfono), emparejar por estructura (cantidad de
  cláusulas, instrucciones, registros).
- Mejor todavía: los binarios capturados son los del compilador exacto del teléfono, y pueden reemplazar a los
  de malioc.

### 4. Resolver las incógnitas con datos

Por cada dibujo capturado:
- La cadena de trabajos: ¿el piloto es un trabajo de cómputo aparte? ¿Cómo recibe `r0`–`r3`? Mirar el "Preload"
  y los uniformes de su Renderer State.
- La tabla de UBO real (ranuras 3, 18 y 19) y a qué apunta la 3.
- El Renderer State completo del principal comparado con `EBIN`/`PDSC`/`FCST`, para mapear campo por campo.
- Las tablas de texturas y samplers y el inmediato del `TEXC`.
- Vértices: cómo se usan las 3 variantes (IDVS y piloto).

Resultado: una especificación en este documento de qué tiene que emitir PanVK para cada dibujo.

### 5. Adaptador en PanVK (JimVulkan compilado por nosotros)

- Cargar los binarios de ARM (de la captura, de malioc o, más adelante, compilados en el teléfono con
  `cmpbe_v2_*` de `libGLES_mali.so`).
- Por dibujo: armar la tabla de UBO con las ranuras de ARM, correr el piloto y apuntar los uniformes del principal
  al bloque que produce. Emitir los descriptores según la especificación de la etapa 4.
- **Depuración por comparación:** el mismo hook de la etapa 1 captura lo que arma PanVK; se decodifica igual y se
  compara con la captura de Samsung del mismo dibujo.

## Resultados de la primera captura (2026-10-03, fotograma 1000 en carrera)

Herramientas: `android_captura_mali.cpp` (cvar `nfsmw_captura_mali`), `tools/mali_arm/leer_captura` (pandecode),
`captura.py` (regiones, antes/después, volcado de bytes) y `correlacionar.py` (empareja shaders con malioc por
secuencia de operaciones).

Detalles de la captura que hubo que resolver:
- Los shaders están fuera de SAME_VA (`0x7f00000000…`): su dirección de GPU es el offset del `mmap`.
- El driver mapea la memoria ejecutable, escribe el código y la desmapea. La captura guarda el contenido al
  desmapear.
- Los eventos de fin de átomo llegan por `__read_chk` sobre el fd de Mali. En cada uno se toma una foto
  "después" de lo que cambió.

Lo que muestra:
- **Cada dibujo es un trabajo IDVS** (vértice + varyings + fragmento) con 17 UBO y **dos bloques de uniformes
  empujados**, uno por etapa (p. ej. `u[54]` del vértice y `u[4]` del fragmento).
- **Los shaders del teléfono son los de malioc.** El fragmento capturado `0x7f0067c80` es el 063 de malioc
  instrucción por instrucción. Solo cambia qué palabra de uniforme usa cada `MOV` (`u1.w0`↔`u1.w1`,
  `u0.w0`↔`u0.w1`): la disposición del bloque cambia entre versiones del compilador. Los vértices coinciden en
  un 86–94 % con la familia correspondiente.
- **Los pilotos no corren en la GPU.** Los bloques de uniformes ya están escritos antes de que corra la GPU y no
  cambian después. El de fragmento del 063 contiene lo que calcula su piloto (p. ej. `0xf478700f` en +0x1c), y el
  de vértice contiene las constantes del juego (0.92, 0.69, 512×288…). El driver ejecuta el trabajo del piloto
  en la CPU y escribe el bloque de cada dibujo.
- Los UBO grandes del juego (`ubuf_14` 4096 B y `ubuf_15` 3584 B, de un búfer dinámico) siguen en la tabla de
  UBO. El bloque de fragmento guarda sus offsets dinámicos.
- **Los únicos cómputos del driver** van antes de cada lote de dibujos, con un grupo de trabajo por dibujo:
  - `0x7f002f000`, de 128 hilos: escanea el rango de índices (registro de 160 B por dibujo, `switch` por tipo de
    índice, mín./máx.).
  - `0x7f0007000`: parchea cada dibujo desde una tabla {dibujo, shader}.

  Los dos son propios del IDVS de ARM; PanVK no los necesita.

Consecuencia para la etapa 5: el adaptador **no tiene que correr pilotos en la GPU**. En cada dibujo, en la CPU (al
grabar o al enviar), hay que llenar el bloque de uniformes con la disposición que espera el binario de ARM, que es
lo que haría el piloto, y apuntar el FAU del shader ahí. Como el binario del teléfono y el de malioc solo
difieren en esa disposición, conviene usar los binarios del teléfono (de la captura, o compilados en el teléfono
con `cmpbe_v2_*`) o sacar la disposición del piloto del mismo binario.

### Pilotos ejecutados en la PC (`tools/mali_arm/piloto.py`)

`piloto.py` interpreta el desensamblado de Mesa del piloto sobre la memoria de la captura (que ahora incluye una
copia de las regiones grandes, como el búfer de UBO dinámico del juego). Resultados con el 063:
- El piloto recibe en `r0:r1` un contexto: +0x18 = tabla de descriptores de UBO y +0x30 = bloque de uniformes de
  salida.
- **La tabla de UBO del piloto es la del hardware corrida 3 entradas.** La ranura 18 de ARM (`g_UboPixel`) es la
  `ubuf_15` del dibujo y la 19 (`g_UboCompartidas`) es la `ubuf_16`. Los tamaños de la tabla `UBUF` del MBS2
  coinciden.
- Las constantes que calcula el piloto (las de `g_UboPixel`, y el `FCST` `0xf478700f` del `TEXC`) **son idénticas**
  a las del bloque que escribió el driver.
- Los índices de textura (el piloto los lee del UBO compartido y se los pasa al `TEXC` del principal) salen con
  +1 respecto del driver: la tabla de texturas del driver del teléfono no tiene el corrimiento que asume malioc.
  En PanVK esa tabla la armamos nosotros.

### Especialización

El juego especializa cada pipeline con una constante de 32 bits (`constant_id 0`; bits `kSpec*` en
`nfsmw_nativo_dibujos.cpp`; el valor más común es `0x8300`). Compilar el SPIR-V sin especializar da binarios más
largos que los del teléfono. `tools/mali_arm/variantes.py` toma los valores de la lista de pipelines del juego, que
la captura copia a la carpeta del juego. Congela cada variante con `spirv-opt` y la compila con malioc.

Con las variantes, los shaders del teléfono y los de malioc tienen casi la misma cantidad de operaciones
(155/157, 54/52, 139/137) y un 83–92 % de parecido. El resto es la diferencia de versión del compilador. Para el
adaptador no importa: usa siempre el par principal + piloto de malioc, que es coherente consigo mismo.

### Especificación del adaptador (etapa 5, primero el fragmento 063)

Lo que hay que emitir por dibujo, todo derivable del MBS2 de malioc más la memoria del dibujo:

| Parte | De dónde sale | Qué hace PanVK hoy |
|---|---|---|
| Código | `OBJC` del primer `EBIN` | binario de Mesa |
| Message preload 1 y 2 del descriptor | `PDSC` (p. ej. `0x0801`, `0x1821` = LD_VAR índice 0 de 2 comp., LD_VAR índice 2 de 4 comp.). Coincide con la palabra del descriptor de Samsung. | los de Mesa |
| Precarga de registros (cobertura, muestra) | la de Samsung para ese shader (063: cobertura + sample mask/ID) | la de Mesa |
| Bloque de uniformes (FAU) | el piloto, ejecutado en la CPU sobre los UBO del dibujo | sysvals + push constants de Mesa |
| Varyings | símbolos del MBS2: índice ARM → location (063: 0 → TEXCOORD0/loc 0, 1 → TEXCOORD1/loc 1, 2 → COLOR0/loc 16) | tabla propia del FS (`link_shaders`, una entrada por slot del layout del FS) |
| Texturas y samplers | el índice que calcula el piloto (heap del juego + 1) | tabla compacta por (set, binding) usados |

Captura de PanVK para comparar: el módulo de captura engancha la librería de `vulkan_icd_android` cuando hay un
driver propio (`out/capturas/panvk/`).

Implementación en nuestro JimVulkan (`out/jim/mesa`, se compila con `tools/mali_arm/compilar_jim.sh`):
1. **Identificación.** En `vk_pipeline_shader_stage_to_nir`, un hash FNV-1a del SPIR-V y de los datos de
   especialización queda en `nir->info.label` (`nfsmw:<hash>`). El mismo hash se calcula en Python.
2. **Carga.** Si existe `$NFSMW_ARM_DIR/<hash>.arm` (lo genera una herramienta en Python a partir del MBS2: código,
   PDSC, precarga, cantidad de uniformes, mapa de varyings y el piloto traducido a un bytecode simple), al compilar
   el FS se compila igual con Mesa (para el resto de la información: blend, early-ZS, varyings del VS) y se
   reemplazan el código y esos campos. La variable de entorno se pasa con `vulkan_driver_env`.
3. **Por dibujo.** Si el FS tiene binario de ARM: se arma su tabla de varyings con el mapa de ARM, se corre el
   piloto en la CPU (intérprete en C del bytecode) sobre los UBO del dibujo (memoria mapeada del juego, con su
   offset dinámico) y el resultado va como uniformes empujados del FS. Los índices de textura del piloto se
   corrigen a la tabla de PanVK.

## Orden y criterios de corte

1. Etapas 1 y 2 juntas: la primera captura decodificada ya responde varias incógnitas.
2. Etapa 3 y la 4 para un solo fragmento (el 097) de punta a punta, antes de generalizar.
3. Si la etapa 4 muestra algo que PanVK no puede imitar (un recurso del hardware que PanVK no usa, o
   dependencias del firmware), se para y se reevalúa. Esas alternativas están en plan-panvk.md: push constants
   en la app y el empuje de UBO en Mesa.

## Reglas

- Todo lo de captura va detrás de una cvar apagada y solo con el driver del sistema; no cambia nada del juego
  normal.
- Las capturas y los binarios de ARM van en `out/` (ignorado por git).
