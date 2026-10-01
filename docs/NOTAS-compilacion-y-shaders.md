# Notas de compilación, shaders y traspaso (Helio G80 / Mali-G52)

> Documento de trabajo personal (en español) para continuar en otra sesión sin re-descubrir todo.
> Fecha: 2025-10-01. Máquina: Windows 11 + WSL2 Ubuntu 26.04. Objetivo: que NFSMW corra bien en un
> **Helio G80 (GPU Mali-G52 MC2)**, que es mucho más débil que las Adreno/Xclipse con las que se probó el port.

---

## 0. ESTADO ACTUAL — problema abierto (empezar por aquí)

> **ACTUALIZACIÓN 2025-10-01 (sesión 2): logcat capturado, causa encontrada.**
> No era un crash: el proceso salía limpio (`exit 0`) porque el selector Vulkan **rechazaba** la
> Mali-G52 por features faltantes. Tras parchear eso, la app arranca (suena audio + overlay de
> controles) pero queda **en negro porque el renderizador nativo exige `shaderInt64` +
> `bufferDeviceAddress` + bindless**, que la Mali-G52 (driver r26p0) no tiene. Detalle completo,
> parches, commits y opciones en **`docs/diagnostico-crash-mali-g52.md`**. Lo de abajo queda como
> referencia histórica del arranque de la investigación.


- El APK **compila entero desde cero** con el SDK/NDK de esta máquina (ver §2). Eso funciona.
- **PERO el juego crashea al pulsar "Jugar"**, y esto pasa **igual con la APK oficial (pensada para Adreno) y con la APK modeada al mínimo**. Probado en el Helio G80 tras instalar e importar los archivos del juego correctamente.
- **Conclusión:** el crash **NO** lo causa el perfil Mali ni los cambios de este trabajo. Es un problema de **arranque del juego en este dispositivo**. El port solo se ha probado en **Adreno 830** y **Xclipse 530**; **nunca en una GPU Mali**. Lo más probable es una incompatibilidad del **driver Vulkan de Mali** (o de la edición/archivos), no un tema de rendimiento.

### Lo primero que hay que hacer en el próximo chat
Capturar un **logcat** del crash para saber la causa real:

```powershell
# Con el teléfono conectado por USB y depuración USB activada:
adb logcat -c                      # limpia el buffer
# (pulsar "Jugar" en el teléfono para provocar el crash)
adb logcat -d > crash_g80.txt      # vuelca el log a archivo
```

Filtrar por: `NFSMW`, `nativo`, `rex`, `Vulkan`, `VK_ERROR`, `SIGSEGV`, `DEBUG`, `libmain`, `tombstone`,
`Adreno`/`Mali`. Buscar: qué extensión/feature de Vulkan pide el renderizador que la Mali-G52 no tiene,
o si peta durante la carga de `nfsmw_shaders.nfsp`, o en la creación de la primera pipeline/surface.
Comparar con el log de un dispositivo donde SÍ funciona (Adreno) ayuda mucho.

Pistas de dónde mirar en el código del renderizador:
- `app/src/nfsmw_nativo_destinos.cpp` — creación de dispositivo/surface Vulkan, lee `VkPhysicalDeviceProperties`
  (sobre la línea 1037). Aquí se vería si falta algún límite/feature en Mali.
- Formatos de textura/vértice, MSAA, o alguna extensión asumida que Adreno/Xclipse tienen y Mali no.
- La corrección de v0.3.4 (sincronización de copias de textura entre pases) añade **barreras**; en Mali (TBDR)
  las barreras fuerzan *flush* de tiles — mirar si algo ahí peta o cuelga en Mali.

---

## 1. Hardware/edición objetivo

- **GPU:** Mali-G52 MC2 (SoC MediaTek Helio G80). Bifrost, gama de entrada. FP16 ~2× el throughput de FP32;
  muy sensible a presión de registros y a barreras/flushes de tiles (TBDR).
