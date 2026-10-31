#!/usr/bin/env python3
"""
Full AOT Static Binary Translator (v3.1) for MHGURecomp on AMD A10-7800 (bdver3).
Fixes register-clobbering in SHLX/SHRX/SARX (when cnt_reg == dst_reg), BZHI (when memory base is %rcx
or W == 0 jae offset), VPBROADCASTB memory load, and MOVBE store register preservation,
and avoids multi-instruction relocation across 4-byte MOVBE boundaries.
"""
import re
import shutil
import struct
import subprocess
import sys

IMAGE_BASE = 0x180000000


def align_up(val, align):
    return (val + align - 1) & ~(align - 1)


def parse_pe_sections(data):
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    num_sections = struct.unpack_from('<H', data, e_lfanew + 6)[0]
    opt_hdr_size = struct.unpack_from('<H', data, e_lfanew + 20)[0]
    sec_table_off = e_lfanew + 24 + opt_hdr_size
    sections = []
    for i in range(num_sections):
        off = sec_table_off + i * 40
        name, vsize, vaddr, rsize, rptr, _, _, _, _, chars = struct.unpack_from('<8sIIIIIIHHI', data, off)
        sections.append({
            'index': i,
            'offset': off,
            'name': name.rstrip(b'\x00').decode('ascii', errors='replace'),
            'vsize': vsize,
            'vaddr': vaddr,
            'rsize': rsize,
            'rptr': rptr,
            'chars': chars,
        })
    return e_lfanew, sec_table_off, sections


def va_to_file_offset(va, sections):
    rva = va - IMAGE_BASE
    for s in sections:
        if s['vaddr'] <= rva < s['vaddr'] + max(s['vsize'], s['rsize']):
            return s['rptr'] + (rva - s['vaddr'])
    raise ValueError(f"VA {hex(va)} (RVA {hex(rva)}) not found in any section")


def append_tramp_section(data, tramp_size=0x100000):
    e_lfanew, sec_table_off, sections = parse_pe_sections(data)
    num_sections = len(sections)
    sect_align = struct.unpack_from('<I', data, e_lfanew + 24 + 32)[0]
    file_align = struct.unpack_from('<I', data, e_lfanew + 24 + 36)[0]

    last_sec = max(sections, key=lambda s: s['vaddr'] + s['vsize'])
    tramp_vaddr = align_up(last_sec['vaddr'] + last_sec['vsize'], sect_align)
    tramp_rptr = align_up(len(data), file_align)
    tramp_vsize = align_up(tramp_size, sect_align)
    tramp_rsize = align_up(tramp_size, file_align)

    if len(data) < tramp_rptr:
        data.extend(b'\x00' * (tramp_rptr - len(data)))
    data.extend(b'\xCC' * tramp_rsize)

    new_sec_off = sec_table_off + num_sections * 40
    chars = 0x60000020
    struct.pack_into(
        '<8sIIIIIIHHI',
        data,
        new_sec_off,
        b'.tramp\x00\x00',
        tramp_vsize,
        tramp_vaddr,
        tramp_rsize,
        tramp_rptr,
        0, 0, 0, 0,
        chars,
    )
    struct.pack_into('<H', data, e_lfanew + 6, num_sections + 1)
    new_size_of_image = align_up(tramp_vaddr + tramp_vsize, sect_align)
    struct.pack_into('<I', data, e_lfanew + 24 + 56, new_size_of_image)
    return tramp_vaddr, tramp_rptr


def encode_modrm_op(opc, reg_op, rm_reg, is_64=False, prefix_66=False):
    out = bytearray()
    if prefix_66:
        out.append(0x66)
    rex = 0x40
    if is_64:
        rex |= 0x08
    if reg_op >= 8:
        rex |= 0x04
    if rm_reg >= 8:
        rex |= 0x01
    if rex != 0x40:
        out.append(rex)
    if isinstance(opc, int):
        out.append(opc)
    else:
        out.extend(opc)
    modrm = 0xC0 | ((reg_op & 7) << 3) | (rm_reg & 7)
    out.append(modrm)
    return bytes(out)


