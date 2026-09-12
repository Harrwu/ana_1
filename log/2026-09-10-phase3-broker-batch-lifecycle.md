# Phase 3 Report — Broker Integration, Batch Signals, Crawler Lifecycle

Date: 2026-09-10
Scope: broker wired into the AI server, per-ticker autonomous execution, batch
completion signals, crawler graceful shutdown.

---

## Summary

| Task | Status |
|---|---|
| 1. Broker init from env, `broker_lock` | Done |
| 1. `getLatestPrice()` + parsing | Done, **never called against the live API** |
| 1. Autonomous BULLISH → trade | Done, **never executed against the broker** |
| 1. `orders.log` + stdout logging | Done |
| 2. `[AI BATCH START]` / `[AI BATCH COMPLETE]` / `[AI STATUS]` | Done |
| 3. `[CRAWLER MILESTONE]` / `[CRAWLER DONE]` | Done, tested |
| 3. `stop()` graceful shutdown | Done, tested |
| — SIGINT/SIGTERM handler | Added to `test_main.cpp` |

---

## Task 1 — Broker integration

### Lifecycle

`broker` is a file-scope `std::unique_ptr<AlpacaBroker>`, created in `main()`
from `APCA_API_KEY_ID` / `APCA_API_SECRET_KEY`. Missing credentials are **not
fatal** — the server still scores sentiment and writes the CSV, it just
doesn't trade. This keeps the AI pipeline testable on a machine with no broker
access.

Paper endpoint is hardcoded. Switching to live requires editing that line;
there is deliberately no environment toggle, because going live should be a
decision rather than a config change.

### On `broker_lock` — the brief aimed at the wrong object

The brief says the mutex prevents threads from "interleaving TLS writes on the
socket." **There is no shared socket**: every `request()` call constructs its
own `TlsSocket_`. The real shared mutable state is the broker's `positions_`
vector (written by `trackPosition`, iterated by `updateTrailingStops`) and the
API key material. `broker_lock` is correct and necessary, just for a different
reason than stated.

Everything touching the broker is under one lock, **including the price
lookup** — `getLatestPrice` is a plain method on the same object and would
otherwise race with `trackPosition` on the position book.

### Price lookup

`getLatestPrice()` issues `GET data.alpaca.markets/v2/stocks/{ticker}/bars/latest?feed=iex`
via the existing `TlsSocket_` / `ReqBuild`. `request()` was refactored into
`request_to(host, ...)` because the market-data API is a **different host**
from the trading API.

Three things the brief didn't account for:

1. **`feed=iex` is pinned, not defaulted.** On a paper/free account the default
   already resolves to `iex`, but if the account is ever upgraded the default
   silently becomes `sip` — changing both the data and the rate limits. An
   explicit `feed=sip` without the subscription fails with
   `{"code":42210000,"message":"subscription does not permit querying recent SIP data"}`.
2. **The `/bars/latest` response nests the bar under a `"bars"` key** keyed by
   symbol, so a plain top-level field lookup would miss. The parser anchors its
   search inside `"bars"`.
3. **A quotes/latest fallback** handles symbols with no latest trade on the IEX
   feed, using the bid/ask midpoint (flagged `from_quote` so the log shows
   which source produced the price).

### Staleness guard

`isQuoteFresh()` rejects quotes older than 120s. This matters because **IEX is
roughly 2.5% of US volume** — a thin or stale print should not drive a trade.
The timestamp is parsed with `timegm` so it is read as UTC rather than local
time.

**Caveat carried into the code:** the IEX price is NOT the consolidated NBBO
print. It is adequate to size a paper order and enforce the 5% cap; it is not
adequate to compute a precise edge, and bracket stop/target levels derived from
it are approximate.

### Portfolio-level cap (addition)

`executeTrade`'s 5% cap is **per order**. An article matching several tickers
can produce several bullish verdicts in one batch, each individually under 5%,
summing to far more of the account than intended. Added a cumulative
`deployed_fraction` tracked under `broker_lock`, with a
`kMaxPortfolioFraction = 0.25` ceiling that skips new positions once reached.
The fraction is recomputed from the **actual floored share count**, not the
notional 5%, since `executeTrade` floors to whole shares.

### Logging

`orders.log` + stdout, one line per verdict:

