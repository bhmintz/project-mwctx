<div align="center">

# NFSMW Android Evolved

**Need for Speed: Most Wanted (2005) para Android ARM64**

APK nativo · Vulkan · controles táctiles · generación local de shaders

[Descargar la última versión](https://github.com/codepdbh/nfsmw-android/releases/latest)

</div>

Este proyecto adapta a Android el trabajo de recompilación de [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) y el SDK [ReXGlue](https://github.com/rexglue/rexglue-sdk). La aplicación usa código C++ recompilado, SDL3 y el renderizador nativo sobre Vulkan. No emula una Xbox 360 ni incluye los archivos del juego.

## Requisitos

- Android 8 o posterior y procesador ARM64 compatible con ARMv8.2-A.
- GPU con Vulkan y un controlador compatible con el renderizador del port. Se han probado un **Samsung Galaxy S25 Ultra** (audio y juego en v0.3.3) y un **Samsung Galaxy A55 con Xclipse 530** (corrección gráfica en v0.3.4); la compatibilidad y el rendimiento en otros teléfonos pueden variar.
- Una copia propia de **Need for Speed: Most Wanted (2005), Xbox 360, edición PAL España**, extraída y con `default.xex`, `NFS/` y `Movies/`. Este APK se compila para esa edición. Los archivos de la versión de PC, PS2 o una ISO sin extraer no sirven para estos pasos.
- Espacio en la memoria interna para el APK, la carpeta completa del juego y los archivos generados. Si importas una carpeta que ya está en el teléfono, necesitas espacio para una copia adicional durante la importación.

## Instalación y primer inicio

1. Descarga `NFSMW-Android-Evolved-v0.3.4.apk` desde [Releases](https://github.com/codepdbh/nfsmw-android/releases/latest) y ábrelo en el teléfono. Si Android lo solicita, permite **Instalar aplicaciones desconocidas** al navegador o gestor de archivos que estés usando.
2. Instala el APK y abre **Need for Speed Most Wanted**. Autoriza el acceso a archivos que solicita la app; en Android 11 o posterior aparece el ajuste de **acceso a todos los archivos**. Después vuelve al launcher.
3. Copia `default.xex`, `NFS/` y `Movies/` directamente dentro de `Memoria interna/nsfmw-androidevolved/`. También puedes pulsar **Elegir carpeta del juego** y seleccionar la carpeta extraída que contiene esos tres elementos; la app la copia a ese destino.
4. Comprueba que el launcher marque los archivos como disponibles y pulsa **Jugar**. La primera vez genera `nfsmw_shaders.nfsp` a partir de tu copia y muestra el avance. Espera a que termine; puede tardar varios minutos según el dispositivo.
5. Al terminar, se abre el juego. Los siguientes inicios usan la biblioteca generada. Para empezar, puedes seleccionar 1280×720 y 60 FPS en el launcher y ajustar después según el rendimiento del teléfono.

La carpeta debe quedar así:

```text
Memoria interna/nsfmw-androidevolved/
├── default.xex
├── NFS/
├── Movies/
└── nfsmw_shaders.nfsp   (generado por la app)
```

No descargues ni compartas archivos del juego. La biblioteca de shaders se genera en el dispositivo desde los archivos locales de tu copia.

### Actualizar desde una versión anterior

Instala el nuevo APK encima del anterior, **sin desinstalar ni borrar los datos de la app**. Las versiones publicadas en este repositorio usan la misma firma y la actualización conserva las partidas y los ajustes. Si ya tienes los archivos del juego y los shaders, no hace falta importarlos ni generarlos otra vez.

### Si no aparece Jugar

- Revisa el permiso de archivos y vuelve a abrir la app.
- Comprueba que `default.xex` esté directamente en `nsfmw-androidevolved/`, junto a `NFS/` y `Movies/`. Evita una carpeta adicional como `nsfmw-androidevolved/Need for Speed Most Wanted/default.xex`.
- Si la importación falla, revisa el espacio libre y selecciona la carpeta que contiene los tres elementos, no la carpeta `NFS` por separado.
- Si Android rechaza la actualización por una firma diferente, la instalación anterior procede de otra compilación. Conserva tus partidas antes de cambiar de instalación.

## Launcher, ajustes y controles

En el launcher puedes cambiar resolución interna, límite de FPS, antialiasing, sombras, reflejos del coche y del asfalto, resplandor del cielo y filtro de imagen. La app también guarda ajustes de controles y formato de pantalla.

En GPU modestas (por ejemplo la **Mali-G52** del Helio G80, o Mali antiguas, Adreno por debajo de la serie 600 y PowerVR) el launcher detecta la GPU y aplica **valores gráficos más ligeros por defecto** (resolución 1024×576, sombras cada 2 fotogramas y a 1024 px, reflejos del coche bajos y asfalto mojado desactivado) para ganar fluidez. Es solo el punto de partida: cualquier opción que elijas tú se respeta, y en GPU potentes (Adreno 830, Xclipse) no cambia nada. El launcher muestra la GPU detectada y avisa cuando el perfil está activo.

El juego se abre en horizontal. La superposición táctil incluye dirección, botones de acción, START, freno y acelerador. Desde el editor de controles puedes mover y redimensionar botones, ocultarlos y ajustar su opacidad. También se admiten mandos Bluetooth y USB.

## Gráficos en v0.3.4

Se corrigieron bloques, manchas y reflejos incorrectos en la carrocería que aparecían tanto en el menú como durante las carreras del Galaxy A55. El renderizador ahora sincroniza las copias de texturas y sus lecturas entre pases de Vulkan. La mejora se comprobó en el teléfono y fue confirmada por su usuario. La corrección se activa automáticamente y no requiere cambiar los archivos del juego ni regenerar los shaders. Consulta [el diagnóstico de Xclipse](docs/android-xclipse-diagnostic.md) para los detalles de la prueba.

## Audio desde v0.3.3

Se corrigió el ruido de los logos y los videos iniciales, incluida la voz de la chica, y el audio doble de las cinemáticas de historia. También se ajustó la salida del juego para reducir cortes y distorsión. La reproducción de intros, cinemáticas de historia y gameplay se comprobó en un Galaxy S25 Ultra con la edición PAL española. Las pruebas técnicas están descritas en [Audio en Android](docs/android-audio.md).

## Compilar

Requisitos: Android SDK, NDK `28.2.13676358`, JDK 17 o posterior y PowerShell.

```powershell
.\build_android.ps1
```

El APK Release se genera en `android/app/build/outputs/apk/release/app-release.apk`. La compilación usa optimización nativa Release y está configurada para `arm64-v8a`.

## Proyecto y licencias

- `android/`: launcher, integración SDL y build Android.
- `app/`, `sdk/`: aplicación recompilada y ReXGlue.
- `docs/`: notas del port y compilación.

Los archivos del juego y el código generado desde `default.xex` se mantienen fuera de Git. Consulta [`LICENSE`](LICENSE) y [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) para las licencias. Proyecto de aficionados, sin afiliación con Electronic Arts; “Need for Speed” es una marca de Electronic Arts Inc.
