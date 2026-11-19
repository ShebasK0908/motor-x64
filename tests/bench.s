	.intel_syntax noprefix
	.text
	.globl _start
_start:
	mov ecx, 200000000	# 200 millones de vueltas
	xor ebx, ebx
bucle:
	add ebx, ecx
	xor eax, eax
	inc eax
	dec ecx
	jnz bucle
	# RBX = suma 200M..1 (mod 2^32 por usar ebx)
	mov eax, 60
	xor edi, edi
	syscall
