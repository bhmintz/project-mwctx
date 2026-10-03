"""Exporta un dibujo capturado como caso para prueba.c: la tabla de UBO de ARM (la del hardware corrida 3 entradas,
ver docs/plan-arm-hibrido.md) y las regiones de memoria a las que apunta.

  python exportar.py captura.bin <tabla_ubo_hw> caso.bin
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import captura

regiones, _ = captura.leer(sys.argv[1])
hw = int(sys.argv[2], 0)
tabla, _ = captura.buscar(regiones, hw - 0x18, 20 * 8, 'antes')
salida = struct.pack('<QI', hw - 0x18, len(tabla)) + tabla
usadas = []
for i in range(20):
    d = struct.unpack_from('<Q', tabla, 8 * i)[0]
    va = (d >> 12) << 4
    if va:
        b, r = captura.buscar(regiones, va, 4, 'antes')
        if r and r not in usadas:
            usadas.append(r)
salida += struct.pack('<I', len(usadas))
for va, t, _, datos in usadas:
    salida += struct.pack('<QI', va, t) + datos
open(sys.argv[3], 'wb').write(salida)
print(len(usadas), 'regiones')
