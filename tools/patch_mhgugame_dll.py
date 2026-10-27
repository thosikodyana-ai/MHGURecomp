#!/usr/bin/env python3
"""
Static In-Place Binary Patcher for morerokk/MHGURecomp mhgugame.dll
1. Rewrites verified 6-byte BMI2 RORX switch-table instructions in-place into standard x86-64 ROR instructions.
2. Rewrites MOVBE [mem], reg + BSWAP reg sequences in-place into BSWAP reg + MOV [mem], reg + NOP.
Compatible with AMD A10-7800 (Steamroller / bdver3) and x86-64-v2 CPUs without altering section sizes or offsets.
All remaining MOVBE, BMI2, and AVX2 (L=0 and L=1) instructions are emulated transparently at runtime by runtime/avx2_bmi2_veh.c.
"""
import sys
import struct


def modrm_tail_len(buf: bytearray, pos: int) -> int:
    if pos >= len(buf):
        return -1
    modrm = buf[pos]
    mod = (modrm >> 6) & 3
    rm = modrm & 7
    if mod == 3:
        return -1
    length = 1
    if rm == 4:
        if pos + 1 >= len(buf):
            return -1
        sib = buf[pos + 1]
        length += 1
        if (sib & 7) == 5 and mod == 0:
            length += 4
    elif rm == 5 and mod == 0:
        length += 4
    if mod == 1:
        length += 1
    elif mod == 2:
        length += 4
    return length


