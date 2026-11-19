	.intel_syntax noprefix
	.text
	.globl _start
_start:
	mov eax, 12345
	mov ecx, 6789
	imul ecx		# edx:eax = 12345*6789 = 83810205
	mov ebx, eax		# RBX = 83810205
	mov eax, 1000000
	xor edx, edx
	mov ecx, 7
	div ecx			# eax = 142857, edx = 1
	mov ecx, edx		# RCX = resto = 1... ojo: div usa ecx; guardamos tras
	# rehacer limpio:
	mov eax, 1000000
	xor edx, edx
	mov esi, 7
	div esi			# eax = 142857, edx = 1
	add ebx, eax		# RBX = 83810205 + 142857 = 83953062
	mov ecx, edx		# RCX = 1
	mov eax, 60
	xor edi, edi
	syscall