- **Juego:** NFS Most Wanted (2005), **Xbox 360, edición PAL España**. El `default.xex` del usuario está en
  `D:\users\projects\x360\nfs\Need for Speed - Most Wanted (Spain)\` (con `NFS/` y `Movies/`). Esa edición es
  justo la que espera la carpeta `app/` del proyecto (ver `docs/editions.md`).

---

## 2. CÓMO COMPILAR DESDE CERO (Windows + WSL)

El codegen (traducir el `.xex` a C++) se hace en **WSL Ubuntu** (Linux); el **APK** se arma en **Windows**.
Ambos ven el repo en la misma ruta (`/mnt/d/...` desde WSL). **La versión del toolchain NUNCA fue el problema**
(clang 21 / NDK 29 / CMake 3.31.6 compilan el C++ recompilado sin incidencias).

### 2.1 Requisitos del host en WSL (una vez)
```bash
sudo apt update && sudo apt install -y clang lld cmake ninja-build git python3 python-is-python3 build-essential
sudo apt install -y pkg-config libx11-xcb-dev libwayland-dev   # los exige src/ui para CONFIGURAR en Linux
```
(ReXGlue exige **Clang ≥ 18**; aquí hay clang 21, OK.)

### 2.2 Pasos (desde la raíz del repo, en WSL salvo que se indique)
```bash
cd /mnt/d/users/projects/x360/extract-xiso-Win64_Release/nfsmw-android-main

# (1) Dependencias de terceros (clona rexglue-sdk + submódulos a sdk/thirdparty). Tarda; FFmpeg es el pesado.
python3 tools/fetch_thirdparty.py

# (2) Colocar el juego donde lo espera el manifest (app/nfsmw_manifest.toml -> ../assets/game_root/default.xex)
mkdir -p assets/game_root
cp "/mnt/d/users/projects/x360/nfs/Need for Speed - Most Wanted (Spain)/default.xex" assets/game_root/default.xex

# (3) Compilar el generador de código (host). Salida -> sdk/out/linux-amd64/rexglue
cmake -S sdk -B ~/rexglue-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_C_FLAGS=-msse4.1 -DCMAKE_CXX_FLAGS=-msse4.1
cmake --build ~/rexglue-build --target rexglue -j"$(nproc)"

