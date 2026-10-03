"""Paquetes .arm para nuestro PanVK: el binario de ARM (malioc) de un shader de fragmento, lo que necesita su
descriptor y su piloto traducido a bytecode, para que el driver lo use en lugar del binario de Mesa.
Ver docs/plan-arm-hibrido.md, "Especificacion del adaptador".

  python tools/mali_arm/paquete_arm.py nfsmw_nativo_pipelines.bin out/mali_arm salida/ [NNN ...]

Por cada variante de fragmento de la lista de pipelines (o solo las de los shaders NNN) escribe salida/<hash>.arm,
donde hash es el que nuestro Mesa pone en nir->info.label (vk_pipeline.c): FNV-1a 64 del SPIR-V tal como lo
recibe vkCreateShaderModule y de los datos de especializacion (el u32 de la clave del pipeline).

Formato (little endian): "NFAR", u32 version (1), u32 bytes del codigo, codigo (relleno a 4), u16 message preload 1,
u16 message preload 2, u64 mascara de precarga, u32 work_reg_count, u32 palabras de uniformes, u32 varyings +
{u32 location (gl_varying_slot), u32 componentes} por indice de ARM, u32 UBO + {u32 ranura ARM, u32 set,
u32 binding}, u32 tamano de la tabla de UBO de ARM, u32 bytes del piloto, piloto (bytecode de piloto.py).
"""
import os, re, struct, sys

import mbs2
import piloto
import variantes

AQUI = os.path.dirname(os.path.abspath(__file__))
SET_UBO = 1  # NFSMW_SET_UBO en el modo Mali (shader_common.h)
SET_TEXTURAS = 0
VARYING_SLOT_VAR0 = 32  # gl_varying_slot de Mesa (compiler/shader_enums.h)


def fnv1a64(*partes):
    h = 0xcbf29ce484222325
    for p in partes:
        for b in p:
            h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


# --- bytecode del piloto (lo interpreta panvk_nfsmw_arm.c) ---------------------------------------------------
CODIGOS = {'NOP': 0, 'MOV': 1, 'SHADDXL': 2, 'IADD': 3, 'IADDC': 3, 'ISUB': 4, 'ISUBB': 4, 'IMUL': 5,
           'LSHIFT_AND': 6, 'LSHIFT_OR': 7, 'LSHIFT_XOR': 8, 'RSHIFT_AND': 9, 'RSHIFT_OR': 10,
           'RSHIFT_DOUBLE': 11, 'SHIFT_DOUBLE': 12, 'CSEL': 13, 'MUX': 14, 'ICMP': 15, 'FCMP': 15, 'FMA': 16,
           'FADD': 17, 'FMAX': 18, 'FMIN': 19, 'FRCP': 20, 'FRSQ': 21, 'U32_TO_F32': 22, 'U16_TO_U32': 23,
           'U8_TO_U32': 24, 'LOAD': 25, 'STORE': 26, 'BRANCHZ': 27, 'JUMP': 28, 'MKVEC': 29, 'LDEXP': 30,
           'SWZ': 31}
TIPOS = {'u32': 0, 's32': 1, 'f32': 2, 'i32': 0, 'i16': 0, 'u16': 0}
COMPARACIONES = {'gt': 0, 'ge': 1, 'eq': 2, 'ne': 3, 'lt': 4, 'gtlt': 5}
MUX_MODOS = {'int_zero': 0, 'neg': 1, 'fp_zero': 2, 'bit': 3}
MODS = {'neg': 1, 'abs': 2, 'not': 4, 'h0': 8, 'h00': 8, 'h1': 16, 'h11': 16, 'h10': 32}


def fuente(texto, etiquetas):
    s, mods = texto, 0
    while True:
        m = re.match(r'^(.*)\.(neg|abs|not|h0|h00|h1|h11|h01|h10|reserved|b0|b1|b2|b3)$', s)
        if not m or re.match(r'^u\d+$', m.group(1)):
            break
        s = m.group(1)
        mods |= MODS.get(m.group(2), 0)
    if s.startswith('clause_'):
        return 6, mods, etiquetas[s]
    if s == 't':
        return 2, mods, 0
    if s == 't0':
        return 3, mods, 0
    if s == 't1':
        return 4, mods, 0
    m = re.match(r'^r(\d+)$', s)
    if m:
        return 0, mods, int(m.group(1))
    m = re.match(r'^u(\d+)\.w([01])$', s)
    if m:
        return 5, mods, 2 * int(m.group(1)) + int(m.group(2))
    if s.startswith('#'):
        return 1, mods, int(s[1:], 0) & 0xffffffff
    if s.startswith('0x'):
        return 1, mods, int(s, 16)
    raise ValueError(f'fuente {texto}')


