#!/usr/bin/env python3
"""Genera, en el directorio de build, las copias Vita de fuentes de rePOP.

El submódulo upstream/rePOP no se edita. En la Vita (ARM de 32 bits, sin mmap)
la memoria guest pasa por la MMU por software de compat/vvm.h, así que:

  1. recomp_types.h del C generado: XBOX_PTR() traduce con vvm_ptr(). La copia
     se pre-incluye (-include) y su guarda RECOMP_TYPES_H hace que el
     #include "recomp_types.h" del código generado no cargue el original.
  2. Runtime: cada `(uintptr_t)X + g_memory_offset|g_xbox_mem_offset` pasa a
     `(uintptr_t)vvm_ptr((uint32_t)(X) + (uint32_t)OFF)`. Es seguro en todas
     partes: en las zonas de memoria real la tabla da desplazamiento 0.

Uso: vitaize.py <raiz rePOP> <dir salida> <target> <archivo.c relativo a src/>...
Falla (exit 1) si un ancla esperada ya no está: upstream cambió y hay que revisar.
"""
import re
import sys
from pathlib import Path

OFFSETS = ("g_memory_offset", "g_xbox_mem_offset")


def operand_before(text, end):
    """Inicio del operando que termina en text[end-1] (grupo con paréntesis o identificador)."""
    i = end
    while i > 0 and text[i - 1].isspace():
        i -= 1
    if text[i - 1] == ")":
        depth = 0
        j = i - 1
        while j >= 0:
            if text[j] == ")":
                depth += 1
            elif text[j] == "(":
                depth -= 1
                if depth == 0:
                    return j, i
            j -= 1
        raise ValueError("paréntesis sin pareja")
    j = i
    while j > 0:
        c = text[j - 1]
        if c == "]":  # índice: saltar hasta su '['
            j = text.rindex("[", 0, j - 1)
        elif c == ">" and text[j - 2] == "-":
            j -= 2
        elif c.isalnum() or c in "_.":
            j -= 1
        else:
            break
    return j, i


def translate_offsets(text):
    """Reescribe '(uintptr_t)OPERANDO + OFF' -> '(uintptr_t)vvm_ptr((uint32_t)(OPERANDO) + (uint32_t)OFF)'."""
    pat = re.compile(r"\+\s*(" + "|".join(OFFSETS) + r")\b")
    out, pos, count = [], 0, 0
    for m in pat.finditer(text):
        if m.start() < pos:
            continue
        try:
            op_lo, op_hi = operand_before(text, m.start())
        except ValueError:
            continue
        cast = text[:op_lo].rstrip()
        if not cast.endswith("(uintptr_t)"):
            continue  # p.ej. 'uintptr_t x = BASE + g_memory_offset': dirección, no acceso
        cast_lo = len(cast) - len("(uintptr_t)")
        out.append(text[pos:cast_lo])
        operand = text[op_lo:op_hi].strip()
        if not operand.startswith("("):
            operand = "(" + operand + ")"
        out.append("(uintptr_t)vvm_ptr((uint32_t)%s + (uint32_t)%s)" % (operand, m.group(1)))
        pos = m.end()
        count += 1
    out.append(text[pos:])
    text = "".join(out)

    # Forma inversa: '(uintptr_t)OFF + EXPR' hasta ')' o ',' de profundidad 0.
    pat = re.compile(r"\(uintptr_t\)\s*(" + "|".join(OFFSETS) + r")\s*\+")
    out, pos = [], 0
    for m in pat.finditer(text):
        if m.start() < pos:
            continue
        depth, j = 0, m.end()
        while j < len(text):
            c = text[j]
            if c in "([":
                depth += 1
            elif c in ")]":
                if depth == 0:
                    break
                depth -= 1
            elif c in ",;" and depth == 0:
                break
            j += 1
        out.append(text[pos:m.start()])
        out.append("(uintptr_t)vvm_ptr((uint32_t)(%s) + (uint32_t)%s)" % (text[m.end():j].strip(), m.group(1)))
        pos = j
        count += 1
    out.append(text[pos:])
    return "".join(out), count


