# Herramientas para medir y analizar el Mali-G52

Las herramientas que se armaron para entender qué hace el driver de Samsung, qué produce el compilador de ARM y qué
hace PanVK, con cómo se compila cada una y qué necesita. Lo que se descubrió con ellas está en
[plan-arm-hibrido.md](plan-arm-hibrido.md) y, para el driver de Samsung, en `docs/mejoras-mali-vk11.md` de la rama
`master`. La forma de medir sin engañarse está en [measuring.md](measuring.md).

Todo lo generado (capturas, binarios de ARM, builds de Mesa) va en `out/`, que git ignora. Lo que hay que conservar
está en `tools/mali_arm/`, incluido el parche de nuestro PanVK.

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
| Empaquetar binarios de ARM para nuestro PanVK | `paquete_arm.py` | Windows |
| Comparar el intérprete de pilotos en C con el de Python | `prueba_piloto/` | WSL |
| Compilar nuestro PanVK (JimVulkan + parche) | `compilar_jim.sh`, `empaquetar_jim.py` | WSL / Windows |
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
- Para compilar PanVK (lo que pide `out/jim/mesa/build.sh`, más lo que faltó en la práctica): `meson`, `ninja`,
  `pkg-config`, `glslang-tools`, `python3-mako`, `python3-packaging`, `python3-yaml`, LLVM 18 completo
  (`llvm-18-dev`, `libclang-18-dev`, `clang-18`, `libclc-18-dev`, `libllvmspirvlib-18-dev`) y
  `spirv-tools-dev`. Si el sistema tiene otro LLVM por defecto (había un 21), `compilar_jim.sh` fuerza el 18 con un
  archivo nativo de meson (`llvm18.ini`).
- El NDK r29 **de Linux**, en `out/jim/android-ndk-r29` (el de Windows no sirve dentro de WSL).

### Disco

El disco virtual de WSL está en `C:` y `C:` tiene poco espacio: llenarlo dejó al sistema sin disco una vez. Todo lo
grande (NDK, builds de Mesa, capturas de 500 MB) va en `D:`, trabajando desde `/mnt/d/...`. Si `C:` se llena:
`wsl --shutdown`, borrar lo parcial, `sudo apt-get clean` y compactar el disco virtual (diskpart) desde Windows.

## 1. Capturar el driver (en el teléfono)

`android/app/src/main/cpp/android_captura_mali.cpp`, detrás de la cvar `nfsmw_captura_mali` (apagada por defecto).
Engancha `ioctl`, `mmap`, `munmap` y `read`/`__read_chk` del driver de Mali y, durante el fotograma N, guarda cada
envío de trabajos, la memoria a la que apunta y lo que la GPU escribió después.

- Activar: en la carpeta del juego, `nfsmw_args.txt` con una línea `--nfsmw_captura_mali=1000` (fotograma 1000,
  ya en carrera). El archivo reemplaza los argumentos del lanzador con el mismo nombre.
- Sale `captura_mali_1000.bin` en la carpeta del juego (unos 500 MB), más una copia de las listas de pipelines del
  juego (`nfsmw_nativo_pipelines*.bin`), que `variantes.py` necesita.
- Funciona con el driver del sistema y con un driver propio (`vulkan_icd_android`, p. ej. nuestro PanVK). Con el
  driver propio engancha esa librería.
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

## 5. Binarios de ARM en nuestro PanVK

```bash
python tools/mali_arm/paquete_arm.py out/capturas/nfsmw_nativo_pipelines.bin out/mali_arm out/mali_arm/arm_todos
MSYS_NO_PATHCONV=1 adb push out/mali_arm/arm_todos/. /storage/emulated/0/nsfmw-androidevolved/arm/
```

- Un `.arm` por variante de fragmento, con nombre `<hash>.arm`. El hash es FNV-1a de 64 bits del SPIR-V y de los
  datos de especialización, y nuestro PanVK calcula el mismo.
- Se activan con una línea en `nfsmw_args.txt`:
  `--vulkan_driver_env=NFSMW_ARM_DIR=/storage/emulated/0/nsfmw-androidevolved/arm` (varias variables se separan con
  `;`). Sin esa variable, el driver es PanVK normal.