def encode_mov_reg_reg(dst, src, is_64=False):
    if dst == src and is_64:
        return b''
    return encode_modrm_op(0x89, src, dst, is_64=is_64)


def encode_bswap(reg, is_64=False, is_16=False):
    if is_16:
        return encode_modrm_op(0xC1, 0, reg, is_64=False, prefix_66=True) + bytes([8])
    out = bytearray()
    rex = 0x40
    if is_64:
        rex |= 0x08
    if reg >= 8:
        rex |= 0x01
    if rex != 0x40:
        out.append(rex)
    out.append(0x0F)
    out.append(0xC8 + (reg & 7))
    return bytes(out)


def encode_push(reg):
    if reg >= 8:
        return bytes([0x41, 0x50 + (reg & 7)])
    return bytes([0x50 + reg])


def encode_pop(reg):
    if reg >= 8:
        return bytes([0x41, 0x58 + (reg & 7)])
    return bytes([0x58 + reg])


def adjust_rsp_disp_in_mem_tail(rex, tail, delta):
    modrm = tail[0]
    mod = (modrm >> 6) & 3
    rm = (modrm & 7) | (8 if (rex & 1) else 0)
    if mod == 3:
        return rex, tail
    uses_rsp = False
    sib_len = 0
    if (modrm & 7) == 4:
        sib_len = 1
        sib = tail[1]
        base = (sib & 7) | (8 if (rex & 1) else 0)
        if base == 4 and not (mod == 0 and (sib & 7) == 5):
            uses_rsp = True
    else:
        if rm == 4:
            uses_rsp = True
    if not uses_rsp:
        return rex, tail
    if mod == 0:
        new_modrm = (modrm & 0x3F) | 0x40
        return rex, bytes([new_modrm]) + tail[1:1 + sib_len] + struct.pack('<b', delta)
    elif mod == 1:
        disp8 = struct.unpack_from('<b', tail, 1 + sib_len)[0]
        new_disp = disp8 + delta
        if -128 <= new_disp <= 127:
            return rex, bytes([modrm]) + tail[1:1 + sib_len] + struct.pack('<b', new_disp)
        else:
            new_modrm = (modrm & 0x3F) | 0x80
            return rex, bytes([new_modrm]) + tail[1:1 + sib_len] + struct.pack('<i', new_disp)
    elif mod == 2:
        disp32 = struct.unpack_from('<i', tail, 1 + sib_len)[0]
        new_disp = disp32 + delta
        return rex, bytes([modrm]) + tail[1:1 + sib_len] + struct.pack('<i', new_disp)
    return rex, tail


def parse_vex(raw_bytes):
    if raw_bytes[0] == 0xC4:
        b1, b2 = raw_bytes[1], raw_bytes[2]
        R = 0 if (b1 & 0x80) else 1
        X = 0 if (b1 & 0x40) else 1
        B = 0 if (b1 & 0x20) else 1
        W = 1 if (b2 & 0x80) else 0
        vvvv = (~(b2 >> 3)) & 0x0F
        pp = b2 & 0x03
        return 3, R, X, B, W, vvvv, pp, raw_bytes[3], raw_bytes[4:]
    raise ValueError("Not VEX3")


def emit_mem_op_from_vex_tail(opc, R, X, B, W, tail, rsp_delta=0, prefix_66=False):
    rex = 0x40 | (0x08 if W else 0) | (0x04 if R else 0) | (0x02 if X else 0) | (0x01 if B else 0)
    if rsp_delta != 0:
        rex, tail = adjust_rsp_disp_in_mem_tail(rex, tail, rsp_delta)
    out = bytearray()
    if prefix_66:
        out.append(0x66)
    if rex != 0x40:
        out.append(rex)
    out.append(opc)
    out.extend(tail)
    return bytes(out)


LINE_RE = re.compile(r'^\s*([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{2}\s)+)\s*(.+)$')
TARGET_RE = re.compile(r'\b(rorx|shlx|sarx|shrx|movbe|mulx|bzhi|pext|pdep|vpbroadcastd|vpbroadcastb)\b')


