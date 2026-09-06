#!/usr/bin/env bash
# Website validation: files exist, JSON parses, no localhost/abs paths
# in production assets, internal links resolve.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/website"
FAIL=0
chk() { if eval "$2"; then echo "✓ $1"; else echo "✗ $1"; FAIL=1; fi; }
chk "index.html exists"        "[ -f '$W/index.html' ]"
chk "demo.html exists"         "[ -f '$W/demo.html' ]"
chk "css exists"               "[ -f '$W/css/style.css' ]"
chk "js exists"                "[ -f '$W/js/app.js' ] && [ -f '$W/js/routing.js' ] && [ -f '$W/js/demo.js' ]"
chk "favicon exists"           "[ -f '$W/assets/favicon.svg' ]"
chk "data exists"              "[ -f '$W/data/benchmark.json' ] && [ -f '$W/data/routing.json' ]"
chk "data JSON parses"         "python3 -c \"
import json,glob
[json.load(open(f)) for f in glob.glob('$W/data/*.json')]\""
# localhost must not appear in any href/src attribute (code-block docs
# mentioning the local dev server are fine)
chk "no localhost hrefs/src" "! grep -E '(href|src)="[^"]*localhost' '$W/index.html' '$W/demo.html' '$W/js/'*"
chk "no absolute filesystem paths" "! grep -rEl 'file://|/Users/' '$W/index.html' '$W/demo.html' '$W/js/'"
for img in $(grep -o 'src="assets/[^"]*"' "$W/index.html" | sed 's/src="//;s/"//'); do
    chk "referenced asset $img" "[ -f '$W/$img' ]"
done
exit "$FAIL"
