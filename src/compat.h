/* Bind to the oldest symbol versions so the binaries also run on older Linux distributions
 * (glibc 2.17 and later) even though they are built with a current toolchain. */
#ifndef DT_COMPAT_H
#define DT_COMPAT_H
#if defined(__x86_64__) && defined(__linux__)
__asm__(".symver dlopen,dlopen@GLIBC_2.2.5");
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver dladdr,dladdr@GLIBC_2.2.5");
__asm__(".symver pthread_create,pthread_create@GLIBC_2.2.5");
__asm__(".symver pthread_detach,pthread_detach@GLIBC_2.2.5");
__asm__(".symver pthread_once,pthread_once@GLIBC_2.2.5");
__asm__(".symver pthread_mutexattr_init,pthread_mutexattr_init@GLIBC_2.2.5");
__asm__(".symver pthread_mutexattr_settype,pthread_mutexattr_settype@GLIBC_2.2.5");
__asm__(".symver __libc_start_main,__libc_start_main@GLIBC_2.2.5");
#endif
#endif
