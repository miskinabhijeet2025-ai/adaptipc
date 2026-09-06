#!/usr/bin/env python3
"""
generate_diagrams.py -- generates publication-grade architecture and pipeline diagrams.
"""
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "assets"
ASSETS.mkdir(parents=True, exist_ok=True)
SHOWCASE_ASSETS = ROOT / "showcase" / "assets"
SHOWCASE_ASSETS.mkdir(parents=True, exist_ok=True)

def draw_architecture():
    fig, ax = plt.subplots(figsize=(10.5, 6.5), dpi=200)
    ax.set_xlim(0, 100)
    ax.set_ylim(0, 100)
    ax.axis("off")

    ax.add_patch(mpatches.Rectangle((0, 0), 100, 100, color="#fdfdfd"))
    ax.text(50, 96, "AdaptIPC: High-Performance Adaptive Hybrid IPC Architecture",
            ha="center", va="center", fontsize=15, fontweight="bold", color="#1a1a1a")

    # Producer Box
    p_box = mpatches.FancyBboxPatch((4, 18), 36, 72, boxstyle="round,pad=1.2",
                                    facecolor="#eef4fb", edgecolor="#2b5c8f", lw=2)
    ax.add_patch(p_box)
    ax.text(22, 86, "Producer Process", ha="center", fontsize=13, fontweight="bold", color="#1c3d5a")

    # Consumer Box
    c_box = mpatches.FancyBboxPatch((60, 18), 36, 72, boxstyle="round,pad=1.2",
                                    facecolor="#f3f9f3", edgecolor="#2e7d32", lw=2)
    ax.add_patch(c_box)
    ax.text(78, 86, "Consumer Process", ha="center", fontsize=13, fontweight="bold", color="#1b4d1e")

    # Producer App Layer
    ax.add_patch(mpatches.FancyBboxPatch((7, 72), 30, 8, boxstyle="round,pad=0.5",
                                        facecolor="#ffffff", edgecolor="#2b5c8f", lw=1.2))
    ax.text(22, 76, "Application (adapt_send)", ha="center", va="center", fontsize=11, fontweight="bold")

    # Router Engine
    ax.add_patch(mpatches.FancyBboxPatch((7, 40), 30, 26, boxstyle="round,pad=0.5",
                                        facecolor="#ffffff", edgecolor="#e67e22", lw=1.5))
    ax.text(22, 62, "Adaptive Routing Core", ha="center", fontsize=11, fontweight="bold", color="#d35400")
    ax.text(22, 56, "* Online EWMA Payload Tracker", ha="center", fontsize=9.5, color="#333")
    ax.text(22, 50, "* Dual-Threshold Deadband Hysteresis", ha="center", fontsize=9.5, color="#333")
    ax.text(22, 44, "* Hardware Cost Model & FSM Health", ha="center", fontsize=9.5, color="#333")

    # Producer transport endpoints
    ax.add_patch(mpatches.Rectangle((7, 22), 13, 12, facecolor="#e8f5e9", edgecolor="#388e3c", lw=1.2))
    ax.text(13.5, 28, "SHM Ring\nProducer", ha="center", va="center", fontsize=9.5, fontweight="bold", color="#1b5e20")

    ax.add_patch(mpatches.Rectangle((24, 22), 13, 12, facecolor="#e1f5fe", edgecolor="#0288d1", lw=1.2))
    ax.text(30.5, 28, "UDS Socket\nClient", ha="center", va="center", fontsize=9.5, fontweight="bold", color="#01579b")

    # Consumer transport endpoints
    ax.add_patch(mpatches.Rectangle((63, 22), 13, 12, facecolor="#e8f5e9", edgecolor="#388e3c", lw=1.2))
    ax.text(69.5, 28, "SHM Ring\nConsumer", ha="center", va="center", fontsize=9.5, fontweight="bold", color="#1b5e20")

    ax.add_patch(mpatches.Rectangle((80, 22), 13, 12, facecolor="#e1f5fe", edgecolor="#0288d1", lw=1.2))
    ax.text(86.5, 28, "UDS Socket\nServer", ha="center", va="center", fontsize=9.5, fontweight="bold", color="#01579b")

    # Consumer Multiplexer & App Layer
    ax.add_patch(mpatches.FancyBboxPatch((63, 45), 30, 18, boxstyle="round,pad=0.5",
                                        facecolor="#ffffff", edgecolor="#2e7d32", lw=1.5))
    ax.text(78, 57, "Dual-Path Demultiplexer", ha="center", fontsize=11, fontweight="bold", color="#1b5e20")
    ax.text(78, 50, "Zero-Copy Poll -> Polling Fallback", ha="center", fontsize=9.5, color="#333")

    ax.add_patch(mpatches.FancyBboxPatch((63, 72), 30, 8, boxstyle="round,pad=0.5",
                                        facecolor="#ffffff", edgecolor="#2e7d32", lw=1.2))
    ax.text(78, 76, "Application (adapt_recv)", ha="center", va="center", fontsize=11, fontweight="bold")

    # Inter-process channels
    ax.annotate("", xy=(63, 30), xytext=(20, 30),
                arrowprops=dict(arrowstyle="->", lw=2.5, color="#2e7d32"))
    ax.text(41.5, 33, "Lock-Free POSIX SHM Ring (Zero-Copy)\n[Atomic Tail/Head, 64 MB Buffer]",
            ha="center", fontsize=9, fontweight="bold", color="#1b5e20")

    ax.annotate("", xy=(80, 24), xytext=(37, 24),
                arrowprops=dict(arrowstyle="->", lw=2, color="#0288d1", linestyle="--"))
    ax.text(58.5, 12, "UNIX Domain Socket (DGRAM / Fallback / Small Msgs)",
            ha="center", fontsize=9, fontweight="bold", color="#01579b")

    # Internal arrows
    ax.annotate("", xy=(22, 66), xytext=(22, 72), arrowprops=dict(arrowstyle="->", lw=1.5, color="#333"))
    ax.annotate("", xy=(13.5, 34), xytext=(18, 40), arrowprops=dict(arrowstyle="->", lw=1.5, color="#2e7d32"))
    ax.annotate("", xy=(30.5, 34), xytext=(26, 40), arrowprops=dict(arrowstyle="->", lw=1.5, color="#0288d1"))

    ax.annotate("", xy=(74, 45), xytext=(69.5, 34), arrowprops=dict(arrowstyle="->", lw=1.5, color="#2e7d32"))
    ax.annotate("", xy=(82, 45), xytext=(86.5, 34), arrowprops=dict(arrowstyle="->", lw=1.5, color="#0288d1"))
    ax.annotate("", xy=(78, 72), xytext=(78, 63), arrowprops=dict(arrowstyle="->", lw=1.5, color="#333"))

    out1 = ASSETS / "architecture.png"
    fig.savefig(out1, bbox_inches="tight")
