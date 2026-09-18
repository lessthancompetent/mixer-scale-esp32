#!/bin/sh
# Build and run every host unit test in this folder. Needs g++ on the PATH
# (Windows: C:\msys64\ucrt64\bin).
set -e
cd "$(dirname "$0")"
for src in test_*.cpp; do
  exe="${src%.cpp}"
  echo "== $exe"
  g++ -std=c++17 -Wall -I.. "$src" -o "$exe"
  "./$exe"
done
