#!/usr/bin/env python3
"""Rewire arch escalation: vertical arc -> parallel dashed lane."""
import re

src = open("main.tex").read()
OLD = ("\\draw[->,dashed] (udsw.north) to[out=95,in=-95]\n"
       "  node[lbl,midway,right,text width=19mm]{\\textbf{escalation "
       "guard}:\\\\\n   classified UDS but oversized ($>$ "
       "\\texttt{UDS\\_MAX\\_DGRAM})\\\\\n   $\\to$ sent via SHM; routing "
       "state unchanged} (shmw.south);")
NEW = ("% escalation guard: parallel dashed lane below the main SHM path\n"
       "\\draw[->,dashed] ([yshift=-2mm]send.east) to[out=-23,in=178]\n"
       "  node[lbl,midway,below,text width=44mm]{\\textbf{escalation "
       "guard}: classified UDS but oversized ($>$ "
       "\\texttt{UDS\\_MAX\\_DGRAM}) $\\to$ sent via SHM; routing state "
       "unchanged}\n  ([yshift=-2mm]shmw.west);")
if OLD not in src:
    i = src.find("(udsw.north) to[out=")
    print("context:", repr(src[i - 60:i + 260]) if i > 0 else "no match")
    raise SystemExit(1)
src = src.replace(OLD, NEW)
open("main.tex", "w").write(src)
print("rewired OK")