def op_bytes(op, etiquetas):
    if op is None or op.nombre == 'NOP':
        return struct.pack('<8B', 0, 0, 0, 0, 0xff, 0xff, 0, 0)
    codigo = CODIGOS[op.nombre]
    mods = op.mods
    tipo = cmp = flags = 0
    if op.nombre in ('CSEL', 'ICMP', 'FCMP'):
        tipo, cmp = TIPOS[mods[0]], COMPARACIONES[mods[1]]
        if op.nombre == 'FCMP':
            tipo = 2
        if 'i1' in mods:
            flags |= 2
    elif op.nombre == 'MUX':
        cmp = next((MUX_MODOS[m] for m in mods if m in MUX_MODOS), 0)
    elif op.nombre in ('LOAD', 'STORE'):
        cmp = int(mods[0][1:]) // 32
    elif op.nombre == 'SHADDXL':
        cmp = int(op.params.get('shift', 0))
    elif op.nombre == 'BRANCHZ':
        cmp = COMPARACIONES[mods[1]]
    if 'clamp' in mods:
        flags |= 1
    destino = 0xff
    if op.destino and re.match(r'^r\d+:', op.destino):
        destino = int(op.destino[1:].split(':')[0])
    staging = op.staging if op.staging is not None else 0xff
    fuentes = [fuente(f, etiquetas) for f in op.fuentes]
    b = struct.pack('<8B', codigo, tipo, cmp, flags, destino, staging, len(fuentes), 0)
    for kind, m, valor in fuentes:
        b += struct.pack('<BBHI', kind, m, 0, valor)
    return b


def bytecode(ruta_txt):
    clausulas = piloto.parsear(ruta_txt)
    etiquetas = {c.nombre: i for i, c in enumerate(clausulas)}
    b = struct.pack('<I', len(clausulas))
    for c in clausulas:
        b += struct.pack('<BBH', 1 if ' eos' in ' ' + c.cabecera else 0, 0, len(c.tuplas))
        for fma, add in c.tuplas:
            b += op_bytes(fma, etiquetas) + op_bytes(add, etiquetas)
    return b


# --- analisis del principal y de los simbolos ----------------------------------------------------------------
def registros(ruta_txt):
    """(primer acceso de cada registro es lectura?, registros usados)"""
    leidos_antes, escritos, usados = set(), set(), set()
    for c in piloto.parsear(ruta_txt):
        for fma, add in c.tuplas:
            escritos_tupla = []
            for op in (fma, add):
                if op is None or op.nombre == 'NOP':
                    continue
                regs = [int(x) for f in op.fuentes for x in re.findall(r'^r(\d+)', f)]
                if op.nombre == 'STORE' or op.nombre in ('BLEND', 'ATEST', 'TEXC', 'ST_CVT'):
                    if op.staging is not None:
                        regs.append(op.staging)
                for r in regs:
                    usados.add(r)
                    if r not in escritos:
                        leidos_antes.add(r)
                if op.destino and re.match(r'^r\d+:', op.destino):
                    escritos_tupla.append(int(op.destino[1:].split(':')[0]))
                if op.staging is not None and op.nombre in ('LOAD', 'LD_VAR_IMM', 'LD_VAR', 'TEXC', 'TEX_FETCH'):
                    escritos_tupla.extend(range(op.staging, op.staging + 4))
            for r in escritos_tupla:
                usados.add(r)
                escritos.add(r)
    return leidos_antes, usados


def uniformes_usados(ruta_txt):
    texto = open(ruta_txt, errors='replace').read()
    us = [2 * int(k) + int(w) for k, w in re.findall(r'\bu(\d+)\.w([01])', texto)]
    return max(us) + 1 if us else 0


