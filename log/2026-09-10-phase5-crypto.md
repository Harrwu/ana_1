# Phase 5 Report — Crypto Market Support

Date: 2026-09-10
Scope: fractional sizing, crypto quote routing, bracket-free crypto orders,
24/7 orchestrator mode.

---

## Summary

| Task | Status |
|---|---|
| 1. `sharesForMaxFraction` → decimal sizing | Done via new `unitsForMaxFraction`; 20/20 tests pass |
| 2. Crypto quote routing (`/v1beta3`) | Done, **never called against the live API** |
| 3. Strip bracket for crypto, market/gtc | Done |
| 4. `--crypto` flag, 24/7, no clock sync | Done, syntax verified, clock-free confirmed |
| — Crypto trailing stop (not requested) | Added — the existing code would have silently failed |

---

## Task 1 — Fractional sizing

Added `unitsForMaxFraction()` rather than changing `sharesForMaxFraction`'s
return type. Reason: the two paths need **opposite** zero-handling, and merging
them would have broken one.

| | Equities | Crypto |
|---|---|---|
| Precision | whole shares | 8 decimal places |
| `0` means | skip — cap can't buy a share | skip — below minimum notional |
| Sub-1.0 size | invalid, must skip | **valid position** |

`sharesForMaxFraction` is unchanged and `unitsForMaxFraction(...,
fractional=false)` delegates to it, so the whole-share rule has one
implementation and the two cannot drift apart.

**Why the zero rule had to differ:** keeping the equity rule would silently
disable crypto. On a $200 account, 5% is $10, which buys 0.00016666 BTC —
flooring to whole units gives 0, and the bot would never trade crypto at all.
Verified both directions:

```
100k equity, BTC @60k, 5%  -> 0.0833333 BTC  (notional $5000)
200 equity,  BTC @60k, 5%  -> 0.00016666 BTC (would be 0 under equity rule)
10 equity,   BTC @60k, 5%  -> 0              (budget $0.50 < $1.00 minimum)
```

Quantity is floored to 8 decimals because Alpaca rejects over-precise
quantities and the exact precision varies per coin.

---

## Task 2 — Crypto quote routing

`getLatestPrice()` branches on `isCryptoSymbol()` **before** the equity path,
since the endpoint and response shape both differ:

- Endpoint: `/v1beta3/crypto/us/latest/quotes?symbols=BTC/USD`
- Shape: `{"quotes":{"BTC/USD":{"ap":..,"bp":..,"t":..}}}` — read `ap`/`bp`,
  use the midpoint (`from_quote=true`; crypto exposes quotes, never a trade bar)
- The pair is passed through **verbatim, slash included**. Substituting a dash
  or stripping the slash returns a 404.

Equity detection is conservative: a `/` is decisive; the legacy no-slash form
(`BTCUSD`) matches only against a known quote-currency suffix *and* requires an
alphabetic base, so `AMD`, `NVDA`, and `BRK.B` are correctly **not** crypto.

---

## Task 3 — Bracket-free crypto orders

Crypto cards are `order_class=simple` only — a bracket returns `42210000`
("crypto orders not allowed for advanced order_class"). So the crypto path
drops `order_class`, `take_profit`, and `stop_loss` entirely and sends:

```json
{ "symbol": "BTC/USD", "qty": "0.08333333", "side": "buy",
  "type": "market", "time_in_force": "gtc" }
```

`gtc` rather than `day`: Alpaca does not support `day` on crypto, which is also
why the 24/7 runner needs no session boundary.

### The consequence, stated plainly

**A crypto entry has no broker-side stop.** The equity bracket provides one
atomically; the crypto path cannot. Between the fill and the protective
`stop_limit`, the position is unprotected — and if the process dies in that
window, nothing closes it.

### A bug this would have caused, and the fix

`updateTrailingStops()` sent `"type": "stop"` for every position. **Crypto
rejects plain `stop`** (only `market`, `limit`, `stop_limit` are supported), so
every crypto stop would have failed with 42210000 and logged a replace failure
— leaving positions unprotected while appearing to try. Crypto positions now
route to `stop_limit` with the limit 2% below the stop, and `TrailingState`
carries `units` + `fractional` so the two order shapes stay distinct.

This was not in the brief. It's included because the alternative was a
protection path that looks like it works and doesn't.

---

## Task 4 — `--crypto` orchestrator flag

Crypto mode bypasses the clock entirely. `/v2/clock` describes the **US equity
session**; consulting it would park the pipeline until a 09:30 ET bell
irrelevant to `BTC/USD` and shut down at a 16:00 close that doesn't apply.

So there is no warmup, no open gate, no close: the broker is live from
startup. Verified by line range (1070–1117) that the branch contains **zero**
calls to `get_alpaca_clock` / `wait_for_warmup_or_open` / `wait_for_market_close`.

`--crypto` is checked before `--now`, so supplying both runs in crypto mode.

Credentials are still required: sizing reads equity from `/v2/account`.

---

## A note on my own process

While implementing Task 1, two overlapping edits corrupted `broker_api.cpp` —
a duplicated `getLatestPrice` and a deleted `executeTrade` with a fragment
spliced in. I rewrote the file rather than patching further. The rebuild then
surfaced a stray-parenthesis typo I'd introduced in the new stop-limit payload;
it's fixed and all four targets build clean.

---

## Outstanding risks

1. **No crypto API call has ever been made.** The `/v1beta3` path, the `ap`/`bp`
   field names, and the `stop_limit` payload are written against documentation,
   not observed responses. The field names in particular should be confirmed
   against a real quote before trading.
2. **Unprotected window on every crypto entry.** Structural, not a bug — see
   Task 3. Mitigation would require a monitoring loop fast enough to place the
   stop immediately after fill.
3. **Crypto symbol supply.** `extractCashtags` matches against
   `data/companies.csv`, which holds equity company names. Nothing currently
   produces `BTC/USD` or `ETH/USD`, so the pipeline has no crypto signals to
   act on until that data is added.
4. **The strategy is still unvalidated.** Everything here is plumbing. No
   backtest in five sessions has shown positive expectancy.
5. **Fees.** Alpaca charges crypto maker/taker fees, which the backtests do not
   model at all.

## Next steps

1. Verify the crypto quote response shape against a live
   `data.alpaca.markets` call before trusting field names.
2. Add crypto pairs to `data/companies.csv` (or seed the crawler with
   crypto-specific sources) — without this, crypto mode has nothing to trade.
3. Backtest with crypto fees included before drawing any conclusion.
