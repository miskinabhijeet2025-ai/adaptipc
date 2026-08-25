#!/usr/bin/env python3
"""Replace the arch figure with a redesigned, non-overlapping version."""
import re

src = open("paper/main.tex").read()
anchor = src.find("escalation guard:")
fstart = src.rfind("\\begin{figure}", 0, anchor)
fend = src.find("\\end{figure}", fstart) + len("\\end{figure}")
print("old arch figure:", fstart, fend)

NEW = r"""\begin{figure*}[t]
\centering
\resizebox{\textwidth}{!}{%
\begin{tikzpicture}[font=\scriptsize,>=Latex,
  box/.style={draw,rounded corners=1pt,align=center,
              text width=27mm,inner sep=3pt},
  wbox/.style={draw,densely dotted,rounded corners=1pt,align=center,
               text width=21mm,inner sep=2pt},
  lbl/.style={font=\tiny,align=center}]
% ---------- sender (producer role) ----------
\node[box] (app) {Application};
\node[box,below=5mm of app,text width=32mm] (send)
  {\texttt{adapt\_send()}:\\ EWMA update $+$ hysteresis route decision};
\node[wbox] (tagw) at ($(send.east)+(14mm,5mm)$)
  {wire frame\\\texttt{[1-B tag $|$ payload]}};
\node[box] (shmw) at ($(send.east)+(36mm,5mm)$)
  {\textbf{SHM ring --- producer}\\ scatter-write record\\
   release-store \texttt{head}};
\node[box] (udsw) at ($(send.east)+(36mm,-15mm)$)
  {\textbf{UDS socket --- send}\\ \texttt{sendto()} datagram\\
   (\texttt{ENOBUFS}/\texttt{EINTR} retry)};
\draw[->] (app) -- (send);
\draw[->] (send) -- (tagw);
\draw[->] (tagw) -- (shmw);
\draw[->] (send.east) to[out=-20,in=180]
  node[lbl,midway,below=1mm]{\texttt{sendto()}} (udsw.west);
\draw[->,dashed] (udsw.north) to[out=100,in=-80]
  node[lbl,midway,right,text width=19mm]{\textbf{escalation guard}:\\
   classified UDS but oversized ($>$ \texttt{UDS\_MAX\_DGRAM})\\
   $\to$ sent via SHM; routing state unchanged} (shmw.south);

% ---------- shared-memory and datagram wires ----------
\node[wbox] (wirem) at ($(shmw.east)+(24mm,0)$)
  {POSIX shared memory\\ (head/tail cursors)};
\node[wbox] (wireu) at ($(shmw.east)+(24mm,-15mm)$)
  {AF\_UNIX datagram\\ kernel socket queue};

% ---------- receiver (consumer role) ----------
\node[box] (shmr) at ($(wirem.east)+(15mm,0)$)
  {\textbf{SHM ring --- consumer}\\ non-blocking pop\\
   acquire-load \texttt{head}\\ release-store \texttt{tail}};
\node[box] (udsr) at ($(wireu.east)+(15mm,-15mm)$)
  {\textbf{UDS --- receive}\\ \texttt{poll(2)}-bounded wait (2\,ms)\\
   \texttt{recvfrom()} datagram};
\node[box] (mux) at ($(shmr.east)+(15mm,-7.5mm)$)
  {\texttt{adapt\_recv()} multiplexer:\\ demultiplex by 1-B transport tag};
\node[box] (app2) at ($(mux.east)+(12mm,-1.5mm)$) {Application};

\draw[<->] (shmw.east) -- (wirem.west);
\draw[->] (wirem.east) -- (shmr.west);
\draw[<->] (udsw.east) -- (wireu.west);
\draw[->] (wireu.east) -- (udsr.west);
\draw[->] (shmr.south) to[out=-90,in=155] (mux.150);
\draw[->] (udsr.north) to[out=90,in=30] (mux.30);
\draw[->] (mux) -- (app2);
\end{tikzpicture}}%
\caption{AdaptIPC architecture: one endpoint shown end to end. Sender side
(left): \texttt{adapt\_send()} classifies each message with the
EWMA/hysteresis router (Fig.~\ref{fig:decision}), attaches the one-byte
transport tag, and forwards it either to the lock-free SHM ring producer
(release-stores \texttt{head}) or to the UDS datagram socket. The dashed
edge is the escalation guard: an individual UDS-classified message larger
than the datagram ceiling is sent via SHM without changing the routing
state. Receiver side (right): the \texttt{adapt\_recv()} multiplexer
accepts whichever transport delivers next, strips the transport tag, and
hands the payload to the application; the SHM consumer acquires
\texttt{head} and release-stores \texttt{tail} per pop.}
\label{fig:arch}
\end{figure*}
"""

src = src[:fstart] + NEW + src[fend:]
open("paper/main.tex", "w").write(src)
print("replaced OK")
