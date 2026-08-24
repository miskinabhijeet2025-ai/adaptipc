#!/usr/bin/env python3
"""Test arch-figure variants: which element breaks compilation?"""
import re
import subprocess

src = open("main.tex").read()
anchor = src.find("escalation guard:")
start = src.rfind("\\begin{figure}[t]", 0, anchor)
end = src.find("\\end{figure}", start) + len("\\end{figure}")
block = src[start:end]

MACROS = ("\\newcommand{\\taulow}{\\tau_{\\mathrm{low}}}\n"
          "\\providecommand{\\tauhigh}{\\tau_{\\mathrm{high}}}\n"
          "\\providecommand{\\sbar}{\\bar{S}}\n")


def comp(blk, tag):
    doc = re.sub(r"\\begin\{document\}.*?\\end\{document\}",
                 lambda _: "\\begin{document}\n" + MACROS + blk +
                 "\n\\end{document}", src, flags=re.S)
    open("_v.tex", "w").write(doc)
    r = subprocess.run(["tectonic", "_v.tex"], capture_output=True,
                       text=True)
    errs = re.findall(r"^!(.*)$", r.stdout + r.stderr, re.M)
    ctxm = re.search(r"^l\.\d+ .*$", r.stdout + r.stderr, re.M)
    print(f"{tag}: errors={len(errs)} {errs[:1]} "
          f"{ctxm.group(0).strip()[:60] if ctxm else ''}")
    return len(errs) == 0


# V0 baseline (should fail)
comp(block, "V0-original")

# V1 without the dashed escalation edge
v1 = re.sub(r"\\draw\[->,dashed\].*?\(shm\.east\);\n", "", block,
            flags=re.S)
comp(v1, "V1-no-dashed-edge")

# V2 without the mux |- arrows
v2 = re.sub(r"\\draw\[->\] \(shm\.south\) \|-\([^;]*;", "", block)
v2 = re.sub(r"\\draw\[->\] \(uds\.south\) \|-\([^;]*;", "", v2)
comp(v2, "V2-no-mux-arrows")

# V3 without rt node's math ($\bar S_t$)
v3 = v1.replace("$\\bar S_t$", "EWMA state")
comp(v3, "V3-no-rt-math")

# V4 without edge labels containing parens/plus
v4 = v1.replace("{frame\\\\ (tag + payload)}",
                "{frame (tag+payload)}")
comp(v4, "V4-no-edge-label-parens")
