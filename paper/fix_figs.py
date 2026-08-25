#!/usr/bin/env python3
"""Repair figure placement:
   - remove BOTH misplaced mux figures
   - restore the decision flowchart (from git HEAD) after eq:hysteresis
   - re-insert the redesigned mux figure in IV-D after 'receive loop'
"""
import subprocess

src = open("main.tex").read()
head = subprocess.run(
    ["git", "show", "HEAD:paper/main.tex"],
    capture_output=True, text=True, cwd=".").stdout


def extract_block(text, start_anchor):
    i = text.find(start_anchor)
    assert i >= 0, start_anchor[:40]
    s = text.rfind("\\begin{figure}", 0, i)
    e = text.find("\\end{figure}", i) + len("\\end{figure}")
    return text[s:e], s, e


# 1. remove the two misplaced mux figures (identified by their captions)
removed = 0
while True:
    i = src.find("multiplexer loop")
    if i < 0:
        break
    s = src.rfind("\\begin{figure}", 0, i)
    e = src.find("\\end{figure}", i) + len("\\end{figure}")
    src = src[:s] + src[e:]
    removed += 1
print("removed misplaced mux figures:", removed)

# 2. restore decision flowchart after eq:hysteresis equation
decision, _, _ = extract_block(head, "Per-message routing decision.")
eq_end = src.find("\\end{equation}",
                  src.find("\\label{eq:hysteresis}")) + len("\\end{equation}")
src = src[:eq_end] + "\n\n" + decision + "\n" + src[eq_end:]

# 3. re-insert redesigned mux figure in IV-D after the receive-loop sentence
MUX_NEW = open("mux_new.tex").read()
anchor = src.find("depicts this receive loop.")
ins = src.find("\n", anchor) + 1
src = src[:ins] + "\n" + MUX_NEW + "\n" + src[ins:]

open("main.tex", "w").write(src)
print("repair complete")
