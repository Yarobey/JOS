#ifndef JOS_KERN_SYSCALL_H
#define JOS_KERN_SYSCALL_H
#ifndef JOS_KERNEL
#error "This is a JOS kernel header; user programs should not #include it"
#endif

#include <inc/syscall.h>

#ifdef SANITIZE_SHADOW_BASE
void platform_asan_unpoison(void *, size_t);
void platform_asan_poison(void *, size_t);
#endif

uintptr_t syscall(uintptr_t num, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);

#endif /* !JOS_KERN_SYSCALL_H */
