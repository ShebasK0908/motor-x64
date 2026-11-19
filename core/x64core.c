// MotorX64 — nucleo interprete x86-64 (v0.1)
// Interprete fetch-decode-execute de un subconjunto real de x86-64:
// MOV/LEA/XCHG, ALU completa con flags, desplazamientos, saltos
// condicionales, CALL/RET, PUSH/POP, IMUL/MUL/DIV/IDIV, SETcc, MOVZX/SX.
// Halt: HLT o SYSCALL con rax=60 (exit, codigo en rdi), como en Linux.
#include "x64core.h"
#include <string.h>

// ---------- memoria invitada ----------
static bool mem_ok(X64Cpu *c, uint64_t a, unsigned n) {
    return a <= c->mem_size && n <= c->mem_size - a;
}
static uint64_t mem_rd(X64Cpu *c, uint64_t a, unsigned n, bool *ok) {
    if (!mem_ok(c, a, n)) { *ok = false; return 0; }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint64_t v = 0;
    __builtin_memcpy(&v, c->mem + a, n); // anfitrion little-endian (x86/ARM): directo
    return v;
#else
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint64_t)c->mem[a + i] << (8 * i);
    return v;
#endif
}
static void mem_wr(X64Cpu *c, uint64_t a, uint64_t v, unsigned n, bool *ok) {
    if (!mem_ok(c, a, n)) { *ok = false; return; }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    __builtin_memcpy(c->mem + a, &v, n);
#else
    for (unsigned i = 0; i < n; i++) c->mem[a + i] = (uint8_t)(v >> (8 * i));
#endif
}

// ---------- registros parciales ----------
static uint64_t reg_rd(X64Cpu *c, int idx, unsigned size) {
    uint64_t v = c->r[idx];
    if (size == 1) return v & 0xFF;
    if (size == 2) return v & 0xFFFF;
    if (size == 4) return v & 0xFFFFFFFFull;
    return v;
}
// Escribe respetando las reglas x86-64: 32 bits pone a cero la parte alta;
// 8/16 bits conservan el resto. high8=true para AH/CH/DH/BH (sin REX).
static void reg_wr(X64Cpu *c, int idx, uint64_t v, unsigned size, bool high8) {
    if (size == 8) { c->r[idx] = v; return; }
    if (size == 4) { c->r[idx] = v & 0xFFFFFFFFull; return; }
    if (size == 2) { c->r[idx] = (c->r[idx] & ~0xFFFFull) | (v & 0xFFFF); return; }
    // 1 byte
    if (high8) c->r[idx] = (c->r[idx] & ~0xFF00ull) | ((v & 0xFF) << 8);
    else       c->r[idx] = (c->r[idx] & ~0xFFull)   |  (v & 0xFF);
}

// ---------- flags ----------
static bool paridad_par(uint64_t v) { // PF: numero par de bits en el byte bajo
    v &= 0xFF; v ^= v >> 4; v &= 0xF;
    return ((0x6996 >> v) & 1) == 0;
}
static void set_flag(X64Cpu *c, uint64_t f, bool on) {
    if (on) c->flags |= f; else c->flags &= ~f;
}
static void flags_sz_p(X64Cpu *c, uint64_t res, unsigned size) {
    uint64_t mask = size == 8 ? ~0ull : ((1ull << (size * 8)) - 1);
    uint64_t sign = 1ull << (size * 8 - 1);
    set_flag(c, X64_ZF, (res & mask) == 0);
    set_flag(c, X64_SF, (res & sign) != 0);
    set_flag(c, X64_PF, paridad_par(res));
}
static void flags_add(X64Cpu *c, uint64_t a, uint64_t b, uint64_t res, unsigned size, bool with_carry) {
    uint64_t mask = size == 8 ? ~0ull : ((1ull << (size * 8)) - 1);
    uint64_t sign = 1ull << (size * 8 - 1);
    (void)with_carry;
    set_flag(c, X64_CF, size == 8 ? (res < a) : ((res & mask) < (a & mask)) || (res >> (size*8)) != 0);
    if (size < 8) set_flag(c, X64_CF, (res >> (size * 8)) != 0);
    set_flag(c, X64_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(c, X64_OF, ((~(a ^ b) & (a ^ res)) & sign) != 0);
    flags_sz_p(c, res, size);
}
static void flags_sub(X64Cpu *c, uint64_t a, uint64_t b, uint64_t res, unsigned size) {
    uint64_t mask = size == 8 ? ~0ull : ((1ull << (size * 8)) - 1);
    uint64_t sign = 1ull << (size * 8 - 1);
    set_flag(c, X64_CF, (a & mask) < (b & mask));
    set_flag(c, X64_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(c, X64_OF, (((a ^ b) & (a ^ res)) & sign) != 0);
    flags_sz_p(c, res, size);
}
static void flags_logic(X64Cpu *c, uint64_t res, unsigned size) {
    set_flag(c, X64_CF, false); set_flag(c, X64_OF, false); set_flag(c, X64_AF, false);
    flags_sz_p(c, res, size);
}
static bool cond(X64Cpu *c, int cc) {
    bool cf = c->flags & X64_CF, pf = c->flags & X64_PF, zf = c->flags & X64_ZF;
    bool sf = c->flags & X64_SF, of = c->flags & X64_OF;
    switch (cc & 0xF) {
        case 0x0: return of;              case 0x1: return !of;
        case 0x2: return cf;              case 0x3: return !cf;
        case 0x4: return zf;              case 0x5: return !zf;
        case 0x6: return cf || zf;        case 0x7: return !cf && !zf;
        case 0x8: return sf;              case 0x9: return !sf;
        case 0xA: return pf;              case 0xB: return !pf;
        case 0xC: return sf != of;        case 0xD: return sf == of;
        case 0xE: return zf || (sf != of);case 0xF: return !zf && (sf == of);
    }
    return false;
}

// ---------- fetch ----------
typedef struct { X64Cpu *c; bool err; } Fetch;
static uint8_t f8(Fetch *f) {
    if (f->c->rip >= f->c->mem_size) { f->err = true; return 0; }
    return f->c->mem[f->c->rip++];
}
static uint64_t fN(Fetch *f, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint64_t)f8(f) << (8 * i);
    return v;
}
static int64_t fsN(Fetch *f, unsigned n) { // con signo
    uint64_t v = fN(f, n);
    if (n < 8 && (v & (1ull << (n * 8 - 1)))) v |= ~0ull << (n * 8);
    return (int64_t)v;
}

// ---------- ModRM ----------
typedef struct {
    int reg;          // campo reg (con REX.R)
    int rm_reg;       // si mod==3: registro destino/fuente
    uint64_t addr;    // si mod!=3: direccion efectiva
    bool is_mem;
} ModRM;

static ModRM decode_modrm(Fetch *f, int rex) {
    X64Cpu *c = f->c;
    ModRM m; uint8_t b = f8(f);
    int mod = b >> 6, reg = (b >> 3) & 7, rm = b & 7;
    m.reg = reg | ((rex & 4) ? 8 : 0); // REX.R
    int rm_ext = rm | ((rex & 1) ? 8 : 0); // REX.B
    m.is_mem = mod != 3; m.rm_reg = rm_ext; m.addr = 0;
    if (mod == 3) return m;
    uint64_t base = 0, index = 0; unsigned scale = 1; bool has_base = true;
    if (rm == 4) { // SIB
        uint8_t sib = f8(f);
        scale = 1u << (sib >> 6);
        int idx = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0); // REX.X
        int bas = (sib & 7) | ((rex & 1) ? 8 : 0);
        if (((sib >> 3) & 7) != 4) index = c->r[idx] * scale;
        if ((sib & 7) == 5 && mod == 0) { has_base = false; base = 0; }
        else base = c->r[bas];
    } else if (rm == 5 && mod == 0) { // RIP + disp32
        int64_t d = fsN(f, 4);
        m.addr = c->rip + (uint64_t)d;
        return m;
    } else {
        base = c->r[rm_ext];
    }
    int64_t disp = 0;
    if (mod == 1) disp = fsN(f, 1);
    else if (mod == 2) disp = fsN(f, 4);
    else if (!has_base) { disp = fsN(f, 4); base = 0; }
    m.addr = base + index + (uint64_t)disp;
    return m;
}

