/* pandecode compiled for Bifrost v7 only: the other architectures' entry points, never called. */
#include <stdint.h>
#include <stdlib.h>
struct pandecode_context;
#define NO(n) void n(void) { abort(); }
NO(pandecode_abort_on_fault_v4) NO(pandecode_abort_on_fault_v5) NO(pandecode_abort_on_fault_v6)
NO(pandecode_abort_on_fault_v9) NO(pandecode_jc_v4) NO(pandecode_jc_v5) NO(pandecode_jc_v6) NO(pandecode_jc_v9)
NO(pandecode_cs_binary_v10) NO(pandecode_cs_binary_v12) NO(pandecode_cs_binary_v13) NO(pandecode_cs_binary_v14)
NO(pandecode_cs_trace_v10) NO(pandecode_cs_trace_v12) NO(pandecode_cs_trace_v13) NO(pandecode_cs_trace_v14)
NO(pandecode_interpret_cs_v10) NO(pandecode_interpret_cs_v12) NO(pandecode_interpret_cs_v13)
NO(pandecode_interpret_cs_v14)

/* Of Mesa's util, what is not worth compiling with its platform detection. */
#include <stdarg.h>
#include <stdio.h>
const char *os_get_option(const char *nombre) { return getenv(nombre); }
const char *os_get_option_cached(const char *nombre) { return getenv(nombre); }
void os_log_message(const char *mensaje) { fputs(mensaje, stderr); }
size_t u_printf_length(const char *fmt, va_list args)
{
   va_list copia;
   va_copy(copia, args);
   int n = vsnprintf(NULL, 0, fmt, copia);
   va_end(copia);
   return n > 0 ? (size_t)n : 0;
}
