// Non-AVX2 / Non-BMI2 / Non-MOVBE Hardware Exception Emulator for AMD A10-7800 (Steamroller / bdver3)
// Catches EXCEPTION_ILLEGAL_INSTRUCTION (0xC000001D) and emulates:
//   1. Legacy 0F 38 F0 / 0F 38 F1 MOVBE (16/32/64-bit load & store byte-swap)
//   2. BMI2 instructions (RORX, SHLX, SARX, SHRX, BZHI, PEXT, PDEP, MULX)
//   3. AVX2 instructions (both L=0 128-bit XMM and L=1 256-bit YMM integer/permute/broadcast/variable-shift ops)

#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    uint8_t b[32];
} ymm256_t;

static uint64_t *get_gpr_ptr(PCONTEXT ctx, int reg) {
    switch (reg & 15) {
    case 0: return &ctx->Rax;
    case 1: return &ctx->Rcx;
    case 2: return &ctx->Rdx;
    case 3: return &ctx->Rbx;
    case 4: return &ctx->Rsp;
    case 5: return &ctx->Rbp;
    case 6: return &ctx->Rsi;
    case 7: return &ctx->Rdi;
    case 8: return &ctx->R8;
    case 9: return &ctx->R9;
    case 10: return &ctx->R10;
    case 11: return &ctx->R11;
    case 12: return &ctx->R12;
    case 13: return &ctx->R13;
    case 14: return &ctx->R14;
    default: return &ctx->R15;
    }
}

static uint64_t read_gpr(PCONTEXT ctx, int reg, int W) {
    uint64_t v = *get_gpr_ptr(ctx, reg);
    return W ? v : (uint64_t)(uint32_t)v;
}

static void write_gpr(PCONTEXT ctx, int reg, int W, uint64_t val) {
    uint64_t *p = get_gpr_ptr(ctx, reg);
    *p = W ? val : (uint64_t)(uint32_t)val;
}

static void read_ymm(PCONTEXT ctx, int reg, ymm256_t *out) {
    reg &= 15;
    M128A *xmm = &(&ctx->Xmm0)[reg];
    memcpy(&out->b[0], xmm, 16);
    DWORD len = 0;
    uint8_t *ymm_hi = (uint8_t *)LocateXStateFeature(ctx, XSTATE_AVX, &len);
    if (ymm_hi && len >= (DWORD)((reg + 1) * 16)) {
        memcpy(&out->b[16], ymm_hi + reg * 16, 16);
    } else {
        memset(&out->b[16], 0, 16);
    }
}

static void write_ymm(PCONTEXT ctx, int reg, const ymm256_t *in) {
    reg &= 15;
    M128A *xmm = &(&ctx->Xmm0)[reg];
    memcpy(xmm, &in->b[0], 16);
    DWORD len = 0;
    uint8_t *ymm_hi = (uint8_t *)LocateXStateFeature(ctx, XSTATE_AVX, &len);
    if (ymm_hi && len >= (DWORD)((reg + 1) * 16)) {
        *(uint64_t *)(ymm_hi - 64) |= (1ULL << XSTATE_AVX);
        memcpy(ymm_hi + reg * 16, &in->b[16], 16);
    }
}

static int decode_modrm(PCONTEXT ctx, const uint8_t *ip, int pos, int rex_r, int rex_x, int rex_b,
                        int imm_bytes, int *out_reg, int *out_rm_reg, uint64_t *out_mem_addr, bool *out_is_mem) {
    uint8_t modrm = ip[pos++];
    int mod = (modrm >> 6) & 3;
    int reg = (rex_r << 3) | ((modrm >> 3) & 7);
    int rm_low = modrm & 7;
    *out_reg = reg;

    if (mod == 3) {
        *out_is_mem = false;
        *out_rm_reg = (rex_b << 3) | rm_low;
        *out_mem_addr = 0;
        return pos;
    }

    *out_is_mem = true;
    *out_rm_reg = -1;
    uint64_t addr = 0;

    if (rm_low == 4) {
        uint8_t sib = ip[pos++];
        int scale = 1 << ((sib >> 6) & 3);
        int idx_reg = (rex_x << 3) | ((sib >> 3) & 7);
        int base_low = sib & 7;
        int base_reg = (rex_b << 3) | base_low;

        if (base_low == 5 && mod == 0) {
            int32_t disp32;
            memcpy(&disp32, &ip[pos], 4);
            pos += 4;
            addr = (uint64_t)(int64_t)disp32;
        } else {
            addr = *get_gpr_ptr(ctx, base_reg);
        }
        if (idx_reg != 4) {
            addr += (*get_gpr_ptr(ctx, idx_reg)) * (uint64_t)scale;
        }
    } else if (rm_low == 5 && mod == 0) {
        int32_t disp32;
        memcpy(&disp32, &ip[pos], 4);
        pos += 4;
        uint64_t next_rip = ctx->Rip + (uint64_t)(pos + imm_bytes);
        addr = next_rip + (uint64_t)(int64_t)disp32;
    } else {
        addr = *get_gpr_ptr(ctx, (rex_b << 3) | rm_low);
    }

    if (mod == 1) {
        int8_t disp8 = (int8_t)ip[pos++];
        addr += (uint64_t)(int64_t)disp8;
    } else if (mod == 2) {
        int32_t disp32;
        memcpy(&disp32, &ip[pos], 4);
        pos += 4;
        addr += (uint64_t)(int64_t)disp32;
    }

    *out_mem_addr = addr;
    return pos;
}

// Emulate MOVBE (0F 38 F0 load / 0F 38 F1 store) for 16-bit, 32-bit, and 64-bit operands
static bool emulate_movbe(PCONTEXT ctx, const uint8_t *ip) {
    int pos = 0;
    bool has_66 = false;
    uint8_t rex = 0;

    while (pos < 8) {
        uint8_t b = ip[pos];
        if (b == 0x66) {
            has_66 = true;
            pos++;
        } else if (b == 0x67 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65) {
            pos++;
        } else if (b >= 0x40 && b <= 0x4F) {
            rex = b;
            pos++;
        } else if (b == 0xF2 || b == 0xF3) {
            // F2/F3 0F 38 F0/F1 is CRC32, not MOVBE
            return false;
        } else {
            break;
        }
    }

    if (ip[pos] != 0x0F || ip[pos + 1] != 0x38) return false;
    uint8_t op = ip[pos + 2];
    if (op != 0xF0 && op != 0xF1) return false;

    int rex_w = (rex >> 3) & 1;
    int rex_r = (rex >> 2) & 1;
    int rex_x = (rex >> 1) & 1;
    int rex_b = rex & 1;

    int dst_reg = 0, rm_reg = 0;
    uint64_t mem_addr = 0;
    bool is_mem = false;
    int next_pos = decode_modrm(ctx, ip, pos + 3, rex_r, rex_x, rex_b, 0, &dst_reg, &rm_reg, &mem_addr, &is_mem);
    if (!is_mem) return false;

    if (op == 0xF0) {
        // MOVBE reg, m16/32/64
        if (rex_w) {
            uint64_t v;
            memcpy(&v, (const void *)(uintptr_t)mem_addr, 8);
            write_gpr(ctx, dst_reg, 1, __builtin_bswap64(v));
        } else if (has_66) {
            uint16_t v;
            memcpy(&v, (const void *)(uintptr_t)mem_addr, 2);
            uint64_t *gp = get_gpr_ptr(ctx, dst_reg);
            *gp = (*gp & ~0xFFFFULL) | (uint64_t)__builtin_bswap16(v);
        } else {
            uint32_t v;
            memcpy(&v, (const void *)(uintptr_t)mem_addr, 4);
            write_gpr(ctx, dst_reg, 0, (uint64_t)__builtin_bswap32(v));
        }
    } else {
        // MOVBE m16/32/64, reg
        if (rex_w) {
            uint64_t v = __builtin_bswap64(read_gpr(ctx, dst_reg, 1));
            memcpy((void *)(uintptr_t)mem_addr, &v, 8);
        } else if (has_66) {
            uint16_t v = __builtin_bswap16((uint16_t)(*get_gpr_ptr(ctx, dst_reg)));
            memcpy((void *)(uintptr_t)mem_addr, &v, 2);
        } else {
            uint32_t v = __builtin_bswap32((uint32_t)read_gpr(ctx, dst_reg, 0));
            memcpy((void *)(uintptr_t)mem_addr, &v, 4);
        }
    }

    ctx->Rip += (uint64_t)next_pos;
    return true;
}

