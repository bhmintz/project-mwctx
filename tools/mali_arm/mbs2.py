"""Lector minimo de MBS2 (binario de shader del compilador de ARM): bloques TAG(4) + tamano(u32) + datos.

  python tools/mali_arm/mbs2.py archivo.mbs2            arbol de bloques
  python tools/mali_arm/mbs2.py archivo.mbs2 OBJC dir   guarda cada bloque OBJC en dir/obj_N.bin
"""
import struct, sys

# Bloques que contienen otros bloques (el resto son hojas).
CONTENEDORES = {b'MBS2', b'CFRA', b'CVER', b'CMMN', b'SSYM', b'SYMB', b'TYPE', b'TPST', b'TPAR', b'TPIB', b'TPSE',
                b'TPSA', b'EBIN', b'UBUF', b'FSHA', b'SPDf', b'SPDv', b'VLKN'}


def bloques(d, ini, fin):
    i = ini
    while i + 8 <= fin:
        tag = d[i:i + 4]
        tam = struct.unpack_from('<I', d, i + 4)[0]
        if not (tag.isalnum() and tag.upper()[:3] == tag[:3].upper()) or i + 8 + tam > fin:
            i += 4  # campos sueltos entre bloques (contadores, u16): se saltan
            continue
        yield tag, i + 8, tam
        i += 8 + ((tam + 3) & ~3)


def arbol(d, ini, fin, nivel, salida):
    for tag, p, tam in bloques(d, ini, fin):
        salida.append((nivel, tag.decode('ascii', 'replace'), p, tam))
        ini_hijos = p + 4 if tag == b'MBS2' else p  # MBS2 empieza con una version (u32)
        hijos = list(bloques(d, ini_hijos, p + tam)) if tag in CONTENEDORES else []
        if hijos:
            arbol(d, ini_hijos, p + tam, nivel + 1, salida)


def main():
    d = open(sys.argv[1], 'rb').read()
    salida = []
    arbol(d, 0, len(d), 0, salida)
    if len(sys.argv) > 2:
        n = 0
        for nivel, tag, p, tam in salida:
            if tag == sys.argv[2]:
                open(f'{sys.argv[3]}/obj_{n}.bin', 'wb').write(d[p:p + tam])
                print(f'{tag} {n}: {tam} bytes en +{p}')
                n += 1
        return
    for nivel, tag, p, tam in salida:
        extra = d[p:p + min(tam, 16)].hex(' ') if tam <= 16 else ''
        print('  ' * nivel + f'{tag} {tam} {extra}')


if __name__ == '__main__':
    main()
