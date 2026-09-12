# Phase 4 Report — Realistic Backtester (Latency + Slippage)

Date: 2026-09-10
Scope: 1-minute entry latency, 0.05%/side slippage, bracket exits, metrics.

---

## Summary

| Task | Status |
|---|---|
| 1. Broker integration in `ai_server.cpp` | **Already complete** (Phase 3) — not redone |
| 2. Ingestion / alignment | Already satisfied; CSV is comma-delimited, one ticker per row |
| 2. 1-minute latency injection | Done, verified |
| 2. 0.05% slippage on entry | Done, applied on exit too |
| 2. Bracket exits + 30-min decay | Done, **one real bug found and fixed** |
| 2. Win rate / avg return / max drawdown | Done |

New file: `backtest_realistic.py`

---

## Task 1 — already done in Phase 3

Verified present in `src/ai_engine/ai_server.cpp`:

- `std::unique_ptr<AlpacaBroker> broker` built from `APCA_API_KEY_ID` /
  `APCA_API_SECRET_KEY` (line 39)
- `std::mutex broker_lock` (line 35)
- `getLatestPrice` → `executeTrade` per BULLISH verdict (lines 453, 470)

Two corrections to the brief's framing, carried over from Phase 3's report:

- **`broker_lock` does not guard TLS writes.** Each `request()` builds its own
  `TlsSocket_`; there is no shared socket. The lock protects the broker's
  `positions_` vector, which *is* genuinely shared mutable state.
- **The CSV has never been space-delimited.** `cashtag_extractor.cpp` emits
  comma-separated symbols, and `ai_server.cpp` writes one row per ticker with a
  bare sentiment in the last column.

---

## Task 2 — Realistic backtester

### Ingestion — no change needed

The CSV is comma-delimited with one ticker per row, so `parts[-1] == 'BULLISH'`
is an exact match and `parts[1:-1]` are tickers. `MACRO` is dropped. The
timezone contract is unchanged: parse as UTC → convert to
`America/New_York` → strip tz, because `ai_server.cpp` stamps UTC while
yfinance with `ignore_tz=True` returns naive ET.

This is important to keep: the "strip timezone metadata" instruction is not
sufficient on its own. The two sources disagree by the UTC/ET offset, and
comparing them raw is what made every lookup return `-1` before.

### Latency

`ENTRY_LATENCY = 1 minute`. The search base is `signal_time + latency`, and the
fill is the first bar at or after that, at its **open**. Verified: PLTR signal
`06:36:37` → earliest fill `06:37:37` → actual fill bar `06:38:00`. Without
latency the same signal would have filled at `06:37:00`.

A `ENTRY_TOLERANCE` of 3 minutes rejects stale matches where the market was
closed. This is doing real work here: **14 of 29 signals were rejected** because
the nearest bar was 13 minutes to 4.5 hours away (signals arriving outside
market hours).

### Slippage

`SLIPPAGE_PCT = 0.0005` (0.05%), applied **adversely on both sides**:

- Entry: `open * (1 + 0.0005)` — you pay more than quoted
- Exit: `price * (1 - 0.0005)` — you receive less than quoted

The brief asked only for entry slippage. Exit slippage is added because a stop
that triggers into a falling market does not fill at the stop price; modelling
entry friction but not exit friction would systematically flatter the results.

Bracket levels are derived from the **slipped** entry, matching how a real
bracket behaves. Verified: raw open `171.1600` → slipped entry `171.2456`.

### Exit logic

- `-1%` stop, `+3%` target, from the slipped entry
- **When one bar spans both levels, the STOP is assumed to fill first.**
  Intra-bar ordering is unknowable from OHLC, and assuming the favourable side
  is the classic way a backtest flatters itself.
- Time decay at the configured limit

### A real bug, found by testing

The first version's time-decay fallback filled at `data.index[-1]` — the last
bar of the **entire 5-day download** — not the last bar inside the hold window.
For a 30-minute hold this meant every unresolved trade "exited" a day and a
half later at a price with no relationship to the window, and was mislabelled
`DATA_END`. Symptom: the 30-minute run reported *15 of 15* trades exiting
`DATA_END`, which is not a plausible outcome for trades with 1,700+ bars of
data after entry.

Fixed by tracking the last bar actually visited inside the window and exiting
there. Time decay is defined by the window, so the exit must happen inside it.

### Results

```
TIME DECAY (hold 30 min)              FULL WINDOW (hold 300 min)
Trades            : 15                Trades            : 15
Win rate          : 26.7%             Win rate          : 13.3%
Avg return/trade  : -0.09%            Avg return/trade  : -0.47%
Max drawdown      : -1.58%            Max drawdown      : -5.93%
Exit reasons:                         Exit reasons:
  TIME_DECAY: 15                        STOP_LOSS: 12
                                        TAKE_PROFIT: 2
                                        TIME_DECAY: 1
```

### What these numbers do and do not say

**Neither run shows an edge.** The 30-minute run is marginally better (−0.09%
vs −0.47%) and its drawdown is smaller, but both are negative with 15 trades.
This is not a statistically meaningful sample in either direction.

**The 300-minute run is the more informative one.** Given time to work, 12 of
15 positions hit the −1% stop while only 2 reached +3%. That asymmetry — the
stop being hit six times as often as the target — is the shape of a signal that
is not predicting direction. It is consistent with the Phase 2 finding.

**These fills are not achievable.** The signals are premarket (20 of 29 fire
between 04:00–09:30 ET, 9 fire when the market is closed). Premarket spreads on
these names are frequently several times wider than the assumed 0.05%, so real
costs are higher than modelled. 0.05% is an assumption, not a measurement.

---

## Outstanding risks

1. **No positive expectancy in any run to date.** Every backtest across four
   sessions has been negative or non-resolving. The pipeline is now
   well-instrumented, correctly aligned, and friction-aware; the *strategy* is
   still unvalidated.
2. **Slippage is a guess.** 0.05% was supplied in the brief. Real premarket
   spreads should be measured from quote data before this number is trusted.
3. **Broker path still never executed.** `getLatestPrice` and `executeTrade`
   remain unexercised against the live API.
4. **Sample size.** 15 trades over two days. No conclusion about the strategy
   can be drawn at this size in either direction.

## Next steps

1. Measure real premarket spreads (bid/ask from `/quotes/latest`) for these
   names and replace the 0.05% assumption with an observed distribution.
2. Accumulate signals until the sample is large enough to be worth analyzing —
   at 15 trades it is not.
3. Resolve the market-hours question: restrict crawling to tradeable hours, or
   queue off-hours signals to the next open and backtest that separately.