static uint64_t op_rd(X64Cpu *c, ModRM *m, unsigned size, bool rex, bool *ok) {
    if (m->is_mem) return mem_rd(c, m->addr, size, ok);
    if (size == 1 && !rex && m->rm_reg >= 4 && m->rm_reg <= 7) // AH..BH
        return (c->r[m->rm_reg - 4] >> 8) & 0xFF;
    return reg_rd(c, m->rm_reg, size);
}
static void op_wr(X64Cpu *c, ModRM *m, uint64_t v, unsigned size, bool rex, bool *ok) {
    if (m->is_mem) { mem_wr(c, m->addr, v, size, ok); return; }
    if (size == 1 && !rex && m->rm_reg >= 4 && m->rm_reg <= 7) {
        reg_wr(c, m->rm_reg - 4, v, 1, true); return;
    }
    reg_wr(c, m->rm_reg, v, size, false);
}
static uint64_t reg_field_rd(X64Cpu *c, ModRM *m, unsigned size, bool rex) {
    if (size == 1 && !rex && m->reg >= 4 && m->reg <= 7)
        return (c->r[m->reg - 4] >> 8) & 0xFF;
    return reg_rd(c, m->reg, size);
}
static void reg_field_wr(X64Cpu *c, ModRM *m, uint64_t v, unsigned size, bool rex) {
    if (size == 1 && !rex && m->reg >= 4 && m->reg <= 7) {
        reg_wr(c, m->reg - 4, v, 1, true); return;
    }
    reg_wr(c, m->reg, v, size, false);
}

// pila
static bool push(X64Cpu *c, uint64_t v) {
    bool ok = true;
    c->r[X64_RSP] -= 8;
    mem_wr(c, c->r[X64_RSP], v, 8, &ok);
    return ok;
}
static uint64_t pop(X64Cpu *c, bool *ok) {
    uint64_t v = mem_rd(c, c->r[X64_RSP], 8, ok);
    c->r[X64_RSP] += 8;
    return v;
}
static int64_t sext(uint64_t v, unsigned size) {
    if (size == 8) return (int64_t)v;
    unsigned bits = size * 8;
    if (v & (1ull << (bits - 1))) v |= ~0ull << bits;
    return (int64_t)v;
}

// ---------- ALU ----------
static uint64_t alu(X64Cpu *c, int which, uint64_t a, uint64_t b, unsigned size, bool setf) {
    uint64_t mask = size == 8 ? ~0ull : ((1ull << (size * 8)) - 1);
    uint64_t res = 0;
    switch (which) {
        case 0: res = a + b; if (setf) flags_add(c, a, b, res, size, false); break; // ADD
        case 1: res = a | b; if (setf) flags_logic(c, res, size); break;             // OR
        case 2: { uint64_t cf = (c->flags & X64_CF) ? 1 : 0; res = a + b + cf;
                  if (setf) flags_add(c, a, b + cf, res, size, true); } break;       // ADC
        case 3: { uint64_t cf = (c->flags & X64_CF) ? 1 : 0; res = a - b - cf;
                  if (setf) flags_sub(c, a, b + cf, res, size); } break;             // SBB
        case 4: res = a & b; if (setf) flags_logic(c, res, size); break;             // AND
        case 5: res = a - b; if (setf) flags_sub(c, a, b, res, size); break;         // SUB
        case 6: res = a ^ b; if (setf) flags_logic(c, res, size); break;             // XOR
        case 7: res = a - b; if (setf) flags_sub(c, a, b, res, size); break;         // CMP
    }
    return res & mask;
}

// ---------- init / load ----------
void x64_init(X64Cpu *cpu, uint8_t *mem, uint64_t mem_size, uint64_t entry) {
    void *cache = cpu->cache; // la cache sobrevive al init; se vacia con x64_cache_clear
    memset(cpu, 0, sizeof(*cpu));
    cpu->cache = cache;
    cpu->mem = mem; cpu->mem_size = mem_size;
    memset(mem, 0, mem_size);
    cpu->rip = entry;
    cpu->r[X64_RSP] = mem_size & ~0xFull;
    cpu->flags = 2; // bit reservado de RFLAGS
}
bool x64_load(X64Cpu *cpu, uint64_t addr, const uint8_t *data, uint64_t len) {
    if (addr > cpu->mem_size || len > cpu->mem_size - addr) return false;
    memcpy(cpu->mem + addr, data, len);
    return true;
}