def patch_dll(path: str) -> None:
    with open(path, "rb") as f:
        data = bytearray(f.read())

    if len(data) < 256 or data[0:2] != b"MZ":
        raise RuntimeError(f"{path}: Not a valid PE binary")

    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off+4] != b"PE\x00\x00":
        raise RuntimeError(f"{path}: Invalid PE signature")

    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_sz = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec_off = pe_off + 24 + opt_sz

    patched_rorx = 0
    patched_movbe = 0

    for s_idx in range(num_sec):
        s = sec_off + s_idx * 40
        name = data[s:s+8].rstrip(b"\x00").decode(errors="ignore")
        vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", data, s + 8)
        if name != ".text":
            continue

        end = min(len(data) - 16, rptr + rsize - 16)
        idx = rptr + 4
        while idx < end:
            # 1. Match 6-byte VEX RORX r32, r32, imm8: C4 [b1] 7B F0 [modrm mod=3] [imm8]
            if data[idx] == 0xC4 and data[idx + 2] == 0x7B and data[idx + 3] == 0xF0:
                b1 = data[idx + 1]
                if (b1 & 0x1F) == 3 and data[idx - 1] not in (0x81, 0x83, 0x89, 0x8B):
                    modrm = data[idx + 4]
                    if (modrm >> 6) == 3:
                        imm8 = data[idx + 5]
                        nxt = data[idx + 6]
                        nxt2 = data[idx + 7]
                        is_cmp_next = (
                            (nxt in (0x83, 0x81) and 0xF8 <= nxt2 <= 0xFF) or
                            (nxt == 0x3D) or
                            (nxt == 0x41 and nxt2 in (0x83, 0x81) and 0xF8 <= data[idx + 8] <= 0xFF)
                        )
                        if is_cmp_next:
                            rex_r = 0 if (b1 & 0x80) else 1
                            rex_b = 0 if (b1 & 0x20) else 1
                            dst_low = (modrm >> 3) & 7
                            src_low = modrm & 7

                            if rex_r == rex_b and dst_low == src_low:
                                if rex_b == 0:
                                    data[idx:idx+6] = bytes([0xC1, 0xC8 | dst_low, imm8, 0x0F, 0x1F, 0x00])
                                else:
                                    data[idx:idx+6] = bytes([0x41, 0xC1, 0xC8 | dst_low, imm8, 0x66, 0x90])
                                patched_rorx += 1
                                idx += 6
                                continue
                            elif rex_r == 0 and rex_b == 0:
                                mov_modrm = 0xC0 | (src_low << 3) | dst_low
                                data[idx:idx+6] = bytes([0x89, mov_modrm, 0xC1, 0xC8 | dst_low, imm8, 0x90])
                                patched_rorx += 1
                                idx += 6
                                continue
                            elif rex_r == 0 and rex_b == 1:
                                mov_modrm = 0xC0 | (dst_low << 3) | src_low
                                data[idx:idx+6] = bytes([0x41, 0x8B, mov_modrm, 0xC1, 0xC8 | dst_low, imm8])
                                patched_rorx += 1
                                idx += 6
                                continue

            # 2A. Match REX + 0F 38 F1 (MOVBE [mem], reg32/64) followed immediately by BSWAP reg32/64
            if 0x40 <= data[idx] <= 0x4F and data[idx+1:idx+4] == b"\x0f\x38\xf1" and data[idx-1] not in (0x66, 0xF2, 0xF3):
                rex = data[idx]
                tlen = modrm_tail_len(data, idx + 4)
                if 0 < tlen <= 9 and idx + 4 + tlen + 3 <= end:
                    modrm = data[idx + 4]
                    reg_low = (modrm >> 3) & 7
                    rex_w = (rex >> 3) & 1
                    rex_r = (rex >> 2) & 1
                    tail = bytes(data[idx+4 : idx+4+tlen])
                    b_idx = idx + 4 + tlen
                    if rex_w == 0 and rex_r == 1 and data[b_idx:b_idx+3] == bytes([0x41, 0x0F, 0xC8 | reg_low]):
                        repl = bytes([0x41, 0x0F, 0xC8 | reg_low, rex, 0x89]) + tail + b"\x66\x90"
                        data[idx : b_idx + 3] = repl
                        patched_movbe += 1
                        idx = b_idx + 3
                        continue
                    elif rex_w == 0 and rex_r == 0 and data[b_idx:b_idx+2] == bytes([0x0F, 0xC8 | reg_low]):
                        repl = bytes([0x0F, 0xC8 | reg_low, rex, 0x89]) + tail + b"\x66\x90"
                        data[idx : b_idx + 2] = repl
                        patched_movbe += 1
                        idx = b_idx + 2
                        continue
                    elif rex_w == 1 and data[b_idx:b_idx+3] == bytes([0x48 | rex_r, 0x0F, 0xC8 | reg_low]):
                        repl = bytes([0x48 | rex_r, 0x0F, 0xC8 | reg_low, rex, 0x89]) + tail + b"\x66\x90"
                        data[idx : b_idx + 3] = repl
                        patched_movbe += 1
                        idx = b_idx + 3
                        continue

            # 2B. Match unprefixed 0F 38 F1 (MOVBE [mem], reg32) followed immediately by BSWAP reg32
            if data[idx:idx+3] == b"\x0f\x38\xf1" and not (0x40 <= data[idx-1] <= 0x4F) and data[idx-1] not in (0x66, 0xF2, 0xF3):
                tlen = modrm_tail_len(data, idx + 3)
                if 0 < tlen <= 9 and idx + 3 + tlen + 2 <= end:
                    modrm = data[idx + 3]
                    reg_low = (modrm >> 3) & 7
                    tail = bytes(data[idx+3 : idx+3+tlen])
                    b_idx = idx + 3 + tlen
                    if data[b_idx:b_idx+2] == bytes([0x0F, 0xC8 | reg_low]):
                        repl = bytes([0x0F, 0xC8 | reg_low, 0x89]) + tail + b"\x66\x90"
                        data[idx : b_idx + 2] = repl
                        patched_movbe += 1
                        idx = b_idx + 2
                        continue

            idx += 1

    with open(path, "wb") as f:
        f.write(data)
    print(f"[patch_mhgugame_dll] Patched {patched_rorx} RORX sites and {patched_movbe} MOVBE+BSWAP sites in-place in {path}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: patch_mhgugame_dll.py <path/to/mhgugame.dll>")
        sys.exit(1)
    patch_dll(sys.argv[1])
