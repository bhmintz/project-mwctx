# Herramientas para medir y analizar el Mali-G52

Las herramientas que se armaron para entender qué hace el driver de Samsung y qué produce el compilador de ARM, con
cómo se compila cada una y qué necesita. Lo que se descubrió con ellas está en
[mejoras-mali-vk11.md](mejoras-mali-vk11.md); la forma de medir sin engañarse, en [measuring.md](measuring.md). Las
herramientas de PanVK (nuestro driver Mesa para Vulkan 1.3) están en la rama `vk1.3`.

Todo lo generado (capturas, binarios de ARM, builds de Mesa) va en `out/`, que git ignora. Lo que hay que conservar
está en `tools/mali_arm/`.

## Resumen

| Para qué | Herramienta | Dónde corre |
|---|---|---|
| Capturar lo que el driver manda a la GPU en un fotograma | `android_captura_mali.cpp` (cvar `nfsmw_captura_mali`) | en el juego |
| Decodificar la captura (trabajos, descriptores, shaders) | `tools/mali_arm/leer_captura` (pandecode de Mesa) | WSL |
| Desensamblar código de Bifrost | `out/bidis/bidis` (`compilar_bidis.sh`) | WSL |
| Leer regiones y bytes de una captura | `captura.py` | Windows |
| Listar los dibujos de un envío decodificado | `dibujos.py` | Windows |
| Compilar los shaders con el compilador de ARM y guardar los binarios | `volcar_mbs2.py` (malioc + Frida) | Windows |
| Compilar las variantes especializadas que usa el juego | `variantes.py` | Windows |
| Emparejar shaders capturados con los de malioc | `correlacionar.py` | Windows + WSL |
| Correr un piloto de ARM sobre la memoria capturada | `piloto.py` | Windows |
| Medir en el teléfono | contador de FPS, sysfs de Mali, `top -H`, logcat, screencap | adb |

## Dependencias

### Windows

- Python 3 con `frida-tools` (`pip install frida-tools`), solo para `volcar_mbs2.py`.
- Arm Performance Studio 2026.5: `malioc` en `D:/arm/mali_offline_compiler/malioc.exe`.
- NDK de Android r29 del SDK (`D:/android/sdk/ndk/29.0.14206865`): `spirv-opt`, `spirv-val` y `spirv-dis` en
  `shader-tools/windows-x86_64/`. `variantes.py` usa ese `spirv-opt`.
- adb (ver "adb" más abajo).

### WSL (distro `Ubuntu`; `wsl -e` sin `-d Ubuntu` entra a `docker-desktop`)

- Para `bidis` y `leer_captura`: `gcc`, `curl`, `python3` con `mako` y `yaml` (`python3-mako`, `python3-yaml`).

### Disco

El disco virtual de WSL está en `C:` y `C:` tiene poco espacio: llenarlo dejó al sistema sin disco una vez. Todo lo
grande (capturas de 500 MB, builds) va en `D:`, trabajando desde `/mnt/d/...`. Si `C:` se llena:
`wsl --shutdown`, borrar lo parcial, `sudo apt-get clean` y compactar el disco virtual (diskpart) desde Windows.

## 1. Capturar el driver (en el teléfono)

`android/app/src/main/cpp/android_captura_mali.cpp`, detrás de la cvar `nfsmw_captura_mali` (apagada por defecto).
Engancha `ioctl`, `mmap`, `munmap` y `read`/`__read_chk` del driver de Mali y, durante el fotograma N, guarda cada
envío de trabajos, la memoria a la que apunta y lo que la GPU escribió después.

- Activar: en la carpeta del juego, `nfsmw_args.txt` con una línea `--nfsmw_captura_mali=1000` (fotograma 1000,
  ya en carrera). El archivo reemplaza los argumentos del lanzador con el mismo nombre.
- Sale `captura_mali_1000.bin` en la carpeta del juego (unos 500 MB), más una copia de las listas de pipelines del
  juego (`nfsmw_nativo_pipelines*.bin`), que `variantes.py` necesita.
- Efecto conocido: mientras copia aparece el aviso de "la app no responde", pero el juego sigue dibujando.
- Al terminar, **borrar `nfsmw_args.txt`**: si queda, la próxima partida vuelve a capturar.

Detalles que costó resolver (por si hay que tocarla): los shaders están fuera de SAME_VA y su dirección de GPU es
el offset del `mmap`; el driver escribe el código y desmapea la memoria, así que se copia al desmapear; los eventos
de fin de trabajo llegan por `__read_chk`; el búfer dinámico de UBO del juego mide más de 4 MB y se copia aparte.

## 2. Decodificar la captura (WSL)

pandecode sale del código de Mesa de JimVulkan (`github.com/JimVulkan/mali-panvk`, commit `fb3b49d`), clonado una
vez en `out/mesa-jim`:

```bash
git clone https://github.com/JimVulkan/mali-panvk out/mesa-jim && git -C out/mesa-jim checkout fb3b49d
bash tools/mali_arm/compilar_bidis.sh out/bidis      # desensamblador de Bifrost (baja fuentes de JimVulkan)
bash tools/mali_arm/compilar_pandecode.sh            # pandecode + bidis -> out/pandecode/leer_captura
PANDECODE_DUMP_FILE=out/capturas/c.pdc out/pandecode/leer_captura out/capturas/captura_mali_1000.bin
```

