#include "bridge.h"
#include "x64core.h"
#include "tests_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *nombre;
    const unsigned char *code;
    unsigned int len;
    unsigned long long esperado_rbx;
    unsigned long long esperado_rcx;
    int check_rcx;
} Prueba;

static const Prueba PRUEBAS[] = {
    { "Suma 1..100",          test_suma,   test_suma_len,   5050ull,     0, 0 },
    { "Fibonacci(20) recursivo", test_fib, test_fib_len,    6765ull,     0, 0 },
    { "Tabla de cuadrados",   test_mem,    test_mem_len,    285ull,      0, 0 },
    { "Multiplicar y dividir", test_muldiv, test_muldiv_len, 83953062ull, 1, 1 },
};

const char *motor_report(void) {
    static char buf[4096];
    const uint64_t MEM = 4ull << 20;
    uint8_t *mem = (uint8_t *)malloc(MEM);
    if (!mem) return "ERROR|sin memoria";
    size_t off = 0;
    unsigned long long total_pasos = 0;
    for (size_t i = 0; i < sizeof(PRUEBAS) / sizeof(PRUEBAS[0]); i++) {
        const Prueba *p = &PRUEBAS[i];
        X64Cpu cpu;
        x64_init(&cpu, mem, MEM, 0);
        x64_load(&cpu, 0, p->code, p->len);
        X64Status s = x64_run(&cpu, 100000000ull);
        total_pasos += cpu.steps;
        int ok = s == X64_HALT
              && cpu.r[X64_RBX] == p->esperado_rbx
              && (!p->check_rcx || cpu.r[X64_RCX] == p->esperado_rcx);
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
            "%s|%llu|%llu|%llu|%llu|%llu|%s\n",
            p->nombre,
            p->esperado_rbx,
            (unsigned long long)cpu.r[X64_RBX],
            p->esperado_rcx,
            (unsigned long long)cpu.r[X64_RCX],
            (unsigned long long)cpu.steps,
            ok ? "OK" : "FALLO");
    }
    snprintf(buf + off, sizeof(buf) - off, "TOTAL|%llu\n", total_pasos);
    free(mem);
    return buf;
}
