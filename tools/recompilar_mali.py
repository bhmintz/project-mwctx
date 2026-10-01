#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Recompilar NFSMW (Android) para el perfil Mali-G52, desde Windows + WSL.

Automatiza el flujo documentado en docs/NOTAS-compilacion-y-shaders.md (§2.2):

  (A) Pasos de Linux (se ejecutan en WSL, necesitan clang):
      - thirdparty : python3 tools/fetch_thirdparty.py        (una sola vez)
      - rexglue    : compila el generador de codigo (host)     (una sola vez / si cambia el SDK)
      - codegen    : traduce el .xex a C++ en app/generated/    (si cambia el .xex/manifest)

  (B) Paso de Windows (gradle + NDK, Android SDK en D:\\android\\sdk):
      - apk        : build_android.ps1 -> android/app/build/outputs/apk/release/app-release.apk

El caso comun (un cambio C++ en app/src/, como las fases del backend Mali) NO necesita
codegen ni rexglue: gradle recompila app/src/*.cpp al armar el APK. Por eso, sin argumentos,
este script hace SOLO el APK.

USO (recomendado: ejecutarlo DESDE WSL; tambien funciona desde Windows):

    python3 tools/recompilar_mali.py                 # solo APK (iteracion tipica de cambios C++)
    python3 tools/recompilar_mali.py --codegen       # codegen + APK (cambio el .xex/manifest)
    python3 tools/recompilar_mali.py --all           # desde cero: thirdparty+rexglue+codegen+APK
    python3 tools/recompilar_mali.py --install        # tras el APK, adb install -r
    python3 tools/recompilar_mali.py --no-apk --codegen   # solo regenerar el C++, sin APK

Flags utiles:
    --rexglue            fuerza (re)compilar el generador host
    --thirdparty         fuerza bajar las dependencias de terceros
    --install            instala el APK con adb al terminar
    --rexglue-build-dir  carpeta de build del host (def: ~/rexglue-build)

El script detecta si corre en WSL o en Windows y cruza la frontera automaticamente
(WSL llama a powershell.exe para el APK; Windows llama a wsl.exe para los pasos Linux).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent  # raiz del repo (tools/..)


# --------------------------------------------------------------------------------------
# Deteccion de entorno
# --------------------------------------------------------------------------------------
def es_windows() -> bool:
    return os.name == "nt"


def es_wsl() -> bool:
    if es_windows():
        return False
    # WSL expone "microsoft" en /proc/version y en uname.
    try:
        with open("/proc/version", "r", encoding="utf-8", errors="ignore") as f:
            return "microsoft" in f.read().lower()
    except OSError:
        return "microsoft" in os.uname().release.lower()


def es_linux_puro() -> bool:
    return (not es_windows()) and (not es_wsl())


# --------------------------------------------------------------------------------------
# Utilidades de ejecucion
# --------------------------------------------------------------------------------------
class ErrorPaso(RuntimeError):
    pass


def banner(texto: str) -> None:
    print(f"\n\033[1;36m== {texto}\033[0m", flush=True)


def correr(cmd, cwd: Path | None = None, env: dict | None = None) -> None:
    """Ejecuta un comando y aborta con mensaje claro si falla."""
    mostrado = cmd if isinstance(cmd, str) else " ".join(str(c) for c in cmd)
    print(f"  $ {mostrado}", flush=True)
    entorno = {**os.environ, **(env or {})}
    try:
        subprocess.run(cmd, cwd=str(cwd) if cwd else None, env=entorno,
                       check=True, shell=isinstance(cmd, str))
    except subprocess.CalledProcessError as e:
        raise ErrorPaso(f"fallo (codigo {e.returncode}): {mostrado}") from e
    except FileNotFoundError as e:
        raise ErrorPaso(f"no se encontro el ejecutable: {mostrado} ({e})") from e


def a_ruta_windows(p: Path) -> str:
    """Convierte una ruta de WSL (/mnt/d/...) a ruta Windows (D:\\...)."""
    out = subprocess.run(["wslpath", "-w", str(p)], check=True,
                         capture_output=True, text=True)
    return out.stdout.strip()


def a_ruta_wsl(p: str) -> str:
    """Convierte una ruta Windows (D:\\...) a ruta WSL (/mnt/d/...), desde Windows."""
    out = subprocess.run(["wsl.exe", "wslpath", "-u", str(p)], check=True,
                         capture_output=True, text=True)
    return out.stdout.strip()


# --------------------------------------------------------------------------------------
# Pasos de Linux (WSL)
# --------------------------------------------------------------------------------------
def paso_thirdparty(forzar: bool) -> None:
    destino = RAIZ / "sdk" / "thirdparty"
    ya_esta = destino.is_dir() and any(destino.iterdir())
    if ya_esta and not forzar:
        print("  (sdk/thirdparty ya tiene contenido; se omite. Usa --thirdparty para forzar)")
        return
    banner("thirdparty: bajando dependencias (tools/fetch_thirdparty.py)")
    correr([sys.executable, "tools/fetch_thirdparty.py"], cwd=RAIZ)


def ruta_rexglue() -> Path:
    return RAIZ / "sdk" / "out" / "linux-amd64" / "rexglue"


def paso_rexglue(build_dir: Path, forzar: bool) -> None:
    binario = ruta_rexglue()
    if binario.exists() and not forzar:
        print(f"  (rexglue ya existe en {binario}; se omite. Usa --rexglue para recompilar)")
        return
    banner("rexglue: compilando el generador de codigo (host)")
    # Mismos flags que docs/NOTAS-compilacion-y-shaders.md §2.2:
    #  - -msse4.1 global: rexcore usa SSSE3/_mm_shuffle_epi8 y el flag del helper no llega a rexcore.
    #  - CMAKE_POLICY_VERSION_MINIMUM=3.5: submodulos de thirdparty con cmake_minimum_required antiguo.
    correr([
        "cmake", "-S", "sdk", "-B", str(build_dir), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        "-DCMAKE_C_FLAGS=-msse4.1", "-DCMAKE_CXX_FLAGS=-msse4.1",
    ], cwd=RAIZ)
    nproc = str(os.cpu_count() or 4)
    correr(["cmake", "--build", str(build_dir), "--target", "rexglue", "-j", nproc], cwd=RAIZ)
    if not binario.exists():
        raise ErrorPaso(f"se compilo rexglue pero no aparece en {binario}")


def paso_codegen() -> None:
    binario = ruta_rexglue()
    if not binario.exists():
        raise ErrorPaso(
            f"no existe {binario}. Ejecuta antes con --rexglue (o --all) para compilarlo.")
    xex = RAIZ / "assets" / "game_root" / "default.xex"
    if not xex.exists():
        raise ErrorPaso(
            f"falta el .xex en {xex}. Copialo ahi (ver docs/NOTAS-compilacion-y-shaders.md §2.2 paso 2).")
    banner("codegen: traduciendo el .xex a C++ (app/generated/)")
    correr(["bash", "tools/codegen.sh", "app"], cwd=RAIZ,
           env={"REXGLUE": str(binario), "PYTHON": "python3"})


# --------------------------------------------------------------------------------------
# Paso del APK (Windows)
# --------------------------------------------------------------------------------------
def ruta_apk() -> Path:
    return RAIZ / "android" / "app" / "build" / "outputs" / "apk" / "release" / "app-release.apk"


def paso_apk_en_windows_nativo() -> None:
    """Corre build_android.ps1 estando ya en Windows."""
    ps1 = RAIZ / "build_android.ps1"
    correr(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ps1)])


def paso_apk_desde_wsl() -> None:
    """Corre build_android.ps1 en Windows invocando powershell.exe desde WSL."""
    ps1_win = a_ruta_windows(RAIZ / "build_android.ps1")
    correr(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1_win])


def paso_apk() -> None:
    banner("APK: armando con gradle (assembleRelease)")
    if es_windows():
        paso_apk_en_windows_nativo()
    elif es_wsl():
        if shutil.which("powershell.exe") is None:
            raise ErrorPaso(
                "no se encontro powershell.exe desde WSL. El APK se arma en Windows; "
                "ejecuta build_android.ps1 a mano en PowerShell, o habilita la interop de WSL.")
        paso_apk_desde_wsl()
    else:
        raise ErrorPaso(
            "en Linux puro no hay toolchain Android de esta maquina (SDK en D:\\android\\sdk). "
            "Arma el APK en Windows/WSL.")
    apk = ruta_apk()
    if apk.exists():
        print(f"\n\033[1;32mAPK listo:\033[0m {apk}")
    else:
        raise ErrorPaso(f"gradle termino pero no aparece el APK en {apk}")


def paso_install() -> None:
    apk = ruta_apk()
    if not apk.exists():
        raise ErrorPaso(f"no hay APK para instalar en {apk}")
    banner("install: adb install -r")
    # adb en Windows (adb.exe) o en WSL; probamos lo que haya en PATH.
    adb = "adb.exe" if (es_wsl() and shutil.which("adb.exe")) else "adb"
    ruta = a_ruta_windows(apk) if es_wsl() else str(apk)
    correr([adb, "install", "-r", ruta])


# --------------------------------------------------------------------------------------
# Re-dispatch a WSL cuando se corre desde Windows (pasos de Linux)
# --------------------------------------------------------------------------------------
def correr_pasos_linux_via_wsl(args_linux: list[str]) -> None:
    """Desde Windows, reejecuta este script dentro de WSL para los pasos de Linux."""
    script_wsl = a_ruta_wsl(str(Path(__file__).resolve()))
    cmd = ["wsl.exe", "python3", script_wsl] + args_linux
    correr(cmd)


# --------------------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(
        description="Recompila NFSMW Android (perfil Mali). Sin flags: solo el APK.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Ver docs/NOTAS-compilacion-y-shaders.md §2.2 para el detalle del flujo.")
    ap.add_argument("--all", action="store_true",
                    help="desde cero: thirdparty + rexglue + codegen + APK")
    ap.add_argument("--thirdparty", action="store_true", help="bajar dependencias de terceros")
    ap.add_argument("--rexglue", action="store_true", help="(re)compilar el generador host")
    ap.add_argument("--codegen", action="store_true", help="regenerar el C++ (app/generated/)")
    ap.add_argument("--no-apk", action="store_true", help="no armar el APK")
    ap.add_argument("--install", action="store_true", help="adb install -r al terminar")
    ap.add_argument("--rexglue-build-dir", default=os.path.expanduser("~/rexglue-build"),
                    help="carpeta de build del host (def: ~/rexglue-build)")
    ap.add_argument("--solo-linux", action="store_true",
                    help=argparse.SUPPRESS)  # uso interno: re-dispatch desde Windows
    args = ap.parse_args()

    # Que pasos de cada grupo se piden.
    hacer_thirdparty = args.thirdparty or args.all
    hacer_rexglue = args.rexglue or args.all
    hacer_codegen = args.codegen or args.all
    hacer_apk = (not args.no_apk) and (not args.solo_linux)
    hacer_install = args.install and not args.solo_linux

    pasos_linux = hacer_thirdparty or hacer_rexglue or hacer_codegen

    print(f"Repo: {RAIZ}")
    print(f"Entorno: {'Windows' if es_windows() else ('WSL' if es_wsl() else 'Linux')}")
    print(f"Plan: thirdparty={hacer_thirdparty} rexglue={hacer_rexglue} "
          f"codegen={hacer_codegen} apk={hacer_apk} install={hacer_install}")

    try:
        # --- Pasos de Linux ---
        if pasos_linux:
            if es_windows():
                # Reejecutar en WSL solo la parte Linux.
                banner("delegando los pasos de Linux a WSL")
                sub = ["--solo-linux"]
                if hacer_thirdparty:
                    sub.append("--thirdparty")
                if hacer_rexglue:
                    sub.append("--rexglue")
                if hacer_codegen:
                    sub.append("--codegen")
                sub += ["--rexglue-build-dir", args.rexglue_build_dir, "--no-apk"]
                correr_pasos_linux_via_wsl(sub)
            else:
                build_dir = Path(args.rexglue_build_dir).expanduser()
                if hacer_thirdparty:
                    paso_thirdparty(forzar=True)
                if hacer_rexglue:
                    paso_rexglue(build_dir, forzar=True)
                if hacer_codegen:
                    # Si falta rexglue y se pidio codegen, lo compilamos (sin forzar si ya esta).
                    if not ruta_rexglue().exists():
                        paso_rexglue(build_dir, forzar=False)
                    paso_codegen()

        # --- Paso del APK (Windows) ---
        if hacer_apk:
            paso_apk()

        if hacer_install:
            paso_install()

    except ErrorPaso as e:
        print(f"\n\033[1;31mERROR:\033[0m {e}", file=sys.stderr)
        return 1

    print("\n\033[1;32mHecho.\033[0m")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
