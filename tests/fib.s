	.intel_syntax noprefix
	.text
	.globl _start
_start:
	mov edi, 20
	call fib		# devuelve en rax
	mov ebx, eax		# RBX = fib(20) = 6765
	mov eax, 60
	xor edi, edi
	syscall

# fib(n): recursiva, n en edi, resultado en eax
fib:
	cmp edi, 2
	jb .base
	push rbx
	push r12
	mov ebx, edi		# guarda n
	lea edi, [rbx-1]
	call fib
	mov r12d, eax		# fib(n-1)
	lea edi, [rbx-2]
	call fib
	add eax, r12d		# fib(n-2) + fib(n-1)
	pop r12
	pop rbx
	ret
.base:
	mov eax, edi
	ret
