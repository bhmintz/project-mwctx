"""Lista los dibujos (trabajos IDVS) de un envio decodificado por leer_captura: shaders, tabla de UBO y uniformes
empujados de cada etapa.

  python tools/mali_arm/dibujos.py captura.bin.pdc..ctx-0.0000
"""
import re, sys


def dibujos(ruta):
    salida, actual, etapa = [], None, None
    for l in open(ruta, errors='replace'):
        m = re.match(r'Job Header \((\w+)\)', l)
        if m:
            actual = {'job': int(m.group(1), 16)}
            continue
        if actual is None:
            continue
        s = l.strip()
        if s.startswith('Type: ') and 'tipo' not in actual:
            actual['tipo'] = s[6:]
            if actual['tipo'] == 'Indexed Vertex':
                salida.append(actual)
        m = re.match(r'(Vertex|Fragment) Draw:', s)
        if m:
            etapa = m.group(1).lower()
        m = re.match(r'(Uniform buffers|Push uniforms|Textures|Samplers): (0x[0-9a-f]+)', s)
        if m and etapa:
            actual[f'{etapa}_{m.group(1).split()[0].lower()}'] = int(m.group(2), 16)
        m = re.match(r'Shader: (0x[0-9a-f]+)', s)
        if m:
            actual.setdefault('shaders', []).append(int(m.group(1), 16))
        m = re.match(r'Secondary shader: (0x[0-9a-f]+)', s)
        if m:
            actual.setdefault('secundarios', []).append(int(m.group(1), 16))
        m = re.match(r'vec4 uniforms\[(\d+)\]', s)
        if m:
            actual.setdefault('n_uniformes', []).append(int(m.group(1)))
    return salida


if __name__ == '__main__':
    for d in dibujos(sys.argv[1]):
        print(f"{d['job']:#x} sh={','.join(hex(x) for x in d.get('shaders', []))} "
              f"ubo={d.get('fragment_uniform', 0):#x} pv={d.get('vertex_push', 0):#x} pf={d.get('fragment_push', 0):#x} "
              f"n={d.get('n_uniformes')}")