def parse_objdump_line(line):
    m = LINE_RE.match(line)
    if not m:
        return None
    va = int(m.group(1), 16)
    raw = bytes(int(x, 16) for x in m.group(2).strip().split())
    disasm = m.group(3).strip()
    return va, raw, disasm


def patch_mhgugame_dll(dll_path):
    objdump_bin = shutil.which('objdump') or shutil.which('x86_64-w64-mingw32-objdump') or 'objdump'
    print(f"[v3.1] Disassembling {dll_path} using {objdump_bin} + grep...")
    cmd = f'"{objdump_bin}" -d --insn-width=16 "{dll_path}" | grep -E "rorx|shlx|sarx|shrx|movbe|mulx|bzhi|pext|pdep|vpbroadcastd|vpbroadcastb"'
    proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, text=True, bufsize=1024 * 1024)

    records = []
    for line in proc.stdout:
        p = parse_objdump_line(line)
        if p is not None and TARGET_RE.search(p[2]):
            records.append(p)

    proc.wait()

    global IMAGE_BASE
    if records:
        IMAGE_BASE = records[0][0] & ~0x0FFFFFFF
    print(f"[v3.1] Found {len(records)} non-bdver3 sites, IMAGE_BASE={hex(IMAGE_BASE)}.")

    with open(dll_path, 'rb') as f:
        data = bytearray(f.read())

    tramp_vaddr, tramp_rptr = append_tramp_section(data, tramp_size=0x100000)
    _, _, sections = parse_pe_sections(data)
    tramp_cur_rva = tramp_vaddr

    def write_tramp(code_bytes):
        nonlocal tramp_cur_rva
        rva = tramp_cur_rva
        foff = tramp_rptr + (rva - tramp_vaddr)
        data[foff:foff + len(code_bytes)] = code_bytes
        tramp_cur_rva = align_up(rva + len(code_bytes), 4)
        return IMAGE_BASE + rva

    def patch_at_va(va, new_bytes):
        foff = va_to_file_offset(va, sections)
        data[foff:foff + len(new_bytes)] = new_bytes

    def make_jmp_rel32(src_va, dst_va, pad_len):
        rel32 = dst_va - (src_va + 5)
        res = bytearray([0xE9]) + struct.pack('<i', rel32)
        if pad_len > 5:
            res.extend(b'\x90' * (pad_len - 5))
        return bytes(res)

    stats = {
        'vpbroadcastd_inplace': 0,
        'vpbroadcastb_tramp': 0,
        'movbe_load_inplace': 0,
        'movbe_tramp': 0,
        'movbe_4byte_veh': 0,
        'rorx_tramp': 0,
        'shiftx_tramp': 0,
        'bzhi_tramp': 0,
    }

    for va, raw, disasm in records:
        mnem = disasm.split()[0]

        if mnem == 'vpbroadcastd':
            new_raw = bytearray(raw)
            new_raw[3] = 0x18
            patch_at_va(va, new_raw)
            stats['vpbroadcastd_inplace'] += 1
            continue

        if mnem == 'vpbroadcastb':
            tramp_va = IMAGE_BASE + tramp_cur_rva
            tcode = bytearray()
            if (va - IMAGE_BASE) == 0x1dc6de:
                # vpbroadcastb (%rcx,%rax,1), %xmm0
                tcode.extend(bytes.fromhex('9c500fb6040169c001010101c5f96ec0c5f970c000589d'))
            elif (va - IMAGE_BASE) == 0x654a10:
                # vpbroadcastb (%r10,%rax,1), %xmm0
                tcode.extend(bytes.fromhex('9c50410fb6040269c001010101c5f96ec0c5f970c000589d'))
            ret_va = va + len(raw)
            rel_back = ret_va - (tramp_va + len(tcode) + 5)
            tcode.extend(b'\xE9' + struct.pack('<i', rel_back))
            write_tramp(tcode)
            patch_at_va(va, make_jmp_rel32(va, tramp_va, len(raw)))
            stats['vpbroadcastb_tramp'] += 1
            continue

        if mnem == 'rorx':
            _, R, X, B, W, _, _, _, tail = parse_vex(raw)
            modrm = tail[0]
            imm8 = tail[-1]
            mem_or_reg_tail = tail[:-1]
            dst_reg = ((modrm >> 3) & 7) | (8 if R else 0)
            mod = (modrm >> 6) & 3

            tramp_va = IMAGE_BASE + tramp_cur_rva
            tcode = bytearray()
            tcode.append(0x9C)  # pushfq

            if mod == 3:
                src_reg = (modrm & 7) | (8 if B else 0)
                tcode.extend(encode_mov_reg_reg(dst_reg, src_reg, is_64=bool(W)))
            else:
                tcode.extend(emit_mem_op_from_vex_tail(0x8B, R, X, B, W, mem_or_reg_tail, rsp_delta=8))

            tcode.extend(encode_modrm_op(0xC1, 1, dst_reg, is_64=bool(W)) + bytes([imm8]))
            tcode.append(0x9D)  # popfq
            ret_va = va + len(raw)
            rel_back = ret_va - (tramp_va + len(tcode) + 5)
            tcode.extend(b'\xE9' + struct.pack('<i', rel_back))
            write_tramp(tcode)
            patch_at_va(va, make_jmp_rel32(va, tramp_va, len(raw)))
            stats['rorx_tramp'] += 1
            continue

        if mnem in ('shlx', 'shrx', 'sarx'):
            _, R, X, B, W, vvvv, pp, _, tail = parse_vex(raw)
            modrm = tail[0]
            dst_reg = ((modrm >> 3) & 7) | (8 if R else 0)
            cnt_reg = vvvv
            mod = (modrm >> 6) & 3
            shift_subop = 4 if pp == 1 else (7 if pp == 2 else 5)

            tramp_va = IMAGE_BASE + tramp_cur_rva
            tcode = bytearray()
            tcode.append(0x9C)  # pushfq (rsp_delta = 8)

            if dst_reg == 1:
                # Use %rax (0) as work_dst so neither %rax nor %rcx is clobbered before reading src!
                tcode.extend(encode_push(0))        # push %rax (rsp_delta = 16)
                tcode.extend(encode_push(cnt_reg))  # push cnt_reg (rsp_delta = 24)
                if mod == 3:
                    src_reg = (modrm & 7) | (8 if B else 0)
                    tcode.extend(encode_mov_reg_reg(0, src_reg, is_64=bool(W)))
                else:
                    s_modrm = (modrm & 0xC7) | (0 << 3)
                    tcode.extend(emit_mem_op_from_vex_tail(0x8B, 0, X, B, W, bytes([s_modrm]) + tail[1:], rsp_delta=24))
                tcode.extend(encode_pop(1))         # pop %rcx (cnt_reg)
                tcode.extend(encode_modrm_op(0xD3, shift_subop, 0, is_64=bool(W)))
                tcode.extend(encode_mov_reg_reg(1, 0, is_64=bool(W)))
                tcode.extend(encode_pop(0))         # pop %rax
            else:
                tcode.extend(encode_push(1))        # push %rcx (rsp_delta = 16)
                tcode.extend(encode_push(cnt_reg))  # push cnt_reg (rsp_delta = 24)
                if mod == 3:
                    src_reg = (modrm & 7) | (8 if B else 0)
                    tcode.extend(encode_mov_reg_reg(dst_reg, src_reg, is_64=bool(W)))
                else:
                    tcode.extend(emit_mem_op_from_vex_tail(0x8B, R, X, B, W, tail, rsp_delta=24))
                tcode.extend(encode_pop(1))         # pop %rcx (cnt_reg)
                tcode.extend(encode_modrm_op(0xD3, shift_subop, dst_reg, is_64=bool(W)))
                tcode.extend(encode_pop(1))         # pop %rcx

            tcode.append(0x9D)  # popfq
            ret_va = va + len(raw)
            rel_back = ret_va - (tramp_va + len(tcode) + 5)
            tcode.extend(b'\xE9' + struct.pack('<i', rel_back))
            write_tramp(tcode)
            patch_at_va(va, make_jmp_rel32(va, tramp_va, len(raw)))
            stats['shiftx_tramp'] += 1
            continue

        if mnem == 'bzhi':
            _, R, X, B, W, vvvv, _, _, tail = parse_vex(raw)
            modrm = tail[0]
            dst_reg = ((modrm >> 3) & 7) | (8 if R else 0)
            idx_reg = vvvv
            mod = (modrm >> 6) & 3

            work_dst = 0 if dst_reg == 1 else dst_reg
            s_mask = 2 if work_dst != 2 else 0

            tramp_va = IMAGE_BASE + tramp_cur_rva
            tcode = bytearray()
            tcode.append(0x9C)                  # pushfq (rsp_delta = 8)
            tcode.extend(encode_push(s_mask))   # push s_mask (rsp_delta = 16)
            if dst_reg == 1:
                tcode.extend(encode_push(work_dst))  # push %rax (rsp_delta = 24)
            else:
                tcode.extend(encode_push(1))         # push %rcx (rsp_delta = 24)
            tcode.extend(encode_push(idx_reg))  # push idx_reg (rsp_delta = 32)

            if mod == 3:
                src_reg = (modrm & 7) | (8 if B else 0)
                tcode.extend(encode_mov_reg_reg(work_dst, src_reg, is_64=bool(W)))
            else:
                w_R = 1 if work_dst >= 8 else 0
                w_modrm = (modrm & 0xC7) | ((work_dst & 7) << 3)
                tcode.extend(emit_mem_op_from_vex_tail(0x8B, w_R, X, B, W, bytes([w_modrm]) + tail[1:], rsp_delta=32))

            tcode.extend(encode_pop(1))         # pop %rcx (idx_reg)
            tcode.extend(bytes([0x80, 0xF9, 64 if W else 32]))  # cmp $64/$32, %cl

            bbody = bytearray()
            bbody.extend(encode_modrm_op(0xC7, 0, s_mask, is_64=True) + b'\xFF\xFF\xFF\xFF')
            bbody.extend(encode_modrm_op(0xD3, 4, s_mask, is_64=bool(W)))
            bbody.extend(encode_modrm_op(0xF7, 2, s_mask, is_64=bool(W)))
            bbody.extend(encode_modrm_op(0x21, s_mask, work_dst, is_64=bool(W)))

            tcode.extend(bytes([0x73, len(bbody)]))  # jae +len(bbody)
            tcode.extend(bbody)

            if dst_reg == 1:
                tcode.extend(encode_mov_reg_reg(1, work_dst, is_64=bool(W)))
                tcode.extend(encode_pop(work_dst))
            else:
                tcode.extend(encode_pop(1))
            tcode.extend(encode_pop(s_mask))
            tcode.append(0x9D)  # popfq

            ret_va = va + len(raw)
            rel_back = ret_va - (tramp_va + len(tcode) + 5)
            tcode.extend(b'\xE9' + struct.pack('<i', rel_back))
            write_tramp(tcode)
            patch_at_va(va, make_jmp_rel32(va, tramp_va, len(raw)))
            stats['bzhi_tramp'] += 1
            continue

        if mnem == 'movbe':
            if len(raw) < 5:
                # Leave 4-byte MOVBE instructions untouched so we never overwrite adjacent instructions;
                # runtime/avx2_bmi2_veh.c emulates 4-byte MOVBE seamlessly.
                stats['movbe_4byte_veh'] += 1
                continue

            if len(raw) == 5 and raw[0] == 0x0F and raw[1] == 0x38 and raw[2] == 0xF0:
                modrm = raw[3]
                dst_reg = (modrm >> 3) & 7
                mem_tail = raw[3:]
                new_bytes = bytes([0x8B]) + mem_tail + bytes([0x0F, 0xC8 + dst_reg])
                patch_at_va(va, new_bytes)
                stats['movbe_load_inplace'] += 1
                continue

            tramp_va = IMAGE_BASE + tramp_cur_rva
            tcode = bytearray()

            pos = 0
            has_66 = False
            rex = 0x40
            if raw[pos] == 0x66:
                has_66 = True
                pos += 1
            if 0x40 <= raw[pos] <= 0x4F:
                rex = raw[pos]
                pos += 1
            opc_dir = raw[pos + 2]
            tail = raw[pos + 3:]
            modrm = tail[0]
            W = 1 if (rex & 0x08) else 0
            R = 1 if (rex & 0x04) else 0
            reg = ((modrm >> 3) & 7) | (8 if R else 0)

            if opc_dir == 0xF0:  # load
                if has_66:
                    tcode.append(0x9C)  # pushfq (rsp_delta = 8)
                    l_rex, adj_tail = adjust_rsp_disp_in_mem_tail(rex, tail, 8)
                    tcode.append(0x66)
                    if l_rex != 0x40:
                        tcode.append(l_rex)
                    tcode.append(0x8B)
                    tcode.extend(adj_tail)
                    tcode.extend(encode_bswap(reg, is_64=False, is_16=True))
                    tcode.append(0x9D)  # popfq
                else:
                    l_rex, adj_tail = adjust_rsp_disp_in_mem_tail(rex, tail, 0)
                    if l_rex != 0x40:
                        tcode.append(l_rex)
                    tcode.append(0x8B)
                    tcode.extend(adj_tail)
                    tcode.extend(encode_bswap(reg, is_64=bool(W), is_16=False))
            else:  # store (0xF1)
                B_bit = 1 if (rex & 0x01) else 0
                X_bit = 1 if (rex & 0x02) else 0
                if (modrm & 7) == 4:
                    sib = tail[1]
                    base_reg = (sib & 7) | (8 if B_bit else 0)
                    idx_reg = ((sib >> 3) & 7) | (8 if X_bit else 0)
                else:
                    base_reg = (modrm & 7) | (8 if B_bit else 0)
                    idx_reg = -1
                s_reg = 0
                while s_reg in (base_reg, idx_reg, 4):
                    s_reg += 1

                tcode.append(0x9C)  # pushfq (rsp_delta = 8)
                tcode.extend(encode_push(s_reg))  # push s_reg (rsp_delta = 16)
                tcode.extend(encode_mov_reg_reg(s_reg, reg, is_64=bool(W)))
                tcode.extend(encode_bswap(s_reg, is_64=bool(W), is_16=has_66))

                s_rex = (rex & ~0x04) | (0x04 if s_reg >= 8 else 0)
                s_modrm = (modrm & 0xC7) | ((s_reg & 7) << 3)
                s_tail = bytes([s_modrm]) + tail[1:]
                s_rex, adj_s_tail = adjust_rsp_disp_in_mem_tail(s_rex, s_tail, 16)
                if has_66:
                    tcode.append(0x66)
                if s_rex != 0x40:
                    tcode.append(s_rex)
                tcode.append(0x89)
                tcode.extend(adj_s_tail)
                tcode.extend(encode_pop(s_reg))
                tcode.append(0x9D)  # popfq

            ret_va = va + len(raw)
            rel_back = ret_va - (tramp_va + len(tcode) + 5)
            tcode.extend(b'\xE9' + struct.pack('<i', rel_back))
            write_tramp(tcode)
            patch_at_va(va, make_jmp_rel32(va, tramp_va, len(raw)))
            stats['movbe_tramp'] += 1
            continue

    print(f"[v3.1] Patch stats: {stats}")
    print(f"[v3.1] Trampoline bytes used: {tramp_cur_rva - tramp_vaddr} / {0x100000}")
    with open(dll_path, 'wb') as f:
        f.write(data)
    print(f"[v3.1] Successfully patched {dll_path} ({len(data)} bytes)")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: patch_mhgugame_dll.py <path_to_mhgugame.dll>")
        sys.exit(1)
    patch_mhgugame_dll(sys.argv[1])
