// MotorX64 — nucleo interprete x86-64 (v0.1)
// Ejecuta codigo maquina x86-64 real, instruccion a instruccion.
// Sin JIT: funciona dentro de cualquier sandbox (iOS incluido).
#ifndef X64CORE_H
#define X64CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { X64_RAX=0, X64_RCX=1, X64_RDX=2, X64_RBX=3, X64_RSP=4, X64_RBP=5,
       X64_RSI=6, X64_RDI=7, X64_R8=8, X64_R9=9, X64_R10=10, X64_R11=11,
       X64_R12=12, X64_R13=13, X64_R14=14, X64_R15=15 };

// Bits de RFLAGS
#define X64_CF (1ull<<0)
#define X64_PF (1ull<<2)
#define X64_AF (1ull<<4)
#define X64_ZF (1ull<<6)
#define X64_SF (1ull<<7)
#define X64_OF (1ull<<11)

typedef enum {
    X64_OK = 0,        // sigue ejecutando
    X64_HALT,          // HLT o syscall exit: termino normal
    X64_ERR_OPCODE,    // instruccion no soportada todavia
    X64_ERR_MEM,       // acceso fuera de la memoria invitada
    X64_ERR_STEPS      // se agoto el limite de pasos
} X64Status;

typedef struct {
    uint64_t r[16];
    uint64_t rip;
    uint64_t flags;
    uint8_t *mem;        // memoria invitada (plana)
    uint64_t mem_size;
    uint64_t steps;      // instrucciones ejecutadas
    int      exit_code;  // si X64_HALT por syscall exit
    uint8_t  bad_opcode; // si X64_ERR_OPCODE
    uint64_t bad_rip;
    void    *cache;      // v0.3: cache de bloques decodificados (opaco)
} X64Cpu;

// Inicializa la CPU: memoria a cero, RSP al final de la memoria, RIP = entry.
void x64_init(X64Cpu *cpu, uint8_t *mem, uint64_t mem_size, uint64_t entry);
// Carga codigo en una direccion invitada. Devuelve false si no cabe.
bool x64_load(X64Cpu *cpu, uint64_t addr, const uint8_t *data, uint64_t len);
// Ejecuta hasta halt/error o agotar max_steps. Devuelve el estado final.
X64Status x64_run(X64Cpu *cpu, uint64_t max_steps);
// v0.3: activa la cache de bloques decodificados (mas rapido).
void x64_enable_cache(X64Cpu *cpu);
// Vacia la cache (obligatorio al cargar un programa distinto en la misma CPU).
void x64_cache_clear(X64Cpu *cpu);
// Como x64_run pero usando la cache de bloques si esta activa.
X64Status x64_run_cached(X64Cpu *cpu, uint64_t max_steps);
// Ejecuta una sola instruccion (para depurar).
X64Status x64_step(X64Cpu *cpu);

#ifdef __cplusplus
}
#endif
#endif
