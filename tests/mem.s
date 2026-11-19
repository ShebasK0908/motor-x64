	.intel_syntax noprefix
	.text
	.globl _start
_start:
	lea rsi, [rip + tabla]	# rsi = direccion de la tabla
	xor ecx, ecx		# i = 0
rellena:
	mov eax, ecx
	imul eax, ecx		# i*i
	mov [rsi + rcx*8], rax	# tabla[i] = i*i (64 bits)
	inc ecx
	cmp ecx, 10
	jl rellena
	xor ebx, ebx		# suma
	xor ecx, ecx
suma:
	add rbx, [rsi + rcx*8]
	inc ecx
	cmp ecx, 10
	jl suma
	# RBX = 0+1+4+...+81 = 285
	mov eax, 60
	xor edi, edi
	syscall
	.balign 8
tabla:
	.space 80
