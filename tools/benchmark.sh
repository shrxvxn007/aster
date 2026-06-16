#!/bin/bash
echo "Generating 100k events..."
./generate_itch large.itch 100000
echo "Single‑threaded benchmark:"
./aster large.itch --benchmark
echo ""
echo "Backtest (direct loop):"
./aster large.itch --sharpe-interval 100
echo ""
echo "Multi‑threaded throughput:"
./multithreaded_aster large.itch
