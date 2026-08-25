#!/usr/bin/env python3
"""Final recovery: d339ead + redesigned arch + redesigned mux figures."""
import re
import subprocess

src = subprocess.run(["git", "show", "d339ead:paper/main.tex"],
                     capture_output=True, text=True).stdout
assert "Per-message routing decision" in src

ra = open("replace_arch.py").read()
ARCH_NEW = re.search(r'NEW = r"""(\\begin\{figure\*\}.*?)"""', ra,
                     re.S).group(1)
MUX_NEW = open("mux_new.tex").read().strip()

# old ARCH figure: uniquely identified by its dashed-edge statement
a = src.find("\\draw[->,dashed] (uds.west) to[out=180,in=0]")
assert a > 0, "old arch not found"
fstart = src.rfind("\\begin{figure}", 0, a)
fend = src.find("\\end{figure}", a) + len("\\end{figure}")
src = src[:fstart] + ARCH_NEW + src[fend:]

# old MUX figure: uniquely identified by its caption opening
m = src.find("The \\texttt{adapt\\_recv()} multiplexer loop")
assert m > 0, "old mux not found"
mstart = src.rfind("\\begin{figure}", 0, m)
mend = src.find("\\end{figure}", m) + len("\\end{figure}")
src = src[:mstart] + MUX_NEW + src[mend:]

open("main.tex", "w").write(src)

for probe in ("Per-message routing decision", "AdaptIPC architecture",
              "multiplexer loop", "EWMA smoothing-factor sensitivity",
              "Measured latency decomposition", "Limitations",
              "\\section{System Architecture",
              "\\section{Performance Evaluation"):
    print(probe[:42], "->", probe in src)
