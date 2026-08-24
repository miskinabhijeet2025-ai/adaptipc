#!/usr/bin/env python3
"""Isolate which part of the mux node breaks compilation."""
import re
import subprocess

PREAMBLE_END = "\\begin{document}"

def compile_snippet(body):
    doc = ("\\documentclass[conference]{IEEEtran}\n"
           "\\usepackage{tikz}\n"
           "\\usetikzlibrary{arrows.meta,positioning,shapes.geometric}\n"
           "\\begin{document}\n\\begin{tikzpicture}"
           "[font=\\scriptsize,>=Latex]\n" + body +
           "\n\\end{tikzpicture}\n\\end{document}\n")
    open("_iso2.tex", "w").write(doc)
    r = subprocess.run(["tectonic", "_iso2.tex"], capture_output=True,
                       text=True)
    outp = r.stdout + r.stderr
    errs = re.findall(r"^!.*$", outp, re.M)
    ctx = re.search(r"^l\.\d+.*$", outp, re.M)
    return errs, (ctx.group(0) if ctx else "")

MUX_FULL = """\\node[align=center] (mux)
  {\\texttt{adapt\\_recv()} multiplexer:\\\\ ring poll $\\\\vee$ bounded UDS wait
   \\\\ (transport-tag demultiplex)};"""

variants = {
    "full": MUX_FULL,
    "no-vee": MUX_FULL.replace("$\\vee$", "OR"),
    "no-parens": MUX_FULL.replace("(transport-tag demultiplex)",
                                  "transport-tag demultiplex"),
    "no-texttt": MUX_FULL.replace("\\texttt{adapt\\_recv()}",
                                  "adapt recv"),
}

for name, body in variants.items():
    errs, ctx = compile_snippet(body)
    print(f"{name}: errors={len(errs)} {ctx.strip()[:80]}")
