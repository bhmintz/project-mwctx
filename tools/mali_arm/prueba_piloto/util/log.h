/* Stand-in for Mesa's util/log.h (prueba_piloto). */
#include <stdio.h>
#define mesa_logi(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#define mesa_loge(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
