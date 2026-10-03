#!/bin/bash
# Desensamblador de Bifrost de Mesa (MIT), suelto, para leer los binarios de ARM (OBJC de un MBS2).
# Se corre en WSL/Linux con gcc; necesita python3 con mako (o generar gen/ desde Windows).
#   bash tools/mali_arm/compilar_bidis.sh out/bidis
#   out/bidis/bidis obj_0.bin [desplazamiento] [verbose]
set -e
DEST=${1:-out/bidis}
AQUI=$(cd "$(dirname "$0")" && pwd)
R=https://raw.githubusercontent.com/JimVulkan/mali-panvk/main/src
mkdir -p "$DEST/bifrost" "$DEST/util" "$DEST/gen"
cd "$DEST"
B=$R/panfrost/compiler/bifrost
for f in bifrost/disassemble.c bifrost/disassemble.h bifrost/bi_disasm.h; do curl -sf -o $f $B/$f; done
curl -sf -o ISA.xml $B/bifrost/ISA.xml
for f in bi_print_common.c bi_print_common.h bifrost.h gen_disasm.py bifrost_isa.py; do curl -sf -o $f $B/$f; done
for f in macros.h compiler.h detect.h detect_arch.h detect_cc.h detect_os.h u_endian.h; do curl -sf -o util/$f $R/util/$f; done
[ -s gen/bifrost_gen_disasm.c ] || python3 gen_disasm.py ISA.xml > gen/bifrost_gen_disasm.c
cp "$AQUI/bidis_main.c" main.c
gcc -O1 -w -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -I. -Ibifrost -o bidis main.c \
  bifrost/disassemble.c bi_print_common.c gen/bifrost_gen_disasm.c
echo "listo: $DEST/bidis"
