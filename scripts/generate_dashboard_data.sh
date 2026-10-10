#!/usr/bin/env bash
set -euo pipefail
trap 'echo "Scenario generation failed (line $LINENO)." >&2' ERR
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Wpedantic \
    "$project_root/market_maker_demo.cpp" -o "$project_root/market_maker_demo"
mkdir -p "$project_root/dashboard/data"
for scenario in spread-capture inventory-risk inventory-limit; do
    "$project_root/market_maker_demo" --scenario "$scenario" \
        --output "$project_root/dashboard/data/$scenario.json"
done
