# Build Report — 2026-09-10

Phase 1: article extraction fix, persistent crawler state, broker execution.

---

## 1. Article extraction (completed, tested)

### Problem

`grep` over `src/crawler/profile.h` showed **10 of 11 profiles** passed the entire
HTTP response into `genericParagraphExtractor(raw_html)`. Only `YahooProfile`
attempted to narrow to an article container, and its fallback was
`body = raw_html` (`profile.h:135`) — the whole page.

Measured against the 359 crawl dumps in `_output/2026-09-09/`:

| Metric | Value |
|---|---|
| Dumps exceeding the 4000-char AI cap | 294 / 359 (**81%**) |
| Dumps containing sidebar/boilerplate markers | 115 / 359 (**32%**) |
| Mean dump size | 14,378 chars |

The practical effect: the model received a truncated *page*, not a truncated
*article*. Yahoo's dump for an Altria/Barchart piece contained the real body
buried under nav, "Recommended Stories", and a footer strip reading:

```
Trending tickers   BTC-USD ... RKLB ... QCOM ... SMR ... TTAN ...
```

Those are not in the article. `extractCashtags` matched them, the AI was asked
about them, and they landed in `trading_signals.csv` as if the article discussed
Rocket Lab and NuScale.

### Fix

Added to `src/crawler/profile.h`, as inline free functions matching the existing
header-only idiom:

- `findOpenTag` — boundary-aware tag match, so `<nav` doesn't match `<navbar>`
  and `<p` doesn't match `class="pt-3"`.
- `stripRawBlocks` — removes `<script>/<style>/<noscript>/<svg>/<iframe>/<template>`
  blocks first. Order matters: JS string literals can contain `<nav` and would
  otherwise confuse the next pass.
- `trimTrailingMarkup` — bounds the region at the first `</body>`/`</html>`.
- `stripBoilerplateBlocks` — removes `<nav>/<header>/<footer>/<aside>/<form>`.
  Keeps the remainder on an unclosed tag rather than eating the rest of the page.
- `paragraphsAsLines` / `stripAllTags` — text extraction, with a div-based
  fallback for sites without `<p>` bodies.
- `decodeCommonEntities` — the ~18 entities that actually appear in body text.
- `scoreText` — paragraph text minus 60 bytes per `<a>` and 40 per `<li>`. This
  is what ranks a sidebar below a real body.
- `extractArticleBody` — the entry point. Strips the above, then scores every
  candidate in `commonArticleClasses()` *plus* the whole cleaned region, and
  returns the best.

Scoring the whole region as a competing candidate matters on pages like Yahoo's,
where `article-wrap` matches a shell that contains the nav as well as the body —
taking the first class hit would have returned the shell.

All 11 profiles now route through `extractArticleBody()`, with
`genericParagraphExtractor(raw_html)` retained as a fallback when the heuristic
returns nothing.

### Verification

Compiled a fixture reproducing the Yahoo structure (nav + header + narrow body +
"Recommended Stories" rail + footer with fake tickers). Result: **1,944 bytes
in → 710 bytes out**, pure body. Confirmed absent from the output:

- nav links, `var tickers` JS, `.nav{` CSS, `Copyright` footer text
- fake tickers `BTC-USD` and `RKLB`

Full `main_client` rebuild is clean.

### Not verified

The fixture is a reconstruction, not a live fetch. **Re-run the crawler against
real URLs and confirm the `[DEBUG]` output in `engine.cpp` changed shape** — that
is the outstanding step. `_output/` currently holds extracted *text*, not raw
HTML, so the fix could not be replayed against the original pages.

---

## 2. Persistent crawler state (completed, tested)

### New files

- `src/crawler/state_manager.h`
- `src/crawler/state_manager.cpp`

`load()` / `save()` / `maybeSave()` over `std::unordered_set<std::string>`, one
URL per line in plain text.

### Design decisions

- **Plain text, not binary.** Greppable and hand-recoverable; a partial write
  costs at most the final line.
- **Atomic save.** Writes `history.txt.tmp` then `rename()`s over the target, so
  an interrupted save cannot truncate the real history.
