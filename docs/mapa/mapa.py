#!/usr/bin/env python3
"""The engine map's keeper (docs/mapa, story notes in Spanish): keeps the
map at HEAD.

  mapa.py check            what the CI runs; fails when the map is behind
  mapa.py sello NOTE...    stamp notes after reviewing them (--todas: all)
  mapa.py perillas         regenerate Perillas/*.md and Perillas.md
  mapa.py hooks            install local git hooks that refresh graphify's
                           code graph in the Obsidian vault (optional)
  mapa.py grafo            refresh that graph now

check fails when:
  - a file of engine/src is in no note's `archivos:`
  - a note's `archivos:` names a file that does not exist
  - a piece note's `sello:` differs from the hash of its own files (the
    code changed and the note was not reviewed): read the note against the
    diff, fix it, then `mapa.py sello "NOTE.md"`. The big files every piece
    touches (SHARED) are not in the seal, or every PR would stale every note.
  - the knob table (perillas.py) lacks a key config.c parses, or has one it
    does not; a generated note differs from what perillas.py makes; or a
    numeric default in the table differs from what the parser itself sets
    (a dumper built from config.c, when a C compiler is at hand)
  - a link to a map note or a story does not open a note
"""
import glob
import hashlib
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(REPO, "engine", "src")
STORIES = os.path.join(REPO, "docs", "historias")
SHARED = {"engine/src/client.c", "engine/src/server.c", "engine/src/config.c", "engine/src/config.h",
          "engine/src/status.c", "engine/src/main.c", "engine/src/os.c", "engine/src/proto.h",
          "engine/src/proto.c", "engine/src/clientpath.h"}
VAULT = os.environ.get("CG_VAULT", os.path.expanduser("~/Obsidian/cengarde"))

sys.path.insert(0, HERE)
import perillas  # noqa: E402


def notes():
    """Every hand-written or generated note: path relative to docs/mapa."""
    out = []
    for p in glob.glob(os.path.join(HERE, "**", "*.md"), recursive=True):
        out.append(os.path.relpath(p, HERE))
    return sorted(out)


def front(text):
    """(frontmatter lines, body) of a note."""
    if not text.startswith("---\n"):
        return [], text
    end = text.find("\n---", 4)
    if end < 0:
        return [], text
    return text[4:end].split("\n"), text[end + 4:]


def archivos(text):
    fm, _ = front(text)
    out, inside = [], False
    for line in fm:
        if line.startswith("archivos:"):
            inside = True
            continue
        if inside:
            m = re.match(r"\s+-\s+(\S+)", line)
            if m:
                out.append(m.group(1))
            elif line and not line.startswith(" "):
                inside = False
    return out


def sello_of(files):
    """The seal: a hash of the note's own files (not the SHARED ones)."""
    h = hashlib.sha256()
    for f in sorted(set(files) - SHARED):
        p = os.path.join(REPO, f)
        h.update(f.encode() + b"\0")
        h.update(open(p, "rb").read() if os.path.exists(p) else b"(missing)")
    return h.hexdigest()[:16]


def sello_in(text):
    fm, _ = front(text)
    for line in fm:
        if line.startswith("sello:"):
            return line.split(":", 1)[1].strip()
    return None


def set_sello(text, value):
    fm, body = front(text)
    fm = [line for line in fm if not line.startswith("sello:")]
    fm.append(f"sello: {value}")
    return "---\n" + "\n".join(fm) + "\n---" + body


def is_piece(rel):
    return not rel.startswith("Perillas/") and rel != "Perillas.md"