# (4) Codegen: traduce el .xex a C++ en app/generated/ (incl. rexglue.cmake, imprescindible para el build)
REXGLUE="$PWD/sdk/out/linux-amd64/rexglue" PYTHON=python3 tools/codegen.sh app
```
```powershell
# (5) APK en WINDOWS (PowerShell). Produce android/app/build/outputs/apk/release/app-release.apk
cd D:\users\projects\x360\extract-xiso-Win64_Release\nfsmw-android-main
.\build_android.ps1
#   build_android.ps1 usa el wrapper (descarga Gradle 8.9 la primera vez).
#   Alternativa sin descarga, usando un Gradle ya cacheado (8.10.2) y el SDK local:
#   $env:ANDROID_HOME="D:\android\sdk"
#   & "<...>\gradle-8.10.2\bin\gradle.bat" -p "<repo>\android" :app:assembleRelease --console=plain
```

### 2.3 Arreglos que hubo que hacer (NO son por versiones; dejar aplicados)
1. **`android/local.properties`** (ignorado por git): apunta el SDK y fuerza las versiones instaladas en vez de
   las canónicas (35 / NDK 28.2 / CMake 3.30.5). Contenido:
   ```
   sdk.dir=D:/android/sdk
   nfsmw.compileSdk=34
   nfsmw.targetSdk=34
   nfsmw.ndkVersion=29.0.14206865
   nfsmw.cmakeVersion=3.31.6
   ```
   `android/app/build.gradle` fue modificado para leer estas claves (con los valores canónicos por defecto, así
   el repo/CI no cambian).
2. **`tools/fetch_thirdparty.py`**: parcheado para saltar `moltenvk` (solo macOS) y symlinks colgantes. El script
   original está pensado para Windows; en WSL git crea symlinks reales y petaba con uno de moltenvk.
3. **Flag SSSE3**: `sdk/src/core/memory.cpp` usa `_mm_shuffle_epi8` (SSSE3). El `-msse4.1` de
   `sdk/cmake/rexglue_helpers.cmake:64` NO se aplica a `rexcore`, así que con clang estándar falla. Solución:
   pasar `-msse4.1` como flag global al configurar el host (ver paso 3). No hace falta editar el SDK.
4. **CMake 4** (aquí 3.31.6/4.x): algunos submódulos de thirdparty tienen `cmake_minimum_required` antiguo →
   `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` al configurar.
5. **Red**: `:app:mergeReleaseNativeLibs` puede fallar al bajar un jar de `dl.google.com` si no hay red en ese
   momento; el `.so` nativo ya está cacheado, así que basta reintentar online.

### 2.4 Toolchain presente en esta máquina
- Android SDK en `D:\android\sdk`: platform **android-34**, build-tools 34.0.0, **NDK 29.0.14206865**, CMake 3.31.6.
- WSL Ubuntu 26.04: clang 21.1.8, cmake 4.2.3, ninja 1.13.2, git 2.53, python 3.14.
- AGP 8.7.3, Gradle wrapper pide 8.9 (cacheado hay 8.10.2 / 9.2 / 9.3; 8.10.2 va bien con AGP 8.7.3).
- **No hay compilador de host nativo en Windows** (ni VS ni LLVM); por eso el codegen se hace en WSL.

### 2.5 Atajo: `tools/recompilar_mali.py`
Automatiza este flujo cruzando WSL↔Windows solo. **Ejecutarlo desde WSL** (también acepta Windows):

```bash
python3 tools/recompilar_mali.py             # SOLO el APK (iteración típica de cambios C++ en app/src/)
python3 tools/recompilar_mali.py --codegen   # codegen + APK (cambió el .xex o el manifest)
python3 tools/recompilar_mali.py --all       # desde cero: thirdparty + rexglue + codegen + APK
python3 tools/recompilar_mali.py --install   # además, adb install -r al terminar
```

- Sin flags hace solo el APK porque un cambio C++ (p. ej. las fases del backend Mali) lo recompila
  gradle al armar el APK; codegen/rexglue solo hacen falta si cambia el `.xex`/manifest.
- Desde WSL, el paso del APK invoca `powershell.exe build_android.ps1`; desde Windows, delega los pasos
  de Linux a `wsl.exe`. Reusa los mismos flags/arreglos de §2.2–2.3.
- **Antes del APK genera/completa `android/local.properties`** (gitignored → no viaja por git, por eso el
  build caía a los defaults heredados: `[CXX1300] CMake 3.30.5 was not found`). Detecta tu `sdk.dir` y la
  **mayor** versión instalada de CMake/NDK/plataforma y las escribe como `nfsmw.cmakeVersion` /
  `nfsmw.ndkVersion` / `nfsmw.compileSdk`. Es **additivo**: respeta las claves que ya tengas. Overrides:
  `--sdk-dir`, `--cmake-version`, `--ndk-version`, `--forzar-local-properties`, `--sin-local-properties`.

---

## 3. QUÉ ES EL PROYECTO (resumen de arquitectura)

Port a Android de NFSMW (Xbox 360) basado en **nfsmw-nx** + SDK **ReXGlue**. No emula; **recompila** el ejecutable
Xbox 360 (`default.xex`) a C++ nativo ARM64, y usa **SDL3** + un **renderizador nativo sobre Vulkan**.

- `app/` : la app recompilada (fuentes del port en `app/src/nfsmw_*.cpp`). `app/generated/` lo produce el codegen
  desde el `.xex` (ignorado por git; por eso hay que generarlo para compilar).
- `sdk/` : ReXGlue (recompilador `rexglue`, runtime, backends gráficos). `sdk/thirdparty/` lo baja `fetch_thirdparty.py`.
- `android/` : launcher Java + integración SDL + build Gradle/CMake del APK (`arm64-v8a`).
- `shaders/` : traductor de shaders (ver §4).
- Docs útiles: `docs/shaders.md`, `docs/native-renderer.md`, `docs/building.md`, `docs/platform-notes.md`,
  `docs/android-xclipse-diagnostic.md` (metodología de depuración gráfica).

### Flujo de ajustes gráficos (IMPORTANTE)
El launcher Java (`android/app/.../GameOptions.java`) pasa **todas** las opciones gráficas como `--cvar=valor` en
la línea de comandos, y eso **gana** sobre `nfsmw.toml` y sobre los defaults nativos. Por tanto, el ajuste por GPU
de las opciones visibles vive en **Java (el launcher)**, no en C++.

---

## 4. QUÉ ENTENDÍ DE LOS SHADERS

### Pipeline
Microcódigo **Xenos** (Xbox 360) → **HLSL** (traductor **XenosRecomp**, en `shaders/XenosRecomp/`, cambios del port
marcados `NFSMW_RECOMP`) → **SPIR-V** (compilado con DXC) → empaquetado en **`nfsmw_shaders.nfsp`** (la *shader
library*), que el renderizador carga al inicio. En Android esa librería se **genera en el dispositivo** la primera vez
(botón Jugar → progreso). Clave: `shaders/shader_common.h` (helpers HLSL + *specialization constants*).

### Optimizaciones ya hechas (afinadas para NVK/Mesa en Switch; ver `docs/shaders.md`)
- Constantes vía **UBO dinámico** en vez de puntero de 64 bits (+18-23% FPS en la prueba de Switch).
- **1/tamaño de textura** por constante (elimina consultas de tamaño en `tfetch2D`).
- Fusión de **bloques condicionales `if(p0)`** (1806 → 641).
- `max(a,a)` como copia directa (evita `fmul(a,1.0)` en Mesa).
- Rutas "baratas" por *spec constant*, **activas por defecto**: **PCF barato** (sombra 3x3 → 1 muestra),
  **sin desenfoque radial**, **sombra mínima** en una sola pasada. → Ya benefician a Mali.

### La palanca grande que FALTA para Mali: precisión FP16 (mediump)
- **Todos los shaders traducidos son FP32**: los registros temporales se declaran `float4 r{}` en
  `shaders/XenosRecomp/shader_recompiler.cpp` (~líneas 1509 y 1537).
- En Mali Bifrost, pasar aritmética a **FP16 (`min16float` / RelaxedPrecision)** ~duplica el throughput de ALU y
  baja a la mitad la presión de registros (más hilos en vuelo). Es *la* optimización clásica de Mali y **no está hecha**.
- Plan si se aborda: emitir `min16float4` en GPRs/interpoladores **detrás de un spec constant nuevo** (p. ej.
  `SPEC_CONSTANT_MEDIAP`), manteniendo **FP32 en posición (`oPos`), profundidad y matrices** para no romper
  geometría/z-fighting. Validar con los métodos del repo (conteo de instrucciones, lecturas bit-idénticas) + prueba
  en el teléfono. Riesgo: artefactos de banding/precisión.

### El perfil Mali que SÍ se implementó en esta sesión
Como las rutas baratas ya estaban ON, el peso real en Mali venía de los **defaults del launcher** (arrancaban
altos para cualquier GPU). Se añadió detección de GPU y defaults ligeros automáticos:
- **`android/app/src/main/java/com/nfsmw/android/GpuInfo.java`** (nuevo): lee `GL_RENDERER` con un contexto EGL
  efímero (cacheado) y clasifica como *débil* a Mali-G31/51/52/57, Mali antiguas, Adreno <600 y PowerVR.
- **`GameOptions.java`**: `lowEndDefault` por opción (solo se aplica en GPU débil y si el usuario no ha tocado la
  opción): resolución 1024×576, sombras cada 2 frames, reflejos del coche "bajos", asfalto off; + arg automático
  `--nfsmw_nativo_sombras_escala=64` (mapas de sombra 1024 en vez de 1600).
- **`MainActivity.java`**: muestra `GPU: <nombre>` y el aviso "Perfil de GPU modesta activo".
- **`README.md`**: documentado.
- GPU potentes (Adreno 830, Xclipse) quedan **exactamente igual** que antes. La elección del usuario siempre gana.

> OJO: este perfil asume que el juego **arranca**. Dado el crash de §0, primero hay que lograr que corra en Mali;
> el perfil de rendimiento es el paso siguiente, y el FP16 el salto grande después.

---

## 5. Cambios locales de esta sesión (lista para revisar)
- `android/app/src/main/java/com/nfsmw/android/GpuInfo.java` (nuevo)
- `android/app/src/main/java/com/nfsmw/android/GameOptions.java`
- `android/app/src/main/java/com/nfsmw/android/MainActivity.java`
- `android/app/build.gradle` (lee versiones de `local.properties`)
- `android/local.properties` (nuevo, ignorado por git)
- `tools/fetch_thirdparty.py` (saltar moltenvk/symlinks colgantes)
- `README.md` (sección del perfil Mali)
- `assets/game_root/default.xex` (copiado; ignorado por git)
- APK resultante: `android/app/build/outputs/apk/release/app-release.apk`

---

## 6. Orden sugerido para el próximo chat
1. **Diagnosticar el crash al pulsar Jugar** con `logcat` (§0). Sin esto, nada de rendimiento importa.
2. Si es el driver Vulkan de Mali: ver qué feature/formato/extensión falta y si el renderizador puede evitarlo o
   degradar en Mali. Probar también la opción de driver **Turnip/AdrenoTools** (solo Adreno, no sirve para Mali) vs
   el driver del sistema.
3. Una vez arranque: medir el perfil Mali en el G80 y ajustar defaults.
4. Salto grande de rendimiento: **FP16/mediump** en los shaders (§4).
```
