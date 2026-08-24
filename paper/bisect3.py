#!/usr/bin/env python3
"""Bisect the arch figure with its REAL options line and paper macros."""
import re
import subprocess

src = open("main.tex").read()
anchor = src.find("escalation guard:\\\\ classified UDS but")
start = src.rfind("\\begin{figure}[t]", 0, anchor)
end = src.find("\\end{figure}", start) + len("\\end{figure}")
block = src[start:end]

open_e = block.find("\n", block.find("\\begin{tikzpicture}")) + 1
close_s = block.rfind("\\end{tikzpicture}")
options_line = block[:open_e]          # includes \begin{tikzpicture}[...]
body = block[open_e:close_s]

stmts, cur = [], []
for line in body.strip().splitlines():
    cur.append(line)
    if line.rstrip().endswith(";"):
        stmts.append("\n".join(cur))
        cur = []
print("statements:", len(stmts))

MACROS = ("\\newcommand{\\taulow}{\\tau_{\\mathrm{low}}}\n"
          "\\providecommand{\\tauhigh}{\\tau_{\\mathrm{high}}}\n"
          "\\providecommand{\\sbar}{\\bar{S}}\n")


def compile_body(b, tag):
    doc = ("\\documentclass[conference]{IEEEtran}\n"
           "\\usepackage{amsmath}\n"
           "\\usepackage{tikz}\n"
           "\\usetikzlibrary{arrows.meta,positioning,shapes.geometric}\n"
           + MACROS +
           "\\begin{document}\n" + options_line + "\n" + b +
           "\n\\end{tikzpicture}\n\\end{document}\n")
    open("_b3.tex", "w").write(doc)
    r = subprocess.run(["tectonic", "_b3.tex"], capture_output=True,
                       text=True)
    errs = re.findall(r"^!(.*)$", r.stdout + r.stderr, re.M)
    print(f"{tag}: errors={len(errs)} {errs[:1]}")
    return not errs


if compile_body(body, "full-body"):
    print("full body compiles: failure must come from elsewhere")
else:
    for i in range(len(stmts)):
        kept = [x for j, x in enumerate(stmts) if j != i]
        if not kept:
            continue
        if compile_body("\n".join(kept), f"drop-{i}"):
            print("==> CULPRIT statement:")
            print(stmts[i])
            break
