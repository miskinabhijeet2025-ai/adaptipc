#!/usr/bin/env python3
"""Add arch statements cumulatively; find where compilation first breaks."""
import re
import subprocess

src = open("main.tex").read()
anchor = src.find("escalation guard:")
start = src.rfind("\\begin{figure}[t]", 0, anchor)
end = src.find("\\end{figure}", start) + len("\\end{figure}")
block = src[start:end]

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

PRE = ("\\documentclass[conference]{IEEEtran}\n"
       "\\usepackage{tikz}\n"
       "\\usetikzlibrary{arrows.meta,positioning}\n"
       "\\begin{document}\n")


def comp(b):
    doc = PRE + options_line + "\n" + b + \
        "\n\\end{tikzpicture}\n\\end{document}\n"
    open("_c.tex", "w").write(doc)
    r = subprocess.run(["tectonic", "_c.tex"], capture_output=True,
                       text=True)
    outp = r.stdout + r.stderr
    errs = re.findall(r"^!(.*)$", outp, re.M)
    return len(errs), (errs[0][:50] if errs else "")


acc = ""
for i, s in enumerate(stmts):
    acc += s + "\n"
    n, msg = comp(acc)
    flag = "" if n == 0 else "  <<< BREAKS HERE"
    print(f"+stmt{i}: errors={n} {msg}{flag}")
    if n:
        print("STATEMENT:", s[:200])
        break
