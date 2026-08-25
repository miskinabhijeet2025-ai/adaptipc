#!/usr/bin/env python3
"""Replace the mux-loop figure with a non-overlapping redesign."""
import re

src = open("paper/main.tex").read()
anchor = src.find("depicts this receive loop.")
fstart = src.rfind("\\begin{figure}", 0, anchor)
fend = src.find("\\end{figure}", fstart) + len("\\end{figure}")
print("old mux figure:", fstart, fend)

NEW = r"""\begin{figure}[t]
\centering
\begin{tikzpicture}[font=\scriptsize,>=Latex,
  box/.style={draw,rounded corners=1pt,align=center,
              text width=40mm,inner sep=3pt},
  dec/.style={draw,diamond,aspect=3.4,align=center,inner sep=1.5pt},
  elbl/.style={font=\tiny,fill=white,inner sep=1pt}]
\node[box] (pop) at (0,0)
  {non-blocking SHM ring poll\\ (\texttt{shm\_ring\_pop()})};
\node[dec] (avail) at (0,-1.8) {record available?};
\node[box] (wait) at (0,-3.6)
  {\texttt{poll(2)} on UDS socket\\ bounded wait: 2\,ms timeout};
\node[dec] (dgram) at (0,-5.4) {datagram arrived?};
\node[box] (del) at (5.4,-2.7,text width=30mm)
  {deliver:\\ strip 1-B transport tag\\ copy payload to caller};
% edges
\draw[->] (pop.south) -- (avail.north);
\draw[->] (avail.east) -- node[elbl,above,pos=0.35]{yes} (del.west);
\draw[->] (avail.south) -- node[elbl,right]{no} (wait.north);
\draw[->] (wait.south) -- (dgram.north);
\draw[->] (dgram.east) -- node[elbl,above,pos=0.4]{yes} (del.west);
% timeout re-poll loop on the left, well clear of everything
\draw[->] (dgram.west) -- ++(-6.5mm,0)
  node[elbl,pos=0.4,left]{timeout: re-poll ring} |-
  (pop.west);
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

src = src[:fstart] + NEW + src[fend:]
open("paper/main.tex", "w").write(src)
print("mux replaced OK")