# Accesos crudos que no tienen la forma '(uintptr_t)X + OFF'. Cada ancla debe existir.
ANCHORS = {
    "xbox_memory_layout.c": [
        # La página de kernel falsa vive en la ventana contigua (memoria aparte en Vita).
        ("            ? (void *)kernel_native\n",
         "            ? (void *)vvm_ptr((uint32_t)kernel_native)\n"),
    ],
    "target_hooks.c": [
        # Que use la copia traducida de guest_probe.h (misma carpeta rt/).
        ('#include "../../port/game/guest_probe.h"', '#include "guest_probe.h"'),
    ],
    # ARM32 no tiene __int128: (a*b)/c exacto con 32x32 (r1 < c < 2^32, sin desbordes).
    "qemu_shim.h": [
        ("    return (uint64_t)(((unsigned __int128)a * b) / c);\n",
         "    uint64_t lo = (a & 0xFFFFFFFFu) * b;\n"
         "    uint64_t hi = (a >> 32) * b + (lo >> 32);\n"
         "    uint64_t q1 = hi / c, r1 = hi % c;\n"
         "    return (q1 << 32) + (((r1 << 32) | (lo & 0xFFFFFFFFu)) / c);\n"),
    ],
    # Encoder GPU de la Vita (src/nv2a_hw_vita.inc) en lugar del de Android
    # (SDL GPU/Vulkan); la parte compartida del .inc se compila igual que allí.
    "nv2a_hw_d3d11.inc": [
        ("#if defined(_WIN32) || defined(__ANDROID__)\n\n/* RE-SoT: Windows builds both encoders",
         "#if defined(_WIN32) || defined(__ANDROID__) || defined(RESOT_HW_VITA)\n\n/* RE-SoT: Windows builds both encoders"),
        ('#include "nv2a_hw_native.inc"\n', '#include "nv2a_hw_vita.inc"\n'),
    ],
    # Stats del flip sin s_hw (build sin backend GPU) y el hilo GL de hosttest.
    "nv2a_pb_exec.c": [
        ('/* RE-SoT: the Direct3D 11 rasteriser, which reads the state above. */\n#include "nv2a_hw_d3d11.inc"\n',
         '/* RE-SoT: the Direct3D 11 rasteriser, which reads the state above. */\n'
         '#include "nv2a_vita_glthread.h"\n#include "nv2a_hw_d3d11.inc"\n'),
        ("xbox_FramebufferWindowFrameStats((uint32_t)(s_hw.draws - draws_at_flip));\n"
         "            draws_at_flip = s_hw.draws;\n",
         "xbox_FramebufferWindowFrameStats(0); (void)draws_at_flip;\n"),
    ],
    "guest_probe.h": [
        ("memcpy(value,(const void *)((uintptr_t)offset+a),sizeof *value);",
         "memcpy(value,vvm_ptr((uint32_t)offset+a),sizeof *value);"),
    ],
    # Pila mínima de cada hilo: en Android 8 MB de espacio reservado; en Vita la
    # pila es memoria física. Un marco de función elevada mide ~140 B en Thumb-2
    # (máx. medido 4 KB), así que 1 MB sobra (Windows corre con 1 MB en x64).
    # Al liberar/descomprometer limpia cada página con H + va crudo (crash en
    # hosttest 2026-10-20: memset en 0xB9765000 = H + 0x742FF000, EXT_VMA).
    "guest_vmem.c": [
        ("            memset((void *)((uintptr_t)s_lo + (uintptr_t)s_offset\n"
         "                            + (size_t)p * VMEM_PAGE), 0, VMEM_PAGE);\n",
         "            memset(vvm_ptr((uint32_t)((uintptr_t)s_lo + (uintptr_t)s_offset\n"
         "                            + (size_t)p * VMEM_PAGE)), 0, VMEM_PAGE);\n"),
    ],
    "win32_compat.c": [
        ("    if (stackSize < (8u << 20))\n        stackSize = 8u << 20;\n",
         "    if (stackSize < (1u << 20))\n        stackSize = 1u << 20;\n"),
    ],
}

# La APU lee la RAM guest como "ram_ptr + dirección": fuera de la RAM baja (la
# ventana contigua 0x80000000, VAs altas) eso no es memoria real en la Vita.
RAM_PTR = re.compile(r"\b(g_apu_ram_ptr|d->ram_ptr)\s*\+")


def translate_ram_ptr(text):
    """'BASE + EXPR' (hasta ')', ',' o ';' de profundidad 0) -> vvm_ptr(BASE + EXPR)."""
    out, pos, count = [], 0, 0
    for m in RAM_PTR.finditer(text):
        if m.start() < pos:
            continue
        depth, j = 0, m.end()
        while j < len(text):
            c = text[j]
            if c in "([":
                depth += 1
            elif c in ")]":
                if depth == 0:
                    break
                depth -= 1
            elif c in ",;" and depth == 0:
                break
            j += 1
        out.append(text[pos:m.start()])
        out.append("((uint8_t *)vvm_ptr((uint32_t)(uintptr_t)%s + (uint32_t)(%s)))"
                   % (m.group(1), text[m.end():j].strip()))
        pos = j
        count += 1
    out.append(text[pos:])
    return "".join(out), count


