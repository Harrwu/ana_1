"""Side-by-side backtest: 30-minute time-decay exit vs the current 300-minute window.

The brief specifies a 30-minute decay exit. The problem with applying it to
THESE signals is timing: the crawler runs ~05:00-05:45 ET, and the regular
session opens at 09:30 ET. A 30-minute hold therefore exits entirely inside
pre-market, hours before the open -- which is the session where the +/-3%/-1%
band is actually reachable. This script runs both so that tradeoff is measured
rather than asserted.

Only MAX_HOLD_MINUTES differs between the two runs. Entry matching, timezone
handling, and the risk bracket are identical, so any difference in the numbers
is attributable to the hold window alone.

Run:
    build/.venv/bin/python backtest_compare.py
"""

import pandas as pd
import yfinance as yf
from datetime import timedelta

# ---------------------------------------------------------------------------
# Shared config -- identical to backtest.py so the two agree.
# ---------------------------------------------------------------------------
ENTRY_TOLERANCE   = pd.Timedelta('3m')
TAKE_PROFIT_PCT   = 0.03
TRAILING_STOP_PCT = 0.01
USE_PREPOST       = True
EQUITY_START      = 100.0
MAX_TICKERS_PER_ROW = 3
SIGNAL_CSV        = 'build/trading_signals.csv'

# The two windows under comparison.
HOLD_WINDOWS = [30, 300]


