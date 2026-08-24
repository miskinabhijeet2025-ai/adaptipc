#!/usr/bin/env python3
"""Bisect the architecture tikzpicture by dropping one statement at a time."""
import re
import subprocess

src = open("main.tex").read()
start = src.find("\\begin{tikzpicture}[font=\\scriptsize,>=Latex,"
                 "node distance=4mm,")
end = src.find("\\end{tikzpicture}", start) + len("\\end{tikzpicture}")
pic = src[start:end]

header_end = pic.find("\n") + 1
options_and_open = pic[:pic.find("\n")]
body = pic[header_end:pic.rfind("\\end{tikzpicture}")]

# split into statements (each starts with \node or \draw)
stmts = []
cur = []
for line in body.strip().splitlines():
    cur.append(line)
    if line.rstrip().endswith(";"):
        stmts.append("\n".join(cur))
        cur = []
if cur:
    stmts.append("\n".join(cur))


def try_compile(body_text, tag):
    doc = ("\\documentclass[conference]{IEEEtran}\n"
           "\\usepackage{amsmath}\n"
           "\\usepackage{tikz}\n"
           "\\usetikzlibrary{arrows.meta,positioning,shapes.geometric}\n"
           "\\begin{document}\n"
           + options_and_open + "\n" + body_text +
           "\n\\end{tikzpicture}\n\\end{document}\n")
    open("_iso5.tex", "w").write(doc)
    r = subprocess.run(["tectonic", "_iso5.tex"], capture_output=True,
                       text=True)
    outp = r.stdout + r.stderr
    errs = re.findall(r"^!(.*)$", outp, re.M)
    ctx = re.search(r"^l\.\d+ .*$", outp, re.M)
    print(f"drop [{tag}]: errors={len(errs)} "
          f"{(ctx.group(0).strip()[:70]) if ctx else ''}")
    return len(errs) == 0


try_compile(body, "baseline-full")

for i, s in enumerate(stmts):
    reduced = "\n".join(x for j, x in enumerate(stmts) if j != i)
    if not reduced.strip():
        continue
    ok = try_compile(reduced, f"without-stmt{i}")
    if ok:
        print(f"--> removing statement {i} FIXES it. Statement was:")
        print(s[:300])
        break
