import pandas as pd
import yfinance as yf
import numpy as np
from datetime import timedelta

# ---------------------------------------------------------------------------
# Tunables
# ---------------------------------------------------------------------------
ENTRY_TOLERANCE   = pd.Timedelta('3m')   # max gap between signal and first tradable bar
TAKE_PROFIT_PCT   = 0.03                 # +3% from entry -> exit at bar close
TRAILING_STOP_PCT = 0.01                 # -1% trailing from running peak -> exit at bar close
# A 5am premarket entry with a short max-hold exits BEFORE the 9:30 open, so
# the +3% take-profit can never be reached (premarket NVDA moved <0.2% that
# morning). A 300-min (5h) window carries the position into the regular
# session, where that band is actually reachable. This is a strategy choice:
# short holds keep you out of the open gap, long holds let the take-profit
# trigger. Set to 300 for a full-session test; set to 60 for a premarket-only
# stress of the same params.
MAX_HOLD_MINUTES  = 300                  # forced exit if neither TP nor stop hits
USE_PREPOST       = True                 # signals land pre-market; without this there are no bars
EQUITY_START      = 100.0                # dummy capital for drawdown calc (returns only)

# ---------------------------------------------------------------------------
# Step 0: Load and parse the signal CSV.
#
# Timezone contract: ai_server.cpp stamps signals with system_clock::now(),
# which renders as UTC. yfinance's ignore_tz=True returns a NAIVE index in
# US/Eastern. The old code compared a naive UTC value against a naive ET
# index (12h apart!) so get_indexer always returned -1 and every trade was
# silently skipped. We therefore parse the timestamp as UTC, then convert it
# to America/New_York and strip tz so it lines up with the ET bar index.
# pd.to_datetime(..., utc=True) also honours an explicit offset if the
# generator ever switches to emitting one, so old and new CSV formats both work.
# ---------------------------------------------------------------------------
print("Loading and filtering trading signals...")

bullish_trades = []

with open('build/trading_signals.csv', 'r') as f:
    lines = f.readlines()

    for line in lines[1:]:  # Skip the header row
        line = line.strip()
        if not line:
            continue

        parts = line.split(',')
        if len(parts) < 3:
            continue

        timestamp_str = parts[0]
        sentiment = parts[-1].strip().upper()
        raw_tickers = parts[1:-1]  # Everything in the middle

        # Clean the tickers and drop MACRO
        valid_tickers = [t.strip() for t in raw_tickers if t.strip() != 'MACRO' and t.strip()]

        if sentiment == 'BULLISH' and valid_tickers:
            # Noise filter: skip sidebar/market-summary rows that tagged too many
            # unrelated tickers -- those are not high-conviction calls, they are
            # the model listing a whole basket at once.
            if len(valid_tickers) <= 3:
                for t in valid_tickers:
                    # Parsed as UTC, converted to ET, tz stripped. This is the
                    # value compared against the ET bar index below.
                    entry_ts = pd.to_datetime(timestamp_str, utc=True) \
                                 .tz_convert('America/New_York').tz_localize(None)
                    # yfinance now returns SECOND-resolution bar indices
                    # (datetime64[s]). The CSV timestamps carry microseconds,
                    # and pandas refuses to losslessly convert a us scalar into
                    # an s unit -> "Cannot losslessly convert units". The
                    # sub-second precision is meaningless against whole-minute
                    # bars, so truncate to seconds. Guarded so an already
                    # second-aligned timestamp is left untouched.
                    if entry_ts.microsecond or entry_ts.nanosecond:
                        entry_ts = entry_ts.floor('s')
                    bullish_trades.append({
                        'Ticker':   t,
                        'SignalTs': entry_ts,
                    })
            else:
                print(f"[NOISE FILTER] Skipping {len(valid_tickers)} unrelated tickers at {timestamp_str}")

if not bullish_trades:
    print('\n[INFO] No clean BULLISH trades found to backtest.')
    exit()

results = []

print(f"\n[BACKTEST] Processing {len(bullish_trades)} high-conviction bullish signals...\n")