# DirectSound (xdk/dsound del C generado) maneja los registros de la APU y el
# bus master AC'97 con MEMn(). Upstream los atrapa con fallos de página; la
# Vita no tiene trampas, así que cada acceso entero pasa por MMIO_Rn/MMIO_Wn
# (compat/mmio_vita.h), que deja la RAM igual y manda el rango de la APU al
# modelo. Los flotantes (MEMF/MEMD) nunca tocan registros: quedan igual.
MMIO_MEM = re.compile(r"\b(S?)MEM(8|16|32)\(")


def _close_paren(text, i):
    depth = 1
    while depth:
        if i >= len(text):
            raise ValueError("paréntesis sin pareja")
        depth += {"(": 1, ")": -1}.get(text[i], 0)
        i += 1
    return i


def trap_mmio(text):
    out, pos, count = [], 0, 0
    for m in MMIO_MEM.finditer(text):
        if m.start() < pos:
            continue
        end = _close_paren(text, m.end())
        addr, na = trap_mmio(text[m.end():end - 1])
        count += na
        bits = m.group(2)
        j = end
        while j < len(text) and text[j] in " \t":
            j += 1
        if text[j:j + 1] == "=" and text[j + 1:j + 2] != "=":
            k, depth = j + 1, 0
            while text[k] != ";" or depth:
                depth += {"(": 1, ")": -1}.get(text[k], 0)
                k += 1
            value, nv = trap_mmio(text[j + 1:k].strip())
            count += nv
            out.append(text[pos:m.start()])
            out.append("MMIO_W%s(%s, %s)" % (bits, addr, value))
            pos = k
        elif re.match(r"(<<|>>|[-+*/&|^%])=|\+\+|--", text[j:j + 3]):
            raise ValueError("vitaize: escritura compuesta no soportada: %r" % text[m.start():j + 3])
        else:
            cast = "(int%s_t)" % bits if m.group(1) else ""
            out.append(text[pos:m.start()])
            out.append("%sMMIO_R%s(%s)" % (cast, bits, addr))
            pos = end
        count += 1
    out.append(text[pos:])
    return "".join(out), count


def write_if_changed(dst, text):
    """Sin tocar el mtime si nada cambió: si no, cada configure recompila todo gen/."""
    if dst.exists() and dst.read_text(encoding="utf-8", errors="surrogateescape") == text:
        return
    dst.write_text(text, encoding="utf-8", errors="surrogateescape")


def patch_recomp_types(src, dst):
    text = src.read_text(encoding="utf-8")
    old = "#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)"
    if old not in text:
        sys.exit("vitaize: ancla XBOX_PTR no encontrada en %s" % src)
    text = text.replace(old, "#include \"vvm.h\"\n"
                        "#define XBOX_PTR(addr) ((uintptr_t)vvm_ptr((uint32_t)(addr) + (uint32_t)g_xbox_mem_offset))")
    write_if_changed(dst, text)


