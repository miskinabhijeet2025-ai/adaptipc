#!/usr/bin/env python3
"""Comment out the dashed escalation edge in the arch figure (bisect)."""
t = open("main.tex").read()
start = t.find("\\draw[->,dashed] (uds.west)")
if start < 0:
    print("edge not found - maybe already disabled")
    raise SystemExit
end_marker = "(shm.east);"
end = t.find(end_marker, start) + len(end_marker)
disabled = t[:start] + "% ESC-EDGE-DISABLED\n" + t[end:]
open("main.tex", "w").write(disabled)
print("escalation edge disabled")
