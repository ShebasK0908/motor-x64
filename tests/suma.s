	.intel_syntax noprefix
	.text
	.globl _start
_start:
	xor ebx, ebx		# acumulador
	mov ecx, 1		# i = 1
bucle:
	add ebx, ecx		# acc += i
	inc ecx
	cmp ecx, 100
	jle bucle		# mientras i <= 100
	# RBX debe ser 5050
	mov eax, 60
	xor edi, edi
	syscall
