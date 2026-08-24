#!/usr/bin/env python3
"""Scan main.tex for math-mode ($) parity violations."""
import re

lines = open("main.tex").read().splitlines()
issues = []
parity = False
for ln, line in enumerate(lines, 1):
    scan = line.replace("\\$", "")
    if "%" in scan:
        scan = scan.split("%")[0]
    n = len(re.findall(r"(?<!\\)\$", scan))
    start_parity = parity
    parity = (parity != (n % 2 == 1))

    if re.search(r"\\(tauhigh|taulow|sbar|mathrm)\b", line):
        in_math_at_line = start_parity or n > 0
        if not in_math_at_line and "$" not in line:
            issues.append((ln, "math macro in TEXT mode", line[:80]))

print("final parity:", "math (UNBALANCED!)" if parity else "text (OK)")
for ln, why, ctx in issues:
    print(f"line {ln}: {why}\n    {ctx}")
print("total flagged:", len(issues))
