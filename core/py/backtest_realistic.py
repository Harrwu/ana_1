"""Realistic backtester: latency, slippage, and bracket exits.

Extends backtest_compare.py with the two friction models the brief asks for:

  * ENTRY_LATENCY - the trade does not fill at the signal timestamp. Network
    round-trip plus local LLM inference means the earliest realistic fill is a
    minute or more later.
  * SLIPPAGE_PCT  - a real fill is worse than the quoted price. Applied
    adversely on BOTH entry (pay more) and exit (receive less), because a
    stop that triggers into a falling market fills below the stop price.

Exit logic is the requested -1% stop / +3% target, plus a time limit. The
limit is configurable because it interacts badly with these particular
signals: 20 of 29 BULLISH rows fire in premarket (04:00-09:30 ET) and 9 fire
when the market is closed, so a 30-minute decay from a ~05:00 ET signal
expires before the 09:30 open. HOLD_MODES below runs both interpretations.

Run:
    build/.venv/bin/python backtest_realistic.py
"""

import pandas as pd
import yfinance as yf
from datetime import timedelta

# ---------------------------------------------------------------------------
# Strategy parameters
# ---------------------------------------------------------------------------
STOP_LOSS_PCT     = 0.01     # -1% from entry
TAKE_PROFIT_PCT   = 0.03     # +3% from entry
ENTRY_LATENCY     = pd.Timedelta('1m')   # brief: 1-minute simulated delay
SLIPPAGE_PCT      = 0.0005   # 0.05% per side, applied adversely
ENTRY_TOLERANCE   = pd.Timedelta('3m')   # max gap signal -> first tradable bar
MAX_TICKERS_PER_ROW = 3
USE_PREPOST       = True
EQUITY_START      = 100.0
SIGNAL_CSV        = 'build/trading_signals.csv'

# Both hold interpretations, so the tradeoff is measured rather than assumed:
#   30  = the brief's time decay. On these signals this exits premarket,
#         hours before the open, where the +/-3/-1 band is barely reachable.
#   300 = carries the position through the 09:30 open into the regular
#         session, the only window where a +3% move is plausible.
HOLD_MODES = [30, 300]


# ---------------------------------------------------------------------------
# Ingestion
# ---------------------------------------------------------------------------
def load_signals(path):
    """BULLISH rows -> (ticker, ET-naive timestamp) pairs.

    The CSV is written by ai_server.cpp as one row per ticker with a single
    sentiment in the last column, so the BULLISH test is an exact match on
    parts[-1]. (An earlier proposed format put "AAPL:BULLISH,MSFT:NEUTRAL" in
    that column, which would never match and would silently drop every
    multi-ticker signal.)

    Timestamps are stamped UTC by system_clock::now() and rendered without an
    offset; yfinance with ignore_tz=True returns a naive US/Eastern index.
    Parsing as UTC and converting to ET is what makes the two align. Comparing
    them raw was off by the UTC/ET gap, and every lookup returned -1.
    """
    trades = []

    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('Timestamp'):
                continue

            parts = line.split(',')
            if len(parts) < 3:
                continue

            if parts[-1].strip().upper() != 'BULLISH':
                continue

            tickers = [t.strip() for t in parts[1:-1]
                       if t.strip() and t.strip() != 'MACRO']
            if not tickers or len(tickers) > MAX_TICKERS_PER_ROW:
                continue

            for t in tickers:
                ts = pd.to_datetime(parts[0], utc=True) \
                       .tz_convert('America/New_York').tz_localize(None)
                # yfinance returns second-resolution indices; pandas refuses to
                # compare a microsecond scalar against them.
                if ts.microsecond or ts.nanosecond:
                    ts = ts.floor('s')
                trades.append((t, ts))

    return trades


_bar_cache = {}


