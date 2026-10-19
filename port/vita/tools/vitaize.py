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
    # Sin backend GPU (el ".inc" deja hw_active() = 0) s_hw no existe; el upstream
    # solo compila esto en Windows/Android.
    "nv2a_pb_exec.c": [
        ("xbox_FramebufferWindowFrameStats((uint32_t)(s_hw.draws - draws_at_flip));\n"
         "            draws_at_flip = s_hw.draws;\n",
         "xbox_FramebufferWindowFrameStats(0); (void)draws_at_flip;\n"),
    ],
    "guest_probe.h": [
        ("memcpy(value,(const void *)((uintptr_t)offset+a),sizeof *value);",
         "memcpy(value,vvm_ptr((uint32_t)offset+a),sizeof *value);"),
    ],
}


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
        text, n = translate_offsets(src.read_text(encoding="utf-8", errors="surrogateescape"))
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