// ---------- un paso ----------
X64Status x64_step(X64Cpu *c) {
    Fetch f = { c, false };
    bool ok = true;
    uint64_t start_rip = c->rip;

    // prefijos
    bool op16 = false, rep = false;
    int rex = 0;
    uint8_t op;
    for (;;) {
        op = f8(&f);
        if (f.err) return X64_ERR_MEM;
        if (op == 0x66) { op16 = true; continue; }
        if (op == 0x67) { continue; } // address-size: ignorado (seguimos en 64)
        if (op == 0xF2 || op == 0xF3) { rep = true; continue; }
        if (op >= 0x40 && op <= 0x4F) { rex = op; continue; }
        if (op == 0x2E || op == 0x3E || op == 0x26 || op == 0x64 || op == 0x65) continue;
        break;
    }
    bool has_rex = rex != 0;
    unsigned osize = (rex & 8) ? 8 : (op16 ? 2 : 4); // REX.W / 66h / defecto
    (void)rep;

    // ---- instrucciones de un byte con registro implicito ----
    if (op >= 0x50 && op <= 0x57) { // PUSH r
        int idx = (op - 0x50) | ((rex & 1) ? 8 : 0);
        if (!push(c, c->r[idx])) return X64_ERR_MEM;
        goto done;
    }
    if (op >= 0x58 && op <= 0x5F) { // POP r
        int idx = (op - 0x58) | ((rex & 1) ? 8 : 0);
        uint64_t v = pop(c, &ok); if (!ok) return X64_ERR_MEM;
        c->r[idx] = v;
        goto done;
    }
    if (op >= 0x91 && op <= 0x97) { // XCHG rax, r
        int idx = (op - 0x90) | ((rex & 1) ? 8 : 0);
        uint64_t t = reg_rd(c, idx, osize);
        reg_wr(c, idx, reg_rd(c, 0, osize), osize, false);
        reg_wr(c, 0, t, osize, false);
        goto done;
    }
    if (op >= 0xB0 && op <= 0xB7) { // MOV r8, imm8
        int idx = (op - 0xB0) | ((rex & 1) ? 8 : 0);
        uint8_t v = f8(&f);
        if (has_rex || idx < 4) reg_wr(c, idx, v, 1, false);
        else reg_wr(c, idx - 4, v, 1, true);
        goto done;
    }
    if (op >= 0xB8 && op <= 0xBF) { // MOV r, imm (32/64; con REX.W imm64)
        int idx = (op - 0xB8) | ((rex & 1) ? 8 : 0);
        uint64_t v = (rex & 8) ? fN(&f, 8) : fN(&f, 4);
        if (f.err) return X64_ERR_MEM;
        c->r[idx] = (rex & 8) ? v : (v & 0xFFFFFFFFull);
        goto done;
    }

    switch (op) {
        case 0x90: goto done; // NOP
        case 0xF4: return X64_HALT; // HLT
        case 0x98: // CBW/CWDE/CDQE
            if (rex & 8) c->r[0] = (uint64_t)sext(c->r[0] & 0xFFFFFFFFull, 4);
            else if (op16) reg_wr(c, 0, (uint64_t)sext(c->r[0] & 0xFF, 1), 2, false);
            else c->r[0] = (c->r[0] & ~0xFFFFFFFFull) | (uint32_t)sext(c->r[0] & 0xFFFF, 2);
            goto done;
        case 0x99: // CWD/CDQ/CQO
            if (rex & 8) c->r[2] = ((int64_t)c->r[0] < 0) ? ~0ull : 0;
            else if (op16) reg_wr(c, 2, ((int16_t)c->r[0] < 0) ? 0xFFFF : 0, 2, false);
            else c->r[2] = (c->r[2] & ~0xFFFFFFFFull) | (((int32_t)c->r[0] < 0) ? 0xFFFFFFFFu : 0);
            goto done;
        case 0x68: { uint64_t v = (uint64_t)fsN(&f, 4); if (!push(c, v)) return X64_ERR_MEM; goto done; }
        case 0x6A: { uint64_t v = (uint64_t)fsN(&f, 1); if (!push(c, v)) return X64_ERR_MEM; goto done; }
        case 0xC3: { // RET
            uint64_t v = pop(c, &ok); if (!ok) return X64_ERR_MEM;
            c->rip = v; goto done;
        }
        case 0xC2: { // RET imm16
            uint16_t n = (uint16_t)fN(&f, 2);
            uint64_t v = pop(c, &ok); if (!ok) return X64_ERR_MEM;
            c->rip = v; c->r[X64_RSP] += n; goto done;
        }
        case 0xE8: { // CALL rel32
            int64_t d = fsN(&f, 4);
            if (!push(c, c->rip)) return X64_ERR_MEM;
            c->rip += (uint64_t)d; goto done;
        }
        case 0xE9: { int64_t d = fsN(&f, 4); c->rip += (uint64_t)d; goto done; }
        case 0xEB: { int64_t d = fsN(&f, 1); c->rip += (uint64_t)d; goto done; }
        case 0xFF: case 0xFE: { // grupo 5 / INC-DEC mem
            ModRM m = decode_modrm(&f, rex);
            unsigned sz = op == 0xFE ? 1 : osize;
            uint64_t a = op_rd(c, &m, sz, has_rex, &ok); if (!ok || f.err) return X64_ERR_MEM;
            switch (m.reg & 7) {
                case 0: { // INC
                    uint64_t cf = c->flags & X64_CF;
                    uint64_t r = alu(c, 0, a, 1, sz, true);
                    set_flag(c, X64_CF, cf != 0);
                    op_wr(c, &m, r, sz, has_rex, &ok); break; }
                case 1: { // DEC
                    uint64_t cf = c->flags & X64_CF;
                    uint64_t r = alu(c, 5, a, 1, sz, true);
                    set_flag(c, X64_CF, cf != 0);
                    op_wr(c, &m, r, sz, has_rex, &ok); break; }
                case 2: { // CALL rm
                    if (!push(c, c->rip)) return X64_ERR_MEM;
                    c->rip = a; break; }
                case 4: c->rip = a; break; // JMP rm
                case 6: if (!push(c, a)) return X64_ERR_MEM; break; // PUSH rm
                default: goto bad;
            }
            if (!ok) return X64_ERR_MEM;
            goto done;
        }
        default: break;
    }

    if (op >= 0x70 && op <= 0x7F) { // Jcc rel8
        int64_t d = fsN(&f, 1);
        if (cond(c, op & 0xF)) c->rip += (uint64_t)d;
        goto done;
    }

    // ---- ALU con ModRM: 00-3D ----
    if (op <= 0x3D && ((op & 7) <= 5)) {
        int which = (op >> 3) & 7, form = op & 7;
        if (form <= 3) { // rm <-> reg (0,2: rm<-reg... en realidad 0/1 rm,reg; 2/3 reg,rm)
            unsigned sz = (form & 1) ? osize : 1;
            ModRM m = decode_modrm(&f, rex);
            uint64_t rm_v = op_rd(c, &m, sz, has_rex, &ok); if (!ok || f.err) return X64_ERR_MEM;
            uint64_t rg_v = reg_field_rd(c, &m, sz, has_rex);
            if (form <= 1) {
                uint64_t r = alu(c, which, rm_v, rg_v, sz, true);
                if (which != 7) { op_wr(c, &m, r, sz, has_rex, &ok); if (!ok) return X64_ERR_MEM; }
            } else {
                uint64_t r = alu(c, which, rg_v, rm_v, sz, true);
                if (which != 7) reg_field_wr(c, &m, r, sz, has_rex);
            }
            goto done;
        }
        if (form == 4 || form == 5) { // acumulador, inmediato
            unsigned sz = (form & 1) ? osize : 1;
            uint64_t imm = (form & 1) ? (osize == 8 ? (uint64_t)fsN(&f, 4) : fN(&f, osize == 2 ? 2 : 4))
                                      : fN(&f, 1);
            if (f.err) return X64_ERR_MEM;
            if (osize == 8 && (form & 1)) imm = (uint64_t)sext(imm, 4);
            uint64_t r = alu(c, which, reg_rd(c, 0, sz), imm, sz, true);
            if (which != 7) reg_wr(c, 0, r, sz, false);
            goto done;
        }
    }
    // TEST / XCHG con ModRM
    if (op == 0x84 || op == 0x85 || op == 0x86 || op == 0x87) {
        unsigned sz = (op & 1) ? osize : 1;
        ModRM m = decode_modrm(&f, rex);
        uint64_t rm_v = op_rd(c, &m, sz, has_rex, &ok); if (!ok || f.err) return X64_ERR_MEM;
        uint64_t rg_v = reg_field_rd(c, &m, sz, has_rex);
        if (op >= 0x86) { // XCHG
            op_wr(c, &m, rg_v, sz, has_rex, &ok); if (!ok) return X64_ERR_MEM;
            reg_field_wr(c, &m, rm_v, sz, has_rex);
        } else {
            alu(c, 4, rm_v, rg_v, sz, true); // AND sin guardar = TEST
        }
        goto done;
    }
    // MOV con ModRM
    if (op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B || op == 0x8D) {
        unsigned sz = (op & 1) ? osize : 1;
        ModRM m = decode_modrm(&f, rex);
        if (op == 0x8D) { // LEA
            if (!m.is_mem) goto bad;
            reg_field_wr(c, &m, m.addr, osize, has_rex);
            goto done;
        }
        if (op <= 0x89) {
            uint64_t v = reg_field_rd(c, &m, sz, has_rex);
            op_wr(c, &m, v, sz, has_rex, &ok);
        } else {
            uint64_t v = op_rd(c, &m, sz, has_rex, &ok);
            if (ok) reg_field_wr(c, &m, v, sz, has_rex);
        }
        if (!ok || f.err) return X64_ERR_MEM;
        goto done;
    }
    // grupo 1: ALU rm, imm (80/81/83)
    if (op == 0x80 || op == 0x81 || op == 0x83) {
        unsigned sz = op == 0x80 ? 1 : osize;
        ModRM m = decode_modrm(&f, rex);
        uint64_t imm;
        if (op == 0x81) imm = osize == 8 ? (uint64_t)fsN(&f, 4) : fN(&f, osize == 2 ? 2 : 4);
        else if (op == 0x83) imm = (uint64_t)fsN(&f, 1);
        else imm = fN(&f, 1);
        if (f.err) return X64_ERR_MEM;
        uint64_t a = op_rd(c, &m, sz, has_rex, &ok); if (!ok) return X64_ERR_MEM;
        uint64_t r = alu(c, m.reg & 7, a, imm, sz, true);
        if ((m.reg & 7) != 7) { op_wr(c, &m, r, sz, has_rex, &ok); if (!ok) return X64_ERR_MEM; }
        goto done;
    }
    // MOV rm, imm (C6/C7)
    if (op == 0xC6 || op == 0xC7) {
        unsigned sz = op == 0xC6 ? 1 : osize;
        ModRM m = decode_modrm(&f, rex);
        if ((m.reg & 7) != 0) goto bad;
        uint64_t imm = op == 0xC6 ? fN(&f, 1) : (osize == 8 ? (uint64_t)fsN(&f, 4) : fN(&f, osize == 2 ? 2 : 4));
        if (f.err) return X64_ERR_MEM;
        op_wr(c, &m, imm, sz, has_rex, &ok); if (!ok) return X64_ERR_MEM;
        goto done;
    }
    // desplazamientos C0/C1/D0-D3
    if (op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3)) {
        unsigned sz = (op == 0xC0 || op == 0xD0 || op == 0xD2) ? 1 : osize;
        if (op == 0xC1 || op == 0xC0) sz = op == 0xC0 ? 1 : osize;
        ModRM m = decode_modrm(&f, rex);
        unsigned cnt;
        if (op == 0xC0 || op == 0xC1) { cnt = f8(&f); if (f.err) return X64_ERR_MEM; }
        else if (op == 0xD0 || op == 0xD1) cnt = 1;
        else cnt = (unsigned)(c->r[X64_RCX] & 0xFF);
        cnt &= (sz == 8 ? 63 : 31);
        uint64_t a = op_rd(c, &m, sz, has_rex, &ok); if (!ok) return X64_ERR_MEM;
        if (cnt == 0) goto done;
        uint64_t mask = sz == 8 ? ~0ull : ((1ull << (sz * 8)) - 1);
        uint64_t sign = 1ull << (sz * 8 - 1);
        uint64_t res = 0; int which = m.reg & 7;
        switch (which) {
            case 0: res = (a << cnt) & mask; // ROL
                res |= (a >> (sz * 8 - cnt)) & mask; res &= mask;
                set_flag(c, X64_CF, res & 1); if (cnt == 1) set_flag(c, X64_OF, ((res & sign) != 0) != ((c->flags & X64_CF) != 0));
                break;
            case 1: res = ((a >> cnt) | (a << (sz * 8 - cnt))) & mask; // ROR
                set_flag(c, X64_CF, (res & sign) != 0); if (cnt == 1) set_flag(c, X64_OF, ((res & sign) != 0) != ((res & (sign >> 1)) != 0));
                break;
            case 4: res = (a << cnt) & mask; // SHL
                set_flag(c, X64_CF, (a >> (sz * 8 - cnt)) & 1);
                if (cnt == 1) set_flag(c, X64_OF, ((res & sign) != 0) != ((c->flags & X64_CF) != 0));
                flags_sz_p(c, res, sz);
                break;
            case 5: res = (a & mask) >> cnt; // SHR
                set_flag(c, X64_CF, (a >> (cnt - 1)) & 1);
                if (cnt == 1) set_flag(c, X64_OF, (a & sign) != 0);
                flags_sz_p(c, res, sz);
                break;
            case 7: res = (uint64_t)(sext(a, sz) >> cnt) & mask; // SAR
                set_flag(c, X64_CF, (sext(a, sz) >> (cnt - 1)) & 1);
                if (cnt == 1) set_flag(c, X64_OF, false);
                flags_sz_p(c, res, sz);
                break;
            default: goto bad;
        }
        op_wr(c, &m, res, sz, has_rex, &ok); if (!ok) return X64_ERR_MEM;
        goto done;
    }
    // grupo 3: F6/F7 (TEST/NOT/NEG/MUL/IMUL/DIV/IDIV)
    if (op == 0xF6 || op == 0xF7) {
        unsigned sz = op == 0xF6 ? 1 : osize;
        ModRM m = decode_modrm(&f, rex);
        uint64_t a = op_rd(c, &m, sz, has_rex, &ok); if (!ok || f.err) return X64_ERR_MEM;
        uint64_t mask = sz == 8 ? ~0ull : ((1ull << (sz * 8)) - 1);
        switch (m.reg & 7) {
            case 0: case 1: { // TEST rm, imm
                uint64_t imm = op == 0xF6 ? fN(&f, 1) : fN(&f, sz == 2 ? 2 : 4);
                if (f.err) return X64_ERR_MEM;
                alu(c, 4, a, imm, sz, true); break; }
            case 2: op_wr(c, &m, ~a & mask, sz, has_rex, &ok); break; // NOT
            case 3: { // NEG
                uint64_t r = (0 - a) & mask;
                flags_sub(c, 0, a, r, sz); set_flag(c, X64_CF, a != 0);
                op_wr(c, &m, r, sz, has_rex, &ok); break; }
            case 4: { // MUL
                if (sz == 8) {
                    __uint128_t r = (__uint128_t)c->r[0] * a;
                    c->r[0] = (uint64_t)r; c->r[2] = (uint64_t)(r >> 64);
                    set_flag(c, X64_CF, c->r[2] != 0); set_flag(c, X64_OF, c->r[2] != 0);
                } else {
                    uint64_t r = reg_rd(c, 0, sz) * a; // ambos <= mascara: el producto cabe en 2*sz bits
                    if (sz == 1) reg_wr(c, 0, r, 2, false);
                    else { reg_wr(c, 0, r & mask, sz, false); reg_wr(c, 2, (r >> (sz * 8)) & mask, sz, false); }
                    bool hi = sz == 1 ? (r >> 8) != 0 : ((r >> (sz * 8)) & mask) != 0;
                    set_flag(c, X64_CF, hi); set_flag(c, X64_OF, hi);
                }
                break; }
            case 5: { // IMUL (una operanda)
                if (sz == 8) {
                    __int128 r = (__int128)(int64_t)c->r[0] * (int64_t)a;
                    c->r[0] = (uint64_t)r; c->r[2] = (uint64_t)(r >> 64);
                    bool ovf = r != (int64_t)c->r[0];
                    set_flag(c, X64_CF, ovf); set_flag(c, X64_OF, ovf);
                } else {
                    int64_t r = sext(reg_rd(c, 0, sz), sz) * sext(a, sz);
                    if (sz == 1) reg_wr(c, 0, (uint64_t)r, 2, false);
                    else { reg_wr(c, 0, (uint64_t)r & mask, sz, false); reg_wr(c, 2, ((uint64_t)r >> (sz * 8)) & mask, sz, false); }
                    bool ovf = r != sext((uint64_t)r & mask, sz);
                    set_flag(c, X64_CF, ovf); set_flag(c, X64_OF, ovf);
                }
                break; }
            case 6: { // DIV
                if (a == 0) goto bad;
                if (sz == 8) {
                    __uint128_t dvd = ((__uint128_t)c->r[2] << 64) | c->r[0];
                    c->r[0] = (uint64_t)(dvd / a); c->r[2] = (uint64_t)(dvd % a);
                } else if (sz == 1) {
                    uint64_t dvd = reg_rd(c, 0, 2);
                    reg_wr(c, 0, dvd / a, 1, false); reg_wr(c, 0, dvd % a, 1, true);
                } else {
                    uint64_t dvd = (reg_rd(c, 2, sz) << (sz * 8)) | reg_rd(c, 0, sz);
                    reg_wr(c, 0, dvd / a, sz, false); reg_wr(c, 2, dvd % a, sz, false);
                }
                break; }
            case 7: { // IDIV
                if ((int64_t)a == 0) goto bad;
                if (sz == 8) {
                    __int128 dvd = (__int128)((( (__uint128_t)(uint64_t)c->r[2]) << 64) | c->r[0]);
                    // reconstruir con signo: si rdx es todo unos/extension, el valor ya es correcto en 128
                    c->r[0] = (uint64_t)(dvd / (int64_t)a); c->r[2] = (uint64_t)(dvd % (int64_t)a);
                } else if (sz == 1) {
                    int64_t dvd = sext(reg_rd(c, 0, 2), 2);
                    reg_wr(c, 0, (uint64_t)(dvd / sext(a, 1)), 1, false);
                    reg_wr(c, 0, (uint64_t)(dvd % sext(a, 1)), 1, true);
                } else {
                    int64_t dvd = sext((reg_rd(c, 2, sz) << (sz * 8)) | reg_rd(c, 0, sz), sz == 2 ? 4 : 8);
                    if (sz == 4) dvd = (int64_t)(int32_t)((reg_rd(c, 2, 4) << 32) | reg_rd(c, 0, 4));
                    reg_wr(c, 0, (uint64_t)(dvd / sext(a, sz)), sz, false);
                    reg_wr(c, 2, (uint64_t)(dvd % sext(a, sz)), sz, false);
                }
                break; }
        }
        if (!ok) return X64_ERR_MEM;
        goto done;
    }
    // IMUL con inmediato / dos operandos
    if (op == 0x69 || op == 0x6B) {
        ModRM m = decode_modrm(&f, rex);
        uint64_t imm = op == 0x69 ? (osize == 8 ? (uint64_t)fsN(&f, 4) : (uint64_t)fsN(&f, osize == 2 ? 2 : 4))
                                  : (uint64_t)fsN(&f, 1);
        if (f.err) return X64_ERR_MEM;
        uint64_t a = op_rd(c, &m, osize, has_rex, &ok); if (!ok) return X64_ERR_MEM;
        int64_t r = sext(a, osize) * (int64_t)imm;
        bool ovf = r != sext((uint64_t)r, osize);
        set_flag(c, X64_CF, ovf); set_flag(c, X64_OF, ovf);
        reg_field_wr(c, &m, (uint64_t)r, osize, has_rex);
        goto done;
    }
    // escape 0F
    if (op == 0x0F) {
        uint8_t op2 = f8(&f); if (f.err) return X64_ERR_MEM;
        if (op2 >= 0x80 && op2 <= 0x8F) { // Jcc rel32
            int64_t d = fsN(&f, 4);
            if (cond(c, op2 & 0xF)) c->rip += (uint64_t)d;
            goto done;
        }
        if (op2 == 0x05) { // SYSCALL
            if (c->r[X64_RAX] == 60) { c->exit_code = (int)c->r[X64_RDI]; return X64_HALT; }
            goto bad;
        }
        if (op2 == 0xAF) { // IMUL reg, rm
            ModRM m = decode_modrm(&f, rex);
            uint64_t a = op_rd(c, &m, osize, has_rex, &ok); if (!ok || f.err) return X64_ERR_MEM;
            int64_t r = sext(reg_field_rd(c, &m, osize, has_rex), osize) * sext(a, osize);
            bool ovf = r != sext((uint64_t)r, osize);
            set_flag(c, X64_CF, ovf); set_flag(c, X64_OF, ovf);
            reg_field_wr(c, &m, (uint64_t)r, osize, has_rex);
            goto done;
        }
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF || op2 == 0x63) {
            unsigned ssz = (op2 == 0xB7 || op2 == 0xBF) ? 2 : (op2 == 0x63 ? 4 : 1);
            ModRM m = decode_modrm(&f, rex);
            uint64_t a = op_rd(c, &m, ssz, has_rex, &ok); if (!ok || f.err) return X64_ERR_MEM;
            uint64_t v = (op2 == 0xBE || op2 == 0xBF || op2 == 0x63) ? (uint64_t)sext(a, ssz) : a;
            reg_field_wr(c, &m, v, osize, has_rex);
            goto done;
        }
        if (op2 >= 0x90 && op2 <= 0x9F) { // SETcc
            ModRM m = decode_modrm(&f, rex);
            op_wr(c, &m, cond(c, op2 & 0xF) ? 1 : 0, 1, has_rex, &ok);
            if (!ok || f.err) return X64_ERR_MEM;
            goto done;
        }
        if (op2 >= 0x40 && op2 <= 0x4F) { // CMOVcc
            ModRM m = decode_modrm(&f, rex);
            if (cond(c, op2 & 0xF)) {
                uint64_t a = op_rd(c, &m, osize, has_rex, &ok); if (!ok || f.err) return X64_ERR_MEM;
                reg_field_wr(c, &m, a, osize, has_rex);
            }
            goto done;
        }
        if (op2 == 0x1F) { // NOP rm (hint)
            ModRM m = decode_modrm(&f, rex); (void)m;
            if (f.err) return X64_ERR_MEM;
            goto done;
        }
        c->bad_opcode = op2; c->bad_rip = start_rip;
        return X64_ERR_OPCODE;
    }

