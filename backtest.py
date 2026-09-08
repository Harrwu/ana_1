from datetime import timedelta
from pathlib import Path
import pandas as pd
import yfinance as yf

# --- CONFIGURATION ---
SIGNAL_FILE = Path('build/trading_signals.csv')
RESULT_FILE = Path('build/backtest_results.csv')

HOLD_MINUTES = 30
STOP_LOSS_PCT = 2.0
TAKE_PROFIT_PCT = 3.0
SLIPPAGE_PCT = 0.05
FEE_PCT = 0.0


def load_signals() -> pd.DataFrame:
    """Loads, validates, and cleans the signal dataset."""
    if not SIGNAL_FILE.exists():
        print(f"[ERROR] '{SIGNAL_FILE}' not found.")
        return pd.DataFrame()

    # Fixed: added on_bad_lines='skip' to prevent crashing on malformed CSV rows
    df = pd.read_csv(SIGNAL_FILE, on_bad_lines='skip')
    
    if df.empty:
        print('[INFO] Signal file is empty.')
        return pd.DataFrame()

    required_columns = {'Timestamp', 'Ticker', 'Sentiment'}
    missing = required_columns - set(df.columns)
    if missing:
        print(f"[ERROR] Missing columns: {', '.join(sorted(missing))}")
        return pd.DataFrame()

    # Clean data
    df['Timestamp'] = pd.to_datetime(df['Timestamp'], errors='coerce')
    df['Sentiment'] = df['Sentiment'].astype(str).str.upper().str.strip()
    df['Ticker'] = df['Ticker'].astype(str).str.upper().str.strip()
    
    df = df.dropna(subset=['Timestamp'])
    df = df[df['Sentiment'] == 'BULLISH']

    if df.empty:
        print('[INFO] No BULLISH trades found.')
        return pd.DataFrame()

    df['Timestamp'] = df['Timestamp'].dt.tz_localize(None)

    # Flatten multiple tickers per row
    signals = []
    for _, row in df.iterrows():
        for ticker in row['Ticker'].split(','):
            ticker = ticker.strip().replace('.', '-')
            if ticker and ticker != 'MACRO':
                signals.append({'Ticker': ticker, 'SignalTime': row['Timestamp']})

    signals_df = pd.DataFrame(signals)
    if signals_df.empty:
        print('[INFO] No valid ticker signals found.')
        return pd.DataFrame()

    return signals_df.drop_duplicates(subset=['Ticker', 'SignalTime']).sort_values('SignalTime')


def backtest_signal(ticker: str, signal_time: pd.Timestamp, data_cache: dict) -> dict | None:
    """Simulates a trade entry/exit logic for a single ticker signal."""
    start_date = signal_time.date()
    end_date = start_date + timedelta(days=1)
    cache_key = (ticker, start_date)

    # Fetch data or pull from cache
    if cache_key not in data_cache:
        data = yf.download(
            tickers=ticker,
            start=start_date.isoformat(),
            end=end_date.isoformat(),
            interval='1m',
            progress=False,
            auto_adjust=False,
            prepost=False,
        )

        if isinstance(data.columns, pd.MultiIndex):
            data = data.xs(ticker, axis=1, level=1) if ticker in data.columns.levels else pd.DataFrame()

        if not data.empty:
            data.index = pd.to_datetime(data.index).tz_localize(None)
            data = data.dropna(subset=['Open', 'High', 'Low', 'Close'])
        
        data_cache[cache_key] = data

    data = data_cache[cache_key]
    if data.empty:
        print(f'[SKIP] No data for {ticker} on {start_date}')
        return None

    # Determine Entry
    entry_candidates = data.index[data.index >= signal_time]
    if len(entry_candidates) == 0:
        print(f'[SKIP] No market bar after signal for {ticker} at {signal_time}')
        return None

    entry_time = entry_candidates[0]
    entry_price = float(data.loc[entry_time, 'Open'])
    exit_target = entry_time + timedelta(minutes=HOLD_MINUTES)

    # Filter Holding Window
    holding_data = data[(data.index >= entry_time) & (data.index <= exit_target)]
    if holding_data.empty:
        print(f'[SKIP] No holding period data for {ticker}')
        return None

    # Simulated Bracket Order Execution
    exit_price = None
    exit_time = None
    exit_reason = 'TIME'

    stop_price = entry_price * (1 - STOP_LOSS_PCT / 100)
    target_price = entry_price * (1 + TAKE_PROFIT_PCT / 100)

    for bar_time, bar in holding_data.iterrows():
        if float(bar['Low']) <= stop_price:
            exit_price = stop_price
            exit_time = bar_time
            exit_reason = 'STOP'
            break
        if float(bar['High']) >= target_price:
            exit_price = target_price
            exit_time = bar_time
            exit_reason = 'TARGET'
            break

    if exit_price is None:
        exit_time = holding_data.index[-1]
        exit_price = float(holding_data.iloc[-1]['Close'])

    # Returns calculation
    gross_return = ((exit_price - entry_price) / entry_price) * 100
    net_return = gross_return - (SLIPPAGE_PCT * 2) - (FEE_PCT * 2)

    print(f'[{entry_time}] {ticker} | Entry: ${entry_price:.2f} | Exit: ${exit_price:.2f} | {exit_reason} | Return: {net_return:+.2f}%')

    return {
        'Ticker': ticker,
        'SignalTime': signal_time,
        'EntryTime': entry_time,
        'ExitTime': exit_time,
        'EntryPrice': entry_price,
        'ExitPrice': exit_price,
        'ExitReason': exit_reason,
        'GrossReturnPct': gross_return,
        'NetReturnPct': net_return,
    }


