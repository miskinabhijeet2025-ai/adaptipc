#!/usr/bin/env python3
"""Detect overlapping TEXT lines on the figure pages of main.pdf.

Uses PyMuPDF word bounding boxes. Skips short math-fragment tokens
('+', 'ms', single glyphs) whose sub/superscript boxes naturally
interleave. Two words 'overlap' if their rectangles intersect by more
than 35% of the smaller box area.
"""
import fitz

d = fitz.open("main.pdf")
total = 0
for pno in (3, 4):  # pages 4 and 5 (0-based)
    page = d[pno]
    words = page.get_text("words")
    boxes = [(fitz.Rect(w[:4]), w[4]) for w in words
             if len(w[4]) > 3 and any(c.isalpha() for c in w[4])]
    overlaps = []
    for i in range(len(boxes)):
        r1, t1 = boxes[i]
        for j in range(i + 1, len(boxes)):
            r2, t2 = boxes[j]
            inter = r1 & r2
            if inter.is_empty:
                continue
            a = inter.get_area()
            smaller = min(r1.get_area(), r2.get_area())
            if smaller > 0 and a / smaller > 0.35:
                overlaps.append((t1, t2,
                                 [round(v, 1) for v in r1],
                                 [round(v, 1) for v in r2]))
    print(f"page {pno+1}: {len(boxes)} prose words, "
          f"{len(overlaps)} significantly overlapping word pairs")
    for ov in sorted(overlaps, reverse=True)[:12]:
        print("   overlap:", ov[0], "<->", ov[1], "at", ov[2], ov[3])
print("TOTAL significant text overlaps:", total)