bad:
    c->bad_opcode = op; c->bad_rip = start_rip;
    return X64_ERR_OPCODE;
done:
    if (f.err) return X64_ERR_MEM;
    c->steps++;
    return X64_OK;
}

X64Status x64_run(X64Cpu *c, uint64_t max_steps) {
    uint64_t n = 0;
    for (;;) {
        X64Status s = x64_step(c);
        if (s != X64_OK) return s;
        if (++n >= max_steps) return X64_ERR_STEPS;
    }
}

// =====================================================================
// v0.3 — Motor rapido: cache de bloques decodificados.
// La primera vez que se ejecuta una direccion, el bloque (hasta la
// siguiente instruccion de control de flujo, max. 64) se decodifica a
// registros internos; despues se ejecutan los registros sin volver a
// decodificar bytes. Las formas raras caen al interprete clasico
// (registro K_STEP, siempre al final de su bloque). Sin soporte de
// codigo automodificable: si el programa se reescribe, hay que vaciar
// la cache con x64_cache_clear().
// =====================================================================
#include <stdlib.h>

enum {
    K_NOP, K_MOV_RM_R, K_MOV_R_RM, K_MOV_RM_I, K_MOV_R_I, K_MOV_R8_I, K_LEA,
    K_ALU_RM_R, K_ALU_R_RM, K_ALU_RM_I, K_ALU_A_I,
    K_TEST_RM_R, K_XCHG_RM_R, K_XCHG_A_R,
    K_INC_RM, K_DEC_RM, K_CALL_RM, K_JMP_RM, K_PUSH_RM,
    K_PUSH_R, K_POP_R, K_PUSH_I,
    K_JMP, K_JCC, K_CALL, K_RET, K_RETI,
    K_SHIFT_I, K_SHIFT_1, K_SHIFT_CL,
    K_IMUL_R_RM, K_IMUL_R_RM_I, K_MOVZX, K_MOVSX, K_SETCC,
    K_CBW, K_CQO,
    K_HALT, K_SYSCALL, K_STEP, K_BAD
};

