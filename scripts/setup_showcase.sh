#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=========================================================="
echo "  AdaptIPC: Showcase Environment Setup"
echo "=========================================================="

cd "${REPO_ROOT}"

# Check Python 3
if ! command -v python3 >/dev/null 2>&1; then
    echo "[-] Error: python3 is required but not installed." >&2
    exit 1
fi

echo "[+] Python 3 detected: $(python3 --version)"

# Check compiler
CC="${CC:-clang}"
if ! command -v "${CC}" >/dev/null 2>&1; then
    CC="/usr/bin/clang"
fi
if ! command -v "${CC}" >/dev/null 2>&1; then
    echo "[-] Warning: clang/cc compiler not found in standard paths." >&2
else
    echo "[+] C Compiler detected: ${CC}"
fi

# Ensure Python requirements
echo "[+] Checking Python packages (matplotlib, numpy, pandas)..."
python3 -c "import matplotlib, numpy, pandas; print('    All Python dependencies verified!')" 2>/dev/null || {
    echo "[*] Installing dependencies from requirements-showcase.txt..."
    python3 -m pip install -r requirements-showcase.txt
}

# Create showcase directory tree
mkdir -p showcase/{dashboard,web,figures,outputs,results,assets,scripts} assets/figures assets/screenshots build-lab

echo "[+] Directory structure verified."
echo "[+] Setup completed successfully!"
echo "    Run './scripts/generate_showcase.sh' to build all figures and reports."
echo "    Run './scripts/demo.sh' for the interactive showcase."
