// Arnés de pruebas del MotorX64: carga un .bin x86-64 y lo ejecuta.
#include <stdio.h>
#include <stdlib.h>
#include "../core/x64core.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s <programa.bin>\n", argv[0]); return 2; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 2; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *code = malloc(n);
    if (fread(code, 1, n, fp) != (size_t)n) { perror("fread"); return 2; }
    fclose(fp);

    const uint64_t MEM = 4ull << 20;
    uint8_t *mem = malloc(MEM);
    X64Cpu cpu;
    x64_init(&cpu, mem, MEM, 0);
    if (!x64_load(&cpu, 0, code, n)) { fprintf(stderr, "no cabe\n"); return 2; }

    X64Status s = x64_run(&cpu, 100000000ull);
    const char *sn = s == X64_HALT ? "HALT" : s == X64_ERR_OPCODE ? "OPCODE_NO_SOPORTADO"
                   : s == X64_ERR_MEM ? "ERROR_MEMORIA" : "LIMITE_PASOS";
    printf("estado=%s pasos=%llu exit=%d RBX=%llu RCX=%llu RAX=%llu\n",
           sn, (unsigned long long)cpu.steps, cpu.exit_code,
           (unsigned long long)cpu.r[X64_RBX],
           (unsigned long long)cpu.r[X64_RCX],
           (unsigned long long)cpu.r[X64_RAX]);
    if (s == X64_ERR_OPCODE)
        printf("opcode malo: 0x%02X en RIP=0x%llX\n", cpu.bad_opcode, (unsigned long long)cpu.bad_rip);
    return s == X64_HALT ? 0 : 1;
}