- Sin `PANDECODE_DUMP_FILE`, pandecode escribe en la carpeta actual.
- Deja un archivo por envío con los trabajos IDVS, los descriptores (RSD, tablas de UBO, texturas, samplers,
  atributos) y cada shader desensamblado.
- Errores de GPU de kbase que aparecen en estos trabajos y en logcat: `0x58` DATA_INVALID_FAULT (descriptor
  inválido), `0x5B` IMPRECISE_FAULT (acceso inválido de un shader). En logcat salen como `event code 88` y `91`.

En Windows:

```bash
python tools/mali_arm/captura.py out/capturas/captura_mali_1000.bin                    # regiones
python tools/mali_arm/captura.py out/capturas/captura_mali_1000.bin 0x7f0067c80 64     # palabras
python tools/mali_arm/captura.py out/capturas/captura_mali_1000.bin 0x7f0067c80 volcar s.bin
python tools/mali_arm/dibujos.py out/capturas/c.pdc..ctx-0.0000                         # dibujos de un envío
```

`antes`/`despues` eligen la copia previa a la GPU o la posterior.

## 3. Compilador de ARM (malioc) y variantes

`malioc` compila el SPIR-V para el Mali-G52 con el compilador real de ARM, pero no guarda el binario.
`volcar_mbs2.py` lo engancha con Frida (`cmpbe_v2_compile_multiple_shaders`) y guarda el bloque MBS2 de cada shader:
código del principal, código del piloto, descriptor (`PDSC`), varyings y tabla de UBO.

```bash
python tools/mali_arm/volcar_mbs2.py out/nfsmw_shaders_mali.nfsp out/mali_arm
python tools/mali_arm/variantes.py out/capturas/nfsmw_nativo_pipelines.bin out/mali_arm
```

- `volcar_mbs2.py` deja `spv/NNN.{vert,frag}.spv`, `mbs2/NNN_*.mbs2` (y `.h` en C) y `stats/NNN.json`.
- Las estadísticas de `stats/` son las de `malioc` en JSON: ciclos por unidad (aritmética, load/store, varyings,
  texturas), registros de trabajo, `fp16_arithmetic`, `modifies_coverage` y `has_uniform_computation`. Es la forma
  más rápida de comparar un shader antes y después de un cambio, sin tocar el teléfono.
- El juego especializa cada pipeline con una constante de 32 bits. `variantes.py` toma los valores de la lista de
  pipelines, congela cada variante con `spirv-opt` y la compila. Usar las variantes para medir lo que corre de
  verdad.
- Desensamblar un binario: `out/bidis/bidis archivo.bin` (en WSL). El principal es `_obj0`, el piloto `_obj1`.

## 4. Correlacionar y pilotos

```bash
python tools/mali_arm/correlacionar.py out/capturas/captura_mali_1000.bin 0x7f0067c80
python tools/mali_arm/piloto.py captura.bin out/mali_arm/dis/063_0_obj1.txt <tabla_ubo> <bloque_capturado>
```

- `correlacionar.py` empareja cada shader capturado con el de `malioc` por la secuencia de operaciones (los bytes no
  coinciden entre versiones del compilador). Deja los desensamblados en `out/mali_arm/dis/`.
- `piloto.py` interpreta el piloto sobre la memoria capturada y compara el bloque que produce con el que escribió
  el driver. Así se validaron los pilotos de los 56 fragmentos.

## 5. Medir en el teléfono

- **FPS:** el contador del juego (arriba a la izquierda). Comparar siempre en el mismo lugar de la misma carrera:
  menús y carrera no se comparan entre sí.
- **Uso de la GPU:** `adb shell cat /sys/kernel/gpu/gpu_busy` (porcentaje) y `/sys/kernel/gpu/gpu_clock` (frecuencia
  en kHz; 560000 = 560 MHz). Cerca de 100% en carrera quiere decir que el cuello es la GPU. Tomar varias lecturas:
  es un valor instantáneo.
- **CPU por hilo:** `adb shell top -H -p $(adb shell pidof com.nfsmw.android) -n 1 -b`. Los hilos se llaman
  "GPU anillo", "Main XThread", etc. Un hilo cerca del 100% de un núcleo marca un cuello de CPU.
- **Memoria:** el encabezado de `top` (libre y swap). Con poca memoria libre aparecen tirones.
- **Imagen:** `adb exec-out screencap -p > pantalla.png`.
- **Arrancar el juego desde la PC:** `adb shell monkey -p com.nfsmw.android -c android.intent.category.LAUNCHER 1`
  y tocar "JUGAR" (`uiautomator dump` da su posición).
- **Perfil de CPU:** simpleperf necesita un APK depurable (`debuggable true` temporal en `build.gradle`, que no se
  sube).

### adb

- En Git Bash, `MSYS_NO_PATHCONV=1` antes de cualquier comando con rutas `/storage/...`.
- Inalámbrico: si hay dos dispositivos (USB y wifi), fijar `ANDROID_SERIAL` con el nombre que da `adb devices`
  (p. ej. `adb-RF8R4237XQK-WZequ4 (2)._adb-tls-connect._tcp`).
- Envolver los comandos con `timeout`: adb se cuelga si el teléfono se desconecta.