def load_signals(path):
    """Parse the signal CSV into (ticker, ET-timestamp) pairs.

    Timezone contract (unchanged from backtest.py): ai_server.cpp stamps with
    system_clock::now() rendered as UTC, while yfinance ignore_tz=True returns
    a naive US/Eastern index. Comparing them directly is what made every match
    fail before. Parse UTC -> convert to ET.

    Sub-second precision is truncated because yfinance now returns
    second-resolution bar indices and pandas refuses to losslessly convert a
    microsecond scalar into a seconds unit.
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

            sentiment = parts[-1].strip().upper()
            raw_tickers = [t.strip() for t in parts[1:-1]
                           if t.strip() and t.strip() != 'MACRO']

            if sentiment != 'BULLISH' or not raw_tickers:
                continue
            if len(raw_tickers) > MAX_TICKERS_PER_ROW:
                continue

            for t in raw_tickers:
                ts = pd.to_datetime(parts[0], utc=True) \
                       .tz_convert('America/New_York').tz_localize(None)
                if ts.microsecond or ts.nanosecond:
                    ts = ts.floor('s')
                trades.append((t, ts))

    return trades


# Cache bars per ticker so the two runs don't re-download the same series.
_bar_cache = {}


def bars_for(ticker):
    if ticker in _bar_cache:
        return _bar_cache[ticker]

    data = yf.download(tickers=ticker, period='5d', interval='1m',
                       progress=False, ignore_tz=True, prepost=USE_PREPOST)

    if not data.empty:
        if isinstance(data.columns, pd.MultiIndex):
            data.columns = data.columns.get_level_values(0)
        data.index = data.index.tz_localize(None)

    _bar_cache[ticker] = data
    return data


def simulate(ticker, signal_time, max_hold_minutes):
    """One trade. Returns a result dict, or None if untradeable.

    Entry is searchsorted(side='left') at the bar OPEN -- the first tradable
    instant after the signal. NOT `nearest`, which can select a bar before the
    signal fired, and not the bar Close, which would be look-ahead.
    """
    data = bars_for(ticker)
    if data.empty:
        return None

    pos = data.index.searchsorted(signal_time, side='left')
    if pos >= len(data):
        return None

    entry_time = data.index[pos]
    entry_price = float(data.iloc[pos]['Open'])

    if (entry_time - signal_time) > ENTRY_TOLERANCE:
        return None

    peak = entry_price
    exit_price = None
    exit_time = None
    reason = None
    limit_time = entry_time + timedelta(minutes=max_hold_minutes)

    for i in range(pos + 1, len(data)):
        bar_time = data.index[i]
        if bar_time > limit_time:
            break

        row = data.iloc[i]
        high = float(row['High'])
        low = float(row['Low'])
        close = float(row['Close'])

        if high >= entry_price * (1 + TAKE_PROFIT_PCT):
            exit_price, exit_time, reason = close, bar_time, 'TAKE_PROFIT'
            break
        if low <= peak * (1 - TRAILING_STOP_PCT):
            exit_price, exit_time, reason = close, bar_time, 'TRAILING_STOP'
            break

        peak = max(peak, high)

    if exit_price is None:
        # Neither bracket leg triggered. Distinguish "ran out of hold window"
        # from "ran out of DATA" -- these mean very different things, and
        # conflating them is what made the earlier run look like a result.
        if len(data) <= pos + 1:
            return None
        exit_price = float(data.iloc[-1]['Close'])
        exit_time = data.index[-1]
        reason = 'DATA_END' if exit_time <= limit_time else 'TIME_DECAY'

    return {
        'Ticker': ticker,
        'EntryTime': entry_time,
        'ExitTime': exit_time,
        'EntryPrice': entry_price,
        'ExitPrice': exit_price,
        'ProfitPct': ((exit_price - entry_price) / entry_price) * 100,
        'Reason': reason,
    }


def summarize(results, label, max_hold):
    df = pd.DataFrame(results).sort_values('EntryTime').reset_index(drop=True)

    equity = EQUITY_START * (1 + df['ProfitPct'] / 100).cumprod()
    drawdown = (equity - equity.cummax()) / equity.cummax()
    returns = df['ProfitPct'] / 100
    std = returns.std(ddof=1)

    reasons = df['Reason'].value_counts().to_dict()

    print(f'--- {label} (max hold {max_hold} min) ---')
    print(f'  Trades            : {len(df)}')
    print(f'  Win rate          : {(df["ProfitPct"] > 0).mean() * 100:.1f}%')
    print(f'  Avg return/trade  : {df["ProfitPct"].mean():+.2f}%')
    print(f'  Total return (sum): {df["ProfitPct"].sum():+.2f}%')
    print(f'  Max drawdown      : {drawdown.min() * 100:.2f}%')
    print(f'  Sharpe (per-trade): {(returns.mean() / std) if std > 0 else 0.0:.2f}')
    print(f'  Exit reasons      : {reasons}')
    print()
    return df


def main():
    print('Loading and filtering trading signals...')
    signals = load_signals(SIGNAL_CSV)

    if not signals:
        print('[INFO] No BULLISH multi-ticker signals found.')
        return

    print(f'[INFO] {len(signals)} ticker-signal pairs pass the '
          f'<= {MAX_TICKERS_PER_ROW}-ticker BULLISH filter\n')

    # Warm the cache once so both runs see identical bars.
    for ticker, _ in signals:
        bars_for(ticker)

    for max_hold in HOLD_WINDOWS:
        results = []
        for ticker, ts in signals:
            r = simulate(ticker, ts, max_hold)
            if r:
                results.append(r)

        if not results:
            print(f'--- max hold {max_hold} min: no tradable trades ---\n')
            continue

        summarize(results, 'TIME DECAY' if max_hold == 30 else 'FULL WINDOW', max_hold)

    print('=' * 66)
    print('READ THIS BEFORE TRUSTING THE NUMBERS ABOVE')
    print()
    print('1. Signals fire ~05:00-05:45 ET; the regular session opens 09:30 ET.')
    print('   A 30-min hold exits entirely in pre-market. The +3%/-1% band is')
    print('   rarely reachable there, so that run mostly exits on TIME_DECAY and')
    print('   its result says more about pre-market quiet than about the signal.')
    print('2. Exit fills use bar CLOSES from yfinance prepost data. Pre-market')
    print('   spreads are wide, so real fills will be worse than simulated.')
    print('3. Count the DATA_END exits: those trades never actually resolved and')
    print('   are NOT evidence for or against the strategy.')


if __name__ == '__main__':
    main()
