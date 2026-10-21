#!/usr/bin/env python3
"""Copia del disco Xbox NTSC de Prince of Persia: The Sands of Time solo lo que
el port de PS Vita lee (como scripts/extract-game-data.py de nfsmw-vita).

Uso:
    python3 -I extract_game_data.py DISCO DESTINO [--all-languages]

DISCO es la XISO NTSC extraída (default.xbe con el SHA-256 de rePOP).
DESTINO es una carpeta local (copiarla entera a ux0:data/pop-sot/) o
ftp://IP:1337 para subir directo a la Vita con VitaShell en modo FTP.
Lo que ya está con el mismo tamaño se salta: se puede relanzar.

Fuera (2,4 GB -> ~1,6 GB):
  - POP1/, POP2/: los clásicos de bonus, con su propio XBE (no recompilado).
  - update.xbe, dashupdate.xbe: actualizaciones del dashboard de la Xbox.
  - Variantes de idioma 1..4 (vídeos CINE_90x?, POPLOGO?, POP_IN?, POP_OUT?,
    soundx?.big/fat). El kernel del runtime contesta inglés (kernel_xbox.c,
    XC_LANGUAGE) y el juego abre la variante 0: soundx0, poplogo0, pop_in0,
    pop_out0 en los logs (2026-10-21). CINE_9010 en lugar de 9011/9012 es
    hipótesis por analogía (aún no se llegó a esas cinemáticas):
    --all-languages lo copia todo si falta alguna.

Solo lee el disco; no ejecuta nada de él.
"""
import ftplib
import hashlib
import re
import sys
from pathlib import Path

XBE_SHA256 = "753C58B191279BF4D1A8B28A936DF36AA222ED566487AB1FBDFCD090E91EE379"
SKIP_DIRS = {"POP1", "POP2"}
SKIP_FILES = {"UPDATE.XBE", "DASHUPDATE.XBE"}
LANG_VARIANT = re.compile(r"^(CINE_90\d|POPLOGO|POP_IN|POP_OUT|SOUNDX)([1-9])\.(INT|BIG|FAT)$", re.I)


def wanted(rel, all_languages=False):
    parts = rel.split("/")
    if parts[-1].startswith("._") or parts[0].upper() in SKIP_DIRS:
        return False
    if len(parts) == 1 and parts[0].upper() in SKIP_FILES:
        return False
    return all_languages or not LANG_VARIANT.match(parts[-1])


def self_check():
    assert wanted("default.xbe") and wanted("prince.bf") and wanted("VIDEO/CINE_9010.INT")
    assert not wanted("VIDEO/CINE_9011.INT") and not wanted("VIDEO/POPLOGO3.INT")
    assert not wanted("SOUND/soundx2.big") and wanted("SOUND/soundx0.fat") and wanted("SOUND/SOUNDX.big")
    assert wanted("VIDEO/Loading3.int") and wanted("VIDEO/VIS_111.int")   # no son idiomas
    assert not wanted("POP2/default.xbe") and not wanted("update.xbe") and not wanted("VIDEO/._x.int")
    assert wanted("VIDEO/CINE_9012.INT", all_languages=True)


def files(disc, all_languages):
    for p in sorted(disc.rglob("*")):
        rel = p.relative_to(disc).as_posix()
        if p.is_file() and wanted(rel, all_languages):
            yield rel, p


class LocalDest:
    def __init__(self, root):
        self.root = Path(root)

    def size(self, rel):
        p = self.root / rel
        return p.stat().st_size if p.is_file() else -1

    def put(self, rel, src):
        dst = self.root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_name(dst.name + ".part")
        with open(src, "rb") as i, open(tmp, "wb") as o:
            while chunk := i.read(1 << 20):
                o.write(chunk)
        tmp.replace(dst)


class FtpDest:
    BASE = "ux0:/data/pop-sot"

    def __init__(self, url):
        host, _, port = url[len("ftp://"):].rstrip("/").partition(":")
        self.ftp = ftplib.FTP()
        self.ftp.connect(host, int(port or 1337), timeout=60)
        self.ftp.login()
        self.dirs = set()
        self.listings = {}

    def size(self, rel):
        d, _, name = (self.BASE + "/" + rel).rpartition("/")
        if d not in self.listings:
            # VitaShell: ni "LIST <ruta>" ni un CWD absoluto desde una subcarpeta; pasar por "/"
            lines = []
            try:
                self.ftp.cwd("/")
                self.ftp.cwd(d)
                self.ftp.retrlines("LIST", lines.append)
            except ftplib.error_perm:
                pass
            self.listings[d] = {f[8]: int(f[4]) for f in (l.split(None, 8) for l in lines) if len(f) == 9}
        return self.listings[d].get(name, -1)

    def put(self, rel, src):
        path = self.BASE + "/" + rel
        parts = path.split("/")
        for i in range(2, len(parts)):
            d = "/".join(parts[:i])
            if d not in self.dirs:
                try:
                    self.ftp.mkd(d)
                except ftplib.error_perm:
                    pass   # ya existe
                self.dirs.add(d)
        with open(src, "rb") as f:
            self.ftp.storbinary("STOR " + path, f, blocksize=1 << 16)


def main():
    self_check()
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.exit(__doc__)
    disc, dest_arg = Path(args[0]), args[1]
    all_languages = "--all-languages" in sys.argv
    xbe = disc / "default.xbe"
    if not xbe.is_file():
        sys.exit("No hay default.xbe en %s" % disc)
    digest = hashlib.sha256(xbe.read_bytes()).hexdigest().upper()
    if digest != XBE_SHA256:
        sys.exit("default.xbe no es el NTSC que usa el port (SHA-256 %s, se espera %s)" % (digest, XBE_SHA256))
    dest = FtpDest(dest_arg) if dest_arg.startswith("ftp://") else LocalDest(dest_arg)
    todo = list(files(disc, all_languages))
    total = sum(p.stat().st_size for _, p in todo)
    done = 0
    print("%d archivos, %.0f MB" % (len(todo), total / 2**20), flush=True)
    for rel, p in todo:
        n = p.stat().st_size
        if dest.size(rel) == n:
            print("  ya está  %s" % rel, flush=True)
        else:
            print("  copiando %s (%.1f MB)" % (rel, n / 2**20), flush=True)
            dest.put(rel, p)
        done += n
    print("Listo: %.0f MB. En la Vita: ux0:data/pop-sot/" % (done / 2**20))


if __name__ == "__main__":
    main()
