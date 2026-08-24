#!/usr/bin/env python3
"""Extract arch picture, confirm failure, then drop statements to isolate."""
import re
import subprocess

src = open("main.tex").read()
anchor = src.find("escalation guard:")
start = src.rfind("\\begin{figure}[t]", 0, anchor)
end = src.find("\\end{figure}", start) + len("\\end{figure}")
block = src[start:end]

open_e = block.find("\n", block.find("\\begin{tikzpicture}")) + 1
close_s = block.rfind("\\end{tikzpicture}")
options = block[open_e - len("\\begin{tikzpicture}"):]
options_line = options[:options.find("\n")]
body = block[open_e:close_s]

PRE = ("\\documentclass[conference]{IEEEtran}\n"
       "\\usepackage{tikz}\n"
       "\\usetikzlibrary{arrows.meta,positioning}\n"
       "\\begin{document}\n")


def compile_body(b, tag):
    doc = (PRE + options_line + "\n" + b +
           "\n\\end{tikzpicture}\n\\end{document}\n")
    open("_b.tex", "w").write(doc)
    r = subprocess.run(["tectonic", "_b.tex"], capture_output=True,
                       text=True)
    errs = re.findall(r"^!(.*)$", r.stdout + r.stderr, re.M)
    print(f"{tag}: errors={len(errs)} {errs[:1]}")
    return len(errs) == 0


# statement split on top-level \draw / \node starts
stmts = [s for s in re.split(r"\n(?=\\(?:draw|node)\b)", body.strip()) if s]
print("statements:", len(stmts))

compile_body(body, "full-original")

for i in range(len(stmts)):
    kept = [x for j, x in enumerate(stmts) if j != i]
    if not kept:
        continue
    if compile_body("\n".join(kept), f"drop-{i}"):
        print("===> CULPRIT STATEMENT:")
        print(stmts[i])
        break
