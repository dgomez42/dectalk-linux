/* Minimal program entry that calls __libc_start_main@GLIBC_2.2.5 (runs on old and new glibc). */
	.symver __libc_start_main,__libc_start_main@GLIBC_2.2.5
	.text
	.globl _start
	.type _start,@function
_start:
	xor %ebp,%ebp
	mov %rdx,%r9
	pop %rsi
	mov %rsp,%rdx
	and $-16,%rsp
	push %rax
	push %rsp
	xor %r8d,%r8d
	xor %ecx,%ecx
	mov $main,%rdi
	call __libc_start_main
	hlt
	.section .note.GNU-stack,"",@progbits