for trade in bullish_trades:
    single_ticker = trade['Ticker']
    signal_time = trade['SignalTs']

    try:
        # Request 5 days of 1-minute data. prepost=True is REQUIRED here:
        # the crawler runs at ~05:00-05:45 ET, i.e. pre-market, and with
        # prepost=False there is genuinely no bar at that time to match.
        # These are thin-liquidity fills and the backtest is optimistic by
        # nature -- see the summary caveat.
        data = yf.download(
            tickers=single_ticker,
            period="5d",
            interval="1m",
            progress=False,
            ignore_tz=True,
            prepost=USE_PREPOST
        )

        if data.empty:
            continue  # API failure or genuinely missing data -> skip quietly

        if isinstance(data.columns, pd.MultiIndex):
            data.columns = data.columns.get_level_values(0)
        data.index = data.index.tz_localize(None)

        # --- Non-lookahead entry ------------------------------------------
        # searchsorted(side='left') finds the FIRST bar whose timestamp is at
        # or after the signal. The old code used method='nearest', which can
        # select a bar BEFORE the signal fires -- peeking at a price that
        # didn't exist yet. We enter at that bar's OPEN, the first tradable
        # instant after the signal, never its Close (which would be look-ahead
        # bias).
        pos = data.index.searchsorted(signal_time, side='left')
        if pos >= len(data):
            continue  # signal after all available bars -> nothing to trade

        entry_time = data.index[pos]
        entry_price = float(data.iloc[pos]['Open'])

        # A signal more than 3m before the first available bar is a broken
        # match (e.g. pre-market Friday gap into Monday, or a bad timestamp).
        if (entry_time - signal_time) > ENTRY_TOLERANCE:
            continue

        # --- Bar-by-bar exit evaluation ------------------------------------
        # Walk forward from the bar AFTER entry. Track the running peak high;
        # the trailing stop sits 1% below that peak. Take-profit is +3% off
        # entry. Exit at the triggering bar's Close (not the trigger price,
        # which would be optimistic). Neither hit within MAX_HOLD -> forced
        # exit at the last bar of available data.
        peak = entry_price
        exit_price = None
        exit_time = None
        limit_time = entry_time + timedelta(minutes=MAX_HOLD_MINUTES)

        for i in range(pos + 1, len(data)):
            bar_time = data.index[i]
            if bar_time > limit_time:
                break

            row = data.iloc[i]
            high     = float(row['High'])
            low      = float(row['Low'])
            close    = float(row['Close'])

            tp_price   = entry_price * (1 + TAKE_PROFIT_PCT)
            trail_stop = peak * (1 - TRAILING_STOP_PCT)

            if high >= tp_price:
                exit_price = close
                exit_time = bar_time
                break
            if low <= trail_stop:
                exit_price = close
                exit_time = bar_time
                break

            peak = max(peak, high)  # new peak only applies from the NEXT bar

        if exit_price is None:
            # Neither TP nor trailing stop triggered within the window: exit at
            # the last available bar. (With today's crawl that can be
            # mid-session, since the market is still open -- flagged in the
            # summary.)
            if len(data) <= pos + 1:
                continue
            exit_price = float(data.iloc[-1]['Close'])
            exit_time = data.index[-1]

        profit_pct = ((exit_price - entry_price) / entry_price) * 100
        results.append({
            'Ticker':   single_ticker,
            'EntryTime': entry_time,
            'ExitTime': exit_time,
            'EntryPrice': entry_price,
            'ExitPrice': exit_price,
            'ProfitPct': profit_pct
        })

        print(f"[{entry_time}] {single_ticker} | Entry: ${entry_price:.2f} | "
              f"Exit: ${exit_price:.2f} | Return: {profit_pct:+.2f}%")

    except Exception as e:
        print(f'[ERROR] Failed processing {single_ticker}: {e}')

if not results:
    print('\n[INFO] No tradable trades after filtering. Check timestamps / market session.')
    exit()

results_df = pd.DataFrame(results).sort_values('EntryTime').reset_index(drop=True)

# ---------------------------------------------------------------------------
# Performance summary
# ---------------------------------------------------------------------------
total_trades = len(results_df)
winning = results_df[results_df['ProfitPct'] > 0]
win_rate = (len(winning) / total_trades) * 100
avg_profit = results_df['ProfitPct'].mean()
total_return = results_df['ProfitPct'].sum()  # simple sum of per-trade returns

# Equity curve (cumulative compounded), in trade order, for max drawdown.
equity = EQUITY_START * (1 + results_df['ProfitPct'] / 100).cumprod()
running_max = equity.cummax()
drawdown = (equity - running_max) / running_max
max_drawdown = drawdown.min() * 100

# Per-trade Sharpe (unannualized). Label this honestly: on a handful of trades
# it is not statistically meaningful, it is just the shape of the sample.
returns_series = results_df['ProfitPct'] / 100
std = returns_series.std(ddof=1)
sharpe = (returns_series.mean() / std) if std > 0 else 0.0

print()
print('BACKTEST PERFORMANCE SUMMARY')
print()
print(f'Total Trades Executed   : {total_trades}')
print(f'Winning Trades          : {len(winning)} ({win_rate:.1f}%)')
print(f'Average Return/Trade    : {avg_profit:+.2f}%')
print(f'Total Return (sum)      : {total_return:+.2f}%')
print(f'Max Drawdown (equity)   : {max_drawdown:.2f}%')
print(f'Sharpe (per-trade)      : {sharpe:.2f}')
print()
print('CAVEAT: these fills are pre-market closes from yfinance prepost data.')
print('Pre-market liquidity is thin, spreads are wide, and the trailing-stop/')
print('take-profit exits use bar CLOSES, so real fills will be worse than this.')
