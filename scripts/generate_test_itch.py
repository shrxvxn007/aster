#!/usr/bin/env python3
"""
generate_test_itch.py — write a small deterministic ITCH-style sample file.

Produces `samples/test.itch` (configurable) containing:
  * a file header (`ITCH` magic, version=1, symbol_count=2)
  * two symbols ("AAPL", "MSFT")
  * a MarketOpen system event
  * alternating bids and asks across both symbols with deterministic
    prices and quantities, with periodic mid-market sweep orders
  * a MarketClose system event at the end

This lets a reader clone the repo and run the backtester immediately:

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    python3 scripts/generate_test_itch.py --out samples/test.itch
    ./build/aster/aster_replay --itch-file samples/test.itch --speed batch

The file is bit-compatible with `aster::replay::ItchParser` (see
src/replay/parser.cpp for the expected layout).

Usage:
  python3 scripts/generate_test_itch.py [--out PATH] [--seconds N]
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


# ---- numeric write helpers (all big-endian) -------------------------------

def u8(v: int) -> bytes:
    return struct.pack(">B", v & 0xFF)


def u16(v: int) -> bytes:
    return struct.pack(">H", v & 0xFFFF)


def u32(v: int) -> bytes:
    return struct.pack(">I", v & 0xFFFFFFFF)


def u48(v: int) -> bytes:
    """ITCH 7-byte (56-bit) timestamp — we emit it big-endian in 7 bytes."""
    if not 0 <= v < (1 << 56):
        raise ValueError(f"u48: value {v} out of range")
    return v.to_bytes(7, "big")


def u64(v: int) -> bytes:
    return struct.pack(">Q", v & 0xFFFFFFFFFFFFFFFF)


# ---- ITCH message builders ------------------------------------------------

SIDE_BUY = 0
SIDE_SELL = 1

SYS_OPEN = ord("O")
SYS_CLOSE = ord("C")
SYS_HALT = ord("H")
SYS_RESUME = ord("R")


def msg_system(ts: int, code: int) -> bytes:
    return u8(ord("S")) + u48(ts) + u8(code)


def msg_add(ts: int, oid: int, sym: int, side: int, price: int, qty: int) -> bytes:
    return u8(ord("A")) + u48(ts) + u64(oid) + u16(sym) + u8(side) + u64(price) + u32(qty)


def msg_execute(ts: int, oid: int, qty: int) -> bytes:
    return u8(ord("E")) + u48(ts) + u64(oid) + u32(qty)


def msg_cancel(ts: int, oid: int, qty: int) -> bytes:
    return u8(ord("C")) + u48(ts) + u64(oid) + u32(qty)


def msg_delete(ts: int, oid: int) -> bytes:
    return u8(ord("D")) + u48(ts) + u64(oid)


def msg_l2(ts: int, sym: int, side: int, price: int, qty: int, n_orders: int) -> bytes:
    return (
        u8(ord("L"))
        + u48(ts)
        + u16(sym)
        + u8(side)
        + u64(price)
        + u32(qty)
        + u32(n_orders)
    )


# ---- synthetic stream -----------------------------------------------------

def write_header(f, symbol_table: list[tuple[str, int]]) -> None:
    f.write(b"ITCH")
    f.write(u32(1))              # version
    f.write(u32(len(symbol_table)))
    for name, sid in symbol_table:
        f.write(u8(len(name)))
        f.write(name.encode("ascii"))
        f.write(u16(sid))


def synth(out_path: Path, seconds: int) -> int:
    """Build a deterministic order-book history.

    Returns the number of messages written.
    """
    out_path.parent.mkdir(parents=True, exist_ok=True)
    seconds = max(1, seconds)

    sym_aapl = 0
    sym_msft = 1
    table = [("AAPL", sym_aapl), ("MSFT", sym_msft)]

    msgs: list[bytes] = []
    msgs.append(msg_system(1_000_000_000, SYS_OPEN))

    # Prices in the ITCH file are uint64; the matching engine interprets them
    # as fixed-point scaled by 1e5 (kPriceScale). So $100.00 → 100 * 1e5.
    SCALE = 100_000
    aapl_mid = 150 * SCALE   # $150.00
    msft_mid = 300 * SCALE   # $300.00
    tick = SCALE // 100      # $0.01

    next_oid = 1
    t0 = 1_000_000_000 + 1     # 1 ns after market open
    ns_per_step = 100_000_000  # 100 ms steps over `seconds` elapsed

    # Pre-fetch some passive quotes alongside synthetic aggressors.
    securities = [(sym_aapl, aapl_mid, SIDE_BUY), (sym_msft, msft_mid, SIDE_BUY),
                  (sym_aapl, aapl_mid, SIDE_SELL), (sym_msft, msft_mid, SIDE_SELL)]

    for step in range(seconds * 10):  # 10 steps / second of wall clock
        ts = t0 + step * ns_per_step

        for sym_id, mid, side in securities:
            # Random-walk the mid ±2 ticks and queue a passive order ±5 ticks.
            delta = ((step + sym_id) % 5) - 2
            px = mid + delta * tick
            oid = next_oid
            next_oid += 1
            qty = 10 + ((step + sym_id + 1) % 7)
            msgs.append(msg_add(ts, oid, sym_id, side, px, qty))

            # Every 4th step a taker sweeps half of the first resting order.
            if step % 4 == 0 and msgs:
                # Execute the most recent opposite-side passive from this symbol.
                aggressor_oid = next_oid
                next_oid += 1
                aggressor_side = SIDE_SELL if side == SIDE_BUY else SIDE_BUY
                msgs.append(
                    msg_add(ts + 50, aggressor_oid, sym_id, aggressor_side,
                            px - tick if aggressor_side == SIDE_BUY else px + tick,
                            # Tight price to cross.
                            max(1, qty // 2))
                )

            # Every 5th step a partial cancel.
            if step % 5 == 0:
                msgs.append(msg_cancel(ts + 100, oid, max(1, qty // 3)))

            # Every 7th step an L2 depth update.
            if step % 7 == 0:
                msgs.append(msg_l2(ts + 200, sym_id, side, px, qty, 1))

    msgs.append(msg_system(t0 + seconds * 10 * ns_per_step + 1, SYS_CLOSE))

    with out_path.open("wb") as f:
        write_header(f, table)
        for m in msgs:
            f.write(m)

    return len(msgs)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    repo_root = Path(__file__).resolve().parent.parent
    parser.add_argument(
        "--out",
        type=Path,
        default=repo_root / "samples" / "test.itch",
        help="Output .itch path (default: samples/test.itch in repo root).",
    )
    parser.add_argument(
        "--seconds",
        type=int,
        default=60,
        help="Approximate simulated seconds of book activity (default 60).",
    )
    args = parser.parse_args(argv)

    n = synth(args.out.resolve(), args.seconds)
    size_kb = args.out.stat().st_size / 1024
    print(f"Wrote {n} messages ({size_kb:.1f} KB) to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
