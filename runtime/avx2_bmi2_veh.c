// Non-AVX2 / Non-BMI2 Hardware Exception Emulator & JIT Patcher for AMD A10-7800 (Steamroller / bdver3)
// Catches EXCEPTION_ILLEGAL_INSTRUCTION (0xC000001D) and emulates BMI2 and AVX2 (256-bit Integer YMM)
// instructions seamlessly using hardware XMM + XSTATE_AVX (YMM_Hi) and GPR context.

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
            if (W) memcpy(&src, (const void *)mem_addr, 8);
            else { uint32_t tmp; memcpy(&tmp, (const void *)mem_addr, 4); src = tmp; }
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
            if (W) memcpy(&src, (const void *)mem_addr, 8);
            else { uint32_t tmp; memcpy(&tmp, (const void *)mem_addr, 4); src = tmp; }
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
            if (W) memcpy(&rm_val, (const void *)mem_addr, 8);
            else { uint32_t tmp; memcpy(&tmp, (const void *)mem_addr, 4); rm_val = tmp; }
        } else {
            rm_val = read_gpr(ctx, rm_reg, W);
        }

        if (pp == 0) {
            // BZHI dst, rm_val, vvvv (updates ZF, CF, SF, OF=0)
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
            // Update EFLAGS: CF(0), ZF(6), SF(7), OF(11)
            uint32_t eflags = ctx->EFlags & ~((1u << 0) | (1u << 6) | (1u << 7) | (1u << 11));
            if (cf) eflags |= (1u << 0);
            if (res == 0) eflags |= (1u << 6);
            if ((res >> (width - 1)) & 1ULL) eflags |= (1u << 7);
            ctx->EFlags = eflags;
        } else if (pp == 2) {
            // PEXT dst, vvvv, rm_val
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
            // PDEP dst, vvvv, rm_val
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
            if (W) memcpy(&src, (const void *)mem_addr, 8);
            else { uint32_t tmp; memcpy(&tmp, (const void *)mem_addr, 4); src = tmp; }
        } else {
            src = read_gpr(ctx, rm_reg, W);
        }
        if (W) {
            unsigned __int128 a = (unsigned __int128)read_gpr(ctx, 2, 1); // RDX
            unsigned __int128 prod = a * (unsigned __int128)src;
            write_gpr(ctx, vvvv, 1, (uint64_t)prod);
            write_gpr(ctx, dst_reg, 1, (uint64_t)(prod >> 64));
        } else {
            uint64_t a = (uint32_t)read_gpr(ctx, 2, 0); // EDX
            uint64_t prod = a * (uint64_t)(uint32_t)src;
            write_gpr(ctx, vvvv, 0, (uint32_t)prod);
            write_gpr(ctx, dst_reg, 0, (uint32_t)(prod >> 32));
        }
        ctx->Rip += (uint64_t)next_pos;
        return true;
    }

    return false;
}

