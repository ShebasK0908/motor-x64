// Arnés de pruebas del MotorX64: carga un .bin plano o un ELF x86-64 y lo ejecuta.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/x64core.h"

// ---------- cargador ELF64 (segmentos PT_LOAD + entry) ----------
typedef struct {
    X64Cpu *cpu;
    uint64_t max_end;   // fin del segmento mas alto cargado
    uint64_t entry;
    int es_dinamico;    // tiene PT_DYNAMIC / PT_INTERP
    int no_cabe;        // algun segmento queda fuera de la memoria
} ElfInfo;

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }

static int load_elf(X64Cpu *cpu, const uint8_t *img, size_t n, ElfInfo *info) {
    memset(info, 0, sizeof(*info));
    info->cpu = cpu;
    if (n < 64 || img[0] != 0x7F || img[1] != 'E' || img[2] != 'L' || img[3] != 'F') return -1;
    if (img[4] != 2) return -2; // solo ELF64
    info->entry = rd64(img + 24);
    uint64_t phoff = rd64(img + 32);
    uint16_t phentsize = rd16(img + 54), phnum = rd16(img + 56);
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = img + phoff + (size_t)i * phentsize;
        if (ph + 56 > img + n) return -3;
        uint32_t type = rd32(ph);
        uint64_t off = rd64(ph + 8), vaddr = rd64(ph + 16);
        uint64_t filesz = rd64(ph + 32), memsz = rd64(ph + 40);
        if (type == 2 || type == 3) info->es_dinamico = 1; // PT_DYNAMIC / PT_INTERP
        if (type != 1) continue; // PT_LOAD
        if (vaddr + memsz > cpu->mem_size) { info->no_cabe = 1; continue; }
        if (off + filesz > n) return -4;
        memcpy(cpu->mem + vaddr, img + off, filesz);
        memset(cpu->mem + vaddr + filesz, 0, memsz - filesz);
        if (vaddr + memsz > info->max_end) info->max_end = vaddr + memsz;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s <programa.bin|programa.elf>\n", argv[0]); return 2; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 2; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *code = malloc(n);
    if (fread(code, 1, n, fp) != (size_t)n) { perror("fread"); return 2; }
    fclose(fp);

    const uint64_t MEM = 256ull << 20;
    uint8_t *mem = malloc(MEM);
    X64Cpu cpu;
    int es_elf = n >= 4 && code[0] == 0x7F && code[1] == 'E' && code[2] == 'L' && code[3] == 'F';
    if (es_elf) {
        x64_init(&cpu, mem, MEM, 0);
        ElfInfo info;
        int r = load_elf(&cpu, code, n, &info);
        if (r != 0) { fprintf(stderr, "ELF invalido (%d)\n", r); return 2; }
        cpu.rip = info.entry;
        printf("ELF: entry=0x%llX fin=0x%llX dinamico=%s no_cabe=%s\n",
               (unsigned long long)info.entry, (unsigned long long)info.max_end,
               info.es_dinamico ? "si" : "no", info.no_cabe ? "si" : "no");
        if (info.no_cabe) { fprintf(stderr, "segmentos fuera de la memoria invitada\n"); return 2; }
    } else {
        x64_init(&cpu, mem, MEM, 0);
        if (!x64_load(&cpu, 0, code, n)) { fprintf(stderr, "no cabe\n"); return 2; }
    }

    uint64_t max_steps = argc > 2 ? strtoull(argv[2], NULL, 0) : 200000000ull;
    X64Status s = x64_run(&cpu, max_steps);
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
