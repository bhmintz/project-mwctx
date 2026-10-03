#!/bin/bash
# pandecode de Mesa (solo Bifrost v7, trabajos JM) compilado suelto, para leer las capturas de
# android_captura_mali.cpp. Usa el codigo de JimVulkan clonado en out/mesa-jim y los empaquetadores generados en
# out/pandecode/genxml (gen_pack.py, con python + mako). Se corre en WSL desde la raiz del repo.
set -e
M=out/mesa-jim
O=out/pandecode
mkdir -p $O/obj $O/genxml $O/util/format
# Generated headers, with the same commands as Mesa's meson.
G=$M/src
for v in common v4 v5 v6 v7 v9 v10 v12 v13 v14; do
   [ -s $O/genxml/${v}_pack.h ] || python3 $G/panfrost/genxml/gen_pack.py $G/panfrost/genxml/$v.xml > $O/genxml/${v}_pack.h
done
[ -s $O/util/format/u_format_gen.h ] || python3 $G/util/format/u_format_table.py $G/util/format/u_format.yaml --enums > $O/util/format/u_format_gen.h
[ -s $O/util/format/u_format_pack.h ] || python3 $G/util/format/u_format_table.py $G/util/format/u_format.yaml --header > $O/util/format/u_format_pack.h
[ -s $O/util/shader_stats.h ] || python3 $G/util/process_shader_stats.py $G/util/shader_stats.rnc $G/util/shader_stats.xml > $O/util/shader_stats.h
INC="-I$O -I$M/src/panfrost/model -I$M/src/compiler -I$M/src/panfrost/compiler -I$M/include -I$M/src -I$M/src/panfrost -I$M/src/panfrost/lib -I$M/src/panfrost/genxml -I$M/src/gallium/include"
DEF="-DHAVE_PTHREAD -DHAVE_ENDIAN_H -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -D_GNU_SOURCE -DHAVE_STRUCT_TIMESPEC"
CF="-O1 -g -w -DNDEBUG -std=gnu11 $INC $DEF"
gcc $CF -DPAN_ARCH=7 -c $M/src/panfrost/genxml/decode.c -o $O/obj/decode_v7.o
gcc $CF -DPAN_ARCH=7 -c $M/src/panfrost/genxml/decode_jm.c -o $O/obj/decode_jm_v7.o
gcc $CF -c $M/src/panfrost/genxml/decode_common.c -o $O/obj/decode_common.o
for f in set hash_table ralloc rb_tree simple_mtx u_call_once u_dynarray u_debug log u_process ${UTIL_EXTRA}; do gcc $CF -c $M/src/util/$f.c -o $O/obj/util_$f.o; done
echo objetos listos
# The reader: pandecode + the Bifrost disassembler (out/bidis, from compilar_bidis.sh) + what they use of util.
B=out/bidis
gcc $CF -c tools/mali_arm/pandecode_stubs.c -o $O/obj/stubs.o
gcc $CF -I$M/src/panfrost/lib -c tools/mali_arm/leer_captura.c -o $O/obj/leer_captura.o
gcc $CF -I$B -I$B/bifrost -c $B/bifrost/disassemble.c -o $O/obj/bi_disasm.o
gcc $CF -I$B -c $B/bi_print_common.c -o $O/obj/bi_print_common.o
gcc $CF -I$B -I$B/bifrost -c $B/gen/bifrost_gen_disasm.c -o $O/obj/bi_gen_disasm.o
gcc -o $O/leer_captura $O/obj/*.o -lpthread -lm ${LIBS_EXTRA}
echo "listo: $O/leer_captura"