static bool emulate_avx2_ymm(PCONTEXT ctx, const uint8_t *ip, int pos_op, int mmmmm, int W, int vvvv, int pp,
                             int rex_r, int rex_x, int rex_b) {
    uint8_t op = ip[pos_op];
    int dst_reg = 0, rm_reg = 0;
    uint64_t mem_addr = 0;
    bool is_mem = false;
    int imm_bytes = (mmmmm == 3 || (mmmmm == 1 && (op == 0x70 || op == 0x71 || op == 0x72 || op == 0x73))) ? 1 : 0;

    int next_pos = decode_modrm(ctx, ip, pos_op + 1, rex_r, rex_x, rex_b, imm_bytes, &dst_reg, &rm_reg, &mem_addr, &is_mem);

    // Map 1 (0F) with pp=1 (66): common 256-bit integer operations
    if (mmmmm == 1 && pp == 1) {
        // Immediate shifts: 0x71 (psllw/psrlw/psraw), 0x72 (pslld/psrld/psrad), 0x73 (psllq/psrlq/psrldq/pslldq)
        if (op == 0x71 || op == 0x72 || op == 0x73) {
            uint8_t imm8 = ip[next_pos++];
            int subop = dst_reg & 7; // ModRM.reg holds shift type (2=srl, 4=sra, 6=sll, 3=srldq, 7=slldq)
            ymm256_t src, res;
            if (is_mem) memcpy(&src, (const void *)mem_addr, 32);
            else read_ymm(ctx, rm_reg, &src);
            memset(&res, 0, 32);

            if (op == 0x72) {
                for (int i = 0; i < 8; i++) {
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
                    // vpsrldq / vpslldq (per 128-bit lane byte shift)
                    uint32_t shift = imm8;
                    for (int lane = 0; lane < 2; lane++) {
                        for (int b = 0; b < 16; b++) {
                            if (subop == 3) res.b[lane * 16 + b] = (b + shift < 16) ? src.b[lane * 16 + b + shift] : 0;
                            else res.b[lane * 16 + b] = (b >= (int)shift) ? src.b[lane * 16 + b - shift] : 0;
                        }
                    }
                } else {
                    for (int i = 0; i < 4; i++) {
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

        // vpshufd ymm, ymm/m256, imm8 (0x70)
        if (op == 0x70) {
            uint8_t imm8 = ip[next_pos++];
            ymm256_t src, res;
            if (is_mem) memcpy(&src, (const void *)mem_addr, 32);
            else read_ymm(ctx, rm_reg, &src);
            for (int lane = 0; lane < 2; lane++) {
                for (int d = 0; d < 4; d++) {
                    int sel = (imm8 >> (d * 2)) & 3;
                    memcpy(&res.b[lane * 16 + d * 4], &src.b[lane * 16 + sel * 4], 4);
                }
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }

        ymm256_t a, b, res;
        read_ymm(ctx, vvvv, &a);
        if (is_mem) memcpy(&b, (const void *)mem_addr, 32);
        else read_ymm(ctx, rm_reg, &b);

        switch (op) {
        case 0xDB: // vpand
            for (int i = 0; i < 32; i++) res.b[i] = a.b[i] & b.b[i];
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0xDF: // vpandn
            for (int i = 0; i < 32; i++) res.b[i] = (~a.b[i]) & b.b[i];
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0xEB: // vpor
            for (int i = 0; i < 32; i++) res.b[i] = a.b[i] | b.b[i];
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0xEF: // vpxor
            for (int i = 0; i < 32; i++) res.b[i] = a.b[i] ^ b.b[i];
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0xFC: // vpaddb
            for (int i = 0; i < 32; i++) res.b[i] = a.b[i] + b.b[i];
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0xFD: // vpaddw
            for (int i = 0; i < 16; i++) {
                uint16_t va, vb, vr;
                memcpy(&va, &a.b[i * 2], 2);
                memcpy(&vb, &b.b[i * 2], 2);
                vr = va + vb;
                memcpy(&res.b[i * 2], &vr, 2);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0xFE: // vpaddd
            for (int i = 0; i < 8; i++) {
                uint32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4);
                memcpy(&vb, &b.b[i * 4], 4);
                vr = va + vb;
                memcpy(&res.b[i * 4], &vr, 4);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0xD4: // vpaddq
            for (int i = 0; i < 4; i++) {
                uint64_t va, vb, vr;
                memcpy(&va, &a.b[i * 8], 8);
                memcpy(&vb, &b.b[i * 8], 8);
                vr = va + vb;
                memcpy(&res.b[i * 8], &vr, 8);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0xF8: // vpsubb
            for (int i = 0; i < 32; i++) res.b[i] = a.b[i] - b.b[i];
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0xFA: // vpsubd
            for (int i = 0; i < 8; i++) {
                uint32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4);
                memcpy(&vb, &b.b[i * 4], 4);
                vr = va - vb;
                memcpy(&res.b[i * 4], &vr, 4);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0xFB: // vpsubq
            for (int i = 0; i < 4; i++) {
                uint64_t va, vb, vr;
                memcpy(&va, &a.b[i * 8], 8);
                memcpy(&vb, &b.b[i * 8], 8);
                vr = va - vb;
                memcpy(&res.b[i * 8], &vr, 8);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0x74: // vpcmpeqb
            for (int i = 0; i < 32; i++) res.b[i] = (a.b[i] == b.b[i]) ? 0xFF : 0x00;
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        case 0x76: // vpcmpeqd
            for (int i = 0; i < 8; i++) {
                uint32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4);
                memcpy(&vb, &b.b[i * 4], 4);
                vr = (va == vb) ? 0xFFFFFFFFu : 0u;
                memcpy(&res.b[i * 4], &vr, 4);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
    }

    // Map 2 (0F 38) with pp=1 (66)
    if (mmmmm == 2 && pp == 1) {
        ymm256_t a, b, res;
        read_ymm(ctx, vvvv, &a);

        // vpbroadcastb (0x78), vpbroadcastw (0x79), vpbroadcastd (0x58), vpbroadcastq (0x59)
        if (op == 0x78 || op == 0x79 || op == 0x58 || op == 0x59) {
            int elem_sz = (op == 0x78) ? 1 : (op == 0x79) ? 2 : (op == 0x58) ? 4 : 8;
            uint8_t elem[8] = {0};
            if (is_mem) memcpy(elem, (const void *)mem_addr, elem_sz);
            else {
                ymm256_t tmp;
                read_ymm(ctx, rm_reg, &tmp);
                memcpy(elem, &tmp.b[0], elem_sz);
            }
            for (int i = 0; i < 32; i += elem_sz) {
                memcpy(&res.b[i], elem, elem_sz);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }

        if (is_mem) memcpy(&b, (const void *)mem_addr, 32);
        else read_ymm(ctx, rm_reg, &b);

        if (op == 0x00) {
            // vpshufb ymm, ymm, ymm/m256
            for (int lane = 0; lane < 2; lane++) {
                for (int i = 0; i < 16; i++) {
                    uint8_t ctl = b.b[lane * 16 + i];
                    res.b[lane * 16 + i] = (ctl & 0x80) ? 0 : a.b[lane * 16 + (ctl & 0x0F)];
                }
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
        if (op == 0x40) {
            // vpmulld ymm, ymm, ymm/m256
            for (int i = 0; i < 8; i++) {
                int32_t va, vb, vr;
                memcpy(&va, &a.b[i * 4], 4);
                memcpy(&vb, &b.b[i * 4], 4);
                vr = va * vb;
                memcpy(&res.b[i * 4], &vr, 4);
            }
            write_ymm(ctx, dst_reg, &res);
            ctx->Rip += (uint64_t)next_pos;
            return true;
        }
    }

    // Map 3 (0F 3A) with pp=1 (66)
    if (mmmmm == 3 && pp == 1) {
        uint8_t imm8 = ip[next_pos++];
        ymm256_t a, b, res;
        read_ymm(ctx, vvvv, &a);
        if (is_mem) memcpy(&b, (const void *)mem_addr, 32);
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
        if (op == 0x00) {
            // vpermq ymm, ymm/m256, imm8
            for (int i = 0; i < 4; i++) {
                int sel = (imm8 >> (i * 2)) & 3;
                memcpy(&res.b[i * 8], &b.b[sel * 8], 8);
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

    // 3-byte VEX prefix (0xC4)
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
        if (L == 1 && emulate_avx2_ymm(ctx, ip, 3, mmmmm, W, vvvv, pp, rex_r, rex_x, rex_b)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // 2-byte VEX prefix (0xC5)
    if (ip[0] == 0xC5) {
        uint8_t b1 = ip[1];
        int rex_r = (b1 & 0x80) ? 0 : 1;
        int vvvv = ((~b1) >> 3) & 0x0F;
        int L = (b1 >> 2) & 1;
        int pp = b1 & 0x03;
        if (L == 1 && emulate_avx2_ymm(ctx, ip, 2, 1, 0, vvvv, pp, rex_r, 0, 0)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((constructor(102))) static void install_mhgu_non_avx2_veh(void) {
    AddVectoredExceptionHandler(1, mhgu_non_avx2_veh);
}
#endif