def self_check():
    t, n = translate_offsets("*(volatile uint32_t *)((uintptr_t)(dev + off) + g_memory_offset);")
    assert n == 1 and "vvm_ptr((uint32_t)(dev + off) + (uint32_t)g_memory_offset)" in t, t
    t, n = translate_offsets("memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);")
    assert n == 1 and "vvm_ptr((uint32_t)(xbox_va)" in t, t
    t, n = translate_offsets("uintptr_t contig_native = XBOX_CONTIG_BASE + g_memory_offset;")
    assert n == 0, t
    t, n = translate_offsets("x = (uintptr_t)g_fence[i].device_ptr_va + g_memory_offset;")
    assert n == 1 and "vvm_ptr((uint32_t)(g_fence[i].device_ptr_va)" in t, t
    t, n = translate_offsets("arg = (float *)((uintptr_t)g_xbox_mem_offset + g_esp + 4);")
    assert n == 1 and "vvm_ptr((uint32_t)(g_esp + 4) + (uint32_t)g_xbox_mem_offset))" in t, t
    t, n = translate_offsets("f(a, *(volatile uint16_t *)((uintptr_t)g_xbox_mem_offset + fb + 0x42),")
    assert n == 1 and "vvm_ptr((uint32_t)(fb + 0x42)" in t, t
    t, n = translate_ram_ptr("    routine = *(uint32_t *)(g_apu_ram_ptr + kint + 0);")
    assert n == 1 and "*(uint32_t *)(((uint8_t *)vvm_ptr((uint32_t)(uintptr_t)g_apu_ram_ptr + (uint32_t)(kint + 0))));" in t, t
    t, n = translate_ram_ptr("    return g_apu_ram_ptr + 0x80000000u + a;\n x = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);")
    assert n == 2 and "(uint32_t)(0x80000000u + a)));" in t and "(uint32_t)(s_dsp_ack[i])))" in t, t
    t, n = trap_mmio("    MEM32(ecx + 4) = MEM32(eax + MEM8(esi));\n    eax = ZX8(MEM8(XBOX_FS_BASE + 0x24));")
    assert n == 4 and t == ("    MMIO_W32(ecx + 4, MMIO_R32(eax + MMIO_R8(esi)));\n"
                            "    eax = ZX8(MMIO_R8(XBOX_FS_BASE + 0x24));"), t
    t, n = trap_mmio("if (MEM16(a) == 3) x = (double)SMEM32(ebp + 8); MEMF(esp) = 1.0f;")
    assert n == 2 and t == "if (MMIO_R16(a) == 3) x = (double)(int32_t)MMIO_R32(ebp + 8); MEMF(esp) = 1.0f;", t
    t, n = trap_mmio("for (_i = 0; _i < ecx; _i++) MEM8(edi + _i*1u) = MEM8(esi + _i*1u); }")
    assert n == 2 and t == "for (_i = 0; _i < ecx; _i++) MMIO_W8(edi + _i*1u, MMIO_R8(esi + _i*1u)); }", t
    try:
        trap_mmio("MEM32(a) |= 1;")
        assert False, "escritura compuesta aceptada"
    except ValueError:
        pass


def main():
    self_check()
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    root, out, target = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    out.mkdir(parents=True, exist_ok=True)
    patch_recomp_types(root / "source" / target / "gen" / "recomp_types.h", out / "recomp_types.h")
    src_root = root / "port" / "third_party" / "xboxrecomp" / "src"
    for rel in sys.argv[4:]:
        src = (root / rel) if rel.startswith(("port/", "source/")) else (src_root / rel)
        text = src.read_text(encoding="utf-8", errors="surrogateescape")
        if "/gen/" in rel:
            # C generado de DirectSound: accesos enteros por los accesores MMIO;
            # el include relativo de recomp_funcs.h apunta al original.
            inc = '#include "../../recomp_funcs.h"\n'
            if inc not in text:
                sys.exit("vitaize: ancla no encontrada en %s: %r" % (src, inc))
            text, n = trap_mmio(text.replace(
                inc, '#include "%s"\n#include "mmio_vita.h"\n' % (src.parent / inc[10:-2]).resolve()))
            dst = out / "gen" / src.name
            dst.parent.mkdir(parents=True, exist_ok=True)
            write_if_changed(dst, "#line 1 \"%s\"\n%s" % (src, text))
            print("vitaize: %-28s %3d accesos MMIO" % (src.name, n))
            continue
        text, n = translate_offsets(text)
        if src.name.startswith("apu_"):
            text, k = translate_ram_ptr(text)
            n += k
        if src.name == "scene.c":
            # Lecturas del estado de Jade para los logs: "(base + EXPR)" con base =
            # g_xbox_mem_offset (crash en hosttest 2026-10-20: read_name en EXT_VMA).
            text, k = re.subn(r"\(base \+ ([^()]+)\)",
                              r"((uintptr_t)vvm_ptr((uint32_t)base + (uint32_t)(\1)))", text)
            if k < 12:
                sys.exit("vitaize: scene.c cambió (%d accesos 'base +', se esperaban 12+)" % k)
            n += k
        for old, new in ANCHORS.get(src.name, []):
            if old not in text:
                sys.exit("vitaize: ancla no encontrada en %s: %r" % (src, old))
            text = text.replace(old, new)
        # qemu_shim.h va a nv2a/: apu_shim.h (copiado a rt/) lo incluye como "../nv2a/qemu_shim.h".
        dst = out / ("nv2a" if src.name == "qemu_shim.h" else "rt") / src.name
        dst.parent.mkdir(parents=True, exist_ok=True)
        write_if_changed(dst, "#include \"vvm.h\"\n#line 1 \"%s\"\n%s" % (src, text))
        print("vitaize: %-28s %3d accesos traducidos" % (src.name, n))


if __name__ == "__main__":
    main()