static bool emulate_bmi2(PCONTEXT ctx, const uint8_t *ip, int mmmmm, int W, int vvvv, int pp, int rex_r, int rex_x, int rex_b) {
    uint8_t op = ip[3];
    int dst_reg = 0, rm_reg = 0;
    uint64_t mem_addr = 0;
    bool is_mem = false;

    // 1. RORX (0F 3A F0, pp=3)
    if (mmmmm == 3 && pp == 3 && op == 0xF0) {
        int next_pos = decode_modrm(ctx, ip, 4, rex_r, rex_x, rex_b, 1, &dst_reg, &rm_reg, &mem_addr, &is_mem);
        uint8_t imm8 = ip[next_pos++];
        uint64_t src = 0;
        if (is_mem) {
            if (W) memcpy(&src, (const void *)(uintptr_t)mem_addr, 8);
            else { uint32_t tmp; memcpy(&tmp, (const void *)(uintptr_t)mem_addr, 4); src = tmp; }
        } else {
            src = read_gpr(ctx, rm_reg, W);
        }
        if (W) {
            uint32_t cnt = imm8 & 63;
            uint64_t res = cnt ? ((src >> cnt) | (src << (64 - cnt))) : src;
            write_gpr(ctx, dst_reg, 1, res);
        } else {
            uint32_t s32 = (uint32_t)src;
            uint32_t cnt = imm8 & 31;
            uint32_t res = cnt ? ((s32 >> cnt) | (s32 << (32 - cnt))) : s32;
            write_gpr(ctx, dst_reg, 0, res);
        }
        ctx->Rip += (uint64_t)next_pos;
        return true;
    }

    if (mmmmm != 2) return false;

    // 2. SHLX (pp=1), SARX (pp=2), SHRX (pp=3) on opcode 0xF7
    if (op == 0xF7 && (pp == 1 || pp == 2 || pp == 3)) {
        int next_pos = decode_modrm(ctx, ip, 4, rex_r, rex_x, rex_b, 0, &dst_reg, &rm_reg, &mem_addr, &is_mem);
        uint64_t src = 0;
        if (is_mem) {
            if (W) memcpy(&src, (const void *)(uintptr_t)mem_addr, 8);
            else { uint32_t tmp; memcpy(&tmp, (const void *)(uintptr_t)mem_addr, 4); src = tmp; }
        } else {
            src = read_gpr(ctx, rm_reg, W);
        }
        uint64_t cnt = read_gpr(ctx, vvvv, W) & (W ? 63 : 31);
        uint64_t res = 0;
        if (W) {
            if (pp == 1) res = src << cnt;
            else if (pp == 2) res = (uint64_t)((int64_t)src >> cnt);
            else res = src >> cnt;
        } else {
            uint32_t s32 = (uint32_t)src;
            if (pp == 1) res = (uint32_t)(s32 << cnt);
            else if (pp == 2) res = (uint32_t)((int32_t)s32 >> cnt);
            else res = (uint32_t)(s32 >> cnt);
        }
        write_gpr(ctx, dst_reg, W, res);
        ctx->Rip += (uint64_t)next_pos;
        return true;
    }

    // 3. BZHI (pp=0), PEXT (pp=2), PDEP (pp=3) on opcode 0xF5
    if (op == 0xF5 && (pp == 0 || pp == 2 || pp == 3)) {
        int next_pos = decode_modrm(ctx, ip, 4, rex_r, rex_x, rex_b, 0, &dst_reg, &rm_reg, &mem_addr, &is_mem);
        uint64_t rm_val = 0;
        if (is_mem) {
            if (W) memcpy(&rm_val, (const void *)(uintptr_t)mem_addr, 8);
            else { uint32_t tmp; memcpy(&tmp, (const void *)(uintptr_t)mem_addr, 4); rm_val = tmp; }
        } else {
            rm_val = read_gpr(ctx, rm_reg, W);
        }

        if (pp == 0) {
            uint32_t idx = (uint32_t)(read_gpr(ctx, vvvv, W) & 0xFF);
            uint32_t width = W ? 64 : 32;
            uint64_t res = rm_val;
            bool cf = false;
            if (idx < width) {
                uint64_t mask = (idx == 0) ? 0ULL : ((1ULL << idx) - 1ULL);
                res = rm_val & mask;
            } else {
                cf = true;
            }
            if (!W) res = (uint32_t)res;
            write_gpr(ctx, dst_reg, W, res);
            uint32_t eflags = ctx->EFlags & ~((1u << 0) | (1u << 6) | (1u << 7) | (1u << 11));
            if (cf) eflags |= (1u << 0);
            if (res == 0) eflags |= (1u << 6);
            if ((res >> (width - 1)) & 1ULL) eflags |= (1u << 7);
            ctx->EFlags = eflags;
        } else if (pp == 2) {
            uint64_t val = read_gpr(ctx, vvvv, W);
            uint64_t mask = rm_val;
            uint64_t res = 0;
            uint64_t bb = 1;
            while (mask != 0) {
                uint64_t lowest = mask & (~mask + 1ULL);
                if (val & lowest) res |= bb;
                mask &= (mask - 1ULL);
                bb <<= 1;
            }
            write_gpr(ctx, dst_reg, W, res);
        } else {
            uint64_t val = read_gpr(ctx, vvvv, W);
            uint64_t mask = rm_val;
            uint64_t res = 0;
            uint64_t bb = 1;
            while (mask != 0) {
                uint64_t lowest = mask & (~mask + 1ULL);
                if (val & bb) res |= lowest;
                mask &= (mask - 1ULL);
                bb <<= 1;
            }
            write_gpr(ctx, dst_reg, W, res);
        }
        ctx->Rip += (uint64_t)next_pos;
        return true;
    }

    // 4. MULX (pp=3, op=0xF6): mulx hi_dst, lo_vvvv, rm_val (uses RDX/EDX)
    if (op == 0xF6 && pp == 3) {
        int next_pos = decode_modrm(ctx, ip, 4, rex_r, rex_x, rex_b, 0, &dst_reg, &rm_reg, &mem_addr, &is_mem);
        uint64_t src = 0;
        if (is_mem) {
            if (W) memcpy(&src, (const void *)(uintptr_t)mem_addr, 8);
            else { uint32_t tmp; memcpy(&tmp, (const void *)(uintptr_t)mem_addr, 4); src = tmp; }
        } else {
            src = read_gpr(ctx, rm_reg, W);
        }
        if (W) {
            unsigned __int128 a = (unsigned __int128)read_gpr(ctx, 2, 1);
            unsigned __int128 prod = a * (unsigned __int128)src;
            write_gpr(ctx, vvvv, 1, (uint64_t)prod);
            write_gpr(ctx, dst_reg, 1, (uint64_t)(prod >> 64));
        } else {
            uint64_t a = (uint32_t)read_gpr(ctx, 2, 0);
            uint64_t prod = a * (uint64_t)(uint32_t)src;
            write_gpr(ctx, vvvv, 0, (uint32_t)prod);
            write_gpr(ctx, dst_reg, 0, (uint32_t)(prod >> 32));
        }
        ctx->Rip += (uint64_t)next_pos;
        return true;
    }

    return false;
}

