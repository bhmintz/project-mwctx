"""Compila con malioc las variantes especializadas que usa el juego: el pipeline especializa cada shader con una
constante de 32 bits (constant_id 0, bits en nfsmw_nativo_dibujos.cpp: kSpecConstantesUbo...) y el driver elimina
el codigo que esos bits apagan, asi que el binario de malioc sin especializar no es el que corre en el telefono.

Los valores salen de la lista de pipelines del juego (nfsmw_nativo_pipelines*.bin, que la captura copia a la
carpeta del juego). Cada (shader, valor) se congela con spirv-opt y se pasa a volcar_mbs2.volcar.

  python tools/mali_arm/variantes.py nfsmw_nativo_pipelines.bin out/mali_arm [malioc.exe]

Deja <salida>/spv/NNN_eXXXXXXXX.{vert,frag}.spv y <salida>/mbs2/NNN_eXXXXXXXX_*.mbs2.
"""
import os, struct, subprocess, sys

import volcar_mbs2

SPIRV_OPT = 'D:/android/sdk/ndk/29.0.14206865/shader-tools/windows-x86_64/spirv-opt.exe'


def variantes(ruta_lista):
    d = open(ruta_lista, 'rb').read()
    bytes_lista = struct.unpack_from('<Q', d, 8)[0]
    lista = d[24:24 + bytes_lista]
    n, tam = struct.unpack_from('<I', lista, 12)[0], struct.unpack_from('<I', lista, 8)[0]
    salida = set()
    for i in range(n):
        vs, ps, _, _, esp = struct.unpack_from('<IIQII', lista, 16 + i * tam)
        salida.add((vs - 1, esp))
        if ps:
            salida.add((ps - 1, esp))
    return sorted(salida)


def main():
    ruta_lista, salida = sys.argv[1], sys.argv[2]
    malioc = sys.argv[3] if len(sys.argv) > 3 else 'D:/arm/mali_offline_compiler/malioc.exe'
    carpeta_spv = os.path.join(salida, 'spv')
    originales = {int(f[:3]): f for f in os.listdir(carpeta_spv) if f[3] == '.'}
    for numero, esp in variantes(ruta_lista):
        original = originales[numero]
        etapa = original.split('.')[1]
        nombre = f'{numero:03d}_e{esp:08x}'
        spv = os.path.join(carpeta_spv, f'{nombre}.{etapa}.spv')
        base = os.path.join(salida, 'mbs2', nombre)
        if os.path.exists(base + '_0.mbs2'):
            continue
        subprocess.run([SPIRV_OPT, '--set-spec-const-default-value', f'0:{esp}', '--freeze-spec-const', '-O',
                        os.path.join(carpeta_spv, original), '-o', spv], check=True)
        print(nombre)
        volcar_mbs2.volcar(malioc, spv, base)


if __name__ == '__main__':
    main()