def simbolos(ruta_h):
    texto = open(ruta_h, errors='replace').read()
    cadenas = dict(re.findall(r'static const uint8_t b_(\d+)\[\] = "([^"]*)";', texto))
    tpge = {}
    for n, cuerpo in re.findall(r'cmpbe_chunk_TPGE TPGE_(\d+) =\s*\{(.*?)\};', texto, re.S):
        valores = re.findall(r'\) (0x[0-9a-f]+)', cuerpo)
        tpge[n] = int(valores[1], 16)
    tipos = {}
    for n, cuerpo in re.findall(r'cmpbe_chunk_TYPE TYPE_(\d+) =\s*\{(.*?)\};', texto, re.S):
        m = re.search(r'TPGE\*\)&TPGE_(\d+)', cuerpo)
        if m:
            tipos[n] = tpge.get(m.group(1), 4)
    salida = []
    for cuerpo in re.findall(r'cmpbe_chunk_SYMB SYMB_\d+ =\s*\{(.*?)\};', texto, re.S):
        stri = re.search(r'STRI_(\d+)', cuerpo).group(1)
        valores = [int(v, 16) for v in re.findall(r'\) (0x[0-9a-f]+)', cuerpo)]
        flags, _sem, _u8, indice, a, b = valores[:6]
        m = re.search(r'\bTYPE_(\d+)', cuerpo)
        salida.append({'nombre': re.sub(r'^_\d+_', '', cadenas.get(stri, '')), 'flags': flags, 'indice': indice,
                       'a': a, 'b': b, 'comps': tipos.get(m.group(1), 4) if m else 4})
    return salida


def paquete(ruta_mbs2, ruta_h, txt_principal, txt_piloto):
    d = open(ruta_mbs2, 'rb').read()
    arbol = []
    mbs2.arbol(d, 0, len(d), 0, arbol)
    objcs = [(p, t) for _, tag, p, t in arbol if tag == 'OBJC']
    pdsc = next((d[p:p + t] for _, tag, p, t in arbol if tag == 'PDSC'), b'\0' * 4)
    codigo = d[objcs[0][0]:objcs[0][0] + objcs[0][1]]
    leidos, usados = registros(txt_principal)
    precarga = sum(1 << r for r in (57, 58, 59, 61) if r in leidos)
    work = 64 if any(16 <= r < 48 for r in usados) else 32
    # Palabras del bloque: lo que lee el principal y lo que escribe el piloto.
    escritas = [int(x, 16) for x in re.findall(r'SHADDXL\.u32 t0, r2, (0x[0-9a-f]+)', open(txt_piloto).read())]
    palabras = max([uniformes_usados(txt_principal)] + [w // 4 + 4 for w in escritas])
    palabras = (palabras + 1) & ~1
    syms = simbolos(ruta_h)
    varyings = sorted((s for s in syms if s['flags'] == 0x26), key=lambda s: s['indice'])
    ubos = [s for s in syms if s['flags'] == 0x34]
    b = b'NFAR' + struct.pack('<II', 1, len(codigo)) + codigo + b'\0' * (-len(codigo) % 4)
    b += pdsc[:4] + struct.pack('<QII', precarga, work, palabras)
    b += struct.pack('<I', len(varyings))
    for s in varyings:
        b += struct.pack('<II', VARYING_SLOT_VAR0 + s['b'], s['comps'])
    b += struct.pack('<I', len(ubos))
    for s in ubos:
        b += struct.pack('<III', s['indice'], SET_UBO, s['a'])
    b += struct.pack('<I', max([s['indice'] for s in ubos] + [-1]) + 1)
    bc = bytecode(txt_piloto)
    b += struct.pack('<I', len(bc)) + bc
    return b, {'precarga': hex(precarga), 'work': work, 'palabras': palabras, 'pdsc': pdsc[:4].hex(),
               'varyings': [(s['nombre'], s['indice'], s['b'], s['comps']) for s in varyings],
               'ubos': [(s['nombre'], s['indice'], s['a']) for s in ubos]}


def main():
    ruta_lista, carpeta, salida = sys.argv[1], sys.argv[2], sys.argv[3]
    solo = {int(x) for x in sys.argv[4:]}
    os.makedirs(salida, exist_ok=True)
    spv = os.path.join(carpeta, 'spv')
    dis = os.path.join(carpeta, 'dis')
    for numero, esp in variantes.variantes(ruta_lista):
        original = os.path.join(spv, f'{numero:03d}.frag.spv')
        if not os.path.exists(original) or (solo and numero not in solo):
            continue
        nombre = f'{numero:03d}_e{esp:08x}_0'
        txt0, txt1 = os.path.join(dis, nombre + '_obj0.txt'), os.path.join(dis, nombre + '_obj1.txt')
        if not os.path.exists(txt1):
            print(nombre, 'sin desensamblado (correr correlacionar.py)')
            continue
        h = fnv1a64(open(original, 'rb').read(), struct.pack('<I', esp))
        datos, info = paquete(os.path.join(carpeta, 'mbs2', nombre + '.mbs2'),
                              os.path.join(carpeta, 'mbs2', nombre + '.h'), txt0, txt1)
        open(os.path.join(salida, f'{h:016x}.arm'), 'wb').write(datos)
        print(f'{h:016x}', nombre, info)


if __name__ == '__main__':
    main()
