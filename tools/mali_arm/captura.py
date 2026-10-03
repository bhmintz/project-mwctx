"""Lee una captura de android_captura_mali.cpp (captura_mali_N.bin).

  python tools/mali_arm/captura.py captura.bin                  # lista de regiones
  python tools/mali_arm/captura.py captura.bin 0x3efe8b000 64   # palabras de 32 bits en esa direccion
  python tools/mali_arm/captura.py captura.bin 0x3efe8b000 64 antes|despues

Cada region tiene el envio en que se copio; con el bit 31 (DESPUES) se copio al llegar un evento de fin de
trabajo, o sea con lo que escribio la GPU.

  python tools/mali_arm/captura.py captura.bin 0x7f0007000 volcar salida.bin   # bytes desde ahi hasta el fin de
                                                                                # la region (para bidis) "antes" toma la ultima copia previa a la GPU y "despues" la ultima
posterior; sin eso, la ultima de todas.
"""
import struct, sys

DESPUES = 0x80000000


def leer(ruta):
    d = open(ruta, 'rb').read()
    assert d[:4] == b'NFCM'
    i, regiones, notas = 8, [], []
    while i + 8 <= len(d):
        tipo, tam = struct.unpack_from('<II', d, i)
        cuerpo = d[i + 8:i + 8 + tam]
        i += 8 + tam
        if tipo == 1:
            va, t, envio = struct.unpack_from('<QQI', cuerpo)
            regiones.append((va, t, envio, cuerpo[20:20 + t]))
        elif tipo == 3:
            notas.append(cuerpo.decode(errors='replace'))
    return regiones, notas


def buscar(regiones, va, tam, cual=None):
    for r in reversed(regiones):
        if cual == 'antes' and r[2] & DESPUES or cual == 'despues' and not r[2] & DESPUES:
            continue
        if r[0] <= va and va + tam <= r[0] + r[1]:
            return r[3][va - r[0]:va - r[0] + tam], r
    return None, None


if __name__ == '__main__':
    regiones, notas = leer(sys.argv[1])
    if len(sys.argv) < 3:
        for n in notas:
            print('#', n)
        for va, t, envio, _ in sorted(regiones):
            print(f'{va:#x} {t:#x} envio {envio & ~DESPUES}' + (' despues' if envio & DESPUES else ''))
    elif len(sys.argv) > 4 and sys.argv[3] == 'volcar':
        va = int(sys.argv[2], 0)
        b, r = buscar(regiones, va, 4)
        if b is None:
            sys.exit('no esta en la captura')
        open(sys.argv[4], 'wb').write(r[3][va - r[0]:])
    else:
        va, n = int(sys.argv[2], 0), int(sys.argv[3]) if len(sys.argv) > 3 else 64
        b, r = buscar(regiones, va, 4 * n, sys.argv[4] if len(sys.argv) > 4 else None)
        if b is None:
            sys.exit('no esta en la captura')
        for k in range(0, n, 4):
            w = struct.unpack_from('<4I', b, 4 * k)
            f = struct.unpack_from('<4f', b, 4 * k)
            print(f'{va + 4 * k:#x}: ' + ' '.join(f'{x:08x}' for x in w) + '   ' + ' '.join(f'{x:.4g}' for x in f))
