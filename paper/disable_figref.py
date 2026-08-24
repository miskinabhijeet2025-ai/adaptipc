#!/usr/bin/env python3
"""Remove the (Fig.~\ref{fig:decision}) line from the rt node."""
t = open("main.tex").read()
a = t.find("(Fig.~\\ref{fig:decision})")
if a < 0:
    print("not found - already removed")
else:
    ls = t.rfind("\n", 0, a)
    le = t.find("\n", a)
    t = t[:ls] + t[le:]
    open("main.tex", "w").write(t)
    print("removed fig-ref line from rt node")