def dump_defaults():
    """{knob: (client value, server value)} as cg_config_parse leaves them,
    for the knobs config.c reads straight into a field; None without cc."""
    s = open(os.path.join(SRC, "config.c")).read()
    pairs = re.findall(r'get_(u32|bool)\(&ini, "", "([a-z0-9_]+)",[^;]*?&c->([a-z0-9_]+)\b', s)
    seen, prints = set(), []
    for _, k, field in pairs:
        if k in seen:
            continue
        seen.add(k)
        prints.append(f'\tprintf("{k} %lld\\n", (long long)c.{field});')
    key = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="
    code = ['#include <stdio.h>', '#include "config.h"', "",
            "static void one(const char *text)", "{",
            "\tstatic struct cg_config c;", "\tchar err[512] = \"\", warn[512] = \"\";", "",
            "\tif (cg_config_parse(&c, text, err, sizeof(err), warn, sizeof(warn)) < 0) {",
            "\t\tfprintf(stderr, \"%s\\n\", err);", "\t\treturn;", "\t}"] + prints + [
            "\tputs(\"--\");", "}", "", "int main(void)", "{",
            f'\tone("mode = client\\nkey = {key}\\nserver = 192.0.2.1:59402\\n");',
            f'\tone("mode = server\\nkey = {key}\\n");',
            "\treturn 0;", "}", ""]
    cc = os.environ.get("CC", "cc")
    core = [os.path.join(SRC, f) for f in ("siphash.c", "proto.c", "util.c", "log.c", "config.c", "blake2s.c", "pair.c")]
    with tempfile.TemporaryDirectory() as d:
        src, exe = os.path.join(d, "dump.c"), os.path.join(d, "dump")
        open(src, "w").write("\n".join(code))
        try:
            r = subprocess.run([cc, "-std=c11", "-D_GNU_SOURCE", '-DCG_VERSION="map"', "-pthread", "-w",
                                "-I" + SRC, "-o", exe, src] + core, capture_output=True, text=True)
        except FileNotFoundError:
            return None
        if r.returncode:
            print("map: the defaults dumper does not build:\n" + r.stderr[-2000:])
            return None
        out = subprocess.run([exe], capture_output=True, text=True).stdout
    blocks = out.split("--\n")
    vals = [dict(line.split(" ", 1) for line in b.strip().split("\n") if line) for b in blocks[:2]]
    if len(vals) < 2:
        return None
    return {k: (vals[0].get(k), vals[1].get(k)) for k in seen}


def check_defaults(errors):
    d = dump_defaults()
    if d is None:
        print("map: no C compiler, numeric defaults not checked")
        return
    for k, (cli, srv) in sorted(d.items()):
        if k not in perillas.T:
            continue
        txt = perillas.T[k][2]
        m = re.search(r"\((-?\d+)\)", txt.replace(" ", "")) or re.search(r"(-?\d+)", txt.replace(" ", ""))
        if not m:
            continue
        stated = m.group(1)
        if stated not in (cli, srv) and not (cli == srv == "0" and re.search(r"\b(no|apagad|vací|ningun)", txt)):
            errors.append(f"Perillas/{k}.md: the table says the default is «{txt}», "
                          f"the parser sets {cli} (client) / {srv} (server): fix T in perillas.py")


def link_targets():
    names = {os.path.splitext(os.path.basename(n))[0] for n in notes()}
    names |= {"Mapa/" + os.path.splitext(n)[0] for n in notes()}
    names |= {os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(STORIES, "**", "*.md"),
                                                                           recursive=True)}
    return names


def check():
    errors = []
    texts = {n: open(os.path.join(HERE, n)).read() for n in notes()}

    # every engine file in some note
    covered = set()
    for n, t in texts.items():
        for f in archivos(t):
            covered.add(f)
            if not os.path.exists(os.path.join(REPO, f)):
                errors.append(f"{n}: `archivos:` names {f}, which does not exist")
    for p in sorted(glob.glob(os.path.join(SRC, "*.[ch]"))):
        f = os.path.relpath(p, REPO)
        if f not in covered and f[:-2] + ".h" not in covered and f[:-2] + ".c" not in covered:
            errors.append(f"{f}: in no note's `archivos:` (add it to its piece, or to Utilidades.md)")

    # seals
    for n, t in texts.items():
        if not is_piece(n):
            continue
        want, have = sello_of(archivos(t)), sello_in(t)
        if have is None:
            errors.append(f"{n}: no `sello:` (review it, then: docs/mapa/mapa.py sello \"{n}\")")
        elif have != want:
            changed = sorted(set(archivos(t)) - SHARED)
            errors.append(f"{n}: behind its code ({', '.join(changed)}): review it against the diff, "
                          f"then: docs/mapa/mapa.py sello \"{n}\"")

    # knobs
    files, missing, extra = perillas.generate()
    for k in missing:
        errors.append(f"knob `{k}`: config.c parses it and perillas.py has no row for it")
    for k in extra:
        errors.append(f"knob `{k}`: perillas.py has a row, config.c no longer parses it")
    for rel, text in files.items():
        if texts.get(rel) != text:
            errors.append(f"{rel}: differs from what perillas.py makes (docs/mapa/mapa.py perillas)")
    for rel in texts:
        if rel.startswith("Perillas/") and rel not in files:
            errors.append(f"{rel}: a knob note perillas.py no longer makes (delete it)")
    check_defaults(errors)

    # links to map notes and stories
    targets = link_targets()
    for n, t in texts.items():
        for m in re.finditer(r"\[\[([^\]|#]+)", t):
            name = m.group(1).strip().rstrip("\\")
            mapish = name.startswith("Mapa/") or re.match(r"\d{3}-", name) or "(" in name or name in (
                "Mapa del motor", "Perillas", "Glosario", "Utilidades")
            if mapish and name not in targets:
                errors.append(f"{n}: [[{name}]] opens no note")

    if errors:
        print(f"map: {len(errors)} problem(s); docs/mapa is behind HEAD:")
        for e in errors:
            print("  - " + e)
        return 1
    print(f"map: ok ({len(texts)} notes, {len(files)} generated, every engine file covered, every seal current)")
    return 0


