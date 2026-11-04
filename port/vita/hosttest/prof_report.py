#!/usr/bin/env python3
"""Informe del perfilador del hosttest: muestras (pc, lr) del hilo game_main -> funciones.

    python3 -I prof_report.py <prof_game_main.txt> <pop-sot.nm> <gen_dir> [top]
    python3 -I prof_report.py --self-check

<pop-sot.nm> = `arm-linux-gnueabihf-nm -n -S --defined-only pop-sot` (lo genera `run.sh prof`).
"Propio" = el pc cae en la función. "Con ayudantes" = además las muestras en funciones que no son
del juego (vvm_ptr, __emutls_*, libm...) llamadas directamente desde ella (por el lr): es lo que se
ahorra al reescribir esa función en C nativo. Fuera del binario = libc/esperas (KickOff, sleeps).
"""
import bisect
import os
import re
import sys

GEN_DEF = re.compile(r"^void (sub_[0-9A-F]{8})(?:_gen)?\(void\)", re.M)


def load_nm(lines):
    syms = []  # (inicio, fin, nombre)
    for line in lines:
        p = line.split()
        if len(p) == 4 and p[2] in "tTwW":
            start = int(p[0], 16) & ~1
            syms.append((start, start + int(p[1], 16), p[3]))
    syms.sort()
    return syms


def load_prof(lines):
    anchor, samples = None, []
    for line in lines:
        if line.startswith("# anchor"):
            anchor = (line.split()[2], int(line.split()[3], 16))
        elif line.strip() and not line.startswith("#"):
            pc, lr, n = line.split()
            samples.append((int(pc, 16), int(lr, 16) & ~1, int(n)))
    return anchor, samples


def module_index(gen_dir):
    index = {}
    for root, _, files in os.walk(gen_dir):
        for f in files:
            if f.endswith(".c") and not f.startswith("._"):
                with open(os.path.join(root, f), errors="replace") as fh:
                    for name in GEN_DEF.findall(fh.read()):
                        index[name] = os.path.relpath(root, gen_dir)
    return index


def report(anchor, samples, syms, modules, top=60):
    starts = [s[0] for s in syms]
    by_name = {s[2]: s[0] for s in syms}
    delta = anchor[1] - by_name[anchor[0]] if anchor else 0

    def func(addr):
        a = addr - delta
        i = bisect.bisect_right(starts, a) - 1
        return syms[i][2] if i >= 0 and a < syms[i][1] else None

    game = lambda n: n is not None and n.startswith("sub_")
    base = lambda n: n[:-4] if n.endswith("_gen") else n
    total = sum(n for _, _, n in samples) or 1
    self_t, incl, helpers, outside = {}, {}, {}, 0
    for pc, lr, n in samples:
        f = func(pc)
        if f is None:
            outside += n
            continue
        if game(f):
            f = base(f)
            self_t[f] = self_t.get(f, 0) + n
            incl[f] = incl.get(f, 0) + n
            continue
        helpers[f] = helpers.get(f, 0) + n
        caller = func(lr)
        if game(caller):
            caller = base(caller)
            incl[caller] = incl.get(caller, 0) + n
    pct = lambda n: f"{100.0 * n / total:5.2f}"
    out = [f"# Perfil de game_main (hosttest)\n",
           f"Muestras: {total} · código del juego (`sub_*`): {pct(sum(self_t.values()))} % · "
           f"ayudantes del runtime: {pct(sum(helpers.values()))} % · fuera del binario (libc/esperas): {pct(outside)} %\n",
           "## Funciones del juego, por tiempo con ayudantes\n",
           "| # | Función | % con ayudantes | % propio | Módulo |", "|---:|---|---:|---:|---|"]
    acc = 0
    for i, (f, n) in enumerate(sorted(incl.items(), key=lambda kv: -kv[1])[:top], 1):
        acc += n
        out.append(f"| {i} | `{f}` | {pct(n)} | {pct(self_t.get(f, 0))} | {modules.get(f, '?')} |")
    out.append(f"\nLas {min(top, len(incl))} primeras suman {pct(acc)} % del hilo.\n")
    out += ["## Ayudantes del runtime (candidatos de C9)\n", "| Función | % |", "|---|---:|"]
    for f, n in sorted(helpers.items(), key=lambda kv: -kv[1])[:25]:
        out.append(f"| `{f}` | {pct(n)} |")
    out.append("\n_CPU armv7 emulada: el orden vale para elegir; los % se confirman en consola._")
    return "\n".join(out) + "\n"


def self_check():
    syms = load_nm(["00001000 00000100 T sub_00011000", "00001100 00000080 T sub_00022000_gen",
                    "00001181 00000020 T vvm_ptr", "00002000 00000010 t hosttest_prof_dump"])
    # PIE cargado en +0x400000: el ancla da el desplazamiento
    anchor, samples = load_prof(["# anchor hosttest_prof_dump 0x00402000 samples 9 lost 0",
                                 "00401010 00000000 4", "00401110 00000000 2",
                                 "00401184 00401051 3", "b6f00000 00401011 1"])
    text = report(anchor, samples, syms, {"sub_00011000": "engine"})
    assert "| 1 | `sub_00011000` | 70.00 | 40.00 | engine |" in text, text
    assert "| 2 | `sub_00022000` | 20.00 | 20.00 | ? |" in text, text
    assert "fuera del binario (libc/esperas): 10.00 %" in text, text
    assert "| `vvm_ptr` | 30.00 |" in text, text
    print("self-check OK")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-check"]:
        self_check()
        sys.exit(0)
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    with open(sys.argv[1]) as f:
        anchor, samples = load_prof(f)
    with open(sys.argv[2]) as f:
        syms = load_nm(f)
    sys.stdout.write(report(anchor, samples, syms, module_index(sys.argv[3]),
                            int(sys.argv[4]) if len(sys.argv) > 4 else 60))