typedef struct {
    uint8_t kind, osize, reg, which; // which: op ALU / op shift / cc / subop
    uint8_t is_mem, rm_reg;
    uint8_t base_reg, idx_reg, scale; // 255 = ninguno; base 254 = EA absoluta en disp
    uint8_t has_rex, ssize;           // ssize: tam. fuente (MOVZX/SX) o flag op16 (CBW/CQO)
    uint8_t regform;                  // v0.4: 1 si el operando r/m es un registro (mod==3)
    int64_t disp;
    uint64_t imm;
    uint64_t target;
    uint64_t rip, rip_next;
} Dec;

typedef struct {
    int reg;
    uint8_t is_mem, rm_reg, base_reg, idx_reg, scale;
    int64_t disp;
} Form;

// Como decode_modrm pero SIN calcular la direccion: guarda la forma.
// RIP-relativo: base_reg=254 y disp=disp32 (se parchea al final de la
// instruccion, cuando rip_next ya se conoce: mas correcto que el clasico).
static Form decode_form(Fetch *f, int rex) {
    X64Cpu *c = f->c;
    Form fm; memset(&fm, 0, sizeof(fm));
    uint8_t b = f8(f);
    int mod = b >> 6, reg = (b >> 3) & 7, rm = b & 7;
    fm.reg = reg | ((rex & 4) ? 8 : 0);
    fm.rm_reg = rm | ((rex & 1) ? 8 : 0);
    fm.base_reg = 255; fm.idx_reg = 255; fm.scale = 1; fm.is_mem = mod != 3;
    if (mod == 3) return fm;
    if (rm == 4) { // SIB
        uint8_t sib = f8(f);
        fm.scale = 1u << (sib >> 6);
        if (((sib >> 3) & 7) != 4) fm.idx_reg = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0);
        if ((sib & 7) == 5 && mod == 0) { fm.disp = fsN(f, 4); return fm; }
        fm.base_reg = (sib & 7) | ((rex & 1) ? 8 : 0);
    } else if (rm == 5 && mod == 0) { // RIP + disp32
        fm.base_reg = 254;
        fm.disp = fsN(f, 4);
        return fm;
    } else {
        fm.base_reg = fm.rm_reg;
    }
    if (mod == 1) fm.disp = fsN(f, 1);
    else if (mod == 2) fm.disp = fsN(f, 4);
    return fm;
}

static void dec_fill_form(Dec *d, Form *fm) {
    d->reg = (uint8_t)fm->reg;
    d->is_mem = fm->is_mem; d->rm_reg = fm->rm_reg;
    d->regform = fm->is_mem ? 0 : 1;
    d->base_reg = fm->base_reg; d->idx_reg = fm->idx_reg;
    d->scale = fm->scale; d->disp = fm->disp;
}

