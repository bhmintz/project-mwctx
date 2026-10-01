#!/usr/bin/env python
"""recompilar_mali.py - como mw.py pero apuntando al backend Mali (Fase 0+).

Reusa tools/mw.py (build / install / run / log / codegen) y, ANTES de compilar, deja el
proyecto "apuntando" a lo que implementamos del backend Mali:

  - android/app/src/main/assets/nfsmw.toml -> nfsmw_renderizador = "nativo"
                                              nfsmw_nativo_mali   = -1 (auto) / 0 / 1
    (sin esto corre el backend "xenos" del experimento viejo y la Fase 0 ni se ejecuta.)
  - android/local.properties               -> tus rutas/versiones si faltan, para no caer
    al CMake heredado ([CXX1300] CMake 3.30.5 was not found).

Ademas, el logcat en vivo resalta las lineas del sub-modo Mali (el gate, "sub-modo Mali = SI",
"no se dibuja", shaderInt64, etc.).

USO (desde Windows, igual que mw.py; 'adb' en el PATH):

    python tools/recompilar_mali.py            # prep + build + install + run  (lo comun)
    python tools/recompilar_mali.py --log      # ...y captura logcat con filtro Mali
    python tools/recompilar_mali.py build      # solo compilar
    python tools/recompilar_mali.py install|run|stop|log|codegen
    python tools/recompilar_mali.py --mali on  # forzar el sub-modo Mali (def: auto)

El resto de flags (--debug, --clean, --no-install, --no-run, --all, --out, --app) se pasan a mw.py.
Para no tocar nada de config: --sin-config (toml) y --sin-local-properties.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

# Reusar la estructura de mw.py (mismo directorio tools/).
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mw  # noqa: E402

TOML = mw.REPO / "android" / "app" / "src" / "main" / "assets" / "nfsmw.toml"
LOCAL_PROPS = mw.ANDROID_DIR / "local.properties"
SDK_DIR_DEFECTO_WIN = "D:/android/sdk"  # ruta de esta maquina (docs/NOTAS §2.3/2.4)

# Lo que interesa ver en vivo del sub-modo Mali (reemplaza el filtro generico de mw.py).
FILTRO_MALI = re.compile(
    r"sub-modo Mali|nfsmw_nativo_mali|no se dibuja|no se pudieron preparar|"
    r"shaderInt64|bufferDeviceAddress|runtimeDescriptorArray|shaderSampledImageArray|"
    r"\[nativo\]|C6|FATAL|tombstone|VK_ERROR|SIGSEGV|AndroidRuntime",
    re.IGNORECASE)

MALI_VALOR = {"auto": "-1", "off": "0", "on": "1"}


# --------------------------------------------------------------------------------------
# apuntar el toml al renderizador nativo + sub-modo Mali
# --------------------------------------------------------------------------------------
def asegurar_config_mali(valor: str) -> None:
    mw.c("Config: dejando el toml en renderizador nativo + sub-modo Mali")
    if not TOML.exists():
        mw.die(f"no encuentro el toml: {TOML}")
    lineas = TOML.read_text(encoding="utf-8", errors="ignore").splitlines()
    salida, puesto_mali = [], False
    for ln in lineas:
        s = ln.strip()
        if re.match(r"nfsmw_renderizador\s*=", s):
            salida.append('nfsmw_renderizador = "nativo"')
        elif re.match(r"nfsmw_nativo_mali\s*=", s):
            salida.append(f"nfsmw_nativo_mali = {valor}")
            puesto_mali = True
        elif "EXPERIMENTO Mali-G52: backend portable" in s:
            salida.append("# Backend Mali (Fase 0+): renderizador nativo con sub-modo Mali (sin bindless/Int64/BDA).")
        elif 'Revertir a "nativo"' in s:
            salida.append("# nfsmw_nativo_mali: -1 auto (GPUs sin bindless), 0 off, 1 forzado. Ver docs/plan-backend-mali.md.")
        else:
            salida.append(ln)
    if not puesto_mali:
        reconstruido = []
        for ln in salida:
            reconstruido.append(ln)
            if ln.strip() == 'nfsmw_renderizador = "nativo"':
                reconstruido.append(f"nfsmw_nativo_mali = {valor}")
        salida = reconstruido
    TOML.write_text("\n".join(salida) + "\n", encoding="utf-8")
    print(f'OK  nfsmw_renderizador = "nativo", nfsmw_nativo_mali = {valor}')


# --------------------------------------------------------------------------------------
# android/local.properties con tus rutas/versiones (gitignored -> no viaja por git)
# --------------------------------------------------------------------------------------
def _subdirs(d: Path) -> list[str]:
    try:
        return [p.name for p in d.iterdir() if p.is_dir()]
    except OSError:
        return []


def _mayor_version(nombres: list[str]) -> str | None:
    def clave(n: str):
        return [int(t) if t.isdigit() else 0 for t in n.replace("-", ".").split(".")]
    validos = [n for n in nombres if any(c.isdigit() for c in n)]
    return max(validos, key=clave) if validos else None


def _leer_props(p: Path) -> dict:
    props = {}
    if p.exists():
        for ln in p.read_text(encoding="utf-8", errors="ignore").splitlines():
            s = ln.strip()
            if s and not s.startswith("#") and "=" in s:
                k, _, v = s.partition("=")
                props[k.strip()] = v.strip()
    return props


def asegurar_local_properties(sdk_dir, cmake_ver, ndk_ver, forzar) -> None:
    mw.c("local.properties: asegurando tus rutas y versiones (gitignored)")
    existentes = _leer_props(LOCAL_PROPS)
    sdk = (sdk_dir or existentes.get("sdk.dir") or os.environ.get("ANDROID_HOME")
           or os.environ.get("ANDROID_SDK_ROOT") or SDK_DIR_DEFECTO_WIN).replace("\\", "/")
    sdk_fs = Path(sdk)
    if not sdk_fs.exists():
        print(f"  aviso: no veo el SDK en {sdk}; se escribe igual (usa --sdk-dir si la ruta es otra).")

    deseados = {"sdk.dir": sdk}
    cmake = cmake_ver or _mayor_version(_subdirs(sdk_fs / "cmake"))
    ndk = ndk_ver or _mayor_version(_subdirs(sdk_fs / "ndk"))
    plat = _mayor_version([n.replace("android-", "") for n in _subdirs(sdk_fs / "platforms")])
    if cmake:
        deseados["nfsmw.cmakeVersion"] = cmake
    if ndk:
        deseados["nfsmw.ndkVersion"] = ndk
    if plat:
        deseados["nfsmw.compileSdk"] = plat
        deseados["nfsmw.targetSdk"] = plat

    por_poner = {k: v for k, v in deseados.items() if forzar or k not in existentes}
    lineas = LOCAL_PROPS.read_text(encoding="utf-8", errors="ignore").splitlines() if LOCAL_PROPS.exists() else []
    salida, vistas = [], set()
    for ln in lineas:
        s = ln.strip()
        if s and not s.startswith("#") and "=" in s and s.partition("=")[0].strip() in por_poner:
            k = s.partition("=")[0].strip()
            salida.append(f"{k}={por_poner[k]}")
            vistas.add(k)
        else:
            salida.append(ln)
    anexar = {k: v for k, v in por_poner.items() if k not in vistas}
    if anexar:
        if salida and salida[-1].strip():
            salida.append("")
        salida.append("# Rutas/versiones de esta maquina (tools/recompilar_mali.py; gitignored)")
        salida += [f"{k}={v}" for k, v in anexar.items()]
    LOCAL_PROPS.parent.mkdir(parents=True, exist_ok=True)
    LOCAL_PROPS.write_text("\n".join(salida) + "\n", encoding="utf-8")
    efectivos = _leer_props(LOCAL_PROPS)
    print("  valores efectivos: " + ", ".join(f"{k}={efectivos.get(k)}" for k in deseados))
    if not por_poner:
        print("  (ya estaba todo; no se cambio nada)")


# --------------------------------------------------------------------------------------
# main: prep (toml + local.properties) y delegar en mw.py
# --------------------------------------------------------------------------------------
def main(argv=None) -> None:
    ap = argparse.ArgumentParser(
        prog="recompilar_mali.py",
        description="Como mw.py pero apuntando al backend Mali (renderizador nativo + nfsmw_nativo_mali).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Sin accion = build + install + run (tras preparar toml y local.properties).")
    ap.add_argument("accion", nargs="?", default="loop",
                    choices=["loop", "all", "build", "install", "run", "stop", "log", "codegen"],
                    help="que hacer (def: loop = build+install+run)")
    ap.add_argument("--mali", choices=["auto", "on", "off"], default="auto",
                    help="valor de nfsmw_nativo_mali en el toml (def: auto = -1)")
    ap.add_argument("--sin-config", action="store_true", help="no tocar el toml")
    ap.add_argument("--sin-local-properties", action="store_true", help="no tocar android/local.properties")
    ap.add_argument("--sdk-dir", default=None, help="ruta del Android SDK (def: la ya puesta o D:/android/sdk)")
    ap.add_argument("--cmake-version", default=None, help="forzar nfsmw.cmakeVersion (def: la mayor instalada)")
    ap.add_argument("--ndk-version", default=None, help="forzar nfsmw.ndkVersion (def: la mayor instalada)")
    ap.add_argument("--forzar-local-properties", action="store_true", help="sobrescribir claves aunque existan")
    # Pass-through a mw.py:
    ap.add_argument("--debug", action="store_true", help="variante debug")
    ap.add_argument("--clean", action="store_true", help="gradle clean antes de compilar")
    ap.add_argument("--no-install", action="store_true", help="no instalar")
    ap.add_argument("--no-run", action="store_true", help="no lanzar")
    ap.add_argument("--log", action="store_true", help="capturar logcat al final (filtro Mali)")
    ap.add_argument("--all", action="store_true", help="(con log) mostrar todo en vivo")
    ap.add_argument("--out", default=None, help="(con log) archivo de salida")
    ap.add_argument("--app", default="app", help="(codegen) carpeta de la edicion")
    args = ap.parse_args(argv)

    # Namespace que esperan las funciones de mw.py.
    ns = argparse.Namespace(debug=args.debug, clean=args.clean, no_install=args.no_install,
                            no_run=args.no_run, log=args.log, all=args.all, out=args.out, app=args.app)

    va_a_compilar = args.accion in ("loop", "all", "build")
    if va_a_compilar:
        if not args.sin_config:
            asegurar_config_mali(MALI_VALOR[args.mali])
        if not args.sin_local_properties:
            asegurar_local_properties(args.sdk_dir, args.cmake_version, args.ndk_version,
                                      args.forzar_local_properties)

    # El logcat del sub-modo Mali usa nuestro filtro.
    if args.log or args.accion == "log":
        mw.LIVE_FILTER = FILTRO_MALI

    despacho = {
        "build": mw.cmd_build, "install": mw.cmd_install, "run": mw.cmd_run,
        "stop": mw.cmd_stop, "log": mw.cmd_log, "codegen": mw.cmd_codegen,
        "loop": mw.cmd_loop, "all": mw.cmd_loop,
    }
    despacho[args.accion](ns)


if __name__ == "__main__":
    main()
