#include <stdio.h>
#include <stdlib.h>
#include "bifrost/disassemble.h"
int main(int argc, char **argv) {
  FILE *f = fopen(argv[1], "rb");
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char *d = malloc(n); fread(d, 1, n, f); fclose(f);
  long ini = argc > 2 ? strtol(argv[2], 0, 0) : 0;
  disassemble_bifrost(stdout, d + ini, n - ini, argc > 3);
  return 0;
}