// Decodifica UNA instruccion en un registro. Devuelve false si el fetch
// sale de la memoria. Lo no soportado -> K_BAD; formas raras -> K_STEP.
static bool decode_one(X64Cpu *c, Dec *d) {
    Fetch f = { c, false };
    memset(d, 0, sizeof(*d));
    d->rip = c->rip;
    d->base_reg = 255; d->idx_reg = 255; d->scale = 1;
    bool op16 = false;
    int rex = 0;
    uint8_t op;
    for (;;) {
        op = f8(&f);
        if (f.err) return false;
        if (op == 0x66) { op16 = true; continue; }
        if (op == 0x67) continue;
        if (op == 0xF2 || op == 0xF3) continue;
        if (op >= 0x40 && op <= 0x4F) { rex = op; continue; }
        if (op == 0x2E || op == 0x3E || op == 0x26 || op == 0x64 || op == 0x65) continue;
        break;
    }
    bool has_rex = rex != 0;
    unsigned osize = (rex & 8) ? 8 : (op16 ? 2 : 4);
    d->osize = (uint8_t)osize; d->has_rex = has_rex;
    Form fm; bool have_form = false;

#define FORM() do { fm = decode_form(&f, rex); have_form = true; dec_fill_form(d, &fm); if (f.err) return false; } while (0)
#define FINISH() do { d->rip_next = c->rip; if (have_form && d->base_reg == 254) d->disp = (int64_t)(d->rip_next + (uint64_t)d->disp); return true; } while (0)
#define BAD() do { d->kind = K_BAD; d->imm = op; FINISH(); } while (0)
#define STEP() do { d->kind = K_STEP; FINISH(); } while (0)

    if (op >= 0x50 && op <= 0x57) { d->kind = K_PUSH_R; d->reg = (op - 0x50) | ((rex & 1) ? 8 : 0); FINISH(); }
    if (op >= 0x58 && op <= 0x5F) { d->kind = K_POP_R; d->reg = (op - 0x58) | ((rex & 1) ? 8 : 0); FINISH(); }
    if (op >= 0x91 && op <= 0x97) { d->kind = K_XCHG_A_R; d->reg = (op - 0x90) | ((rex & 1) ? 8 : 0); FINISH(); }
    if (op >= 0xB0 && op <= 0xB7) { d->kind = K_MOV_R8_I; d->reg = (op - 0xB0) | ((rex & 1) ? 8 : 0); d->imm = f8(&f); if (f.err) return false; FINISH(); }
    if (op >= 0xB8 && op <= 0xBF) {
        d->kind = K_MOV_R_I; d->reg = (op - 0xB8) | ((rex & 1) ? 8 : 0);
        uint64_t v = (rex & 8) ? fN(&f, 8) : fN(&f, 4);
        if (f.err) return false;
        d->imm = (rex & 8) ? v : (v & 0xFFFFFFFFull);
        FINISH();
    }
    if (op >= 0x70 && op <= 0x7F) {
        int64_t disp = fsN(&f, 1); if (f.err) return false;
        d->kind = K_JCC; d->which = op & 0xF; d->rip_next = c->rip;
        d->target = c->rip + (uint64_t)disp;
        return true;
    }

    switch (op) {
        case 0x90: d->kind = K_NOP; FINISH();
        case 0xF4: d->kind = K_HALT; FINISH();
        case 0x98: d->kind = K_CBW; d->ssize = op16 ? 1 : 0; FINISH();
        case 0x99: d->kind = K_CQO; d->ssize = op16 ? 1 : 0; FINISH();
        case 0x68: d->kind = K_PUSH_I; d->imm = (uint64_t)fsN(&f, 4); if (f.err) return false; FINISH();
        case 0x6A: d->kind = K_PUSH_I; d->imm = (uint64_t)fsN(&f, 1); if (f.err) return false; FINISH();
        case 0xC3: d->kind = K_RET; FINISH();
        case 0xC2: d->kind = K_RETI; d->imm = fN(&f, 2); if (f.err) return false; FINISH();
        case 0xE8: { int64_t disp = fsN(&f, 4); if (f.err) return false;
                     d->kind = K_CALL; d->rip_next = c->rip; d->target = c->rip + (uint64_t)disp; return true; }
        case 0xE9: { int64_t disp = fsN(&f, 4); if (f.err) return false;
                     d->kind = K_JMP; d->rip_next = c->rip; d->target = c->rip + (uint64_t)disp; return true; }
        case 0xEB: { int64_t disp = fsN(&f, 1); if (f.err) return false;
                     d->kind = K_JMP; d->rip_next = c->rip; d->target = c->rip + (uint64_t)disp; return true; }
        case 0xFF: case 0xFE: {
            FORM();
            int sub = d->reg & 7;
            d->osize = op == 0xFE ? 1 : (uint8_t)osize;
            if (sub == 0) { d->kind = K_INC_RM; FINISH(); }
            if (sub == 1) { d->kind = K_DEC_RM; FINISH(); }
            if (sub == 2) { d->kind = K_CALL_RM; FINISH(); }
            if (sub == 4) { d->kind = K_JMP_RM; FINISH(); }
            if (sub == 6) { d->kind = K_PUSH_RM; FINISH(); }
            BAD();
        }
        default: break;
    }

    // ALU con ModRM: 00-3D
    if (op <= 0x3D && ((op & 7) <= 5)) {
        int which = (op >> 3) & 7, form = op & 7;
        d->which = (uint8_t)which;
        if (form <= 3) {
            d->osize = (form & 1) ? (uint8_t)osize : 1;
            FORM();
            d->kind = (form <= 1) ? K_ALU_RM_R : K_ALU_R_RM;
            FINISH();
        }
        // acumulador con inmediato
        d->osize = (form & 1) ? (uint8_t)osize : 1;
        uint64_t imm;
        if (form & 1) imm = osize == 8 ? (uint64_t)fsN(&f, 4) : fN(&f, osize == 2 ? 2 : 4);
        else imm = fN(&f, 1);
        if (f.err) return false;
        d->kind = K_ALU_A_I; d->imm = imm;
        FINISH();
    }
    if (op == 0x84 || op == 0x85 || op == 0x86 || op == 0x87) {
        d->osize = (op & 1) ? (uint8_t)osize : 1;
        FORM();
        d->kind = op >= 0x86 ? K_XCHG_RM_R : K_TEST_RM_R;
        FINISH();
    }
    if (op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B || op == 0x8D) {
        d->osize = (op & 1) ? (uint8_t)osize : 1;
        FORM();
        if (op == 0x8D) { d->kind = K_LEA; d->osize = (uint8_t)osize; FINISH(); }
        d->kind = op <= 0x89 ? K_MOV_RM_R : K_MOV_R_RM;
        FINISH();
    }
    if (op == 0x80 || op == 0x81 || op == 0x83) {
        d->osize = op == 0x80 ? 1 : (uint8_t)osize;
        FORM();
        uint64_t imm;
        if (op == 0x81) imm = osize == 8 ? (uint64_t)fsN(&f, 4) : fN(&f, osize == 2 ? 2 : 4);
        else if (op == 0x83) imm = (uint64_t)fsN(&f, 1);
        else imm = fN(&f, 1);
        if (f.err) return false;
        d->kind = K_ALU_RM_I; d->which = d->reg & 7; d->imm = imm;
        FINISH();
    }
    if (op == 0xC6 || op == 0xC7) {
        d->osize = op == 0xC6 ? 1 : (uint8_t)osize;
        FORM();
        if ((d->reg & 7) != 0) BAD();
        uint64_t imm = op == 0xC6 ? fN(&f, 1) : (osize == 8 ? (uint64_t)fsN(&f, 4) : fN(&f, osize == 2 ? 2 : 4));
        if (f.err) return false;
        d->kind = K_MOV_RM_I; d->imm = imm;
        FINISH();
    }
    if (op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3)) {
        d->osize = (op == 0xC0 || op == 0xD0 || op == 0xD2) ? 1 : (uint8_t)osize;
        FORM();
        d->which = d->reg & 7;
        if (d->which == 2 || d->which == 3 || d->which == 6) BAD(); // RCL/RCR/SHLD: no soportados (como el clasico)
        if (op == 0xC0 || op == 0xC1) { d->kind = K_SHIFT_I; d->imm = f8(&f); if (f.err) return false; }
        else if (op == 0xD0 || op == 0xD1) d->kind = K_SHIFT_1;
        else d->kind = K_SHIFT_CL;
        FINISH();
    }
    if (op == 0xF6 || op == 0xF7) { // grupo 3: fallback al clasico
        FORM();
        int sub = d->reg & 7;
        if (sub == 0 || sub == 1) { // TEST con inmediato: hay que saltarlo
            if (op == 0xF6) fN(&f, 1); else fN(&f, osize == 2 ? 2 : 4);
            if (f.err) return false;
        }
        STEP();
    }
    if (op == 0x69 || op == 0x6B) {
        FORM();
        uint64_t imm = op == 0x69 ? (uint64_t)fsN(&f, osize == 2 ? 2 : 4) : (uint64_t)fsN(&f, 1);
        if (f.err) return false;
        d->kind = K_IMUL_R_RM_I; d->imm = imm;
        FINISH();
    }
    if (op == 0x0F) {
        uint8_t op2 = f8(&f); if (f.err) return false;
        if (op2 >= 0x80 && op2 <= 0x8F) {
            int64_t disp = fsN(&f, 4); if (f.err) return false;
            d->kind = K_JCC; d->which = op2 & 0xF; d->rip_next = c->rip;
            d->target = c->rip + (uint64_t)disp;
            return true;
        }
        if (op2 == 0x05) { d->kind = K_SYSCALL; FINISH(); }
        if (op2 == 0xAF) { FORM(); d->kind = K_IMUL_R_RM; FINISH(); }
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF || op2 == 0x63) {
            d->ssize = (op2 == 0xB7 || op2 == 0xBF) ? 2 : (op2 == 0x63 ? 4 : 1);
            FORM();
            d->kind = (op2 == 0xBE || op2 == 0xBF || op2 == 0x63) ? K_MOVSX : K_MOVZX;
            FINISH();
        }
        if (op2 >= 0x90 && op2 <= 0x9F) { FORM(); d->osize = 1; d->kind = K_SETCC; d->which = op2 & 0xF; FINISH(); }
        if (op2 >= 0x40 && op2 <= 0x4F) { FORM(); STEP(); } // CMOV: fallback
        if (op2 == 0x1F) { FORM(); d->kind = K_NOP; FINISH(); }
        d->kind = K_BAD; d->imm = op2; FINISH();
    }
    BAD();
