#!/usr/bin/env python3
"""
Full AOT Static Binary Translator (v3) for MHGURecomp on AMD A10-7800 (bdver3).
Appends an executable .tramp section (RVA 0x057af000) to mhgugame.dll and statically translates
all 19,953 non-bdver3 instructions (10,068 rorx, 8,832 shlx/shrx/sarx, 971 movbe, 57 bzhi,
23 vpbroadcastd, 2 vpbroadcastb) so the game executes with ZERO runtime #UD exceptions.
"""
import re
import shutil
import struct
import subprocess
import sys

IMAGE_BASE = 0x260000000


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
    raise ValueError(f"VA {hex(va)} not found in any section")


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
    if dst == src and not is_64:
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
BRANCH_RE = re.compile(r'\b(?:j[a-z]+|callq?)\s+(?:0x)?([0-9a-fA-F]+)\s*<')


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
    print(f"[v3] Disassembling {dll_path} using {objdump_bin} + grep...")
    cmd = f'"{objdump_bin}" -d --insn-width=16 "{dll_path}" | grep -B 40 -A 5 -E "rorx|shlx|sarx|shrx|movbe|mulx|bzhi|pext|pdep|vpbroadcastd|vpbroadcastb"'
    proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, text=True, bufsize=1024 * 1024)

    parsed_window = []
    branch_targets = set()

    for line in proc.stdout:
        p = parse_objdump_line(line)
        if p is None:
            continue
        va, raw, disasm = p
        if '<' in disasm:
            bm = BRANCH_RE.search(disasm)
            if bm:
                branch_targets.add(int(bm.group(1), 16))
        parsed_window.append(p)

    proc.wait()

    records = []
    for i, item in enumerate(parsed_window):
        if TARGET_RE.search(item[2]):
            prev_i = parsed_window[i - 1] if i > 0 else None
            next_i = parsed_window[i + 1] if i + 1 < len(parsed_window) else None
            records.append((prev_i, item, next_i))

    global IMAGE_BASE
    if records:
        IMAGE_BASE = records[0][1][0] & ~0x0FFFFFFF
    print(f"[v3] Found {len(records)} non-bdver3 sites, {len(branch_targets)} nearby branch targets, IMAGE_BASE={hex(IMAGE_BASE)}.")

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

    patched_vas = set()
    stats = {
        'vpbroadcastd_inplace': 0,
        'vpbroadcastb_tramp': 0,
        'movbe_load_inplace': 0,
        'movbe_tramp': 0,
        'rorx_tramp': 0,
        'shiftx_tramp': 0,
        'bzhi_tramp': 0,
    }

    for prev_insn, cur_insn, next_insn in records:
        va, raw, disasm = cur_insn
        if va in patched_vas:
            continue
        mnem = disasm.split()[0]

        if mnem == 'vpbroadcastd':
            new_raw = bytearray(raw)
            new_raw[3] = 0x18
            patch_at_va(va, new_raw)
            patched_vas.add(va)
            stats['vpbroadcastd_inplace'] += 1
            continue

        if mnem == 'vpbroadcastb':
            tramp_va = IMAGE_BASE + tramp_cur_rva
            tcode = bytearray()
            if (va - IMAGE_BASE) == 0x1dc6de:
                tcode.extend(bytes.fromhex('c5f96ec0c5f970c000'))
            elif (va - IMAGE_BASE) == 0x654a10:
                tcode.extend(bytes.fromhex('c4c1796ec0c5f970c000'))
            ret_va = va + len(raw)
            rel_back = ret_va - (tramp_va + len(tcode) + 5)
            tcode.extend(b'\xE9' + struct.pack('<i', rel_back))
            write_tramp(tcode)
            patch_at_va(va, make_jmp_rel32(va, tramp_va, len(raw)))
            patched_vas.add(va)
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
            tcode.append(0x9C)

            if mod == 3:
                src_reg = (modrm & 7) | (8 if B else 0)
                tcode.extend(encode_mov_reg_reg(dst_reg, src_reg, is_64=bool(W)))
            else:
                tcode.extend(emit_mem_op_from_vex_tail(0x8B, R, X, B, W, mem_or_reg_tail, rsp_delta=8))

            tcode.extend(encode_modrm_op(0xC1, 1, dst_reg, is_64=bool(W)) + bytes([imm8]))
            tcode.append(0x9D)
            ret_va = va + len(raw)
            rel_back = ret_va - (tramp_va + len(tcode) + 5)
            tcode.extend(b'\xE9' + struct.pack('<i', rel_back))
            write_tramp(tcode)
            patch_at_va(va, make_jmp_rel32(va, tramp_va, len(raw)))
            patched_vas.add(va)
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
            tcode.append(0x9C)

            if dst_reg == 1:
                scratch = 0 if cnt_reg != 0 else 2
                tcode.extend(encode_push(scratch))
                if mod == 3:
                    src_reg = (modrm & 7) | (8 if B else 0)
                    tcode.extend(encode_mov_reg_reg(scratch, src_reg, is_64=bool(W)))
                else:
                    s_R = 1 if scratch >= 8 else 0
                    s_modrm = (modrm & 0xC7) | ((scratch & 7) << 3)
                    tcode.extend(emit_mem_op_from_vex_tail(0x8B, s_R, X, B, W, bytes([s_modrm]) + tail[1:], rsp_delta=16))
                if cnt_reg != 1:
                    tcode.extend(encode_mov_reg_reg(1, cnt_reg, is_64=False))
                tcode.extend(encode_modrm_op(0xD3, shift_subop, scratch, is_64=bool(W)))
                tcode.extend(encode_mov_reg_reg(1, scratch, is_64=bool(W)))
                tcode.extend(encode_pop(scratch))
            else:
                tcode.extend(encode_push(1))
                if cnt_reg == dst_reg:
                    tcode.extend(encode_mov_reg_reg(1, cnt_reg, is_64=False))
                    if mod == 3:
                        src_reg = (modrm & 7) | (8 if B else 0)
                        tcode.extend(encode_mov_reg_reg(dst_reg, src_reg, is_64=bool(W)))
                    else:
                        tcode.extend(emit_mem_op_from_vex_tail(0x8B, R, X, B, W, tail, rsp_delta=16))
                else:
                    if mod == 3:
                        src_reg = (modrm & 7) | (8 if B else 0)
                        tcode.extend(encode_mov_reg_reg(dst_reg, src_reg, is_64=bool(W)))
                    else:
                        tcode.extend(emit_mem_op_from_vex_tail(0x8B, R, X, B, W, tail, rsp_delta=16))
                    if cnt_reg != 1:
                        tcode.extend(encode_mov_reg_reg(1, cnt_reg, is_64=False))
                tcode.extend(encode_modrm_op(0xD3, shift_subop, dst_reg, is_64=bool(W)))
                tcode.extend(encode_pop(1))

            tcode.append(0x9D)
            ret_va = va + len(raw)
            rel_back = ret_va - (tramp_va + len(tcode) + 5)
            tcode.extend(b'\xE9' + struct.pack('<i', rel_back))
            write_tramp(tcode)
            patch_at_va(va, make_jmp_rel32(va, tramp_va, len(raw)))
            patched_vas.add(va)
            stats['shiftx_tramp'] += 1
            continue

        if mnem == 'bzhi':
            _, R, X, B, W, vvvv, _, _, tail = parse_vex(raw)
            modrm = tail[0]
            dst_reg = ((modrm >> 3) & 7) | (8 if R else 0)
            idx_reg = vvvv
            mod = (modrm >> 6) & 3

            scratch = 0
            while scratch in (dst_reg, idx_reg, 4):
                scratch += 1

            tramp_va = IMAGE_BASE + tramp_cur_rva
            tcode = bytearray()
            tcode.append(0x9C)
            tcode.extend(encode_push(scratch))
            tcode.extend(encode_push(1))

            tcode.extend(encode_mov_reg_reg(1, idx_reg, is_64=False))
            tcode.extend(bytes([0x80, 0xE1, 0xFF]))

            if mod == 3:
                src_reg = (modrm & 7) | (8 if B else 0)
                if src_reg == 1:
                    tcode.extend(bytes([0x48, 0x8B, 0x0C, 0x24]))
                    tcode.extend(encode_mov_reg_reg(dst_reg, 1, is_64=bool(W)))
                    tcode.extend(encode_mov_reg_reg(1, idx_reg, is_64=False))
                else:
                    tcode.extend(encode_mov_reg_reg(dst_reg, src_reg, is_64=bool(W)))
            else:
                tcode.extend(emit_mem_op_from_vex_tail(0x8B, R, X, B, W, tail, rsp_delta=24))

            tcode.extend(bytes([0x80, 0xF9, 64 if W else 32]))
            tcode.extend(bytes([0x73, 14 if W else 12]))
            tcode.extend(bytes([0x48, 0xC7, 0xC0 + scratch, 0xFF, 0xFF, 0xFF, 0xFF]))
            tcode.extend(encode_modrm_op(0xD3, 4, scratch, is_64=bool(W)))
            tcode.extend(encode_modrm_op(0xF7, 2, scratch, is_64=bool(W)))
            tcode.extend(encode_modrm_op(0x21, scratch, dst_reg, is_64=bool(W)))

            tcode.extend(encode_pop(1))
            tcode.extend(encode_pop(scratch))
            tcode.append(0x9D)
            ret_va = va + len(raw)
            rel_back = ret_va - (tramp_va + len(tcode) + 5)
            tcode.extend(b'\xE9' + struct.pack('<i', rel_back))
            write_tramp(tcode)
            patch_at_va(va, make_jmp_rel32(va, tramp_va, len(raw)))
            patched_vas.add(va)
            stats['bzhi_tramp'] += 1
            continue

        if mnem == 'movbe':
            if len(raw) == 5 and raw[0] == 0x0F and raw[1] == 0x38 and raw[2] == 0xF0:
                modrm = raw[3]
                dst_reg = (modrm >> 3) & 7
                mem_tail = raw[3:]
                new_bytes = bytes([0x8B]) + mem_tail + bytes([0x0F, 0xC8 + dst_reg])
                patch_at_va(va, new_bytes)
                patched_vas.add(va)
                stats['movbe_load_inplace'] += 1
                continue

            insns_to_emulate = [(va, raw, disasm)]
            total_len = len(raw)
            patch_start_va = va

            if total_len == 4:
                next_va, next_raw, next_disasm = next_insn
                prev_va, prev_raw, prev_disasm = prev_insn
                if next_va not in branch_targets:
                    insns_to_emulate.append((next_va, next_raw, next_disasm))
                    total_len += len(next_raw)
                    patched_vas.add(next_va)
                elif prev_disasm.startswith('movbe') and (prev_va + len(prev_raw) == va):
                    patch_start_va = prev_va
                    insns_to_emulate = [(prev_va, prev_raw, prev_disasm), (va, raw, disasm)]
                    total_len = len(prev_raw) + len(raw)
                    patched_vas.add(prev_va)
                else:
                    modrm = raw[3]
                    src_reg = (modrm >> 3) & 7
                    rm_reg = modrm & 7
                    inplace_bytes = bytes([0x0F, 0xC8 + src_reg, 0x89, 0x00 | (src_reg << 3) | rm_reg])
                    patch_at_va(va, inplace_bytes)
                    patched_vas.add(va)
                    stats['movbe_load_inplace'] += 1
                    continue

            tramp_va = IMAGE_BASE + tramp_cur_rva
            tcode = bytearray()

            for _, iraw, idisasm in insns_to_emulate:
                if not idisasm.startswith('movbe'):
                    tcode.extend(iraw)
                    continue

                pos = 0
                has_66 = False
                rex = 0x40
                if iraw[pos] == 0x66:
                    has_66 = True
                    pos += 1
                if 0x40 <= iraw[pos] <= 0x4F:
                    rex = iraw[pos]
                    pos += 1
                opc_dir = iraw[pos + 2]
                tail = iraw[pos + 3:]
                modrm = tail[0]
                W = 1 if (rex & 0x08) else 0
                R = 1 if (rex & 0x04) else 0
                reg = ((modrm >> 3) & 7) | (8 if R else 0)

                if opc_dir == 0xF0:
                    _, adj_tail = adjust_rsp_disp_in_mem_tail(rex, tail, 0)
                    if has_66:
                        tcode.append(0x66)
                    if rex != 0x40:
                        tcode.append(rex)
                    tcode.append(0x8B)
                    tcode.extend(adj_tail)
                    if has_66:
                        tcode.append(0x9C)
                        tcode.extend(encode_bswap(reg, is_64=False, is_16=True))
                        tcode.append(0x9D)
                    else:
                        tcode.extend(encode_bswap(reg, is_64=bool(W), is_16=False))
                else:
                    if has_66:
                        tcode.append(0x9C)
                        tcode.extend(encode_bswap(reg, is_64=False, is_16=True))
                        tcode.append(0x66)
                        if rex != 0x40:
                            tcode.append(rex)
                        tcode.append(0x89)
                        tcode.extend(tail)
                        tcode.extend(encode_bswap(reg, is_64=False, is_16=True))
                        tcode.append(0x9D)
                    else:
                        tcode.extend(encode_bswap(reg, is_64=bool(W), is_16=False))
                        if rex != 0x40:
                            tcode.append(rex)
                        tcode.append(0x89)
                        tcode.extend(tail)
                        tcode.extend(encode_bswap(reg, is_64=bool(W), is_16=False))

            ret_va = patch_start_va + total_len
            rel_back = ret_va - (tramp_va + len(tcode) + 5)
            tcode.extend(b'\xE9' + struct.pack('<i', rel_back))
            write_tramp(tcode)
            patch_at_va(patch_start_va, make_jmp_rel32(patch_start_va, tramp_va, total_len))
            patched_vas.add(va)
            stats['movbe_tramp'] += 1
            continue

    print(f"[v3] Patch stats: {stats}")
    print(f"[v3] Trampoline bytes used: {tramp_cur_rva - tramp_vaddr} / {0x100000}")
    with open(dll_path, 'wb') as f:
        f.write(data)
    print(f"[v3] Successfully patched {dll_path} ({len(data)} bytes)")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: patch_mhgugame_dll.py <path_to_mhgugame.dll>")
        sys.exit(1)
    patch_mhgugame_dll(sys.argv[1])