- En logcat (`adb logcat -d | grep MESA`):
  - `nfsmw: binario de ARM <hash>`: el fragmento usa el código de ARM.
  - `sin binario de ARM <hash>`: no hay paquete para ese hash.
  - `nfsmw: piloto ...`: los primeros bloques que llenó el piloto, para compararlos con `piloto.py`.
  - `the kbase submission did not complete`: la GPU falló (ver los códigos en la sección 2).

Prueba del intérprete en C contra el de Python (WSL):

```bash
python tools/mali_arm/prueba_piloto/exportar.py captura.bin <tabla_ubo_hw> out/mali_arm/caso_063.bin
gcc -O1 -w -Itools/mali_arm/prueba_piloto -Iout/jim/mesa/src/panfrost/vulkan -o /tmp/prueba_piloto \
    tools/mali_arm/prueba_piloto/prueba.c out/jim/mesa/src/panfrost/vulkan/panvk_nfsmw_arm.c -lm
NFSMW_ARM_DIR=out/mali_arm/arm /tmp/prueba_piloto a17692be07e6899c out/mali_arm/caso_063.bin > /tmp/c.txt
```

Redirigir la salida directo a `/mnt/d` dejó archivos vacíos alguna vez: escribir en `/tmp` y copiar.

Lecciones del adaptador (cada una costó un menú negro):
- El descriptor del shader tiene que precargar todos los uniformes que escribe el piloto (`info->fau.count`). Si
  no, los `TEXC` leen descriptores basura.
- Después del código van 128 bytes en cero, por el prefetch de instrucciones.
- Los `TEXC` de ARM toman los índices de registros, primero el sampler y después la textura. Solo los samplers van
  corridos en +1 respecto del heap del juego.
- El caché de pipelines guarda si se aplicó ARM y vuelve a compilar cuando aparece o desaparece un paquete.

## 6. Medir en el teléfono

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

## 7. Compilar nuestro PanVK

Es JimVulkan (PanVK de Mesa para Bifrost sobre kbase, `github.com/JimVulkan/mali-panvk`, commit `fb3b49d`) más
`tools/mali_arm/jimvulkan-nfsmw.patch`.

Primera vez (WSL, desde la raíz del repo):

```bash
# out/mesa-jim ya clonado (sección 2)
git clone out/mesa-jim out/jim/mesa
git -C out/jim/mesa apply ../../../tools/mali_arm/jimvulkan-nfsmw.patch
# NDK r29 de Linux descomprimido en out/jim/android-ndk-r29
```

Cada vez:

```bash
sh tools/mali_arm/compilar_jim.sh > out/jim/build.log 2>&1   # WSL; la primera vez tarda (etapa host + Android)
python tools/mali_arm/empaquetar_jim.py                       # Windows; deja out/jim/panvk-nfsmw.zip
MSYS_NO_PATHCONV=1 adb push out/jim/panvk-nfsmw.zip /storage/emulated/0/nsfmw-androidevolved/drivers/
```

- `compilar_jim.sh` configura `build-host` con LLVM 18 la primera vez y después llama a `build.sh` con el NDK.
  Las herramientas del host (`mesa_clc`, `panfrost_compile`) se compilan del mismo árbol.
- `empaquetar_jim.py` renombra la librería a `libvulkan_panfrost_nfsmw.so`. El juego copia el zip de `drivers/`
  cuando cambian su tamaño o su fecha, y se elige con la opción `vulkan_driver` del lanzador.
- Para actualizar el parche después de tocar `out/jim/mesa`:
  `git -C out/jim/mesa add -N src/panfrost/vulkan/panvk_nfsmw_arm.c src/panfrost/vulkan/panvk_nfsmw_arm.h` y
  `git -C out/jim/mesa diff -- src > tools/mali_arm/jimvulkan-nfsmw.patch`.
- Opciones de depuración de PanVK por `vulkan_driver_env`: `PANVK_DEBUG=trace,sync,dump` con
  `PANDECODE_DUMP_FILE=<archivo>`. Hacen todo mucho más lento; usarlas solo para atrapar un fallo.
