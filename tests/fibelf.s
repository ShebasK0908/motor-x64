	.intel_syntax noprefix
	.text
	.globl _start
_start:
	mov edi, 25
	call fib		# fib(25) = 75025
	mov ebx, eax
	mov eax, 60
	xor edi, edi
	syscall
fib:
	cmp edi, 2
	jb .base
	push rbx
	push r12
	mov ebx, edi
	lea edi, [rbx-1]
	call fib
	mov r12d, eax
	lea edi, [rbx-2]
	call fib
	add eax, r12d
	pop r12
	pop rbx
	ret
.base:
	mov eax, edi
	ret