```
<TIMESTAMP> | <TICKER> | FILLED | id=... | status=accepted | entry~$... | stop~$... | target~$... | price_src=last
<TIMESTAMP> | <TICKER> | SKIP | stale quote (2026-09-10T14:31:00Z)
<TIMESTAMP> | <TICKER> | REJECTED | <message> | http=403
```

Skip reasons are explicit: no broker, portfolio cap, no price, stale quote.
`orders.log` uses a **separate mutex** from `csv_lock` so a slow broker
submission can't block signal logging.

---

## Task 2 — Batch signals

`[AI BATCH START]` prints the ticker count and article length on receipt.
`[AI BATCH COMPLETE]` prints scored/total plus elapsed milliseconds, and
`[AI STATUS]` gives the BULLISH/BEARISH/NEUTRAL breakdown.

Ordering note: scoring and CSV logging complete **before** any broker call, so
a broker failure degrades to "signal recorded, no trade" rather than losing the
signal. Detached workers and per-connection socket handling are unchanged — the
server never blocks on a slow order.

---

## Task 3 — Crawler lifecycle

### `[CRAWLER DONE]`

`processUrl` now returns a `UrlOutcome {links_found, dispatched}` and the worker
prints the completion line. Link counting covers the XML/RSS path and redirects;
note that **HTML article pages don't increment it** — `HtmlParse_::extract()`
doesn't expose its link list the way `RssParse_` does, so HTML-sourced links
report 0. `Dispatched to AI` is accurate for all paths.

### `stop()` and graceful shutdown

`stop()` sets `stop_requested_`, calls `queue_cv_.notify_all()`, and returns.
`run()` owns joining. A SIGINT/SIGTERM handler was added to `test_main.cpp`; the
handler only sets an atomic flag (taking mutexes and joining threads is not
async-signal-safe), and a watcher thread performs the real shutdown.

### Two real bugs found and fixed by testing

**1. Concurrent join / data race (shutdown hung).** My first version had both
`run()` and `stop()` iterate and `clear()` the shared `workers_` vector. Since
`stop()` is typically called from another thread while `run()` is blocked
joining, that was a data race plus a double-join. Symptom: `run()` returned,
then the process wedged with one thread parked and the final save never
happening. Fixed by making `run()` the sole joiner; `stop()` only signals.

**2. Drain milestone never fired.** The condition was
`idle_workers_ + 1 >= worker_count_`, which required every other worker to be
parked at the same instant. Workers cycle through that branch asynchronously,
so the alignment essentially never occurred — verified by a 3-worker test with
one URL that still failed to print. Replaced with an explicit `in_flight_`
counter: drained means `url_queue.empty() && in_flight_ == 0`. Now fires
reliably.

Note: an earlier "hang" I reported during testing was a **test artifact** —
piping output through `head` was interfering with the process, not a product
bug. Re-tested with file redirection and the shutdown path was correct.

### Verification

- Clean shutdown from an idle queue: exit 0, no hang, history flushed.
- Natural drain: `[CRAWLER DONE]` then `[CRAWLER MILESTONE]` in that order.
- All four CMake targets build clean.

---

## Outstanding risks

1. **No broker call has ever been made.** `getLatestPrice`, `executeTrade`, the
   order-submission path, and the new `sip`/`iex` handling are all
   **unexercised**. The JSON field parsing in particular is written against
   documented response shapes, not observed ones.
2. **The strategy still has no demonstrated edge.** Everything is now wired to
   trade autonomously on signals that have never shown positive expectancy.
3. **Signals fire when markets are closed.** From the Phase 2 finding: ~60% of
   signals arrived outside tradeable hours. With autonomous execution live,
   those now produce `SKIP | no price available` or stale-quote lines rather
   than trades — the cap and freshness guards contain the damage, but the
   underlying scheduling problem is unresolved.
4. **IEX price quality.** Brackets are derived from a feed covering ~2.5% of
   volume.

## Next steps

1. **Before enabling trading:** run `brokercli account` with paper credentials
   to confirm auth, then `brokercli buy AAPL BULLISH <price> --dry-run`, then a
   single paper order. Treat the price parsing as unverified until a real
   response comes back.
2. Verify `getLatestPrice` against a live `data.alpaca.markets` response —
   field names should be confirmed, not assumed.
3. Decide the market-hours scheduling question, which now has money attached.
