# Phase 2 Report — Profiles, Per-Ticker AI, Backtest Comparison

Date: 2026-09-10
Scope: 11 new site profiles + per-domain rate limiting, per-ticker sentiment
scoring, 30-minute vs 300-minute exit comparison.

---

## Summary

| Task | Status |
|---|---|
| 1. Profile registry to 20 targets | Done, builds, **not validated on live sites** |
| 1. Per-domain rate limiting | Done, needed by EDGAR |
| 1. Persistent `visited_url` | Already existed (`state_manager`), unchanged |
| 2. Multi-ticker independent scoring | Done, 8/8 parser tests pass, **not run against live Ollama** |
| 3. Backtest ingestion + BULLISH isolation | Already existed, extended |
| 3. Risk bracket (−1% / +3%) | Already existed, unchanged |
| 3. 30-minute decay exit | Implemented as a **comparison**, not a replacement |

---

## Task 1 — Profiles (9 → 20)

### New profiles

Bloomberg, Reuters, SEC EDGAR, CoinDesk, CoinTelegraph, Decrypt, Barron's,
Nasdaq, Business Wire, GlobeNewswire, Financial Times.

Only **SEC EDGAR** has a bespoke `extractNewsText`. Its filings are inline-XBRL
financial tables, not article prose — a `<p>` scan finds almost nothing — so it
uses `stripAllTags` over the filing body instead. The other 10 inherit
`extractArticleBody()`.

**This was deliberate.** Writing 11 hand-tuned `extractNewsText` overrides is
what produced the original whole-page bug: every profile guessed at container
classes and fell back to the entire page. The generic scorer already handles
these sites, and a site only earns an override when real output shows it needs
one. Add container class names to `commonArticleClasses()` first.

### Base-class changes

- `userAgent()` — default returns the original `OptionsScraper/1.0`.
  **SEC EDGAR overrides it** because EDGAR 403s a generic UA; it requires a
  descriptive agent with contact info. The placeholder address in
  `SecEdgarProfile::userAgent()` must be replaced before enabling that profile.
- `minSecondsBetweenRequests()` — default 1.0s; EDGAR 0.125s (8 req/s, under
  its documented 10 req/s ceiling); CoinDesk/CoinTelegraph 2.0s.

### Per-domain rate limiting

Added `waitForRateLimit()` in `engine.cpp`, keyed on host via a
`std::map<std::string, steady_clock::time_point>`.

Two things this fixes:

1. **EDGAR compliance.** Exceeding 10 req/s gets the IP temporarily banned.
2. **A pre-existing latent bug.** Nothing previously spaced requests to the same
   host, so all 8 workers could hit one domain simultaneously.

The limiter reserves the next slot *inside* the lock and then sleeps *outside*
it, so waiting workers don't serialize every other host behind one slow domain.

### Not verified

**No new profile has been run against a live site.** Bloomberg, Reuters,
Barron's, and FT are paywalled or bot-hostile and may return teasers rather
than bodies. The extraction fix makes a teaser *cleaner*, not *longer*. Check
the `[DEBUG]` output in `engine.cpp` before trusting any of them.

---

## Task 2 — Per-ticker sentiment

### Prompt and parsing

The prompt now requests one verdict per ticker (`TICKER:SENTIMENT`) instead of
one grouped label. A grouped label dilutes: an article strongly bullish on NVDA
and neutral on MSFT averaged to a single word describing neither.

Three supporting fixes:

- **`num_predict` scales with ticker count.** It was hardcoded to 6, which is
  enough for one word but truncates a multi-line reply mid-list — dropping
  verdicts that would then be silently recorded as NEUTRAL.
- **Newlines are unescaped.** Ollama returns the response with `\n` escaped, so
  without unescaping every verdict lands on one line and only the first parses.
- **Ticker validation.** `isPlausibleTicker()` rejects words in the ticker slot,
  so a model returning `Article:BULLISH` cannot write a junk row.

### CSV schema preserved

**One row per ticker, not a combined cell.** The brief's example format
(`AAPL:BULLISH,MSFT:NEUTRAL`) would have gone into the last CSV column, where
`backtest.py` reads `parts[-1]` and compares it to `'BULLISH'` exactly — a
combined cell never matches, so every multi-ticker signal would have been
silently dropped. Row-per-ticker keeps the schema and parser intact.

Missing tickers are backfilled as NEUTRAL so no requested symbol is lost.
`std::lock_guard<std::mutex>` retained.

### Verification

8/8 parser tests pass: comma and space payloads, clean output, bulleted/spaced
output, omitted tickers, garbage output, invented tickers, trailing prose.

### Not verified

**Not run against live Ollama.** The prompt's actual output format is unconfirmed;
the parser is tolerant by design but the real model may need a prompt tweak.

---

## Task 3 — Backtest comparison

### What changed

New `backtest_compare.py` runs the same signals under two hold windows with
**everything else identical**, so any difference is attributable to the window.

### Results

```
TIME DECAY (30 min)     FULL WINDOW (300 min)
Trades            : 5   Trades            : 5
Win rate          : 40% Win rate          : 0%
Avg return/trade  : -0.19%  Avg return/trade  : -0.42%
Total return      : -0.97%  Total return      : -2.12%
Max drawdown      : -0.64%  Max drawdown      : -1.28%
Exit reasons:           Exit reasons:
  TIME_DECAY: 4           TRAILING_STOP: 4
  TRAILING_STOP: 1        TIME_DECAY: 1
```

The 30-minute window looks better, but **neither number is evidence**. Both
runs are 5 trades, all premarket, from a sample this small. The stated reason
for the 30-minute decay — "capture momentum before human traders digest the
news" — is not testable on this data, because the hold expires before the
session where that digestion happens.

The 300-minute run's 4 trailing stops are the more informative signal: given
more time, these positions went *down* 1% from their peak more often than they
went up 3%.

### The real finding: signals fire when markets are closed

17 ticker-signal pairs passed the BULLISH filter, but only **5 executed**. The
diagnosis:

```
TOLERANCE_GAP        : 10
NO_BAR_AFTER_SIGNAL  :  2
TRADABLE             :  5
```

The dropped signals are not a filter artifact. Their timestamps:

| Batch | Signal time (ET) | Outcome |
|---|---|---|
| 2026-09-09 | 06:36 – 06:49 | **Traded** — inside premarket |
| 2026-09-10 | 01:53 – 02:59 | **Dropped** — market fully closed |

The 09-10 signals arrived ~1–2 hours before the 04:00 ET premarket open, so the
nearest available bar was beyond the 3-minute entry tolerance.

**The crawler runs around the clock and generates signals at times when there
is no market to trade them.** On this evidence, ~60% of signals are
structurally untradeable. No amount of backtester tuning fixes that — it is a
scheduling/strategy problem.

---

## Outstanding risks

1. **No demonstrated edge.** Every run to date is small-sample, premarket-only,
   and mostly negative. Cleaner extraction did not create signal.
2. **Extraction unvalidated on live pages.** New profiles especially.
3. **Rate limiter untested under concurrency.** Logic is straightforward but
   there is no multi-threaded test.
4. **AI changes unvalidated against the real model.**
5. **Broker code from the previous session remains unexercised.**

## Next steps

1. Run the crawler live on 2–3 new profiles; confirm extraction and that
   `history.txt` is written.
2. Run one article through the AI server with Ollama up; confirm the per-ticker
   output format parses.
3. **Decide what to do about signals arriving outside market hours** — the
   single highest-value open question. Options: restrict crawling to
   04:00–20:00 ET, or queue off-hours signals for the next open (which needs
   its own backtest, since the entry price assumption changes).
