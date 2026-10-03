/* Reads a capture from android_captura_mali.cpp (captura_mali_N.bin) and decodes it with Mesa's pandecode
 * (Bifrost v7, JM jobs), disassembling each shader with Mesa's Bifrost disassembler.
 *
 *   leer_captura captura_mali_N.bin [gpu_id hex, default 0x74021000 = Mali-G52 r1]
 *
 * The file: "NFCM", version, then records {u32 type, u32 bytes, data}: 1 = region {u64 va, u64 size,
 * u32 submit, bytes}, 2 = submit {u32 index, u32 atoms, u32 stride, atoms}, 3 = note, 4 = raw gpuprops,
 * 5 = completion events. A region whose submit has bit 31 set was copied after the GPU ran (on a completion
 * event): those are skipped here, so each submit is decoded with what the CPU wrote before it. */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wrap.h"

void disassemble_bifrost(FILE *fp, const void *code, size_t size, bool verbose);

static void
desensamblar(FILE *fp, const void *code, size_t size, uint64_t gpu_id, bool verbose)
{
   (void)gpu_id;
   disassemble_bifrost(fp, code, size, verbose);
}

int
main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "uso: %s captura.bin [gpu_id]\n", argv[0]);
      return 1;
   }
   uint64_t gpu_id = argc > 2 ? strtoull(argv[2], NULL, 16) : 0x74021000ull;
   FILE *f = fopen(argv[1], "rb");
   if (!f) {
      perror(argv[1]);
      return 1;
   }
   uint32_t cab[2];
   if (fread(cab, 4, 2, f) != 2 || cab[0] != 0x4D43464Eu) {
      fprintf(stderr, "no es una captura NFCM\n");
      return 1;
   }
   /* pandecode writes <base>..ctx-0.NNNN, one file per submit (pandecode_next_frame) */
   char base[1024];
   snprintf(base, sizeof(base), "%s.pdc", argv[1]);
   setenv("PANDECODE_DUMP_FILE", base, 1);
   struct pandecode_context *ctx = pandecode_create_context(false);
   pandecode_set_disassemble(ctx, desensamblar);
   uint32_t reg[2];
   while (fread(reg, 4, 2, f) == 2) {
      uint8_t *d = malloc(reg[1] ? reg[1] : 1);
      if (fread(d, 1, reg[1], f) != reg[1])
         break;
      if (reg[0] == 1) {
         uint64_t va, tam;
         memcpy(&va, d, 8);
         memcpy(&tam, d + 8, 8);
         uint32_t envio;
         memcpy(&envio, d + 16, 4);
         if (envio & 0x80000000u) {
            free(d);
            continue;
         }
         pandecode_inject_free(ctx, va, (unsigned)tam);
         pandecode_inject_mmap(ctx, va, d + 20, (unsigned)tam, NULL); /* d stays alive: pandecode keeps the pointer */
         continue;
      } else if (reg[0] == 2) {
         uint32_t indice, n, paso;
         memcpy(&indice, d, 4);
         memcpy(&n, d + 4, 4);
         memcpy(&paso, d + 8, 4);
         printf("\n===== envio %u: %u atomos (paso %u) =====\n", indice, n, paso);
         for (uint32_t i = 0; i < n; i++) {
            /* base_jd_atom of UAPI 11.x (64 bytes): seq_nr, jc, udata[2], extres_list, nr_extres, jit_id[2],
             * pre_dep[2], atom_number (+48), prio, device_nr, jobslot, core_req (+52). The old v2 one (48
             * bytes) starts with jc. */
            const uint8_t *a = d + 12 + (size_t)i * paso;
            uint64_t jc;
            uint32_t core_req = 0;
            memcpy(&jc, a + (paso >= 64 ? 8 : 0), 8);
            if (paso >= 64)
               memcpy(&core_req, a + 52, 4);
            printf("--- atomo %u: jc 0x%" PRIx64 ", core_req 0x%x\n", i, jc, core_req);
            fflush(stdout);
            /* soft jobs (BASE_JD_REQ_SOFT_JOB) have no job chain */
            if (jc && !(core_req & 0x200))
               pandecode_jc(ctx, jc, gpu_id);
         }
         pandecode_next_frame(ctx);
      } else if (reg[0] == 3) {
         printf("# %.*s\n", (int)reg[1], (char *)d);
      } else if (reg[0] == 4) {
         printf("# gpuprops: %u bytes\n", reg[1]);
      }
      free(d);
   }
   pandecode_destroy_context(ctx);
   return 0;
}
