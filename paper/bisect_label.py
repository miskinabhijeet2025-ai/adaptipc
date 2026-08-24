#!/usr/bin/env python3
"""Bisect the arch figure: drop one statement at a time."""
import re
import subprocess

src = open("main.tex").read()
anchor = src.find("escalation guard:\\\\ classified UDS but")
start = src.rfind("\\begin{figure}[t]", 0, anchor)
end = src.find("\\end{figure}", start) + len("\\end{figure}")
block = src[start:end]

pic_open_end = block.find("\n", block.find("\\begin{tikzpicture}")) + 1
pic_end = block.rfind("\\end{tikzpicture}")
options_and_open = block[:pic_open_end]
body = block[pic_open_end:pic_end]

stmts, cur = [], []
for line in body.strip().splitlines():
    cur.append(line)
    if line.rstrip().endswith(";"):
        stmts.append("\n".join(cur))
        cur = []

PRE = ("\\documentclass[conference]{IEEEtran}\n"
       "\\usepackage{amsmath}\n"
       "\\usepackage{tikz}\n"
       "\\usetikzlibrary{arrows.meta,positioning,shapes.geometric}\n"
       "\\begin{document}\n"
       "\\newcommand{\\taulow}{\\tau_{\\mathrm{low}}}\n"
       "\\newcommand{\\tauhigh}{\\tau_{\\mathrm{high}}}\n"
       "\\newcommand{\\sbar}{\\bar{S}}\n")


def try_compile(body_text, tag):
    doc = (PRE + options_and_open + "\n" + body_text +
           "\n\\end{tikzpicture}\n\\end{document}\n")
    open("_iso6.tex", "w").write(doc)
    r = subprocess.run(["tectonic", "_iso6.tex"], capture_output=True,
                       text=True)
    outp = r.stdout + r.stderr
    errs = re.findall(r"^!(.*)$", outp, re.M)
    ctx = re.search(r"^l\.\d+ .*$", outp, re.M)
    print(f"drop[{tag}]: errors={len(errs)} "
          f"{(ctx.group(0).strip()[:60]) if ctx else ''}")
    return len(errs) == 0


try_compile(body, "baseline-full")

for i in range(len(stmts)):
    reduced = "\n".join(x for j, x in enumerate(stmts) if j != i)
    if not reduced.strip():
        continue
    if try_compile(reduced, f"drop-{i}"):
        print("--> DROPPING this statement fixes compilation:")
        print(stmts[i])
        break