#undef FORM
#undef FINISH
#undef BAD
#undef STEP
}

// ---------- acceso a operandos decodificados ----------
static uint64_t dec_ea(X64Cpu *c, Dec *d) {
    if (d->base_reg == 254) return (uint64_t)d->disp; // absoluta (RIP-rel resuelta)
    uint64_t a = (uint64_t)d->disp;
    if (d->base_reg != 255) a += c->r[d->base_reg];
    if (d->idx_reg != 255) a += c->r[d->idx_reg] * d->scale;
    return a;
}
static uint64_t dec_rd(X64Cpu *c, Dec *d, unsigned sz, bool *ok) {
    if (d->is_mem) return mem_rd(c, dec_ea(c, d), sz, ok);
    if (sz == 1 && !d->has_rex && d->rm_reg >= 4 && d->rm_reg <= 7)
        return (c->r[d->rm_reg - 4] >> 8) & 0xFF;
    return reg_rd(c, d->rm_reg, sz);
}
static void dec_wr(X64Cpu *c, Dec *d, uint64_t v, unsigned sz, bool *ok) {
    if (d->is_mem) { mem_wr(c, dec_ea(c, d), v, sz, ok); return; }
    if (sz == 1 && !d->has_rex && d->rm_reg >= 4 && d->rm_reg <= 7) {
        reg_wr(c, d->rm_reg - 4, v, 1, true); return;
    }
    reg_wr(c, d->rm_reg, v, sz, false);
}
static uint64_t dec_regrd(X64Cpu *c, Dec *d, unsigned sz) {
    if (sz == 1 && !d->has_rex && d->reg >= 4 && d->reg <= 7)
        return (c->r[d->reg - 4] >> 8) & 0xFF;
    return reg_rd(c, d->reg, sz);
}
static void dec_regwr(X64Cpu *c, Dec *d, uint64_t v, unsigned sz) {
    if (sz == 1 && !d->has_rex && d->reg >= 4 && d->reg <= 7) {
        reg_wr(c, d->reg - 4, v, 1, true); return;
    }
    reg_wr(c, d->reg, v, sz, false);
}

// ---------- cache de bloques ----------
#define BLOCK_MAX 64
#define CACHE_SLOTS 16384
typedef struct Block {
    uint64_t start;
    int n;
    Dec ins[BLOCK_MAX];
} Block;
// Cache directa (una entrada por ranura, la colision expulsa): una sola
// sonda de array por bloque, sin listas ni persecucion de punteros.
typedef struct { Block *slots[CACHE_SLOTS]; } Cache;

void x64_enable_cache(X64Cpu *cpu) {
    if (!cpu->cache) cpu->cache = calloc(1, sizeof(Cache));
}
void x64_cache_clear(X64Cpu *cpu) {
    if (!cpu->cache) return;
    Cache *ch = (Cache *)cpu->cache;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        free(ch->slots[i]);
        ch->slots[i] = NULL;
    }
}
static Block *cache_find(Cache *ch, uint64_t start) {
    Block *b = ch->slots[(start >> 3) & (CACHE_SLOTS - 1)];
    return (b && b->start == start) ? b : NULL;
}
static bool block_is_end(uint8_t kind) {
    return kind == K_JMP || kind == K_JCC || kind == K_CALL || kind == K_RET ||
           kind == K_RETI || kind == K_CALL_RM || kind == K_JMP_RM ||
           kind == K_HALT || kind == K_SYSCALL || kind == K_STEP || kind == K_BAD;
}
static Block *cache_decode(X64Cpu *c, Cache *ch, uint64_t start) {
    Block *b = (Block *)calloc(1, sizeof(Block));
    if (!b) return NULL;
    b->start = start;
    c->rip = start;
    while (b->n < BLOCK_MAX) {
        Dec *d = &b->ins[b->n];
        if (!decode_one(c, d)) { // fetch fuera de memoria
            d->kind = K_BAD; d->imm = 0; d->rip = c->rip; d->rip_next = c->rip;
            b->n++;
            break;
        }
        b->n++;
        if (block_is_end(d->kind)) break;
    }
    unsigned h = (start >> 3) & (CACHE_SLOTS - 1);
    free(ch->slots[h]); // expulsa al anterior si lo habia
    ch->slots[h] = b;
    return b;
}

// v0.4: via rapida registro-registro. Solo si el operando es un registro y
// no hay registros de byte alto (AH/BH/CH/DH) de por medio: en ese caso
// reg_rd/reg_wr directos son equivalentes a la via generica.
static bool lean_ok(Dec *d, unsigned sz) {
    if (!d->regform) return false;
    if (sz == 1 && !d->has_rex &&
        ((d->rm_reg >= 4 && d->rm_reg <= 7) || (d->reg >= 4 && d->reg <= 7)))
        return false;
    return true;
}

