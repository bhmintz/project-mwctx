/* Runs panvk_nfsmw_arm.c's pilot interpreter on a case exported by exportar.py and prints the block, to compare
 * with tools/mali_arm/piloto.py.  prueba <hash> caso.bin  (NFSMW_ARM_DIR = folder of the .arm) */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "panvk_nfsmw_arm.h"

int main(int argc, char **argv)
{
   const struct nfsmw_arm *arm = nfsmw_arm_get(strtoull(argv[1], NULL, 16));
   if (!arm) { fprintf(stderr, "sin paquete\n"); return 1; }
   FILE *f = fopen(argv[2], "rb");
   uint64_t tabla_gpu; uint32_t tam, n;
   fread(&tabla_gpu, 8, 1, f); fread(&tam, 4, 1, f);
   void *tabla = malloc(tam); fread(tabla, 1, tam, f);
   fread(&n, 4, 1, f);
   for (uint32_t i = 0; i < n; i++) {
      uint64_t va; uint32_t t;
      fread(&va, 8, 1, f); fread(&t, 4, 1, f);
      void *d = malloc(t); fread(d, 1, t, f);
      nfsmw_arm_map_add(va, t, d);
   }
   uint32_t out[256];
   int ok = nfsmw_arm_run_pilot(arm, tabla_gpu, tabla, out);
   printf("ok %d\n", ok);
   for (uint32_t i = 0; i < arm->fau_words; i++) printf("%u %08x\n", i, out[i]);
   return 0;
}
