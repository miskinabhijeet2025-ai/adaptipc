#!/usr/bin/env python3
"""Restore two-author block + Acknowledgment section."""
t = open("paper/main.tex").read()

old = (
    "\\author{\\IEEEauthorblockN{AdaptIPC Project}\n"
    "\\IEEEauthorblockA{OS Systems Research Group\\\\\n"
    "Email: adaptipc@example.org}}"
)
new = (
    "\\author{\n"
    "\\IEEEauthorblockN{Miskin Abhijeet Laxman}\n"
    "\\IEEEauthorblockA{School of Computer Science and Engineering\\\\\n"
    "Vellore Institute of Technology, Chennai, India\\\\\n"
    "miskin.abhijeet2025@vitstudent.ac.in}\n"
    "\\and\n"
    "\\IEEEauthorblockN{Vivek}\n"
    "\\IEEEauthorblockA{School of Computer Science and Engineering\\\\\n"
    "Vellore Institute of Technology, Chennai, India\\\\\n"
    "vivek2025@vitstudent.ac.in}}"
)
assert old in t, "placeholder author block not found"
t = t.replace(old, new)

anchor = "\\begin{thebibliography}{00}"
assert "Acknowledgment" not in t
ack = ("\\section*{Acknowledgment}\n"
       "The authors thank Dr.~Padmanaban R for guidance on this project.\n\n")
t = t.replace(anchor, ack + anchor)

open("paper/main.tex", "w").write(t)
print("authors + acknowledgment restored")