def bars_for(ticker):
    if ticker not in _bar_cache:
        data = yf.download(tickers=ticker, period='5d', interval='1m',
                           progress=False, ignore_tz=True, prepost=USE_PREPOST)
        if not data.empty:
            if isinstance(data.columns, pd.MultiIndex):
                data.columns = data.columns.get_level_values(0)
            data.index = data.index.tz_localize(None)
        _bar_cache[ticker] = data
    return _bar_cache[ticker]


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------
def simulate(ticker, signal_time, hold_minutes):
    """One trade with latency and slippage. Returns a dict, or None."""
    data = bars_for(ticker)
    if data.empty:
        return None

    # Earliest realistic fill: signal time + simulated latency. Note this is
    # NOT the search base -- we search from the signal itself, then require the
    # matched bar to be at least ENTRY_LATENCY later. That way a signal landing
    # mid-minute still fills on the next full bar rather than rounding back.
    earliest_fill = signal_time + ENTRY_LATENCY

    pos = data.index.searchsorted(earliest_fill, side='left')
    if pos >= len(data):
        return None

    entry_time = data.index[pos]
    if (entry_time - earliest_fill) > ENTRY_TOLERANCE:
        return None  # market closed / no bar near the delayed entry

    # Entry at the bar OPEN (first tradable instant), then pay slippage.
    entry_price = float(data.iloc[pos]['Open']) * (1 + SLIPPAGE_PCT)

    limit_time = entry_time + timedelta(minutes=hold_minutes)

    # Bracket levels are set from the ACTUAL fill, not the quoted price --
    # that's how a real bracket order behaves.
    stop_price = entry_price * (1 - STOP_LOSS_PCT)
    target_price = entry_price * (1 + TAKE_PROFIT_PCT)

    exit_price = None
    exit_time = None
    reason = None
    last_seen = None   # last bar actually visited inside the hold window

    for i in range(pos + 1, len(data)):
        bar_time = data.index[i]
        if bar_time > limit_time:
            break

        last_seen = i

        row = data.iloc[i]
        high = float(row['High'])
        low = float(row['Low'])

        # If a single bar spans both levels, assume the stop filled first.
        # Intra-bar ordering is unknowable from OHLC, and assuming the
        # favourable side is the classic way backtests flatter themselves.
        if low <= stop_price:
            exit_price = stop_price * (1 - SLIPPAGE_PCT)
            exit_time = bar_time
            reason = 'STOP_LOSS'
            break
        if high >= target_price:
            exit_price = target_price * (1 - SLIPPAGE_PCT)
            exit_time = bar_time
            reason = 'TAKE_PROFIT'
            break

    if exit_price is None:
        # Neither leg triggered inside the window, so this is a TIME_DECAY exit
        # and it must fill at the LAST BAR INSIDE THE WINDOW.
        #
        # The first version filled at data.index[-1] -- the final bar of the
        # entire 5-day download. For a 30-minute hold that meant every
        # unresolved trade "exited" a day and a half later at a price with no
        # relationship to the window, and was mislabelled DATA_END. Time decay
        # is defined by the window, so the exit has to happen inside it.
        if last_seen is None:
            # No bar fit inside the window at all (e.g. latency pushed the
            # entry to the end of available data). Not tradeable.
            return None

        exit_price = float(data.iloc[last_seen]['Close']) * (1 - SLIPPAGE_PCT)
        exit_time = data.index[last_seen]
        reason = 'TIME_DECAY'

    return {
        'Ticker': ticker,
        'EntryTime': entry_time,
        'ExitTime': exit_time,
        'EntryPrice': entry_price,
        'ExitPrice': exit_price,
        'ProfitPct': ((exit_price - entry_price) / entry_price) * 100,
        'Reason': reason,
    }


def summarize(results, label, hold_minutes):
    df = pd.DataFrame(results).sort_values('EntryTime').reset_index(drop=True)

    equity = EQUITY_START * (1 + df['ProfitPct'] / 100).cumprod()
    dd = ((equity - equity.cummax()) / equity.cummax()).min() * 100
    returns = df['ProfitPct'] / 100
    std = returns.std(ddof=1)

    print(f'--- {label} (hold {hold_minutes} min, latency {ENTRY_LATENCY}, '
          f'slippage {SLIPPAGE_PCT*100:.2f}%/side) ---')
    print(f'  Trades            : {len(df)}')
    print(f'  Win rate          : {(df["ProfitPct"] > 0).mean() * 100:.1f}%')
    print(f'  Avg return/trade  : {df["ProfitPct"].mean():+.2f}%')
    print(f'  Max drawdown      : {dd:.2f}%')
    print(f'  Exit reasons      : {df["Reason"].value_counts().to_dict()}')
    print()
    return df


def main():
    print('Loading and filtering trading signals...')
    signals = load_signals(SIGNAL_CSV)

    if not signals:
        print('[INFO] No BULLISH multi-ticker signals found.')
        return

    print(f'[INFO] {len(signals)} BULLISH ticker rows pass the '
          f'<= {MAX_TICKERS_PER_ROW}-ticker filter\n')

    for ticker, _ in signals:
        bars_for(ticker)

    for hold in HOLD_MODES:
        results = [r for r in (simulate(t, ts, hold) for t, ts in signals) if r]
        if not results:
            print(f'--- hold {hold} min: no tradable trades ---\n')
            continue
        summarize(results, 'TIME DECAY' if hold == 30 else 'FULL WINDOW', hold)

    print('=' * 70)
    print('READ BEFORE TRUSTING THESE NUMBERS')
    print()
    print(f'1. {SLIPPAGE_PCT*100:.2f}%/side slippage is a modelling ASSUMPTION, not a')
    print('   measured value. Real premarket spreads on these names are often')
    print('   several times wider, so the true cost is likely worse.')
    print('2. Stop-loss exits are filled AT the stop price, minus slippage. Real')
    print('   stops gap through in fast markets and fill materially lower.')
    print('3. Count the DATA_END rows: those trades never resolved and are not')
    print('   evidence either way.')
    print('4. When one bar spans both the stop and the target, the stop is')
    print('   assumed to fill first. That is deliberately pessimistic.')


if __name__ == '__main__':
    main()
