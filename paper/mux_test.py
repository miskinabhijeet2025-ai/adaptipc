#!/usr/bin/env python3
"""Test the exact mux node text in isolation."""
import re
import subprocess

NODES = {
    "exact-mux": "\\node[draw,align=center] {\\texttt{adapt\\_recv()}: ring "
                 "poll / bounded \\texttt{poll(2)} on UDS};",
    "mux-no-texttt": "\\node[draw,align=center] {adapt recv(): ring poll / "
                     "bounded poll(2) on UDS};",
    "mux-no-parens": "\\node[draw,align=center] {\\texttt{adapt\\_recv()}: "
                     "ring poll / bounded \\texttt{poll} on UDS};",
}

for name, body in NODES.items():
    doc = ("\\documentclass[conference]{IEEEtran}\n"
           "\\usepackage{tikz}\n"
           "\\usetikzlibrary{arrows.meta,positioning}\n"
           "\\begin{document}\n\\begin{tikzpicture}"
           "[font=\\scriptsize]\n" + body +
           "\n\\end{tikzpicture}\n\\end{document}\n")
    open("_m.tex", "w").write(doc)
    r = subprocess.run(["tectonic", "_m.tex"], capture_output=True,
                       text=True)
    outp = r.stdout + r.stderr
    errs = [e for e in re.findall(r"^!(.*)$", outp, re.M)]
    print(f"{name}: errors={len(errs)} {errs[:1]}")
