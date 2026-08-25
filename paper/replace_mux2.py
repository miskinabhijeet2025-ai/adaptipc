#!/usr/bin/env python3
"""Replace the mux figure with a vertical single-column layout."""
src = open("paper/main.tex").read()
anchor = src.find("datagram arrived?")
fstart = src.rfind("\\begin{figure}", 0, anchor)
fend = src.find("\\end{figure}", fstart) + len("\\end{figure}")
print("old mux fig:", fstart, fend)

NEW = r"""\begin{figure}[t]
\centering
\begin{tikzpicture}[font=\scriptsize,>=Latex,
  box/.style={draw,rounded corners=1pt,align=center,
              text width=34mm,inner sep=3pt},
  dec/.style={draw,diamond,aspect=2.6,align=center,inner sep=1.5pt},
  elbl/.style={font=\tiny,fill=white,inner sep=1pt}]
\node[box] (pop) at (0,0)
  {non-blocking SHM ring poll\\ (\texttt{shm\_ring\_pop()})};
\node[dec] (avail) at (0,-1.7) {record available?};
\node[box] (wait) at (0,-3.4)
  {\texttt{poll(2)} on UDS socket\\ bounded wait: 2\,ms};
\node[dec] (dgram) at (0,-5.1) {datagram arrived?};
\node[box,text width=30mm] (del) at (1.8,-7.3)
  {deliver: strip 1-B transport tag, copy payload to caller};
% ring-record path: yes -> right lane -> down into deliver
\draw[->] (avail.east) -- ++(14mm,0)
  node[elbl,pos=0.3,above]{yes} |-
  ([xshift=7mm]del.north) -- (del.north);
% no -> poll UDS
\draw[->] (avail.south) -- node[elbl,right]{no} (wait.north);
\draw[->] (wait.south) -- (dgram.north);
\draw[->] (dgram.south) -- node[elbl,right]{yes} (del.north);
% timeout: re-poll ring (left lane)
\draw[->] (dgram.west) -- ++(-15mm,0)
  node[elbl,pos=0.45,above]{timeout: re-poll ring} |-
  ([xshift=-9mm]pop.west) -- (pop.west);
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
open("main.tex", "w").write(src)
print("mux rewritten (vertical layout)")
