#!/usr/bin/env python3
"""
Static In-Place Binary Patcher for morerokk/MHGURecomp mhgugame.dll
Rewrites verified 6-byte BMI2 RORX switch-table instructions in-place into standard x86-64 ROR instructions
compatible with AMD A10-7800 (Steamroller / bdver3) and x86-64-v2 CPUs, without altering section sizes or offsets.
Remaining BMI2 and AVX2 instructions are handled transparently at runtime by runtime/avx2_bmi2_veh.c.
"""
import sys
import struct

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

    patched_inplace = 0

    for s_idx in range(num_sec):
        s = sec_off + s_idx * 40
        name = data[s:s+8].rstrip(b"\x00").decode(errors="ignore")
        vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", data, s + 8)
        if name != ".text":
            continue

        end = min(len(data) - 12, rptr + rsize - 12)
        idx = rptr + 4
        while idx < end:
            # Match 6-byte VEX RORX r32, r32, imm8: C4 [b1] 7B F0 [modrm mod=3] [imm8]
            # preceded by ADD/SUB/MOV and followed immediately by CMP (which overwrites EFLAGS)
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
                                # Same register: ror dst, imm8 + NOP padding to 6 bytes
                                if rex_b == 0:
                                    # C1 [11 001 dst] imm8 + 0F 1F 00 (3-byte NOP)
                                    data[idx:idx+6] = bytes([0xC1, 0xC8 | dst_low, imm8, 0x0F, 0x1F, 0x00])
                                else:
                                    # 41 C1 [11 001 dst] imm8 + 66 90 (2-byte NOP)
                                    data[idx:idx+6] = bytes([0x41, 0xC1, 0xC8 | dst_low, imm8, 0x66, 0x90])
                                patched_inplace += 1
                                idx += 6
                                continue
                            elif rex_r == 0 and rex_b == 0:
                                # Both in eax..edi: mov dst, src (2B) + ror dst, imm8 (3B) + nop (1B) = 6B
                                mov_modrm = 0xC0 | (src_low << 3) | dst_low
                                data[idx:idx+6] = bytes([0x89, mov_modrm, 0xC1, 0xC8 | dst_low, imm8, 0x90])
                                patched_inplace += 1
                                idx += 6
                                continue
                            elif rex_r == 0 and rex_b == 1:
                                # src in r8d..r15d, dst in eax..edi: 41 8B [11 dst src] (3B) + C1 [11 001 dst] imm8 (3B) = 6B
                                mov_modrm = 0xC0 | (dst_low << 3) | src_low
                                data[idx:idx+6] = bytes([0x41, 0x8B, mov_modrm, 0xC1, 0xC8 | dst_low, imm8])
                                patched_inplace += 1
                                idx += 6
                                continue
            idx += 1

    with open(path, "wb") as f:
        f.write(data)

    print(f"[patch_mhgugame_dll] Successfully patched {patched_inplace} verified RORX sites in-place in {path}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: patch_mhgugame_dll.py <path/to/mhgugame.dll>")
        sys.exit(1)
    patch_dll(sys.argv[1])
