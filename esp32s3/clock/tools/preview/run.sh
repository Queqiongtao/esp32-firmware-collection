#!/usr/bin/env bash
# 主机端主题预览：构建 → 渲染 PPM → 转 PNG + 对比图
set -euo pipefail

cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# 清掉上一轮产物，避免不同时刻的旧图混进对比图。
rm -rf out/ppm out/raw out/round out/r1.0
rm -f out/*.ppm out/contact_native.png out/contact_m5.png out/contact_all.png out/report.txt

./build/preview "$@"
python3 render.py "$@"