def print_summary(df: pd.DataFrame):
    """Calculates metrics and outputs a clean performance breakdown report."""
    total = len(df)
    winners = (df['NetReturnPct'] > 0).sum()
    losers = (df['NetReturnPct'] < 0).sum()
    breakeven = (df['NetReturnPct'] == 0).sum()

    gross_profit = df.loc[df['NetReturnPct'] > 0, 'NetReturnPct'].sum()
    gross_loss = abs(df.loc[df['NetReturnPct'] < 0, 'NetReturnPct'].sum())
    profit_factor = (gross_profit / gross_loss) if gross_loss > 0 else float('inf')

    print('\n' + '=' * 50)
    print('              BACKTEST SUMMARY')
    print('=' * 50)
    print(f'Total Trades       : {total}')
    print(f'Winning Trades     : {winners}')
    print(f'Losing Trades      : {losers}')
    print(f'Breakeven Trades   : {breakeven}')
    print(f'Win Rate           : {(winners / total) * 100:.1f}%')
    print(f'Average Return     : {df["NetReturnPct"].mean():+.2f}%')
    print(f'Median Return      : {df["NetReturnPct"].median():+.2f}%')
    print(f'Best Trade         : {df["NetReturnPct"].max():+.2f}%')
    print(f'Worst Trade        : {df["NetReturnPct"].min():+.2f}%')
    print(f'Profit Factor      : {profit_factor:.2f}')
    print(f'Stop Loss Exits    : {(df["ExitReason"] == "STOP").sum()}')
    print(f'Take Profit Exits  : {(df["ExitReason"] == "TARGET").sum()}')
    print(f'Time Exits         : {(df["ExitReason"] == "TIME").sum()}')
    print('=' * 50)


def main():
    signals_df = load_signals()
    if signals_df.empty:
        return

    print(f"[BACKTEST] Processing {len(signals_df)} signals...\n")
    
    results = []
    data_cache = {}

    for _, signal in signals_df.iterrows():
        try:
            res = backtest_signal(signal['Ticker'], signal['SignalTime'], data_cache)
            if res:
                results.append(res)
        except Exception as e:
            print(f"[ERROR] Unexpected issue with {signal['Ticker']}: {e}")

    if not results:
        print('\n[INFO] No trades could be simulated.')
        return

    # Ensure build output folder path exists before file writing
    RESULT_FILE.parent.mkdir(parents=True, exist_ok=True)

    results_df = pd.DataFrame(results)
    results_df.to_csv(RESULT_FILE, index=False)
    
    print_summary(results_df)
    print(f'[SAVED] {RESULT_FILE}')


if __name__ == '__main__':
    main()

