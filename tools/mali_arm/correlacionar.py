"""Empareja los shaders de una captura (android_captura_mali.cpp) con los de malioc (volcar_mbs2.py) por estructura:
la secuencia de operaciones del desensamblado de Bifrost (sin registros ni inmediatos), porque los bytes no
coinciden entre versiones del compilador ni con los valores que el driver incrusta.

  python tools/mali_arm/correlacionar.py captura.bin 0x7f0067c80 [0x7f005ec80 ...]

Necesita out/bidis/bidis (WSL) y out/mali_arm/mbs2/*.mbs2. Deja los desensamblados en out/mali_arm/dis/.
"""
import difflib, glob, os, re, subprocess, sys

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(os.path.dirname(AQUI))
sys.path.insert(0, AQUI)
import captura, mbs2

DIS = os.path.join(RAIZ, 'out', 'mali_arm', 'dis')


def wsl(ruta):
    ruta = os.path.abspath(ruta).replace(os.sep, '/')
    return '/mnt/' + ruta[0].lower() + ruta[2:]


def desensamblar(bins):
    """bins: rutas .bin -> escribe .txt al lado (una sola llamada a WSL)."""
    faltan = [b for b in bins if not os.path.exists(b[:-4] + '.txt')]
    if faltan:
        guion = os.path.join(DIS, 'desensamblar.sh')
        with open(guion, 'w', newline='\n') as g:
            for b in faltan:
                g.write(f"{wsl(os.path.join(RAIZ, 'out/bidis/bidis'))} '{wsl(b)}' > '{wsl(b[:-4] + '.txt')}' 2>/dev/null\n")
        subprocess.run(['wsl', '-d', 'Ubuntu', '-e', 'bash', wsl(guion)], check=False)


def firma(txt):
    ops = []
    for l in open(txt, errors='replace'):
        m = re.match(r'\s*[*+]([A-Z_0-9]+)', l)
        if m and m.group(1) != 'NOP':
            ops.append(m.group(1))
    return ops


def objc_de_malioc():
    os.makedirs(DIS, exist_ok=True)
    salida = []
    for f in sorted(glob.glob(os.path.join(RAIZ, 'out', 'mali_arm', 'mbs2', '*.mbs2'))):
        d = open(f, 'rb').read()
        arbol = []
        mbs2.arbol(d, 0, len(d), 0, arbol)
        nombre = os.path.basename(f)[:-5]
        for k, (nivel, tag, p, tam) in enumerate(x for x in arbol if x[1] == 'OBJC'):
            ruta = os.path.join(DIS, f'{nombre}_obj{k}.bin')
            if not os.path.exists(ruta):
                open(ruta, 'wb').write(d[p:p + tam])
            salida.append(ruta)
    return salida


def main():
    regiones, _ = captura.leer(sys.argv[1])
    propios = []
    for va in sys.argv[2:]:
        va = int(va, 0)
        b, r = captura.buscar(regiones, va, 4, 'antes')
        if b is None:
            print(hex(va), 'no esta en la captura')
            continue
        ruta = os.path.join(DIS, f'cap_{va:x}.bin')
        os.makedirs(DIS, exist_ok=True)
        open(ruta, 'wb').write(r[3][va - r[0]:])
        propios.append((va, ruta))
    ajenos = objc_de_malioc()
    desensamblar([r for _, r in propios] + ajenos)
    firmas = {r: firma(r[:-4] + '.txt') for r in ajenos if os.path.exists(r[:-4] + '.txt')}
    for va, ruta in propios:
        f = firma(ruta[:-4] + '.txt')
        mejores = sorted(((difflib.SequenceMatcher(None, f, g, autojunk=False).ratio(), os.path.basename(r)[:-4], len(g))
                          for r, g in firmas.items() if g), reverse=True)[:5]
        print(f'{va:#x} ({len(f)} ops):', ', '.join(f'{n} {p:.2f} ({k} ops)' for p, n, k in mejores))


if __name__ == '__main__':
    main()
