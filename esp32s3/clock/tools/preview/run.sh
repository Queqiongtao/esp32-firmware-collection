#!/usr/bin/env bash
# 主机端主题预览：构建 → 渲染 PPM → 转 PNG + 对比图
set -euo pipefail

cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

./build/preview "$@"
python3 render.py
