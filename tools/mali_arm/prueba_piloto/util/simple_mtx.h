/* Stand-in for Mesa's util/simple_mtx.h, to build panvk_nfsmw_arm.c alone (prueba_piloto). */
#include <pthread.h>
typedef pthread_mutex_t simple_mtx_t;
#define SIMPLE_MTX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define simple_mtx_lock pthread_mutex_lock
#define simple_mtx_unlock pthread_mutex_unlock
