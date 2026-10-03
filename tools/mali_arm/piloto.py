"""Ejecuta en la PC un piloto de ARM (el segundo binario de un fragmento en el MBS2 de malioc) sobre la memoria de
una captura, para comparar el bloque de uniformes que produce con el que escribio el driver de Samsung.

Interpreta el texto del desensamblador de Bifrost de Mesa (out/bidis/bidis): clausulas de tuplas FMA (*) y ADD (+).
- Dentro de una tupla las dos operaciones leen los registros de antes de la tupla; "t" en la ADD es el resultado de
  la FMA de la misma tupla; "t0"/"t1" son los resultados de la FMA/ADD de la tupla anterior.
- Las cargas y guardados son inmediatos: el compilador ya espera sus dependencias.
- El piloto recibe en r0:r1 un puntero a un contexto: +0x18 = tabla de descriptores de UBO del dibujo, +0x30 =
  bloque de uniformes a llenar (ver docs/herramientas-mali.md).

  python tools/mali_arm/piloto.py captura.bin piloto.txt <tabla_ubo> <bloque_capturado> [palabras]
"""
import math, re, struct, sys

import captura

M32 = 0xffffffff


def f2u(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def u2f(u):
    return struct.unpack('<f', struct.pack('<I', u & M32))[0]


class Memoria:
    """Captura (solo lectura, copias "antes") + segmentos propios (contexto, bloque de salida)."""

    def __init__(self, regiones):
        self.regiones = regiones
        self.propia = {}  # direccion de palabra -> u32
        self.leidas = []

    def leer32(self, va):
        if va in self.propia:
            return self.propia[va]
        b, _ = captura.buscar(self.regiones, va, 4, 'antes')
        if b is None:
            raise KeyError(f'lectura fuera de la captura: {va:#x}')
        self.leidas.append(va)
        return struct.unpack('<I', b)[0]

    def escribir32(self, va, v):
        self.propia[va] = v & M32


class Op:
    def __init__(self, unidad, texto):
        self.unidad = unidad
        texto = re.sub(r'/\*.*?\*/', '', texto).strip()
        m = re.match(r'([A-Z_0-9]+)((?:\.[a-z0-9_]+)*)\s*(.*)', texto)
        self.nombre, mods, resto = m.group(1), m.group(2), m.group(3)
        self.mods = mods.split('.')[1:]
        self.staging = None
        ms = re.search(r',?\s*@r(\d+)\s*$', resto)
        if ms:
            self.staging = int(ms.group(1))
            resto = resto[:ms.start()]
        self.params = dict(re.findall(r'(\w+):(\d+)', resto))
        partes = [p.strip() for p in resto.split(',') if p.strip()]
        partes = [p for p in partes if not re.match(r'^\w+:\d+$', p) or re.match(r'^r\d+:t[01]$', p)]
        self.destino = partes[0] if partes else None
        self.fuentes = partes[1:]
        self.texto = texto


class Clausula:
    def __init__(self, nombre, cabecera):
        self.nombre = nombre
        self.cabecera = cabecera
        self.tuplas = []  # [(fma, add)]


def parsear(ruta):
    clausulas, actual, pendiente = [], None, None
    for linea in open(ruta, errors='replace'):
        s = linea.strip()
        m = re.match(r'^(clause_\d+):$', s)
        if m:
            actual = Clausula(m.group(1), '')
            clausulas.append(actual)
            continue
        if actual is None:
            continue
        if s.startswith('ds('):
            actual.cabecera = s
        elif s.startswith('*'):
            pendiente = Op('fma', s[1:])
        elif s.startswith('+'):
            actual.tuplas.append((pendiente, Op('add', s[1:])))
            pendiente = None
    return clausulas


class Maquina:
    def __init__(self, memoria, fau=None):
        self.r = [0] * 64
        self.mem = memoria
        self.fau = fau or {}
        self.t0 = self.t1 = 0
        self.traza = []

    def valor(self, src, t_actual):
        s = src
        neg = absv = False
        sufijos = []
        while True:
            m = re.match(r'^(.*)\.(neg|abs|h0|h1|h00|h01|h10|h11|b0|b1|b2|b3|reserved|not|w0|w1|x|y)$', s)
            if not m or m.group(1) == '':
                break
            if re.match(r'^u\d+$', m.group(1)) and m.group(2) in ('w0', 'w1'):
                break
            s, suf = m.group(1), m.group(2)
            sufijos.append(suf)
        if s.startswith('clause_'):
            return ('clausula', s)
        if s == 't':
            v = t_actual
        elif s == 't0':
            v = self.t0
        elif s == 't1':
            v = self.t1
        elif re.match(r'^r\d+$', s):
            v = self.r[int(s[1:])]
        elif re.match(r'^u\d+\.w[01]$', s):
            k, w = re.match(r'^u(\d+)\.w([01])$', s).groups()
            v = self.fau.get(2 * int(k) + int(w), 0)
        elif s.startswith('#'):
            v = int(s[1:], 0)
        elif s.startswith('0x'):
            v = int(s, 16)
        else:
            raise ValueError(f'fuente desconocida {src}')
        if isinstance(v, tuple):
            return v
        for suf in sufijos:
            if suf == 'neg':
                v ^= 0x80000000
            elif suf == 'abs':
                v &= 0x7fffffff
            elif suf == 'not':
                v = ~v & M32
            elif suf in ('h0', 'h00'):
                v = (v & 0xffff) | ((v & 0xffff) << 16)
            elif suf in ('h1', 'h11'):
                v = (v >> 16) | (v & 0xffff0000)
            elif suf == 'h10':
                v = (v >> 16) | ((v & 0xffff) << 16)
        return v

    def ejecutar_op(self, op, t_actual, escrituras):
        if op is None or op.nombre == 'NOP':
            return 0, None
        f = [self.valor(x, t_actual) for x in op.fuentes]
        n, mods = op.nombre, op.mods
        salto = None
        if n == 'MOV':
            v = f[0]
        elif n == 'SHADDXL':
            v = (f[0] + (f[1] << int(op.params.get('shift', 0)))) & M32
        elif n in ('IADD', 'IADDC'):
            v = (f[0] + f[1] + (f[2] if len(f) > 2 else 0)) & M32
        elif n in ('ISUB', 'ISUBB'):
            v = (f[0] - f[1] - (f[2] if len(f) > 2 else 0)) & M32
        elif n == 'IMUL':
            v = (f[0] * f[1]) & M32
        elif n == 'LSHIFT_AND':
            v = ((f[0] << (f[2] & 31)) & M32) & f[1]
        elif n == 'LSHIFT_OR':
            v = ((f[0] << (f[2] & 31)) & M32) | f[1]
        elif n == 'LSHIFT_XOR':
            v = ((f[0] << (f[2] & 31)) & M32) ^ f[1]
        elif n == 'RSHIFT_AND':
            v = (f[0] >> (f[2] & 31)) & f[1]
        elif n == 'RSHIFT_OR':
            v = (f[0] >> (f[2] & 31)) | f[1]
        elif n == 'RSHIFT_DOUBLE':
            v = (((f[1] << 32) | f[0]) >> (f[2] & 63)) & M32
        elif n == 'SHIFT_DOUBLE':
            v = (f[1] >> (f[2] & 31)) & M32
        elif n == 'CSEL':
            a, b = f[0], f[1]
            tipo, cmp = mods[0], mods[1]
            if tipo == 's32':
                a, b = (a ^ 0x80000000) - 0x80000000, (b ^ 0x80000000) - 0x80000000
            elif tipo == 'f32':
                a, b = u2f(a), u2f(b)
            c = {'gt': a > b, 'ge': a >= b, 'eq': a == b, 'ne': a != b, 'lt': a < b}[cmp]
            v = f[2] if c else f[3]
        elif n == 'MUX':
            # Mode in the modifiers; without one it is int_zero (bifrost ISA.xml): src2 == 0 ? src0 : src1.
            c = f[2]
            if 'bit' in mods:
                v = (f[0] & c) | (f[1] & ~c & M32)
            elif 'neg' in mods:
                v = f[0] if c & 0x80000000 else f[1]
            elif 'fp_zero' in mods:
                v = f[0] if u2f(c) == 0.0 else f[1]
            else:
                v = f[0] if c == 0 else f[1]
        elif n in ('ICMP', 'FCMP'):
            a, b = f[0], f[1]
            tipo, cmp = mods[0], mods[1]
            if tipo == 's32':
                a, b = (a ^ 0x80000000) - 0x80000000, (b ^ 0x80000000) - 0x80000000
            elif tipo == 'f32':
                a, b = u2f(a), u2f(b)
            c = {'gt': a > b, 'ge': a >= b, 'eq': a == b, 'ne': a != b, 'lt': a < b,
                 'gtlt': a != b}[cmp]
            v = (1 if 'i1' in mods else M32) if c else 0
        elif n == 'FMA':
            v = f2u(u2f(f[0]) * u2f(f[1]) + u2f(f[2]))
        elif n == 'FADD':
            v = f2u(u2f(f[0]) + u2f(f[1]))
        elif n in ('FMAX', 'FMIN'):
            v = f2u((max if n == 'FMAX' else min)(u2f(f[0]), u2f(f[1])))
        elif n == 'FRCP':
            x = u2f(f[0])
            v = f2u(1.0 / x if x else math.copysign(math.inf, x))
        elif n == 'FRSQ':
            x = u2f(f[0])
            v = f2u(1.0 / math.sqrt(x) if x > 0 else math.inf)
        elif n == 'SWZ':
            v = f[0]
        elif n == 'MKVEC':
            v = (f[0] & 0xffff) | ((f[1] & 0xffff) << 16)
        elif n == 'LDEXP':
            e = (f[1] ^ 0x80000000) - 0x80000000
            v = f2u(math.ldexp(u2f(f[0]), max(min(e, 300), -300)))
        elif n == 'U32_TO_F32':
            v = f2u(float(f[0]))
        elif n == 'U16_TO_U32':
            v = f[0] & 0xffff
        elif n == 'U8_TO_U32':
            v = f[0] & 0xff
        elif n == 'LOAD':
            bits = int(mods[0][1:])
            va = f[0] | (f[1] << 32)
            for k in range(bits // 32):
                escrituras.append((op.staging + k, self.mem.leer32(va + 4 * k)))
            self.traza.append(f'LOAD {va:#x} x{bits // 32}')
            v = 0
        elif n == 'STORE':
            bits = int(mods[0][1:])
            va = f[0] | (f[1] << 32)
            for k in range(bits // 32):
                self.mem.escribir32(va + 4 * k, self.r[op.staging + k])
            self.traza.append(f'STORE {va:#x} x{bits // 32}')
            v = 0
        elif n in ('BRANCHZ',):
            c = f[0]
            cond = {'eq': c == 0, 'ne': c != 0}[mods[1]]
            salto = f[1] if cond else None
            v = 0
        elif n == 'JUMP':
            salto = f[0]
            v = 0
        else:
            raise NotImplementedError(op.texto)
        if op.fuentes and op.destino and re.match(r'^r\d+:', op.destino):
            escrituras.append((int(op.destino[1:].split(':')[0]), v))
        if 'clamp' in mods and n.startswith('F'):
            v = f2u(min(max(u2f(v), 0.0), 1.0))
        return v, salto

    def correr(self, clausulas, limite=100000):
        por_nombre = {c.nombre: i for i, c in enumerate(clausulas)}
        i = 0
        pasos = 0
        while i < len(clausulas):
            c = clausulas[i]
            salto = None
            for fma, add in c.tuplas:
                escrituras = []
                vf, s1 = self.ejecutar_op(fma, 0, escrituras)
                va, s2 = self.ejecutar_op(add, vf, escrituras)
                salto = s2 or s1 or salto
                for reg, v in escrituras:
                    self.r[reg] = v & M32
                self.t0, self.t1 = vf, va
            pasos += 1
            if pasos > limite:
                raise RuntimeError('demasiados pasos')
            if ' eos' in ' ' + c.cabecera and salto is None:
                return
            if salto is not None:
                destino = salto[1] if isinstance(salto, tuple) else None
                if destino is None:
                    raise RuntimeError(f'salto a {salto!r}')
                i = por_nombre[destino]
            else:
                i += 1


CTX = 0x100000000000
BLOQUE = 0x100000010000


def ejecutar(regiones, ruta_piloto, tabla_ubo):
    mem = Memoria(regiones)
    for k in range(0, 0x200, 4):
        mem.escribir32(CTX + k, 0)
    mem.escribir32(CTX + 0x18, tabla_ubo & M32)
    mem.escribir32(CTX + 0x1c, tabla_ubo >> 32)
    mem.escribir32(CTX + 0x30, BLOQUE & M32)
    mem.escribir32(CTX + 0x34, BLOQUE >> 32)
    maq = Maquina(mem)
    maq.r[0], maq.r[1] = CTX & M32, CTX >> 32
    maq.correr(parsear(ruta_piloto))
    bloque = {(va - BLOQUE) // 4: v for va, v in mem.propia.items() if BLOQUE <= va < BLOQUE + 0x10000}
    return bloque, maq


def main():
    regiones, _ = captura.leer(sys.argv[1])
    bloque, maq = ejecutar(regiones, sys.argv[2], int(sys.argv[3], 0))
    for l in maq.traza:
        print('  ', l)
    real = int(sys.argv[4], 0)
    n = int(sys.argv[5]) if len(sys.argv) > 5 else max(bloque) + 1
    b, _ = captura.buscar(regiones, real, 4 * n, 'antes')
    capt = struct.unpack(f'<{n}I', b)
    print('palabra  piloto     driver')
    for k in range(n):
        p = bloque.get(k)
        print(f'{k:4d} u{k // 2}.w{k % 2}  ' + (f'{p:08x}' if p is not None else '   --   ') + f'   {capt[k]:08x}'
              + ('' if p is None or p == capt[k] else '   <>'))


if __name__ == '__main__':
    main()
