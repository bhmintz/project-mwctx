#!/usr/bin/env python
"""mw.py - recompilar / instalar / ejecutar NFSMW-android desde Windows con un comando.

Automatiza el ciclo que, si no, se hace a mano:
    build (gradlew assembleRelease)  ->  adb install -r  ->  adb shell am start  ->  adb logcat

Uso rapido (desde cualquier carpeta; el script encuentra la raiz del repo):

    python tools/mw.py                # build + install + run  (lo mas comun al tocar C++/shaders)
    python tools/mw.py --log          # ...y ademas captura logcat a archivo + vista en vivo
    python tools/mw.py build          # solo compilar el APK
    python tools/mw.py install        # solo reinstalar el APK ya compilado
    python tools/mw.py run            # solo lanzar la app
    python tools/mw.py stop           # force-stop de la app
    python tools/mw.py log            # limpiar buffers y capturar logcat (Ctrl-C para parar)
    python tools/mw.py codegen        # (opcional, necesita WSL) regenerar C++ desde el .xex

Notas:
  - 'adb' debe estar en el PATH (ya lo esta en esta maquina).
  - El SDK/NDK sale de android/local.properties (sdk.dir, versiones). No hace falta tocar nada mas.
  - El codegen solo hace falta cuando cambia el .xex o la tabla de edicion, no al editar C++/shaders.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

# --- rutas y constantes del proyecto -----------------------------------------------------------
REPO = Path(__file__).resolve().parent.parent
ANDROID_DIR = REPO / "android"
GRADLEW = ANDROID_DIR / "gradlew.bat"
APK = {
    "release": ANDROID_DIR / "app" / "build" / "outputs" / "apk" / "release" / "app-release.apk",
    "debug": ANDROID_DIR / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk",
}
PACKAGE = "com.nfsmw.android"
ACTIVITY = f"{PACKAGE}/.MainActivity"
LOG_DIR = REPO / "logs"
# Prerrequisitos del build (un checkout recien clonado no los trae):
#  - SDL: lo aporta tools/fetch_thirdparty.py (git). Sin el, SDLActivity no existe y la compilacion
#    de Java revienta con decenas de "cannot find symbol" (getAssets, etc.) que NO arregla un clean.
#  - codegen: lo aporta 'mw.py codegen' (WSL). Sin rexglue.cmake el CMake nativo del APK falla.
SDL_SENTINEL = (REPO / "sdk" / "thirdparty" / "sdl3" / "android-project" / "app" / "src" / "main" /
                "java" / "org" / "libsdl" / "app" / "SDLActivity.java")
GEN_SENTINEL = REPO / "app" / "generated" / "rexglue.cmake"
# Lo que normalmente interesa ver en vivo del motor y del sistema.
LIVE_FILTER = re.compile(r"NFSMW|rex|FATAL|tombstone|VK_ERROR|SIGSEGV|DEBUG\b|AndroidRuntime|No se pudo",
                         re.IGNORECASE)


def c(msg: str) -> None:
    """Imprime un paso del script, destacado."""
    print(f"\n>>> {msg}", flush=True)


def die(msg: str, code: int = 1) -> "None":
    print(f"ERROR: {msg}", file=sys.stderr, flush=True)
    sys.exit(code)


def have(exe: str) -> bool:
    from shutil import which
    return which(exe) is not None


# --- entorno de compilacion (replica build_android.ps1) --------------------------------------
def build_env() -> dict:
    env = dict(os.environ)
    if not env.get("JAVA_HOME"):
        jbr = Path(env.get("ProgramFiles", r"C:\Program Files")) / "Android" / "Android Studio" / "jbr"
        if (jbr / "bin" / "java.exe").exists():
            env["JAVA_HOME"] = str(jbr)
    # ANDROID_HOME: lo ideal lo da local.properties (sdk.dir), pero lo ponemos como respaldo.
    if not env.get("ANDROID_HOME"):
        sdk = _sdk_dir_from_local_properties()
        if sdk is None:
            cand = Path(env.get("LOCALAPPDATA", "")) / "Android" / "Sdk"
            sdk = cand if cand.exists() else None
        if sdk:
            env["ANDROID_HOME"] = str(sdk)
            env["ANDROID_SDK_ROOT"] = str(sdk)
    return env


def _sdk_dir_from_local_properties() -> Path | None:
    lp = ANDROID_DIR / "local.properties"
    if not lp.exists():
        return None
    for line in lp.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = line.strip()
        if line.startswith("sdk.dir="):
            return Path(line.split("=", 1)[1].strip())
    return None


# --- prerrequisitos (fresh checkout) -----------------------------------------------------------
def _ensure_prereqs(auto: bool = True) -> None:
    """Comprueba (y, con auto=True, resuelve) lo que un checkout recien clonado no trae.

    Esto evita el fallo clasico del primer build: sin SDL salen ~43 'cannot find symbol' en Java, y
    sin el codegen el CMake nativo no encuentra rexglue.cmake. Un 'clean' no arregla ninguno de los dos.
    """
    # 1) SDL (y demas thirdparty): tools/fetch_thirdparty.py, solo necesita git.
    if not SDL_SENTINEL.exists():
        if not auto or not have("git"):
            die("faltan las fuentes de SDL (sdk/thirdparty). No es un problema de 'clean'.\n"
                "       Arreglalo con:  python tools/fetch_thirdparty.py   (necesita git)")
        c("Prerrequisito: faltan los thirdparty (SDL). Ejecutando tools/fetch_thirdparty.py")
        rc = subprocess.run([sys.executable, str(REPO / "tools" / "fetch_thirdparty.py")],
                            cwd=str(REPO)).returncode
        if rc != 0:
            die(f"fetch_thirdparty.py fallo (exit {rc}).", rc)
        if not SDL_SENTINEL.exists():
            die("fetch_thirdparty termino pero sigo sin ver SDLActivity.java. Revisa la salida de arriba.")
        print("OK  thirdparty listos (SDL presente).")

    # 2) Codegen: produce app/generated/rexglue.cmake. Corre en WSL.
    if not GEN_SENTINEL.exists():
        if not auto or not have("wsl"):
            die("falta el codegen (app/generated/rexglue.cmake). No es un problema de 'clean'.\n"
                "       Arreglalo con:  python tools/mw.py codegen   (necesita WSL)")
        c("Prerrequisito: falta el codegen (app/generated). Ejecutando codegen en WSL")
        cmd_codegen(argparse.Namespace(app="app"))
        if not GEN_SENTINEL.exists():
            die("el codegen termino pero sigo sin ver app/generated/rexglue.cmake.")


# --- comandos ----------------------------------------------------------------------------------
def cmd_build(args) -> None:
    if not GRADLEW.exists():
        die(f"no encuentro el wrapper de Gradle: {GRADLEW}")
    _ensure_prereqs(auto=not getattr(args, "no_bootstrap", False))
    env = build_env()
    if not have("java") and not env.get("JAVA_HOME"):
        die("hace falta un JDK 17+ (java en el PATH o Android Studio instalado).")
    variant = "debug" if args.debug else "release"
    tasks = []
    if args.clean:
        tasks.append("clean")
    tasks.append("assembleDebug" if args.debug else "assembleRelease")
    c(f"Compilando APK ({variant}): gradlew {' '.join(tasks)}")
    # gradlew.bat va con cmd /c y cwd=android.
    rc = subprocess.run(["cmd", "/c", str(GRADLEW), *tasks], cwd=str(ANDROID_DIR), env=env).returncode
    if rc != 0:
        die(f"la compilacion fallo (gradle exit {rc}).", rc)
    apk = APK[variant]
    if not apk.exists():
        die(f"gradle termino OK pero no veo el APK en {apk}")
    size_mb = apk.stat().st_size / (1024 * 1024)
    print(f"OK  APK: {apk}  ({size_mb:.1f} MB)")


def _adb(*adb_args, check=True, **kw) -> subprocess.CompletedProcess:
    if not have("adb"):
        die("'adb' no esta en el PATH.")
    return subprocess.run(["adb", *adb_args], check=False, **kw)


def _require_device() -> None:
    out = _adb("devices", capture_output=True, text=True).stdout
    lines = [l for l in out.splitlines()[1:] if l.strip()]
    ok = [l for l in lines if l.split("\t")[-1].strip() == "device"]
    if not ok:
        unauth = [l for l in lines if "unauthorized" in l]
        if unauth:
            die("el dispositivo esta 'unauthorized': acepta el dialogo de depuracion USB en el telefono.")
        die("no hay ningun dispositivo adb conectado (revisa el cable / 'adb devices').")


def cmd_install(args) -> None:
    variant = "debug" if args.debug else "release"
    apk = APK[variant]
    if not apk.exists():
        die(f"no hay APK en {apk}. Compila primero: python tools/mw.py build"
            + (" --debug" if args.debug else ""))
    _require_device()
    c(f"Instalando ({variant}): adb install -r")
    rc = _adb("install", "-r", str(apk)).returncode
    if rc != 0:
        die(f"adb install fallo (exit {rc}).", rc)
    print("OK  instalado.")


def cmd_run(args) -> None:
    _require_device()
    c(f"Lanzando {ACTIVITY}")
    _adb("shell", "am", "start", "-n", ACTIVITY)


def cmd_stop(args) -> None:
    _require_device()
    c(f"Force-stop {PACKAGE}")
    _adb("shell", "am", "force-stop", PACKAGE)
    print("OK  detenida.")


def cmd_log(args) -> None:
    _require_device()
    LOG_DIR.mkdir(exist_ok=True)
    out_path = Path(args.out) if args.out else LOG_DIR / (
        "logcat_" + _dt.datetime.now().strftime("%Y%m%d_%H%M%S") + ".txt")
    c("Limpiando buffers de logcat")
    _adb("logcat", "-b", "all", "-c")
    live = None if args.all else LIVE_FILTER
    print(f"Capturando a {out_path}")
    print("Vista en vivo: " + ("TODO" if args.all else "lineas NFSMW / errores") + "   (Ctrl-C para parar)\n")
    proc = subprocess.Popen(["adb", "logcat", "-b", "all", "-v", "threadtime"],
                            stdout=subprocess.PIPE, text=True, errors="replace", bufsize=1)
    try:
        with open(out_path, "w", encoding="utf-8", errors="replace") as f:
            assert proc.stdout is not None
            for line in proc.stdout:
                f.write(line)
                if live is None or live.search(line):
                    sys.stdout.write(line)
                    sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
    print(f"\nOK  log completo en {out_path}")


def cmd_loop(args) -> None:
    """Lo habitual: build + install + run (+ log opcional)."""
    cmd_build(args)
    if not args.no_install:
        cmd_install(args)
    if not args.no_run:
        cmd_run(args)
    if args.log:
        cmd_log(args)


def _wsl_distro() -> str:
    """Distro WSL con el toolchain Linux. Por defecto 'Ubuntu' (override: MW_WSL_DISTRO).

    'wsl' a secas usa la distro por defecto, que en esta maquina es 'docker-desktop' (sin bash/clang):
    de ahi el '/bin/sh: bash: not found'. El toolchain (clang/cmake/ninja) esta en 'Ubuntu'.
    """
    return os.environ.get("MW_WSL_DISTRO", "Ubuntu")


def cmd_codegen(args) -> None:
    """Regenera C++ desde el .xex. Necesita WSL (el codegen corre en Linux). Opcional/raro."""
    if not have("wsl"):
        die("no encuentro 'wsl'. El codegen corre en WSL; ver docs/building.md / NOTAS-compilacion.")
    distro = _wsl_distro()
    wsl_repo = _to_wsl_path(REPO)
    # Directorio de build de rexglue unico POR CHECKOUT. Compartir un solo build entre varios clones
    # hace que CMake rechace el cache ("source ... does not match ... used to generate cache"), porque
    # el cache recuerda la ruta de origen. La clave es un hash de la ruta del repo. Se usa $HOME (no '~',
    # que entre comillas no expande) para que la ruta sea absoluta y sin sorpresas.
    bdir = "$HOME/.cache/rexglue-build/" + hashlib.md5(str(REPO.resolve()).encode()).hexdigest()[:12]
    script = (
        "set -e\n"
        f'cd "{wsl_repo}"\n'
        f'BDIR="{bdir}"\n'
        'echo "== compilando rexglue (host, incremental) en $BDIR"\n'
        'cmake -S sdk -B "$BDIR" -G Ninja -DCMAKE_BUILD_TYPE=Release '
        "-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_C_FLAGS=-msse4.1 -DCMAKE_CXX_FLAGS=-msse4.1\n"
        'cmake --build "$BDIR" --target rexglue --parallel "$(nproc)"\n'
        'RG=$(find "$BDIR" sdk/out -name rexglue -type f 2>/dev/null | head -1)\n'
        '[ -n "$RG" ] || { echo "no encuentro el binario rexglue"; exit 1; }\n'
        # Ruta ABSOLUTA: codegen.sh hace 'cd app/' antes de ejecutar $REXGLUE, asi que una ruta
        # relativa (sdk/out/...) dejaria de resolver desde app/.
        'RG=$(realpath "$RG")\n'
        'chmod +x "$RG" 2>/dev/null || true\n'
        'echo "== codegen con $RG"\n'
        # Se invoca con 'bash' explicito: en /mnt/* los .sh de Windows no traen el bit de ejecucion y
        # ejecutarlos directo da exit 126 (permission denied).
        f'REXGLUE="$RG" PYTHON=python3 bash tools/codegen.sh {args.app}\n'
        'echo "== codegen OK"\n'
    )
    c(f"Codegen en WSL ({distro}, {args.app})")
    # Se ejecuta como ARCHIVO, no como 'bash -lc <string>': pasar un script multilinea largo por -lc
    # a wsl.exe pierde variables asignadas dentro del propio script (se vacian). Un archivo .sh en /tmp
    # de WSL evita todo el enredo de comillas/escapado de la frontera Windows->wsl.
    rc = _wsl_run_script(distro, script)
    if rc != 0:
        die(f"el codegen fallo (exit {rc}). Revisa la salida de arriba y app/*/codegen.log.", rc)
    print("OK  codegen terminado. Ahora compila el APK: python tools/mw.py build")


def _wsl_run_script(distro: str, script: str) -> int:
    """Corre un script bash en WSL escribiendolo primero en /tmp (evita el lio de 'bash -lc <string>')."""
    name = f"/tmp/mw_{os.getpid()}.sh"
    write = subprocess.run(["wsl", "-d", distro, "bash", "-c", f"cat > {name}"],
                           input=script.encode("utf-8"))
    if write.returncode != 0:
        return write.returncode
    try:
        return subprocess.run(["wsl", "-d", distro, "bash", name]).returncode
    finally:
        subprocess.run(["wsl", "-d", distro, "rm", "-f", name])


def cmd_bootstrap(args) -> None:
    """Deja el checkout listo para compilar: thirdparty (SDL) + codegen. Idempotente."""
    _ensure_prereqs(auto=True)
    print("OK  prerrequisitos listos. Ya puedes: python tools/mw.py")


def _to_wsl_path(p: Path) -> str:
    """D:\\users\\... -> /mnt/d/users/..."""
    s = str(p.resolve())
    drive, rest = s[0], s[2:]
    return "/mnt/" + drive.lower() + rest.replace("\\", "/")


# --- argumentos --------------------------------------------------------------------------------
def main(argv=None) -> None:
    p = argparse.ArgumentParser(
        prog="mw.py",
        description="Recompilar/instalar/ejecutar NFSMW-android desde Windows.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Sin subcomando = build + install + run.",
    )
    sub = p.add_subparsers(dest="cmd")

    def add_variant(sp):
        sp.add_argument("--debug", action="store_true", help="usar la variante debug en vez de release")

    sp = sub.add_parser("build", help="compilar el APK")
    add_variant(sp)
    sp.add_argument("--clean", action="store_true", help="gradle clean antes de compilar")
    sp.add_argument("--no-bootstrap", action="store_true",
                    help="no auto-resolver prerrequisitos (thirdparty/codegen); solo avisar")
    sp.set_defaults(func=cmd_build)

    sp = sub.add_parser("install", help="reinstalar el APK ya compilado")
    add_variant(sp)
    sp.set_defaults(func=cmd_install)

    sp = sub.add_parser("run", help="lanzar la app")
    sp.set_defaults(func=cmd_run)

    sp = sub.add_parser("stop", help="force-stop de la app")
    sp.set_defaults(func=cmd_stop)

    sp = sub.add_parser("log", help="limpiar buffers y capturar logcat")
    sp.add_argument("--out", help="archivo de salida (por defecto logs/logcat_<fecha>.txt)")
    sp.add_argument("--all", action="store_true", help="mostrar TODO en vivo (no solo lineas NFSMW/errores)")
    sp.set_defaults(func=cmd_log)

    sp = sub.add_parser("codegen", help="(WSL) regenerar C++ desde el .xex")
    sp.add_argument("--app", default="app", help="carpeta de la edicion (por defecto 'app')")
    sp.set_defaults(func=cmd_codegen)

    sp = sub.add_parser("bootstrap", help="dejar el checkout listo: thirdparty (SDL) + codegen")
    sp.set_defaults(func=cmd_bootstrap)

    # subcomando por defecto: loop (build+install+run)
    for name in ("loop", "all"):
        sp = sub.add_parser(name, help="build + install + run")
        add_variant(sp)
        sp.add_argument("--clean", action="store_true", help="gradle clean antes de compilar")
        sp.add_argument("--no-bootstrap", action="store_true",
                        help="no auto-resolver prerrequisitos (thirdparty/codegen); solo avisar")
        sp.add_argument("--no-install", action="store_true", help="no instalar")
        sp.add_argument("--no-run", action="store_true", help="no lanzar")
        sp.add_argument("--log", action="store_true", help="capturar logcat al final")
        sp.add_argument("--all", action="store_true", help="(con --log) mostrar todo en vivo")
        sp.add_argument("--out", help="(con --log) archivo de salida")
        sp.set_defaults(func=cmd_loop)

    args = p.parse_args(argv)
    if not getattr(args, "cmd", None):
        # Sin subcomando: loop con valores por defecto.
        ns = argparse.Namespace(debug=False, clean=False, no_install=False, no_run=False,
                                log=False, all=False, out=None, func=cmd_loop)
        cmd_loop(ns)
        return
    args.func(args)


if __name__ == "__main__":
    main()