// ---------- ejecutor de bloques ----------
// Devuelve el estado; c->rip queda en la siguiente direccion a ejecutar.
static X64Status exec_block(X64Cpu *c, Block *b) {
    bool ok = true;
    for (int i = 0; i < b->n; i++) {
        Dec *d = &b->ins[i];
        unsigned sz = d->osize;
        switch (d->kind) {
            case K_NOP: break;
            case K_HALT: c->rip = d->rip_next; return X64_HALT;
            case K_BAD: c->bad_opcode = (uint8_t)d->imm; c->bad_rip = d->rip; c->rip = d->rip; return X64_ERR_OPCODE;
            case K_STEP: {
                c->rip = d->rip;
                X64Status s = x64_step(c); // cuenta su propio paso
                return s == X64_OK ? X64_OK : s;
            }
            case K_SYSCALL:
                if (c->r[X64_RAX] == 60) { c->exit_code = (int)c->r[X64_RDI]; c->rip = d->rip_next; return X64_HALT; }
                c->bad_opcode = 0x05; c->bad_rip = d->rip; c->rip = d->rip; return X64_ERR_OPCODE;
            case K_MOV_RM_R:
                if (lean_ok(d, sz)) reg_wr(c, d->rm_reg, reg_rd(c, d->reg, sz), sz, false);
                else dec_wr(c, d, dec_regrd(c, d, sz), sz, &ok);
                break;
            case K_MOV_R_RM:
                if (lean_ok(d, sz)) reg_wr(c, d->reg, reg_rd(c, d->rm_reg, sz), sz, false);
                else { uint64_t v = dec_rd(c, d, sz, &ok); if (ok) dec_regwr(c, d, v, sz); }
                break;
            case K_MOV_RM_I: dec_wr(c, d, d->imm, sz, &ok); break;
            case K_MOV_R_I: c->r[d->reg] = d->imm; break;
            case K_MOV_R8_I:
                if (d->has_rex || d->reg < 4) reg_wr(c, d->reg, d->imm, 1, false);
                else reg_wr(c, d->reg - 4, d->imm, 1, true);
                break;
            case K_LEA: dec_regwr(c, d, dec_ea(c, d), sz); break;
            case K_ALU_RM_R: {
                if (lean_ok(d, sz)) {
                    uint64_t r = alu(c, d->which, reg_rd(c, d->rm_reg, sz), reg_rd(c, d->reg, sz), sz, true);
                    if (d->which != 7) reg_wr(c, d->rm_reg, r, sz, false);
                    break;
                }
                uint64_t r = alu(c, d->which, dec_rd(c, d, sz, &ok), dec_regrd(c, d, sz), sz, true);
                if (ok && d->which != 7) dec_wr(c, d, r, sz, &ok);
                break; }
            case K_ALU_R_RM: {
                if (lean_ok(d, sz)) {
                    uint64_t r = alu(c, d->which, reg_rd(c, d->reg, sz), reg_rd(c, d->rm_reg, sz), sz, true);
                    if (d->which != 7) reg_wr(c, d->reg, r, sz, false);
                    break;
                }
                uint64_t r = alu(c, d->which, dec_regrd(c, d, sz), dec_rd(c, d, sz, &ok), sz, true);
                if (ok && d->which != 7) dec_regwr(c, d, r, sz);
                break; }
            case K_ALU_RM_I: {
                if (lean_ok(d, sz)) {
                    uint64_t r = alu(c, d->which, reg_rd(c, d->rm_reg, sz), d->imm, sz, true);
                    if (d->which != 7) reg_wr(c, d->rm_reg, r, sz, false);
                    break;
                }
                uint64_t r = alu(c, d->which, dec_rd(c, d, sz, &ok), d->imm, sz, true);
                if (ok && d->which != 7) dec_wr(c, d, r, sz, &ok);
                break; }
            case K_ALU_A_I: {
                uint64_t r = alu(c, d->which, reg_rd(c, 0, sz), d->imm, sz, true);
                if (d->which != 7) reg_wr(c, 0, r, sz, false);
                break; }
            case K_TEST_RM_R: alu(c, 4, dec_rd(c, d, sz, &ok), dec_regrd(c, d, sz), sz, true); break;
            case K_XCHG_RM_R: {
                uint64_t a = dec_rd(c, d, sz, &ok); if (!ok) break;
                uint64_t g = dec_regrd(c, d, sz);
                dec_wr(c, d, g, sz, &ok); if (ok) dec_regwr(c, d, a, sz);
                break; }
            case K_XCHG_A_R: {
                uint64_t t = reg_rd(c, d->reg, sz);
                reg_wr(c, d->reg, reg_rd(c, 0, sz), sz, false);
                reg_wr(c, 0, t, sz, false);
                break; }
            case K_INC_RM: case K_DEC_RM: {
                uint64_t a = dec_rd(c, d, sz, &ok); if (!ok) break;
                uint64_t cf = c->flags & X64_CF;
                uint64_t r = alu(c, d->kind == K_INC_RM ? 0 : 5, a, 1, sz, true);
                set_flag(c, X64_CF, cf != 0);
                dec_wr(c, d, r, sz, &ok);
                break; }
            case K_PUSH_R: if (!push(c, c->r[d->reg])) return X64_ERR_MEM; break;
            case K_POP_R: { uint64_t v = pop(c, &ok); if (!ok) return X64_ERR_MEM; c->r[d->reg] = v; } break;
            case K_PUSH_I: if (!push(c, d->imm)) return X64_ERR_MEM; break;
            case K_PUSH_RM: {
                uint64_t v = lean_ok(d, sz) ? reg_rd(c, d->rm_reg, sz) : dec_rd(c, d, sz, &ok);
                if (!ok || !push(c, v)) return X64_ERR_MEM;
                } break;
            case K_CALL_RM: {
                uint64_t t = dec_rd(c, d, sz, &ok); if (!ok) return X64_ERR_MEM;
                if (!push(c, d->rip_next)) return X64_ERR_MEM;
                c->rip = t; c->steps++; return X64_OK; }
            case K_JMP_RM: c->rip = dec_rd(c, d, sz, &ok); if (!ok) return X64_ERR_MEM; c->steps++; return X64_OK;
            case K_JMP: c->rip = d->target; c->steps++; return X64_OK;
            case K_JCC: c->rip = cond(c, d->which) ? d->target : d->rip_next; c->steps++; return X64_OK;
            case K_CALL:
                if (!push(c, d->rip_next)) return X64_ERR_MEM;
                c->rip = d->target; c->steps++; return X64_OK;
            case K_RET: { uint64_t v = pop(c, &ok); if (!ok) return X64_ERR_MEM; c->rip = v; c->steps++; return X64_OK; }
            case K_RETI: { uint64_t v = pop(c, &ok); if (!ok) return X64_ERR_MEM; c->rip = v; c->r[X64_RSP] += d->imm; c->steps++; return X64_OK; }
            case K_SHIFT_I: case K_SHIFT_1: case K_SHIFT_CL: {
                unsigned cnt = d->kind == K_SHIFT_I ? (unsigned)d->imm : d->kind == K_SHIFT_1 ? 1 : (unsigned)(c->r[X64_RCX] & 0xFF);
                cnt &= (sz == 8 ? 63 : 31);
                uint64_t a = dec_rd(c, d, sz, &ok); if (!ok) break;
                if (cnt == 0) break;
                uint64_t mask = sz == 8 ? ~0ull : ((1ull << (sz * 8)) - 1);
                uint64_t sign = 1ull << (sz * 8 - 1);
                uint64_t res = 0;
                switch (d->which) {
                    case 0: res = ((a << cnt) | (a >> (sz * 8 - cnt))) & mask;
                        set_flag(c, X64_CF, res & 1); if (cnt == 1) set_flag(c, X64_OF, ((res & sign) != 0) != ((c->flags & X64_CF) != 0));
                        break;
                    case 1: res = ((a >> cnt) | (a << (sz * 8 - cnt))) & mask;
                        set_flag(c, X64_CF, (res & sign) != 0); if (cnt == 1) set_flag(c, X64_OF, ((res & sign) != 0) != ((res & (sign >> 1)) != 0));
                        break;
                    case 4: res = (a << cnt) & mask;
                        set_flag(c, X64_CF, (a >> (sz * 8 - cnt)) & 1);
                        if (cnt == 1) set_flag(c, X64_OF, ((res & sign) != 0) != ((c->flags & X64_CF) != 0));
                        flags_sz_p(c, res, sz);
                        break;
                    case 5: res = (a & mask) >> cnt;
                        set_flag(c, X64_CF, (a >> (cnt - 1)) & 1);
                        if (cnt == 1) set_flag(c, X64_OF, (a & sign) != 0);
                        flags_sz_p(c, res, sz);
                        break;
                    case 7: res = (uint64_t)(sext(a, sz) >> cnt) & mask;
                        set_flag(c, X64_CF, (sext(a, sz) >> (cnt - 1)) & 1);
                        if (cnt == 1) set_flag(c, X64_OF, false);
                        flags_sz_p(c, res, sz);
                        break;
                    default: c->bad_opcode = 0; c->bad_rip = d->rip; return X64_ERR_OPCODE;
                }
                dec_wr(c, d, res, sz, &ok);
                break; }
            case K_IMUL_R_RM: {
                uint64_t a = dec_rd(c, d, sz, &ok); if (!ok) break;
                int64_t r = sext(dec_regrd(c, d, sz), sz) * sext(a, sz);
                bool ovf = r != sext((uint64_t)r, sz);
                set_flag(c, X64_CF, ovf); set_flag(c, X64_OF, ovf);
                dec_regwr(c, d, (uint64_t)r, sz);
                break; }
            case K_IMUL_R_RM_I: {
                uint64_t a = dec_rd(c, d, sz, &ok); if (!ok) break;
                int64_t r = sext(a, sz) * (int64_t)d->imm;
                bool ovf = r != sext((uint64_t)r, sz);
                set_flag(c, X64_CF, ovf); set_flag(c, X64_OF, ovf);
                dec_regwr(c, d, (uint64_t)r, sz);
                break; }
            case K_MOVZX: { uint64_t a = dec_rd(c, d, d->ssize, &ok); if (ok) dec_regwr(c, d, a, sz); } break;
            case K_MOVSX: { uint64_t a = dec_rd(c, d, d->ssize, &ok); if (ok) dec_regwr(c, d, (uint64_t)sext(a, d->ssize), sz); } break;
            case K_SETCC: dec_wr(c, d, cond(c, d->which) ? 1 : 0, 1, &ok); break;
            case K_CBW:
                if (sz == 8) c->r[0] = (uint64_t)sext(c->r[0] & 0xFFFFFFFFull, 4);
                else if (d->ssize) reg_wr(c, 0, (uint64_t)sext(c->r[0] & 0xFF, 1), 2, false);
                else c->r[0] = (c->r[0] & ~0xFFFFFFFFull) | (uint32_t)sext(c->r[0] & 0xFFFF, 2);
                break;
            case K_CQO:
                if (sz == 8) c->r[2] = ((int64_t)c->r[0] < 0) ? ~0ull : 0;
                else if (d->ssize) reg_wr(c, 2, ((int16_t)c->r[0] < 0) ? 0xFFFF : 0, 2, false);
                else c->r[2] = (c->r[2] & ~0xFFFFFFFFull) | (((int32_t)c->r[0] < 0) ? 0xFFFFFFFFu : 0);
                break;
            default:
                c->bad_opcode = 0; c->bad_rip = d->rip; return X64_ERR_OPCODE;
        }
        if (!ok) return X64_ERR_MEM;
        c->steps++;
    }
    c->rip = b->ins[b->n - 1].rip_next;
    return X64_OK;
}

X64Status x64_run_cached(X64Cpu *c, uint64_t max_steps) {
    if (!c->cache) x64_enable_cache(c);
    Cache *ch = (Cache *)c->cache;
    for (;;) {
        Block *b = cache_find(ch, c->rip);
        if (!b) {
            b = cache_decode(c, ch, c->rip);
            if (!b) return X64_ERR_MEM;
        }
        X64Status s = exec_block(c, b);
        if (s != X64_OK) return s;
        if (c->steps >= max_steps) return X64_ERR_STEPS;
    }
}