- **Garbage tolerance.** Lines shorter than 8 bytes or lacking `://` are skipped
  individually rather than aborting the load.
- **Amortized writes.** `maybeSave()` skips the flush until the set has grown by
  50 entries, so a long crawl isn't fsyncing per URL.

### Engine integration

`crawlEngine(worker_count, state_path)` loads history in the constructor, before
any worker starts. `workerLoop()` counts idle workers under `queue_mutex_`; when
the queue drains and the last idle worker parks, it forces a full save — the
"queue emptied" checkpoint. `shutting_down_` was added so a drained queue no
longer spins.

### Verification

Round-trip tested: 3 real URLs persist and reload; a `not-a-url` sentinel is
skipped on reload with a count reported; `maybeSave` correctly returns false when
unchanged and true after growth past threshold.

---

## 3. Broker execution (built, not exercised against the broker)

### New files

- `src/trading/broker_api.h` / `broker_api.cpp` — `AlpacaBroker`
- `src/trading/broker_cli.cpp` — CLI, new `brokercli` CMake target

Uses the existing `TlsSocket_` and `ReqBuild` to POST JSON to
`paper-api.alpaca.markets:443`, as specified. Credentials come from
`APCA_API_KEY_ID` / `APCA_API_SECRET_KEY` — never argv, since command lines are
readable via `/proc` and these keys can move money.

### A requirement that had to change

**Task 2 asked for fractional shares; Task 3 asked to attach a 1% trailing stop
as a bracket leg. These are mutually exclusive on Alpaca.** Verified against
Alpaca's order documentation:

- Fractional/notional sizing supports market, limit, stop, and stop-limit with
  `time_in_force=day` only.
- A trailing stop **cannot be a bracket leg** — Alpaca lists trailing-stop-as-
  bracket-leg as a *future* feature. Trailing stop is a standalone order type.
- Trailing stops also do not trigger outside regular market hours.

Combined with the 05:00 ET signal timing from the previous session, the last
point is decisive: **a trailing stop on these signals would not be active when
they fire.**

Resolution: the client buys **whole shares** so a native bracket is valid, and
maintains the 1% trail in software (`updateTrailingStops`), which cancels and
re-places the stop leg as the high-water mark rises. The ratchet only moves up.

### Risk controls

- 5% cap: `sharesForMaxFraction()` returns `floor(equity * 0.05 / price)`.
  Returning **0 is a skip, never a round-up** — a $800 stock on a $5k account
  cannot be bought under this cap, and the code logs that rather than trading.
- Bracket: `take_profit.limit_price` at +3%, `stop_loss.stop_price` at -1%.
- Rejected orders surface Alpaca's `message` field instead of failing silently.

### Verification

- Sizing: $100k equity / $226.06 → 22 shares, **4.97% of equity** (under cap).
- Sizing: $5k equity / $800 → 0 shares, flagged as skip.
- Credential guard: refuses to run with exit 1 when env vars are unset.
- All four CMake targets build clean.

### Not verified

**No order has been sent to Alpaca.** The HTTP round trip, TLS handshake,
response parsing, de-chunking, and trailing-stop replacement are unexercised.
Test with `brokercli buy <T> BULLISH <price> --dry-run` first, then a single
paper order.

---

## Outstanding risks

1. **The strategy still has no demonstrated edge.** Fixing extraction makes the
   *input* cleaner; it does not show the signals predict anything. The last
   backtest run resolved to a premarket snapshot with 6 of 8 exits on the
   data-end fallback, because the regular session had not opened. That has not
   been revisited.

2. **Extraction fix is unverified on live pages.** Fixture-only so far.

3. **Broker code is fully unexercised.** Treat it as untested until one paper
   order completes end to end.

4. **The 4000-char truncation in `ai_server.cpp` is unchanged.** Cleaner
   extraction makes it far less destructive, but the cap is still there and
   still cuts long articles mid-body.

## Next steps

1. Re-crawl a sample, confirm dumps shrink and fake tickers disappear.
2. Re-run `backtest.py` after the 09:30 ET open to get real regular-session
   exits for the existing signals.
3. Only then size up profiles or tune the ±3%/-1% band — both are unreasoned
   while the sample is premarket-only.
