#!/usr/bin/env python3
"""Apply layout fixes: decision-node spacing, arch label lane, mux rebuild."""
t = open("main.tex").read()

# --- fix 1: fig:decision -- bigger diamonds, more vertical room,
#     constrained escalation-box width so lines wrap instead of overflow
t = t.replace(
    "dec/.style={draw,diamond,aspect=2.2,align=center,inner sep=1pt}]",
    "dec/.style={draw,diamond,aspect=3,align=center,inner sep=2pt}]")
t = t.replace("\\node[dec,below=4mm of upd] (d1)",
              "\\node[dec,below=9mm of upd] (d1)")
t = t.replace("\\node[box,right=6mm of d1] (shm)",
              "\\node[box,right=9mm of d1] (shm)")
t = t.replace("\\node[box,right=6mm of d2] (uds)",
              "\\node[box,right=9mm of d2] (uds)")
t = t.replace("\\node[dec,below=4mm of d2] (sticky)",
              "\\node[dec,below=9mm of d2] (sticky)")
t = t.replace("\\node[box,below=4mm of sticky] (eg)",
              "\\node[box,below=9mm of sticky,text width=64mm] (eg)")

# --- fix 2: fig:arch -- escalation label into the empty inter-row
#     corridor with a white background, arc raised
t = t.replace(
    "node[lbl,midway,right,text width=15mm]{\\textbf{escalation guard}:\\\\\n"
    "   classified UDS but oversized ($>$ \\texttt{UDS\\_MAX\\_DGRAM})\\\\\n"
    "   $\\to$ sent via SHM; routing state unchanged}",
    "node[lbl,midway,below=2mm,fill=white,text width=34mm]"
    "{\\textbf{escalation guard}: classified UDS but oversized "
    "($>$ \\texttt{UDS\\_MAX\\_DGRAM}) $\\to$ sent via SHM; routing "
    "state unchanged}")
t = t.replace("to[out=100,in=-80]", "to[out=95,in=-95]")

# --- fix 3: fig:mux -- deliver box clear right, timeout loop far left
i = t.find("datagram arrived?")
fstart = t.rfind("\\begin{figure}", 0, i)
fend = t.find("\\end{figure}", fstart) + len("\\end{figure}")
MUX = r"""\begin{figure}[t]
\centering
\begin{tikzpicture}[font=\scriptsize,>=Latex,
  box/.style={draw,rounded corners=1pt,align=center,
              text width=40mm,inner sep=3pt},
  dec/.style={draw,diamond,aspect=3.4,align=center,inner sep=1.5pt},
  elbl/.style={font=\tiny,fill=white,inner sep=1pt}]
\node[box] (pop) at (0,0)
  {non-blocking SHM ring poll\\ (\texttt{shm\_ring\_pop()})};
\node[dec] (avail) at (0,-1.9) {record available?};
\node[box] (wait) at (0,-4.0)
  {\texttt{poll(2)} on UDS socket\\ bounded wait: 2\,ms timeout};
\node[dec] (dgram) at (0,-6.0) {datagram arrived?};
\node[box,text width=27mm] (del) at (8.4,-3.0)
  {deliver:\\ strip 1-B transport tag\\ copy payload to caller};
\draw[->] (pop.south) -- node[elbl,right]{no} (avail.north);
\draw[->] (avail.east) -- node[elbl,above,pos=0.4]{yes} (del.west);
\draw[->] (avail.south) -- node[elbl,right]{no} (wait.north);
\draw[->] (wait.south) -- (dgram.north);
\draw[->] (dgram.east) -- node[elbl,above,pos=0.45]{yes}
  ([yshift=-3mm]del.south);
% timeout loop: dedicated far-left lane, rotated label along the wire
\draw[->] (dgram.west) -- ++(-3.4cm,0)
  node[elbl,pos=0.45,rotate=90]{timeout: re-poll ring} |-
  ([xshift=-3mm]pop.west) -- (pop.west);
\end{tikzpicture}
\caption{The \texttt{adapt\_recv()} multiplexer loop
(Section~IV-D). Each iteration first polls the shared-memory ring without
blocking; if it is empty, the UDS socket is waited on through a bounded
\texttt{poll(2)} (2\,ms) before the ring is re-polled, so arrivals on
either transport are always observed. Whichever transport delivers next,
the receiver strips the one-byte transport tag and hands the payload to
the caller; this loop is what fixes the backlog deadlock described in the
text.}
\label{fig:mux}
\end{figure}
"""
t = t[:fstart] + MUX + t[fend:]

open("main.tex", "w").write(t)
print("layout fixes applied")
