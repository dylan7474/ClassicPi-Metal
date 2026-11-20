/* ClassicPi-Metal CPU benchmark: Basilisk II's UAE core on Circle */
#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG __SIZEOF_LONG__
#define SIZEOF_LONG_LONG 8
#define SIZEOF_VOID_P __SIZEOF_POINTER__
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define DIRECT_ADDRESSING 1
#define USE_CPU_EMUL_SERVICES 1
#define FPU_IEEE 1
#define EXCEPTIONS_VIA_LONGJMP 1
#define SIZEOF_FLOAT 4
#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE __SIZEOF_LONG_DOUBLE__
/* EXCEPTIONS_VIA_LONGJMP: the platform supplies these */
#include <setjmp.h>
#define JMP_BUF jmp_buf
#define SETJMP(b) setjmp(b)
#define LONGJMP(b, v) longjmp(b, v)
/* newlib has no exp10: the FPU's 10^x */
#define fp_pow10(x) powl(10.0L, (x))
