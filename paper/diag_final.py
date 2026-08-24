#!/usr/bin/env python3
"""Definitive diagnosis of the arch-figure compile failure."""
import re
import subprocess

src = open("main.tex").read()
anchor = src.find("escalation guard:")
fstart = src.rfind("\\begin{figure}[t]", 0, anchor)
fend = src.find("\\end{figure}", fstart) + len("\\end{figure}")
block = src[fstart:fend]

print("=== 1. blank lines inside block ===")
blanks = [i for i, l in enumerate(block.splitlines()) if not l.strip()]
print(blanks if blanks else "none")

print("\n=== 2. odd-dollar lines ===")
odd = [(i, l) for i, l in enumerate(block.splitlines())
       if l.count("$") % 2 == 1]
print(odd if odd else "none")

print("\n=== 3. bare underscores (unescaped _) ===")
bare = [(i, l.strip()[:60]) for i, l in enumerate(block.splitlines())
        if re.search(r"(?<!\\)_", l)]
print(bare if bare else "none")

print("\n=== 4. compile tests ===")
open_e = block.find("\n", block.find("\\begin{tikzpicture}")) + 1
close_s = block.rfind("\\end{tikzpicture}")
options_line = block[:open_e]
body = block[open_e:close_s]

stmts, cur = [], []
for line in body.strip().splitlines():
    cur.append(line)
    if line.rstrip().endswith(";"):
        stmts.append("\n".join(cur))
        cur = []


def comp(b, tag):
    doc = ("\\documentclass[conference]{IEEEtran}\n"
           "\\usepackage{tikz}\n"
           "\\usetikzlibrary{arrows.meta,positioning}\n"
           "\\begin{document}\n"
           "\\begin{tikzpicture}[font=\\scriptsize]\n" + b +
           "\n\\end{tikzpicture}\n\\end{document}\n")
    open("_d.tex", "w").write(doc)
    r = subprocess.run(["tectonic", "_d.tex"], capture_output=True,
                       text=True)
    errs = re.findall(r"^!(.*)$", r.stdout + r.stderr, re.M)
    ctx = re.search(r"^l\.\d+ .*$", r.stdout + r.stderr, re.M)
    print(f"{tag}: errors={len(errs)} {errs[:1]} "
          f"{ctx.group(0).strip()[:55] if ctx else ''}")
    return len(errs) == 0


comp(body, "full-body-plain-options")
for i in range(len(stmts)):
    kept = [x for j, x in enumerate(stmts) if j != i]
    comp("\n".join(kept), f"without-stmt{i}")