static int16_t sat_s16(int32_t x) {
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

static int8_t sat_s8(int32_t x) {
    if (x > 127) return 127;
    if (x < -128) return -128;
    return (int8_t)x;
}

static uint8_t sat_u8(int32_t x) {
    if (x > 255) return 255;
    if (x < 0) return 0;
    return (uint8_t)x;
}

static uint16_t sat_u16(int32_t x) {
    if (x > 65535) return 65535;
    if (x < 0) return 0;
    return (uint16_t)x;
}

// Emulates AVX2 instructions for both L=0 (128-bit XMM, upper 128 bits zeroed) and L=1 (256-bit YMM)
static bool emulate_avx2(PCONTEXT ctx, const uint8_t *ip, int pos_op, int mmmmm, int W, int vvvv, int L, int pp,
                         int rex_r, int rex_x, int rex_b) {
    uint8_t op = ip[pos_op];
    int dst_reg = 0, rm_reg = 0;
    uint64_t mem_addr = 0;
    bool is_mem = false;
    int vbytes = L ? 32 : 16;
    int lanes = L ? 2 : 1;

    int imm_bytes = (mmmmm == 3 || (mmmmm == 1 && (op == 0x70 || op == 0x71 || op == 0x72 || op == 0x73 || op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6))) ? 1 : 0;
    int next_pos = decode_modrm(ctx, ip, pos_op + 1, rex_r, rex_x, rex_b, imm_bytes, &dst_reg, &rm_reg, &mem_addr, &is_mem);

    // Map 1 (0F)
    if (mmmmm == 1) {
        // vmovdqa / vmovdqu load (0x6F, pp=1 or pp=2)
        if (op == 0x6F && (pp == 1 || pp == 2)) {
            ymm256_t res;
            memset(&res, 0, 32);
            if (is_mem) memcpy(&res.b[0], (const void *)(uintptr_t)mem_addr, vbytes);
            else {
                ymm256_t tmp;
                read_ymm(ctx, rm_reg, &tmp);
                memcpy(&res.b[0], &tmp.b[0], vbytes);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
        // vmovdqa / vmovdqu store (0x7F, pp=1 or pp=2)
        if (op == 0x7F && (pp == 1 || pp == 2)) {
            ymm256_t src;
            read_ymm(ctx, dst_reg, &src);
            if (is_mem) {
                memcpy((void *)(uintptr_t)mem_addr, &src.b[0], vbytes);
            } else {
                ymm256_t res;
                memset(&res, 0, 32);
                memcpy(&res.b[0], &src.b[0], vbytes);
                write_ymm(ctx, rm_reg, &res);
            }
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
        // vpshufd (pp=1), vpshufhw (pp=2), vpshuflw (pp=3) on 0x70
        if (op == 0x70 && (pp == 1 || pp == 2 || pp == 3)) {
            uint8_t imm8 = ip[next_pos++];
            ymm256_t src, res;
            memset(&src, 0, 32);
            memset(&res, 0, 32);
            if (is_mem) memcpy(&src.b[0], (const void *)(uintptr_t)mem_addr, vbytes);
            else read_ymm(ctx, rm_reg, &src);
            for (int lane = 0; lane < lanes; lane++) {
                uint8_t *s_lane = &src.b[lane * 16];
                uint8_t *d_lane = &res.b[lane * 16];
                if (pp == 1) {
                    for (int d = 0; d < 4; d++) {
                        int sel = (imm8 >> (d * 2)) & 3;
                        memcpy(&d_lane[d * 4], &s_lane[sel * 4], 4);
                    }
                } else if (pp == 2) {
                    memcpy(&d_lane[0], &s_lane[0], 8);
                    for (int w = 0; w < 4; w++) {
                        int sel = (imm8 >> (w * 2)) & 3;
                        memcpy(&d_lane[8 + w * 2], &s_lane[8 + sel * 2], 2);
                    }
                } else {
                    for (int w = 0; w < 4; w++) {
                        int sel = (imm8 >> (w * 2)) & 3;
                        memcpy(&d_lane[w * 2], &s_lane[sel * 2], 2);
                    }
                    memcpy(&d_lane[8], &s_lane[8], 8);
                }
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
    }

    // Map 1 (0F) with pp=1 (66)
    if (mmmmm == 1 && pp == 1) {
        // Immediate shifts: 0x71 (16-bit), 0x72 (32-bit), 0x73 (64-bit / 128-bit lane byte shift)
        if (op == 0x71 || op == 0x72 || op == 0x73) {
            uint8_t imm8 = ip[next_pos++];
            int subop = dst_reg & 7;
            ymm256_t src, res;
            memset(&src, 0, 32);
            memset(&res, 0, 32);
            if (is_mem) memcpy(&src.b[0], (const void *)(uintptr_t)mem_addr, vbytes);
            else read_ymm(ctx, rm_reg, &src);

            if (op == 0x71) {
                for (int i = 0; i < vbytes / 2; i++) {
                    uint16_t v;
                    memcpy(&v, &src.b[i * 2], 2);
                    if (subop == 6) v = (imm8 < 16) ? (uint16_t)(v << imm8) : 0;
                    else if (subop == 2) v = (imm8 < 16) ? (uint16_t)(v >> imm8) : 0;
                    else if (subop == 4) v = (uint16_t)((imm8 < 16) ? (((int16_t)v) >> imm8) : (((int16_t)v) >> 15));
                    memcpy(&res.b[i * 2], &v, 2);
                }
                write_ymm(ctx, vvvv, &res);
                ctx->Rip += (uint64_t)next_pos;
                return true;
            }
            if (op == 0x72) {
                for (int i = 0; i < vbytes / 4; i++) {
                    uint32_t v;
                    memcpy(&v, &src.b[i * 4], 4);
                    if (subop == 6) v = (imm8 < 32) ? (v << imm8) : 0;
                    else if (subop == 2) v = (imm8 < 32) ? (v >> imm8) : 0;
                    else if (subop == 4) v = (uint32_t)((imm8 < 32) ? (((int32_t)v) >> imm8) : (((int32_t)v) >> 31));
                    memcpy(&res.b[i * 4], &v, 4);
                }
                write_ymm(ctx, vvvv, &res);
                ctx->Rip += (uint64_t)next_pos;
                return true;
            }
            if (op == 0x73) {
                if (subop == 3 || subop == 7) {
                    uint32_t shift = imm8;
                    for (int lane = 0; lane < lanes; lane++) {
                        for (int b = 0; b < 16; b++) {
                            if (subop == 3) res.b[lane * 16 + b] = (b + shift < 16) ? src.b[lane * 16 + b + shift] : 0;
                            else res.b[lane * 16 + b] = (b >= (int)shift) ? src.b[lane * 16 + b - shift] : 0;
                        }
                    }
                } else {
                    for (int i = 0; i < vbytes / 8; i++) {
                        uint64_t v;
                        memcpy(&v, &src.b[i * 8], 8);
                        if (subop == 6) v = (imm8 < 64) ? (v << imm8) : 0;
                        else if (subop == 2) v = (imm8 < 64) ? (v >> imm8) : 0;
                        memcpy(&res.b[i * 8], &v, 8);
                    }
                }
                write_ymm(ctx, vvvv, &res);
                ctx->Rip += (uint64_t)next_pos;
                return true;
            }
        }

        // vpmovmskb r32, ymm (0xD7)
        if (op == 0xD7 && !is_mem) {
            ymm256_t src;
            read_ymm(ctx, rm_reg, &src);
            uint32_t mask = 0;
            for (int i = 0; i < vbytes; i++) {
                if (src.b[i] & 0x80) mask |= (1u << i);
            }
            write_gpr(ctx, dst_reg, 0, mask);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }

        ymm256_t a, b, res;
        memset(&a, 0, 32);
        memset(&b, 0, 32);
        memset(&res, 0, 32);
        read_ymm(ctx, vvvv, &a);
        if (is_mem) {
            // Shift instructions D1..D3, E1..E2, F1..F3 only read 128-bit memory operand
            int mem_sz = (op == 0xD1 || op == 0xD2 || op == 0xD3 || op == 0xE1 || op == 0xE2 || op == 0xF1 || op == 0xF2 || op == 0xF3) ? 16 : vbytes;
            memcpy(&b.b[0], (const void *)(uintptr_t)mem_addr, mem_sz);
        } else {
            read_ymm(ctx, rm_reg, &b);
        }

        switch (op) {
        case 0xDB: // vpand
            for (int i = 0; i < vbytes; i++) res.b[i] = a.b[i] & b.b[i];
            break;
        case 0xDF: // vpandn
            for (int i = 0; i < vbytes; i++) res.b[i] = (~a.b[i]) & b.b[i];
            break;
        case 0xEB: // vpor
            for (int i = 0; i < vbytes; i++) res.b[i] = a.b[i] | b.b[i];
            break;
        case 0xEF: // vpxor
            for (int i = 0; i < vbytes; i++) res.b[i] = a.b[i] ^ b.b[i];
            break;
        case 0xFC: // vpaddb
            for (int i = 0; i < vbytes; i++) res.b[i] = a.b[i] + b.b[i];
            break;
        case 0xFD: // vpaddw
            for (int i = 0; i < vbytes / 2; i++) {
                uint16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = va + vb; memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xFE: // vpaddd
            for (int i = 0; i < vbytes / 4; i++) {
                uint32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4); memcpy(&vb, &b.b[i * 4], 4);
                vr = va + vb; memcpy(&res.b[i * 4], &vr, 4);
            }
            break;
        case 0xD4: // vpaddq
            for (int i = 0; i < vbytes / 8; i++) {
                uint64_t va, vb, vr;
                memcpy(&va, &a.b[i * 8], 8); memcpy(&vb, &b.b[i * 8], 8);
                vr = va + vb; memcpy(&res.b[i * 8], &vr, 8);
            }
            break;
        case 0xF8: // vpsubb
            for (int i = 0; i < vbytes; i++) res.b[i] = a.b[i] - b.b[i];
            break;
        case 0xF9: // vpsubw
            for (int i = 0; i < vbytes / 2; i++) {
                uint16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = va - vb; memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xFA: // vpsubd
            for (int i = 0; i < vbytes / 4; i++) {
                uint32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4); memcpy(&vb, &b.b[i * 4], 4);
                vr = va - vb; memcpy(&res.b[i * 4], &vr, 4);
            }
            break;
        case 0xFB: // vpsubq
            for (int i = 0; i < vbytes / 8; i++) {
                uint64_t va, vb, vr;
                memcpy(&va, &a.b[i * 8], 8); memcpy(&vb, &b.b[i * 8], 8);
                vr = va - vb; memcpy(&res.b[i * 8], &vr, 8);
            }
            break;
        case 0xEC: // vpaddsb
            for (int i = 0; i < vbytes; i++) res.b[i] = (uint8_t)sat_s8((int8_t)a.b[i] + (int8_t)b.b[i]);
            break;
        case 0xED: // vpaddsw
            for (int i = 0; i < vbytes / 2; i++) {
                int16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = sat_s16((int32_t)va + (int32_t)vb); memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xDC: // vpaddusb
            for (int i = 0; i < vbytes; i++) res.b[i] = sat_u8((int32_t)a.b[i] + (int32_t)b.b[i]);
            break;
        case 0xDD: // vpaddusw
            for (int i = 0; i < vbytes / 2; i++) {
                uint16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = sat_u16((int32_t)va + (int32_t)vb); memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xE8: // vpsubsb
            for (int i = 0; i < vbytes; i++) res.b[i] = (uint8_t)sat_s8((int8_t)a.b[i] - (int8_t)b.b[i]);
            break;
        case 0xE9: // vpsubsw
            for (int i = 0; i < vbytes / 2; i++) {
                int16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = sat_s16((int32_t)va - (int32_t)vb); memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xD8: // vpsubusb
            for (int i = 0; i < vbytes; i++) res.b[i] = sat_u8((int32_t)a.b[i] - (int32_t)b.b[i]);
            break;
        case 0xD9: // vpsubusw
            for (int i = 0; i < vbytes / 2; i++) {
                uint16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = sat_u16((int32_t)va - (int32_t)vb); memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xD5: // vpmullw
            for (int i = 0; i < vbytes / 2; i++) {
                int16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = (int16_t)((int32_t)va * (int32_t)vb); memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xE5: // vpmulhw
            for (int i = 0; i < vbytes / 2; i++) {
                int16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = (int16_t)(((int32_t)va * (int32_t)vb) >> 16); memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xE4: // vpmulhuw
            for (int i = 0; i < vbytes / 2; i++) {
                uint16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = (uint16_t)(((uint32_t)va * (uint32_t)vb) >> 16); memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xF4: // vpmuludq
            for (int i = 0; i < vbytes / 8; i++) {
                uint32_t va, vb;
                memcpy(&va, &a.b[i * 8], 4); memcpy(&vb, &b.b[i * 8], 4);
                uint64_t vr = (uint64_t)va * (uint64_t)vb;
                memcpy(&res.b[i * 8], &vr, 8);
            }
            break;
        case 0xF5: // vpmaddwd
            for (int i = 0; i < vbytes / 4; i++) {
                int16_t a0, a1, b0, b1;
                memcpy(&a0, &a.b[i * 4], 2); memcpy(&a1, &a.b[i * 4 + 2], 2);
                memcpy(&b0, &b.b[i * 4], 2); memcpy(&b1, &b.b[i * 4 + 2], 2);
                int32_t vr = (int32_t)a0 * (int32_t)b0 + (int32_t)a1 * (int32_t)b1;
                memcpy(&res.b[i * 4], &vr, 4);
            }
            break;
        case 0x74: // vpcmpeqb
            for (int i = 0; i < vbytes; i++) res.b[i] = (a.b[i] == b.b[i]) ? 0xFF : 0x00;
            break;
        case 0x75: // vpcmpeqw
            for (int i = 0; i < vbytes / 2; i++) {
                uint16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = (va == vb) ? 0xFFFFu : 0u; memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0x76: // vpcmpeqd
            for (int i = 0; i < vbytes / 4; i++) {
                uint32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4); memcpy(&vb, &b.b[i * 4], 4);
                vr = (va == vb) ? 0xFFFFFFFFu : 0u; memcpy(&res.b[i * 4], &vr, 4);
            }
            break;
        case 0x64: // vpcmpgtb
            for (int i = 0; i < vbytes; i++) res.b[i] = ((int8_t)a.b[i] > (int8_t)b.b[i]) ? 0xFF : 0x00;
            break;
        case 0x65: // vpcmpgtw
            for (int i = 0; i < vbytes / 2; i++) {
                int16_t va, vb; uint16_t vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = (va > vb) ? 0xFFFFu : 0u; memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0x66: // vpcmpgtd
            for (int i = 0; i < vbytes / 4; i++) {
                int32_t va, vb; uint32_t vr;
                memcpy(&va, &a.b[i * 4], 4); memcpy(&vb, &b.b[i * 4], 4);
                vr = (va > vb) ? 0xFFFFFFFFu : 0u; memcpy(&res.b[i * 4], &vr, 4);
            }
            break;
        case 0xDA: // vpminub
            for (int i = 0; i < vbytes; i++) res.b[i] = (a.b[i] < b.b[i]) ? a.b[i] : b.b[i];
            break;
        case 0xDE: // vpmaxub
            for (int i = 0; i < vbytes; i++) res.b[i] = (a.b[i] > b.b[i]) ? a.b[i] : b.b[i];
            break;
        case 0xEA: // vpminsw
            for (int i = 0; i < vbytes / 2; i++) {
                int16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = (va < vb) ? va : vb; memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xEE: // vpmaxsw
            for (int i = 0; i < vbytes / 2; i++) {
                int16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = (va > vb) ? va : vb; memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xE0: // vpavgb
            for (int i = 0; i < vbytes; i++) res.b[i] = (uint8_t)(((uint32_t)a.b[i] + (uint32_t)b.b[i] + 1u) >> 1);
            break;
        case 0xE3: // vpavgw
            for (int i = 0; i < vbytes / 2; i++) {
                uint16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = (uint16_t)(((uint32_t)va + (uint32_t)vb + 1u) >> 1); memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0xF6: // vpsadbw
            for (int q = 0; q < vbytes / 8; q++) {
                uint64_t sum = 0;
                for (int j = 0; j < 8; j++) {
                    int diff = (int)a.b[q * 8 + j] - (int)b.b[q * 8 + j];
                    sum += (uint64_t)(diff < 0 ? -diff : diff);
                }
                memcpy(&res.b[q * 8], &sum, 8);
            }
            break;
        case 0x60: // vpunpcklbw
            for (int lane = 0; lane < lanes; lane++) {
                for (int i = 0; i < 8; i++) {
                    res.b[lane * 16 + i * 2] = a.b[lane * 16 + i];
                    res.b[lane * 16 + i * 2 + 1] = b.b[lane * 16 + i];
                }
            }
            break;
        case 0x68: // vpunpckhbw
            for (int lane = 0; lane < lanes; lane++) {
                for (int i = 0; i < 8; i++) {
                    res.b[lane * 16 + i * 2] = a.b[lane * 16 + 8 + i];
                    res.b[lane * 16 + i * 2 + 1] = b.b[lane * 16 + 8 + i];
                }
            }
            break;
        case 0x61: // vpunpcklwd
            for (int lane = 0; lane < lanes; lane++) {
                for (int i = 0; i < 4; i++) {
                    memcpy(&res.b[lane * 16 + i * 4], &a.b[lane * 16 + i * 2], 2);
                    memcpy(&res.b[lane * 16 + i * 4 + 2], &b.b[lane * 16 + i * 2], 2);
                }
            }
            break;
        case 0x69: // vpunpckhwd
            for (int lane = 0; lane < lanes; lane++) {
                for (int i = 0; i < 4; i++) {
                    memcpy(&res.b[lane * 16 + i * 4], &a.b[lane * 16 + 8 + i * 2], 2);
                    memcpy(&res.b[lane * 16 + i * 4 + 2], &b.b[lane * 16 + 8 + i * 2], 2);
                }
            }
            break;
        case 0x62: // vpunpckldq
            for (int lane = 0; lane < lanes; lane++) {
                for (int i = 0; i < 2; i++) {
                    memcpy(&res.b[lane * 16 + i * 8], &a.b[lane * 16 + i * 4], 4);
                    memcpy(&res.b[lane * 16 + i * 8 + 4], &b.b[lane * 16 + i * 4], 4);
                }
            }
            break;
        case 0x6A: // vpunpckhdq
            for (int lane = 0; lane < lanes; lane++) {
                for (int i = 0; i < 2; i++) {
                    memcpy(&res.b[lane * 16 + i * 8], &a.b[lane * 16 + 8 + i * 4], 4);
                    memcpy(&res.b[lane * 16 + i * 8 + 4], &b.b[lane * 16 + 8 + i * 4], 4);
                }
            }
            break;
        case 0x6C: // vpunpcklqdq
            for (int lane = 0; lane < lanes; lane++) {
                memcpy(&res.b[lane * 16], &a.b[lane * 16], 8);
                memcpy(&res.b[lane * 16 + 8], &b.b[lane * 16], 8);
            }
            break;
        case 0x6D: // vpunpckhqdq
            for (int lane = 0; lane < lanes; lane++) {
                memcpy(&res.b[lane * 16], &a.b[lane * 16 + 8], 8);
                memcpy(&res.b[lane * 16 + 8], &b.b[lane * 16 + 8], 8);
            }
            break;
        case 0x63: // vpacksswb
        case 0x67: // vpackuswb
            for (int lane = 0; lane < lanes; lane++) {
                for (int i = 0; i < 8; i++) {
                    int16_t va, vb;
                    memcpy(&va, &a.b[lane * 16 + i * 2], 2);
                    memcpy(&vb, &b.b[lane * 16 + i * 2], 2);
                    res.b[lane * 16 + i] = (op == 0x63) ? (uint8_t)sat_s8(va) : sat_u8(va);
                    res.b[lane * 16 + 8 + i] = (op == 0x63) ? (uint8_t)sat_s8(vb) : sat_u8(vb);
                }
            }
            break;
        case 0x6B: // vpackssdw
            for (int lane = 0; lane < lanes; lane++) {
                for (int i = 0; i < 4; i++) {
                    int32_t va, vb;
                    memcpy(&va, &a.b[lane * 16 + i * 4], 4);
                    memcpy(&vb, &b.b[lane * 16 + i * 4], 4);
                    int16_t ra = sat_s16(va), rb = sat_s16(vb);
                    memcpy(&res.b[lane * 16 + i * 2], &ra, 2);
                    memcpy(&res.b[lane * 16 + 8 + i * 2], &rb, 2);
                }
            }
            break;
        case 0xD1: case 0xE1: case 0xF1: { // vpsrlw / vpsraw / vpsllw
            uint64_t cnt;
            memcpy(&cnt, &b.b[0], 8);
            for (int i = 0; i < vbytes / 2; i++) {
                uint16_t v;
                memcpy(&v, &a.b[i * 2], 2);
                if (op == 0xF1) v = (cnt < 16) ? (uint16_t)(v << cnt) : 0;
                else if (op == 0xD1) v = (cnt < 16) ? (uint16_t)(v >> cnt) : 0;
                else v = (uint16_t)((cnt < 16) ? (((int16_t)v) >> cnt) : (((int16_t)v) >> 15));
                memcpy(&res.b[i * 2], &v, 2);
            }
            break;
        }
        case 0xD2: case 0xE2: case 0xF2: { // vpsrld / vpsrad / vpslld
            uint64_t cnt;
            memcpy(&cnt, &b.b[0], 8);
            for (int i = 0; i < vbytes / 4; i++) {
                uint32_t v;
                memcpy(&v, &a.b[i * 4], 4);
                if (op == 0xF2) v = (cnt < 32) ? (v << cnt) : 0;
                else if (op == 0xD2) v = (cnt < 32) ? (v >> cnt) : 0;
                else v = (uint32_t)((cnt < 32) ? (((int32_t)v) >> cnt) : (((int32_t)v) >> 31));
                memcpy(&res.b[i * 4], &v, 4);
            }
            break;
        }
        case 0xD3: case 0xF3: { // vpsrlq / vpsllq
            uint64_t cnt;
            memcpy(&cnt, &b.b[0], 8);
            for (int i = 0; i < vbytes / 8; i++) {
                uint64_t v;
                memcpy(&v, &a.b[i * 8], 8);
                if (op == 0xF3) v = (cnt < 64) ? (v << cnt) : 0;
                else v = (cnt < 64) ? (v >> cnt) : 0;
                memcpy(&res.b[i * 8], &v, 8);
            }
            break;
        }
        default:
            return false;
        }

        write_ymm(ctx, dst_reg, &res);
        ctx->Rip += (uint64_t)next_pos;
        return true;
    }

    // Map 2 (0F 38) with pp=1 (66)
    if (mmmmm == 2 && pp == 1) {
        ymm256_t a, b, res;
        memset(&a, 0, 32);
        memset(&b, 0, 32);
        memset(&res, 0, 32);
        read_ymm(ctx, vvvv, &a);

        // Broadcasts: vpbroadcastb (0x78), vpbroadcastw (0x79), vpbroadcastd (0x58), vpbroadcastq (0x59), vbroadcastss (0x18), vbroadcastsd (0x19)
        if (op == 0x78 || op == 0x79 || op == 0x58 || op == 0x59 || op == 0x18 || op == 0x19) {
            int elem_sz = (op == 0x78) ? 1 : (op == 0x79) ? 2 : (op == 0x58 || op == 0x18) ? 4 : 8;
            uint8_t elem[8] = {0};
            if (is_mem) memcpy(elem, (const void *)(uintptr_t)mem_addr, elem_sz);
            else {
                ymm256_t tmp;
                read_ymm(ctx, rm_reg, &tmp);
                memcpy(elem, &tmp.b[0], elem_sz);
            }
            for (int i = 0; i < vbytes; i += elem_sz) {
                memcpy(&res.b[i], elem, elem_sz);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }

        // vbroadcasti128 ymm, m128 (0x5A)
        if (op == 0x5A && is_mem) {
            memcpy(&res.b[0], (const void *)(uintptr_t)mem_addr, 16);
            memcpy(&res.b[16], &res.b[0], 16);
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }

        // Sign / Zero extensions (0x20..0x25 and 0x30..0x35)
        if ((op >= 0x20 && op <= 0x25) || (op >= 0x30 && op <= 0x35)) {
            bool is_signed = (op < 0x30);
            int kind = op & 0x0F;
            int src_elem = (kind <= 2) ? 1 : (kind <= 4) ? 2 : 4;
            int dst_elem = (kind == 0) ? 2 : (kind == 1 || kind == 3) ? 4 : 8;
            int count = vbytes / dst_elem;
            uint8_t sbuf[16] = {0};
            if (is_mem) memcpy(sbuf, (const void *)(uintptr_t)mem_addr, count * src_elem);
            else {
                ymm256_t tmp;
                read_ymm(ctx, rm_reg, &tmp);
                memcpy(sbuf, &tmp.b[0], count * src_elem);
            }
            for (int i = 0; i < count; i++) {
                int64_t sval = 0;
                uint64_t uval = 0;
                if (src_elem == 1) {
                    uint8_t u8 = sbuf[i];
                    uval = u8; sval = (int8_t)u8;
                } else if (src_elem == 2) {
                    uint16_t u16; memcpy(&u16, &sbuf[i * 2], 2);
                    uval = u16; sval = (int16_t)u16;
                } else {
                    uint32_t u32; memcpy(&u32, &sbuf[i * 4], 4);
                    uval = u32; sval = (int32_t)u32;
                }
                uint64_t outv = is_signed ? (uint64_t)sval : uval;
                memcpy(&res.b[i * dst_elem], &outv, dst_elem);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }

        // vpmaskmovd / vpmaskmovq load (0x8C) and store (0x8E)
        if ((op == 0x8C || op == 0x8E) && is_mem) {
            int esz = W ? 8 : 4;
            if (op == 0x8C) {
                for (int i = 0; i < vbytes / esz; i++) {
                    uint8_t msb = a.b[i * esz + (esz - 1)];
                    if (msb & 0x80) memcpy(&res.b[i * esz], (const void *)(uintptr_t)(mem_addr + i * esz), esz);
                }
                write_ymm(ctx, dst_reg, &res);
            } else {
                ymm256_t src;
                read_ymm(ctx, dst_reg, &src);
                for (int i = 0; i < vbytes / esz; i++) {
                    uint8_t msb = a.b[i * esz + (esz - 1)];
                    if (msb & 0x80) memcpy((void *)(uintptr_t)(mem_addr + i * esz), &src.b[i * esz], esz);
                }
            }
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }

        if (is_mem) memcpy(&b.b[0], (const void *)(uintptr_t)mem_addr, vbytes);
        else read_ymm(ctx, rm_reg, &b);

        switch (op) {
        case 0x00: // vpshufb
            for (int lane = 0; lane < lanes; lane++) {
                for (int i = 0; i < 16; i++) {
                    uint8_t ctl = b.b[lane * 16 + i];
                    res.b[lane * 16 + i] = (ctl & 0x80) ? 0 : a.b[lane * 16 + (ctl & 0x0F)];
                }
            }
            break;
        case 0x16: // vpermps
        case 0x36: // vpermd
            for (int i = 0; i < 8; i++) {
                uint32_t idx;
                memcpy(&idx, &a.b[i * 4], 4);
                idx &= 7;
                memcpy(&res.b[i * 4], &b.b[idx * 4], 4);
            }
            break;
        case 0x45: // vpsrlvd / vpsrlvq
        case 0x46: // vpsravd
        case 0x47: // vpsllvd / vpsllvq
            if (W && op != 0x46) {
                for (int i = 0; i < vbytes / 8; i++) {
                    uint64_t va, vb, vr;
                    memcpy(&va, &a.b[i * 8], 8); memcpy(&vb, &b.b[i * 8], 8);
                    if (op == 0x45) vr = (vb < 64) ? (va >> vb) : 0;
                    else vr = (vb < 64) ? (va << vb) : 0;
                    memcpy(&res.b[i * 8], &vr, 8);
                }
            } else {
                for (int i = 0; i < vbytes / 4; i++) {
                    uint32_t va, vb, vr;
                    memcpy(&va, &a.b[i * 4], 4); memcpy(&vb, &b.b[i * 4], 4);
                    if (op == 0x45) vr = (vb < 32) ? (va >> vb) : 0;
                    else if (op == 0x47) vr = (vb < 32) ? (va << vb) : 0;
                    else vr = (uint32_t)((vb < 32) ? (((int32_t)va) >> vb) : (((int32_t)va) >> 31));
                    memcpy(&res.b[i * 4], &vr, 4);
                }
            }
            break;
        case 0x40: // vpmulld
            for (int i = 0; i < vbytes / 4; i++) {
                int32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4); memcpy(&vb, &b.b[i * 4], 4);
                vr = va * vb; memcpy(&res.b[i * 4], &vr, 4);
            }
            break;
        case 0x28: // vpmuldq
            for (int i = 0; i < vbytes / 8; i++) {
                int32_t va, vb;
                memcpy(&va, &a.b[i * 8], 4); memcpy(&vb, &b.b[i * 8], 4);
                int64_t vr = (int64_t)va * (int64_t)vb;
                memcpy(&res.b[i * 8], &vr, 8);
            }
            break;
        case 0x29: // vpcmpeqq
            for (int i = 0; i < vbytes / 8; i++) {
                uint64_t va, vb, vr;
                memcpy(&va, &a.b[i * 8], 8); memcpy(&vb, &b.b[i * 8], 8);
                vr = (va == vb) ? ~0ULL : 0ULL; memcpy(&res.b[i * 8], &vr, 8);
            }
            break;
        case 0x37: // vpcmpgtq
            for (int i = 0; i < vbytes / 8; i++) {
                int64_t va, vb; uint64_t vr;
                memcpy(&va, &a.b[i * 8], 8); memcpy(&vb, &b.b[i * 8], 8);
                vr = (va > vb) ? ~0ULL : 0ULL; memcpy(&res.b[i * 8], &vr, 8);
            }
            break;
        case 0x38: // vpminsb
            for (int i = 0; i < vbytes; i++) res.b[i] = ((int8_t)a.b[i] < (int8_t)b.b[i]) ? a.b[i] : b.b[i];
            break;
        case 0x3C: // vpmaxsb
            for (int i = 0; i < vbytes; i++) res.b[i] = ((int8_t)a.b[i] > (int8_t)b.b[i]) ? a.b[i] : b.b[i];
            break;
        case 0x39: // vpminsd
            for (int i = 0; i < vbytes / 4; i++) {
                int32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4); memcpy(&vb, &b.b[i * 4], 4);
                vr = (va < vb) ? va : vb; memcpy(&res.b[i * 4], &vr, 4);
            }
            break;
        case 0x3D: // vpmaxsd
            for (int i = 0; i < vbytes / 4; i++) {
                int32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4); memcpy(&vb, &b.b[i * 4], 4);
                vr = (va > vb) ? va : vb; memcpy(&res.b[i * 4], &vr, 4);
            }
            break;
        case 0x3A: // vpminuw
            for (int i = 0; i < vbytes / 2; i++) {
                uint16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = (va < vb) ? va : vb; memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0x3E: // vpmaxuw
            for (int i = 0; i < vbytes / 2; i++) {
                uint16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2); memcpy(&vb, &b.b[i * 2], 2);
                vr = (va > vb) ? va : vb; memcpy(&res.b[i * 2], &vr, 2);
            }
            break;
        case 0x3B: // vpminud
            for (int i = 0; i < vbytes / 4; i++) {
                uint32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4); memcpy(&vb, &b.b[i * 4], 4);
                vr = (va < vb) ? va : vb; memcpy(&res.b[i * 4], &vr, 4);
            }
            break;
        case 0x3F: // vpmaxud
            for (int i = 0; i < vbytes / 4; i++) {
                uint32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4); memcpy(&vb, &b.b[i * 4], 4);
                vr = (va > vb) ? va : vb; memcpy(&res.b[i * 4], &vr, 4);
            }
            break;
        case 0x1C: // vpabsb
            for (int i = 0; i < vbytes; i++) { int8_t v = (int8_t)b.b[i]; res.b[i] = (uint8_t)(v < 0 ? -v : v); }
            break;
        case 0x1D: // vpabsw
            for (int i = 0; i < vbytes / 2; i++) {
                int16_t v; memcpy(&v, &b.b[i * 2], 2);
                uint16_t r = (uint16_t)(v < 0 ? -v : v); memcpy(&res.b[i * 2], &r, 2);
            }
            break;
        case 0x1E: // vpabsd
            for (int i = 0; i < vbytes / 4; i++) {
                int32_t v; memcpy(&v, &b.b[i * 4], 4);
                uint32_t r = (uint32_t)(v < 0 ? -v : v); memcpy(&res.b[i * 4], &r, 4);
            }
            break;
        case 0x2B: // vpackusdw
            for (int lane = 0; lane < lanes; lane++) {
                for (int i = 0; i < 4; i++) {
                    int32_t va, vb;
                    memcpy(&va, &a.b[lane * 16 + i * 4], 4);
                    memcpy(&vb, &b.b[lane * 16 + i * 4], 4);
                    uint16_t ra = sat_u16(va), rb = sat_u16(vb);
                    memcpy(&res.b[lane * 16 + i * 2], &ra, 2);
                    memcpy(&res.b[lane * 16 + 8 + i * 2], &rb, 2);
                }
            }
            break;
        default:
            return false;
        }

        write_ymm(ctx, dst_reg, &res);
        ctx->Rip += (uint64_t)next_pos;
        return true;
    }

    // Map 3 (0F 3A) with pp=1 (66)
    if (mmmmm == 3 && pp == 1) {
        uint8_t imm8 = ip[next_pos++];
        ymm256_t a, b, res;
        memset(&a, 0, 32);
        memset(&b, 0, 32);
        memset(&res, 0, 32);
        read_ymm(ctx, vvvv, &a);

        // vextracti128 xmm/m128, ymm, imm8 (0x39)
        if (op == 0x39) {
            ymm256_t src;
            read_ymm(ctx, dst_reg, &src);
            int lane = imm8 & 1;
            if (is_mem) {
                memcpy((void *)(uintptr_t)mem_addr, &src.b[lane * 16], 16);
            } else {
                memcpy(&res.b[0], &src.b[lane * 16], 16);
                write_ymm(ctx, rm_reg, &res);
            }
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }

        // vinserti128 ymm, ymm, xmm/m128, imm8 (0x38)
        if (op == 0x38) {
            memcpy(&res, &a, 32);
            int lane = imm8 & 1;
            if (is_mem) memcpy(&res.b[lane * 16], (const void *)(uintptr_t)mem_addr, 16);
            else {
                ymm256_t tmp;
                read_ymm(ctx, rm_reg, &tmp);
                memcpy(&res.b[lane * 16], &tmp.b[0], 16);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }

        if (is_mem) memcpy(&b.b[0], (const void *)(uintptr_t)mem_addr, vbytes);
        else read_ymm(ctx, rm_reg, &b);

        if (op == 0x46 || op == 0x06) {
            // vperm2i128 (0x46) / vperm2f128 (0x06)
            for (int lane = 0; lane < 2; lane++) {
                int ctl = (lane == 0) ? (imm8 & 0x0F) : ((imm8 >> 4) & 0x0F);
                if (ctl & 0x08) {
                    memset(&res.b[lane * 16], 0, 16);
                } else {
                    const uint8_t *src_lane = (ctl & 2) ? &b.b[(ctl & 1) * 16] : &a.b[(ctl & 1) * 16];
                    memcpy(&res.b[lane * 16], src_lane, 16);
                }
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
        if (op == 0x00 || op == 0x01) {
            // vpermq (0x00) / vpermpd (0x01) ymm, ymm/m256, imm8
            for (int i = 0; i < 4; i++) {
                int sel = (imm8 >> (i * 2)) & 3;
                memcpy(&res.b[i * 8], &b.b[sel * 8], 8);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
        if (op == 0x02) {
            // vpblendd xmm/ymm, xmm/ymm, xmm/m128/ymm/m256, imm8
            for (int i = 0; i < vbytes / 4; i++) {
                const uint8_t *src_dw = ((imm8 >> i) & 1) ? &b.b[i * 4] : &a.b[i * 4];
                memcpy(&res.b[i * 4], src_dw, 4);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
        if (op == 0x0E) {
            // vpblendw
            for (int lane = 0; lane < lanes; lane++) {
                for (int i = 0; i < 8; i++) {
                    const uint8_t *src_w = ((imm8 >> i) & 1) ? &b.b[lane * 16 + i * 2] : &a.b[lane * 16 + i * 2];
                    memcpy(&res.b[lane * 16 + i * 2], src_w, 2);
                }
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
        if (op == 0x4C) {
            // vpblendvb
            ymm256_t mask;
            read_ymm(ctx, (imm8 >> 4) & 0x0F, &mask);
            for (int i = 0; i < vbytes; i++) {
                res.b[i] = (mask.b[i] & 0x80) ? b.b[i] : a.b[i];
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
        if (op == 0x0F) {
            // vpalignr
            for (int lane = 0; lane < lanes; lane++) {
                uint8_t tmp[32];
                memcpy(&tmp[0], &b.b[lane * 16], 16);
                memcpy(&tmp[16], &a.b[lane * 16], 16);
                for (int i = 0; i < 16; i++) {
                    int idx = (int)imm8 + i;
                    res.b[lane * 16 + i] = (idx < 32) ? tmp[idx] : 0;
                }
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
    }

    return false;
}

static LONG CALLBACK mhgu_non_avx2_veh(PEXCEPTION_POINTERS ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    PCONTEXT ctx = ep->ContextRecord;
    const uint8_t *ip = (const uint8_t *)ctx->Rip;

    // 1. Check Legacy 0F 38 F0 / 0F 38 F1 MOVBE instructions (not supported on AMD A10-7800 bdver3)
    if (emulate_movbe(ctx, ip)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // 2. Check 3-byte VEX prefix (0xC4)
    if (ip[0] == 0xC4) {
        uint8_t b1 = ip[1];
        uint8_t b2 = ip[2];
        int rex_r = (b1 & 0x80) ? 0 : 1;
        int rex_x = (b1 & 0x40) ? 0 : 1;
        int rex_b = (b1 & 0x20) ? 0 : 1;
        int mmmmm = b1 & 0x1F;
        int W = (b2 >> 7) & 1;
        int vvvv = ((~b2) >> 3) & 0x0F;
        int L = (b2 >> 2) & 1;
        int pp = b2 & 0x03;

        if (L == 0 && emulate_bmi2(ctx, ip, mmmmm, W, vvvv, pp, rex_r, rex_x, rex_b)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (emulate_avx2(ctx, ip, 3, mmmmm, W, vvvv, L, pp, rex_r, rex_x, rex_b)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // 3. Check 2-byte VEX prefix (0xC5)
    if (ip[0] == 0xC5) {
        uint8_t b1 = ip[1];
        int rex_r = (b1 & 0x80) ? 0 : 1;
        int vvvv = ((~b1) >> 3) & 0x0F;
        int L = (b1 >> 2) & 1;
        int pp = b1 & 0x03;

        if (emulate_avx2(ctx, ip, 2, 1, 0, vvvv, L, pp, rex_r, 0, 0)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((constructor(102))) static void install_mhgu_non_avx2_veh(void) {
    AddVectoredExceptionHandler(1, mhgu_non_avx2_veh);
}

#endif
