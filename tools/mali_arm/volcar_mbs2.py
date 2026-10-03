"""Shaders del .nfsp Mali compilados con el compilador de ARM (malioc de Arm Performance Studio) para el Mali-G52.

malioc no guarda el binario: Frida engancha cmpbe_v2_compile_multiple_shaders (exportada por la DLL del
compilador) y guarda, por shader, el bloque MBS2 que genera y su volcado a C (cmpbe_v2_deserialize_MBS2_to_C).
Idea de github.com/leegao/mali-msb2-disassembler (Valhall, Linux); aqui Windows y Bifrost.

  python tools/mali_arm/volcar_mbs2.py out/nfsmw_shaders_mali.nfsp out/mali_arm [malioc.exe]

Requiere frida (pip install frida-tools). Deja <salida>/spv/NNN.{vert,frag}.spv, <salida>/mbs2/NNN_*.mbs2 y .h
y <salida>/stats/NNN.json (las estadisticas de malioc).
"""
import os, struct, subprocess, sys, threading

import frida

AQUI = os.path.dirname(os.path.abspath(__file__))


def extraer_spirv(nfsp, carpeta):
    d = open(nfsp, 'rb').read()
    magia = bytes.fromhex('03022307')
    pos = [i for i in range(0, len(d) - 4, 4) if d[i:i + 4] == magia]
    modelos = {0: 'vert', 4: 'frag', 5: 'comp'}
    rutas = []
    for k, p in enumerate(pos):
        lim = pos[k + 1] if k + 1 < len(pos) else len(d)
        i, modelo = p + 20, None
        while i + 4 <= lim:
            w = struct.unpack_from('<I', d, i)[0]
            n, op = w >> 16, w & 0xffff
            if n == 0 or i + 4 * n > lim:
                break
            if op == 15 and modelo is None:
                modelo = struct.unpack_from('<I', d, i + 4)[0]
            i += 4 * n
        ruta = os.path.join(carpeta, f'{k:03d}.{modelos.get(modelo, "x")}.spv')
        open(ruta, 'wb').write(d[p:i])
        rutas.append(ruta)
    return rutas


def volcar(malioc, spv, base):
    etapa = 'fragment' if '.frag.' in spv else 'vertex'
    js = open(os.path.join(AQUI, 'volcar_mbs2.js'), encoding='utf-8').read()
    js = js.replace('SALIDA_RUTA', repr(base.replace(os.sep, '/')))
    fin = threading.Event()
    dev = frida.get_local_device()
    pid = dev.spawn([malioc, '--core', 'Mali-G52', '--' + etapa, spv], stdio='pipe')
    ses = dev.attach(pid)
    ses.on('detached', lambda *a: fin.set())
    sc = ses.create_script(js)

    def mensaje(m, datos):
        if m['type'] == 'send' and isinstance(m['payload'], dict) and datos is not None:
            open(m['payload']['archivo'], 'wb').write(datos)
        elif m['type'] == 'send':
            print('  ', os.path.basename(spv), m['payload'])
        else:
            print('  ', os.path.basename(spv), m)

    sc.on('message', mensaje)
    sc.load()
    dev.resume(pid)
    fin.wait(120)


def main():
    nfsp, salida = sys.argv[1], sys.argv[2]
    malioc = sys.argv[3] if len(sys.argv) > 3 else 'D:/arm/mali_offline_compiler/malioc.exe'
    for sub in ('spv', 'mbs2', 'stats'):
        os.makedirs(os.path.join(salida, sub), exist_ok=True)
    for spv in extraer_spirv(nfsp, os.path.join(salida, 'spv')):
        nombre = os.path.basename(spv).split('.')[0]
        volcar(malioc, spv, os.path.join(salida, 'mbs2', nombre))
        etapa = 'fragment' if '.frag.' in spv else 'vertex'
        with open(os.path.join(salida, 'stats', nombre + '.json'), 'w') as f:
            subprocess.run([malioc, '--core', 'Mali-G52', '--' + etapa, spv, '--format', 'json'], stdout=f,
                           stderr=subprocess.DEVNULL)
    print('listo:', len(os.listdir(os.path.join(salida, 'mbs2'))), 'archivos en', os.path.join(salida, 'mbs2'))


if __name__ == '__main__':
    main()