def draw_decision_pipeline():
    fig, ax = plt.subplots(figsize=(10.5, 6.0), dpi=200)
    ax.set_xlim(0, 100)
    ax.set_ylim(0, 100)
    ax.axis("off")

    ax.add_patch(mpatches.Rectangle((0, 0), 100, 100, color="#fafafa"))
    ax.text(50, 95, "AdaptIPC: Real-Time Dynamic Routing Decision Pipeline",
            ha="center", va="center", fontsize=15, fontweight="bold", color="#111")

    # Step 1: Incoming Message
    ax.add_patch(mpatches.FancyBboxPatch((5, 52), 16, 26, boxstyle="round,pad=0.6",
                                        facecolor="#e8eaf6", edgecolor="#3949ab", lw=1.5))
    ax.text(13, 71, "1. Ingress", ha="center", fontsize=11, fontweight="bold", color="#1a237e")
    ax.text(13, 62, "Message size S\n+ Latency Target", ha="center", fontsize=9.5, color="#283593")

    # Step 2: Smoothing & EWMA
    ax.add_patch(mpatches.FancyBboxPatch((26, 52), 20, 26, boxstyle="round,pad=0.6",
                                        facecolor="#e0f2f1", edgecolor="#00897b", lw=1.5))
    ax.text(36, 71, "2. Trend Filter", ha="center", fontsize=11, fontweight="bold", color="#004d40")
    ax.text(36, 61, "EWMA update:\nS_t = (1-a)S_t-1\n+ a * S", ha="center", fontsize=9.5, color="#004d40")

    # Step 3: Deadband & Hysteresis
    ax.add_patch(mpatches.FancyBboxPatch((51, 52), 20, 26, boxstyle="round,pad=0.6",
                                        facecolor="#fff3e0", edgecolor="#fb8c00", lw=1.5))
    ax.text(61, 71, "3. Hysteresis Guard", ha="center", fontsize=11, fontweight="bold", color="#e65100")
    ax.text(61, 61, "Deadband check:\n[tau_low, tau_high]\nAnti-Thrashing", ha="center", fontsize=9.5, color="#e65100")

    # Step 4: Health & Backpressure
    ax.add_patch(mpatches.FancyBboxPatch((76, 52), 19, 26, boxstyle="round,pad=0.6",
                                        facecolor="#fce4ec", edgecolor="#d81b60", lw=1.5))
    ax.text(85.5, 71, "4. Safety Check", ha="center", fontsize=11, fontweight="bold", color="#880e4f")
    ax.text(85.5, 61, "Queue occupancy\n+ Transport FSM\nDegraded / Stalled", ha="center", fontsize=9.5, color="#880e4f")

    # Arrow chain along top
    ax.annotate("", xy=(26, 65), xytext=(21, 65), arrowprops=dict(arrowstyle="->", lw=2, color="#444"))
    ax.annotate("", xy=(51, 65), xytext=(46, 65), arrowprops=dict(arrowstyle="->", lw=2, color="#444"))
    ax.annotate("", xy=(76, 65), xytext=(71, 65), arrowprops=dict(arrowstyle="->", lw=2, color="#444"))

    # Final Dispatch Decisions
    ax.add_patch(mpatches.FancyBboxPatch((20, 12), 26, 22, boxstyle="round,pad=0.6",
                                        facecolor="#e8f5e9", edgecolor="#2e7d32", lw=2))
    ax.text(33, 27, "ROUTE: SHM RING", ha="center", fontsize=11.5, fontweight="bold", color="#1b5e20")
    ax.text(33, 19, "* S >= tau_high & healthy\n* Zero-copy bulk transfer", ha="center", fontsize=9.5, color="#2e7d32")

    ax.add_patch(mpatches.FancyBboxPatch((54, 12), 26, 22, boxstyle="round,pad=0.6",
                                        facecolor="#e1f5fe", edgecolor="#0288d1", lw=2))
    ax.text(67, 27, "ROUTE: UDS DGRAM", ha="center", fontsize=11.5, fontweight="bold", color="#01579b")
    ax.text(67, 19, "* S <= tau_low OR degraded\n* Low-latency small message", ha="center", fontsize=9.5, color="#0288d1")

    # Branches to endpoints
    ax.annotate("", xy=(33, 34), xytext=(61, 52),
                arrowprops=dict(arrowstyle="->", lw=2, color="#2e7d32"))
    ax.annotate("", xy=(67, 34), xytext=(61, 52),
                arrowprops=dict(arrowstyle="->", lw=2, color="#0288d1"))
    ax.annotate("", xy=(67, 34), xytext=(85.5, 52),
                arrowprops=dict(arrowstyle="->", lw=2, color="#d81b60", linestyle="--"))

    out2 = ASSETS / "decision_pipeline.png"
    fig.savefig(out2, bbox_inches="tight")
    fig.savefig(SHOWCASE_ASSETS / "decision_pipeline.png", bbox_inches="tight")
    plt.close(fig)
    print("Generated:", out2)

if __name__ == "__main__":
    draw_architecture()
    draw_decision_pipeline()

