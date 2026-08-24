#!/usr/bin/env python3
"""Compile main.tex with each new figure individually disabled."""
import re
import subprocess

src = open("main.tex").read()

blocks = {
    "arch": ("escalation guard:\\\\ classified UDS but", None),
    "decision": ("Per-message routing decision.", None),
    "alpha": ("visualizes both sides of this tradeoff.", None),
    "mux": ("depicts this receive loop.", None),
    "breakdown": ("visualizes the measured decomposition", None),
}

for name, (anchor, _) in blocks.items():
    a = src.find(anchor)
    if a < 0:
        print(f"{name}: anchor not found")
        continue
    s = src.rfind("\\begin{figure}", 0, a)
    e = src.find("\\end{figure}", a) + len("\\end{figure}")
    t = src[:s] + src[e:]
    open(f"_no_{name}.tex", "w").write(t)
    r = subprocess.run(["tectonic", f"_no_{name}.tex"],
                       capture_output=True, text=True)
    outp = r.stdout + r.stderr
    errs = re.findall(r"^!(.*)$", outp, re.M)
    print(f"without {name}: errors={len(errs)} {errs[:1]}")
