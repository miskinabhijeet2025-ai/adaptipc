#!/usr/bin/env python3
"""plot_alpha_sensitivity.py -- Fig. 'alpha tradeoff' for the AdaptIPC paper.

Reads:   benchmarks/alpha_sensitivity.txt   (written by benchmarks/alpha_sweep.c)
Writes:  benchmarks/fig4_alpha_tradeoff.{pdf,png}

Panel (a): messages-to-flip after a sustained control->5000B regime change,
           empirical (from scenario=B lines) vs. the Section III-D flip-time
           bound n > ln((S-e0)/(S-tau_high))/ln(1/(1-alpha)) evaluated with
           the paper's constants (e0=128B settled control level, S=5000B).
Panel (b): EWMA route switches caused by isolated 64KB spikes among control
           traffic (scenario=C lines) -- the spike-immunity knee.
"""
import os
import re

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

SRC = os.path.join(os.path.dirname(__file__), "..", "benchmarks",
                   "alpha_sensitivity.txt")
OUT = os.path.join(os.path.dirname(__file__), "..", "benchmarks",
                   "fig4_alpha_tradeoff")

# Paper constants (Section III-C/D): tau_low=1024, tau_high=4096,
# adversarial input S=5000 B, settled EWMA level e0 ~= 128 B (control stream).
TAU_LOW, TAU_HIGH, S, E0 = 1024.0, 4096.0, 5000.0, 128.0


def predicted_n(alpha):
    """Section III-D bound: consecutive out-of-band messages required."""
    import math
    return math.log((S - E0) / (S - TAU_HIGH)) / math.log(1.0 / (1.0 - alpha))


def main():
    alphas, flips, spikes = [], [], []
    for line in open(SRC):
        m = re.match(
            r"alpha=([\d.]+)\s+scenario=B\(.*msgs_to_SHM_flip=(\d+)", line)
        if m:
            alphas.append(float(m.group(1)))
            flips.append(int(m.group(2)))
        m = re.match(r"alpha=([\d.]+)\s+scenario=C\(.*switches=(\d+)", line)
        if m:
            spikes.append((float(m.group(1)), int(m.group(2))))

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.0, 2.9))

    # (a) responsiveness: empirical markers + predicted bound curve
    grid = [0.03 + 0.01 * k for k in range(80)]
    pred = [max(1.0, round(predicted_n(a))) for a in grid]
    ax1.plot(grid, pred, "-", color="#888", lw=1.2,
             label="bound (Sec. III-D)")
    ax1.plot(alphas, flips, "o", color="#1f77b4", ms=6, label="measured")
    ax1.set_yscale("log")
    ax1.set_xlabel(r"$\alpha$")
    ax1.set_ylabel("messages to flip UDS$\\to$SHM")
    ax1.set_title("(a) Responsiveness", fontsize=9)
    ax1.legend(fontsize=7, loc="upper right")

    # (b) spike immunity: switches caused by isolated 64KB bursts
    sa = sorted(spikes)
    ax2.step([a for a, _ in sa], [s for _, s in sa], where="post",
             color="#d62728", lw=1.6)
    ax2.plot([a for a, _ in sa], [s for _, s in sa], "o",
             color="#d62728", ms=4)
    ax2.set_xlabel(r"$\alpha$")
    ax2.set_ylabel("switches per 100 isolated spikes")
    ax2.set_title("(b) Spike immunity", fontsize=9)
    ax2.annotate("knee:\n$\\alpha \\lesssim 0.1$ immune",
                 xy=(0.05, 200), xytext=(0.15, 120), fontsize=7,
                 arrowprops=dict(arrowstyle="->", lw=0.8))

    for ax in (ax1, ax2):
        ax.tick_params(labelsize=8)
        ax.set_xlim(-0.03, 0.85)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(f"{OUT}.{ext}", dpi=300)
        print(f"wrote {OUT}.{ext}")
    plt.close(fig)


if __name__ == "__main__":
    main()