def sello(args):
    todas = "--todas" in args
    targets = notes() if todas else [a[len("docs/mapa/"):] if a.startswith("docs/mapa/") else a
                                      for a in args if a != "--todas"]
    for n in targets:
        if not is_piece(n):
            continue
        p = os.path.join(HERE, n)
        t = open(p).read()
        open(p, "w").write(set_sello(t, sello_of(archivos(t))))
        print("stamped", n)
    return 0


def write_perillas():
    files, missing, extra = perillas.generate()
    for rel, text in files.items():
        p = os.path.join(HERE, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if not os.path.exists(p) or open(p).read() != text:
            open(p, "w").write(text)
            print("wrote", rel)
    for rel in notes():
        if rel.startswith("Perillas/") and rel not in files:
            os.remove(os.path.join(HERE, rel))
            print("removed", rel)
    if missing or extra:
        print("add rows to perillas.py for:", missing, "; drop:", extra)
        return 1
    return 0


GRAPHIFY = os.path.expanduser("~/.local/share/graphify-venv/bin/graphify")


def grafo():
    """graphify's code graph of this checkout into the vault's «Grafo código»."""
    if not os.path.exists(GRAPHIFY) or not os.path.isdir(VAULT):
        return 0
    out = os.path.expanduser("~/Obsidian/_graphify/cengarde")
    ign = os.path.join(REPO, ".graphifyignore")
    made = not os.path.exists(ign)
    if made:
        open(ign, "w").write(".claude/\n")
    try:
        subprocess.run([GRAPHIFY, "extract", REPO, "--code-only", "--out", out], cwd=REPO,
                       capture_output=True)
    finally:
        if made:
            os.remove(ign)
    g = os.path.join(out, "graphify-out")
    subprocess.run([GRAPHIFY, "cluster-only", out, "--no-label"], capture_output=True)
    subprocess.run([sys.executable, os.path.join(HERE, "grafo_labels.py"), g], capture_output=True)
    dest = os.path.join(VAULT, "Grafo código")
    subprocess.run([GRAPHIFY, "export", "obsidian", "--graph", os.path.join(g, "graph.json"),
                    "--labels", os.path.join(g, ".graphify_labels.json"), "--dir", dest], capture_output=True)
    subprocess.run(["rm", "-rf", os.path.join(dest, ".obsidian")])
    rep = os.path.join(g, "GRAPH_REPORT.md")
    if os.path.exists(rep):
        open(os.path.join(dest, "_REPORTE graphify.md"), "w").write(open(rep).read())
    return 0


HOOK = """#!/bin/sh
# docs/mapa/mapa.py hooks: refresh graphify's code graph in the vault, in
# the background (local only; the map's notes are checked by the CI).
( python3 "$(git rev-parse --show-toplevel)/docs/mapa/mapa.py" grafo >/dev/null 2>&1 & )
"""


def hooks():
    gitdir = subprocess.run(["git", "rev-parse", "--git-common-dir"], cwd=REPO, capture_output=True,
                            text=True).stdout.strip()
    hd = os.path.join(REPO, gitdir, "hooks") if not os.path.isabs(gitdir) else os.path.join(gitdir, "hooks")
    for name in ("post-commit", "post-checkout", "post-merge"):
        p = os.path.join(hd, name)
        if os.path.exists(p) and "mapa.py" not in open(p).read():
            print(f"{p} exists and is not ours: left as is")
            continue
        open(p, "w").write(HOOK)
        os.chmod(p, 0o755)
        print("installed", p)
    return 0


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "check"
    if cmd == "check":
        return check()
    if cmd == "sello":
        return sello(argv[2:])
    if cmd == "perillas":
        return write_perillas()
    if cmd == "hooks":
        return hooks()
    if cmd == "grafo":
        return grafo()
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
