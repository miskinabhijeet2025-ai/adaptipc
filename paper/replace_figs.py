#!/usr/bin/env python3
"""Replace Fig. arch and Fig. mux TikZ blocks with redesigned versions."""
import sys

src = open("paper/main.tex").read()

# ---------------- new architecture figure (double-column) --------------
ARCH_NEW = r"""\begin{figure*}[t]
\centering
\begin{tikzpicture}[font=\scriptsize,>=Latex,
  box/.style={draw,rounded corners=1pt,align=center,
              text width=30mm,inner sep=3pt},
  wbox/.style={draw,densely dotted,rounded corners=1pt,align=center,
               text width=24mm,inner sep=2pt},
  lbl/.style={font=\tiny,align=center}]
% ---------- sender (producer role) ----------
\node[box] (app) {Application};
\node[box,below=5mm of app,text width=34mm] (send)
  {\texttt{adapt\_send()}: EWMA update $+$ hysteresis route decision};
\node[wbox] (tagw) at ($(send.east)+(15mm,6mm)$)
  {wire frame\\\texttt{[1-B tag $|$ payload]}};
\node[box] (shmw) at ($(send.east)+(38mm,6mm)$)
  {\textbf{SHM ring --- producer}\\ scatter-write
   \texttt{[1-B tag $|$ payload]}\\ release-store \texttt{head}};
\node[box] (udsw) at ($(send.east)+(38mm,-14mm)$)
  {\textbf{UDS socket --- send}\\ \texttt{sendto()} datagram\\
   (\texttt{ENOBUFS}/\texttt{EINTR} retry)};
% sender arrows
\draw[->] (app) -- (send);
\draw[->] (send) -- (tagw);
\draw[->] (tagw) -- (shmw);
\draw[->] (send.east) to[out=-25,in=180]
  node[lbl,midway,below]{\texttt{sendto()}} (udsw.west);
% escalation guard (distinct dashed path)
\draw[->,dashed] (udsw.north) to[out=105,in=-75]
  node[lbl,midway,right]{\textbf{escalation guard}: classified UDS but
  oversized ($>$ \texttt{UDS\_MAX\_DGRAM})\\ $\to$ sent via SHM;
  EWMA state unchanged} (shmw.south);

% ---------- shared-memory and datagram wires ----------
\node[wbox] (wirem) at ($(shmw.east)+(20mm,0)$)
  {POSIX shared memory\\ (head/tail cursors)};
\node[wbox] (wireu) at ($(shmw.east)+(20mm,-14mm)$)
  {AF\_UNIX datagram\\ (kernel socket queue)};

% ---------- receiver (consumer role) ----------
\node[box] (shmr) at ($(wirem.east)+(16mm,0)$)
  {\textbf{SHM ring --- consumer}\\ non-blocking pop\\
   acquire-load \texttt{head}\\ release-store \texttt{tail}};
\node[box] (udsr) at ($(wireu.east)+(16mm,0)$)
  {\textbf{UDS --- receive}\\ \texttt{poll(2)}-bounded wait (2\,ms)\\
   \texttt{recvfrom()} datagram};
\node[box] (mux) at ($(shmr.east)+(16mm,-7mm)$)
  {\texttt{adapt\_recv()} multiplexer:\\ demultiplex by 1-B transport tag};
\node[box] (app2) at ($(mux.east)+(13mm,-1.5mm)$) {Application};

% receiver arrows
\draw[<->] (shmw.east) -- (wirem.west);
\draw[->] (wirem.east) -- (shmr.west);
\draw[<->] (udsw.east) -- (wireu.west);
\draw[->] (wireu.east) -- (udsr.west);
\draw[->] (shmr.south) to[out=-90,in=155] (mux.150);
\draw[->] (udsr.north) to[out=90,in=25] (mux.30);
\draw[->] (mux) -- (app2);
\end{tikzpicture}
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

MUX_NEW = r"""\begin{figure}[t]
\centering
\begin{tikzpicture}[font=\scriptsize,>=Latex,
  box/.style={draw,rounded corners=1pt,align=center,
              text width=44mm,inner sep=3pt},
  dec/.style={draw,diamond,aspect=3,align=center,inner sep=1.5pt},
  elbl/.style={font=\tiny,fill=white,inner sep=1pt}]
\node[box] (pop) {non-blocking SHM ring poll\\ (\texttt{shm\_ring\_pop()})};
\node[dec,below=11mm of pop] (avail) {record available?};
\node[box,below=11mm of avail] (wait)
  {\texttt{poll(2)} on UDS socket\\ bounded wait: 2\,ms timeout};
\node[dec,below=9mm of wait] (dgram) {datagram arrived?};
\node[box,right=20mm of avail,text width=30mm] (del1)
  {deliver: strip 1-B transport tag\\ copy payload to caller};
% edges -- labels are standalone nodes kept clear of boxes
\draw[->] (pop.south) -- node[elbl,right]{no} (avail.north);
\draw[->] (avail.west) -| node[elbl,pos=0.75,above]{yes}
  ([xshift=-8mm]del1.north) -| (del1.north);
\draw[->] (avail.south) -- node[elbl,right]{no} (wait.north);
\draw[->] (wait.south) -- node[elbl,right]{after 2\,ms} (dgram.north);
\draw[->] (dgram.west) -| node[elbl,pos=0.2,above]{timeout:
  re-poll ring} ([xshift=-8mm]pop.south);
\draw[->] (dgram.west) -- ++(-6mm,0) |- ([yshift=1mm]del1.west);
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

# ---- locate and replace the architecture figure -----------------------
a = src.find("escalation guard:")
fstart = src.rfind("\\begin{figure}", 0, a)
fend = src.find("\\end{figure}", fstart) + len("\\end{figure}")
src = src[:fstart] + ARCH_NEW + src[fend:]

# ---- locate and replace the mux figure --------------------------------
m = src.find("depicts this receive loop.")
fstart = src.rfind("\\begin{figure}", 0, m)
fend = src.find("\\end{figure}", fstart) + len("\\end{figure}")
src = src[:fstart] + MUX_NEW + src[fend:]

open("paper/main.tex", "w").write(src)
print("both figures replaced")
