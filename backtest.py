import pandas as pd
import yfinance as yf
from datetime import timedelta

print("Loading and filtering trading signals...")

bullish_trades = []

# 1. Manual parsing to bypass the CSV column mismatch
with open('build/trading_signals.csv', 'r') as f:
    lines = f.readlines()

for line in lines[1:]: # Skip the header row
    line = line.strip()
    if not line: continue
    
    parts = line.split(',')
    if len(parts) < 3: continue
    
    timestamp_str = parts[0]
    sentiment = parts[-1].strip().upper()
    raw_tickers = parts[1:-1] # Everything in the middle
    
    # 2. Clean the tickers and remove MACRO
    valid_tickers = [t.strip() for t in raw_tickers if t.strip() != 'MACRO' and t.strip()]
    
    if sentiment == 'BULLISH' and valid_tickers:
        # 3. The Noise Filter: Skip sidebar/market summary garbage
        if len(valid_tickers) <= 3:
            for t in valid_tickers:
                bullish_trades.append({
                    'Timestamp': pd.to_datetime(timestamp_str).tz_localize(None), 
                    'Ticker': t
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
    trade_time = trade['Timestamp']

    try:
        # FIX: Request the last 5 days of 1-minute data to bypass timezone traps
        data = yf.download(
            tickers=single_ticker,
            period="5d",       # Grabs a rolling 5-day window
            interval="1m",
            progress=False,
            ignore_tz=True     # Strips Yahoo's complex timezone metadata
        )

        if data.empty:
            continue # Silently skip if the API fails or data is genuinely missing

        if isinstance(data.columns, pd.MultiIndex):
            data.columns = data.columns.get_level_values(0)

        data.index = data.index.tz_localize(None)

        # Look for the exact minute the AI triggered the trade
        try:
            # Use a strict 2-minute tolerance so we don't accidentally match yesterday's prices
            entry_idx = data.index.get_indexer([trade_time], method='nearest', tolerance=pd.Timedelta('2m'))[0]
            if entry_idx == -1:
                continue # Market was closed at this exact time, skip quietly
        except Exception:
            continue

        exit_price = float(data.iloc[exit_idx]['Close'])
        
        profit_pct = ((exit_price - entry_price) / entry_price) * 100
        results.append({'ProfitPct': profit_pct})

        print(f"[{entry_actual_time}] {single_ticker} | Entry: ${entry_price:.2f} | Exit: ${exit_price:.2f} | Return: {profit_pct:+.2f}%")

    except Exception as e:
        print(f'[ERROR] Failed processing {single_ticker}: {e}')

if results:
    results_df = pd.DataFrame(results)
    total_trades = len(results_df)
    winning_trades = len(results_df[results_df['ProfitPct'] > 0])
    win_rate = (winning_trades / total_trades) * 100
    avg_profit = results_df['ProfitPct'].mean()

    print('\n' + '=' * 40)
    print('       BACKTEST PERFORMANCE SUMMARY       ')
    print('=' * 40)
    print(f'Total Trades Executed : {total_trades}')
    print(f'Winning Trades        : {winning_trades} ({win_rate:.1f}%)')
    print(f'Average Return/Trade  : {avg_profit:+.2f}%')
    print('=' * 40)
