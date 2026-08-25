#!/usr/bin/env python3
"""Remove the duplicate architecture figure (keep the first)."""
src = open("main.tex").read()
cap = "AdaptIPC architecture: one endpoint shown end to end."
first = src.find(cap)
second = src.find(cap, first + 1)
assert second > 0, "no duplicate found"
s = src.rfind("\\begin{figure}", 0, second)
e = src.find("\\end{figure}", second) + len("\\end{figure}")
# also swallow a leading blank line
while s > 0 and src[s - 1] == "\n":
    s -= 1
src = src[:s] + src[e:]
open("main.tex", "w").write(src)
print("duplicate arch figure removed; remaining:",
      src.count(cap), "occurrence(s) of the caption")
