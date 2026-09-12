#pragma once
#include <string>
#include <optional>
#include <vector>

// Minimal Alpaca (paper) trading client.
//
// SECURITY NOTE: this speaks TLS to a real broker endpoint. Start it against
// paper-api.alpaca.markets and keep it there until the strategy has a
// verified edge. Nothing in this class prevents live trading -- the endpoint
// is constructor-injected, so switching to api.alpaca.markets is a one-line
// change and that is the *only* thing standing between this and real money.
// Deliberate: it should be an explicit, visible decision, not a hidden flag.
//
// ORDER-SHAPE CONSTRAINTS (verified against Alpaca's order docs):
//   * Brackets (order_class=bracket) do NOT support fractional/notional
//     sizing. Fractional supports market/limit/stop/stop-limit, day TIF only.
//   * A trailing stop cannot be a bracket leg -- Alpaca lists that as a
//     future feature, not a current one. A trailing stop is a standalone
//     order type, and it does not trigger outside regular market hours.
//   * Therefore "fractional buy + attached trailing-stop bracket" is not a
//     representable order. This client buys WHOLE shares so a native
//     bracket (hard stop_loss + take_profit) is valid, and tracks the 1%
//     trailing level in software (see trailing_state_ below), cancelling and
//     re-placing the stop leg as the high-water mark rises.
class AlpacaBroker {
public:
    struct Account {
        double equity{0.0};            // total account value
        double buying_power{0.0};
        double cash{0.0};
        std::string currency{"USD"};
    };

    struct OrderResult {
        bool success{false};
        std::string order_id;
        std::string status;            // e.g. "accepted", "rejected"
        std::string error;             // populated when success == false
        int http_status{0};
    };

    // The "ask" phrasing for a paper account is a nicer default than silently
    // pointing at paper while pretending it's live.
    explicit AlpacaBroker(std::string api_key,
                          std::string secret_key,
                          std::string base_host = "paper-api.alpaca.markets",
                          bool paper = true);

    // --- account -----------------------------------------------------------
    // GET /v2/account. Used for position sizing: the 5% cap needs real equity.
    std::optional<Account> getAccount();

    // --- sizing ------------------------------------------------------------
    // Whole shares such that notional <= max_fraction * equity, floored.
    // Returns 0 when equity is unknown or a single share exceeds the cap --
    // the caller must treat 0 as "do not trade", never as "buy 1".
    //
    // EQUITIES ONLY. Alpaca supports fractional equity orders, but a bracket
    // order requires whole-share qty, and this client brackets every equity
    // entry. Use unitsForMaxFraction() with fractional=true for crypto.
    static int sharesForMaxFraction(double equity, double price, double max_fraction = 0.05);

    // Fractional-aware sizing. Returns a decimal quantity.
    //
    // When `fractional` is false this floors to whole units, matching
    // sharesForMaxFraction exactly. When true (crypto) it floors to 8 decimals,
    // which is more precision than any supported pair uses -- Alpaca rejects
    // over-precise quantities, so flooring here is what keeps the order valid.
    //
    // Distinct from sharesForMaxFraction because the whole-share "0 means
    // skip" rule does not transfer: on crypto, 0.001 BTC is a real position,
    // and treating sub-1.0 sizes as zero would silently disable crypto
    // trading on any account smaller than one coin. The fractional path
    // therefore has its own minimum: units worth at least `min_notional`.
    static double unitsForMaxFraction(double equity, double price,
                                      double max_fraction = 0.05,
                                      bool fractional = false,
                                      double min_notional = 1.0);

    // True when `ticker` is a crypto pair (contains "/USD", the modern Alpaca
    // format; the legacy "BTCUSD" form is also accepted).
    static bool isCryptoSymbol(const std::string& ticker);

    // --- pricing -----------------------------------------------------------
    struct Quote {
        double price{0.0};          // trade close, or quote midpoint
        std::string timestamp;      // RFC3339 from the feed, UTC
        bool from_quote{false};     // true = midpoint, false = last trade
        bool valid{false};
    };

    // GET data.alpaca.markets/v2/stocks/{ticker}/bars/latest?feed=iex
    //
    // Requires a separate host from the trading API, and the feed is pinned to
    // "iex" deliberately. On a free/paper account the default already resolves
    // to iex, but if the account is ever upgraded the default silently becomes
    // "sip" -- which changes both the data and the rate limits. Pinning keeps
    // this predictable.
    //
    // CAVEAT: IEX is roughly 2.5% of US volume, so this price is NOT the
    // consolidated NBBO print. It is good enough to size a paper order and
    // enforce the 5% cap; it is not good enough to compute a precise edge, and
    // a bracket's stop/target levels derived from it are approximate.
    std::optional<Quote> getLatestPrice(const std::string& ticker);

    // Rejects a quote older than max_age_seconds so a thin or halted feed
    // can't drive a trade off a stale print. Returns true when fresh.
    static bool isQuoteFresh(const Quote& quote,
                             int max_age_seconds = 120);

    // --- execution ---------------------------------------------------------
    // Places a bracketed entry for `ticker` when `sentiment` is BULLISH.
    // Anything else (BEARISH/NEUTRAL) is a no-op by design: this system only
    // has a long entry path, and inventing a short path is out of scope.
    //
    // `price` is the last known price, used for the 5% sizing math and the
    // bracket levels. It is NOT a limit price -- the entry is a market order.
    OrderResult executeTrade(const std::string& ticker,
                             const std::string& sentiment,
                             double price);

    // Cancels a previously placed stop leg. Used by the trailing-stop updater
    // before re-placing at a higher level.
    OrderResult cancelOrder(const std::string& order_id);

    // --- trailing stop (software-side) -------------------------------------
    // Alpaca has no trailing-stop bracket leg, so the 1% trail lives here:
    // for each position we remember the high-water price and the current stop
    // order id. Call updateTrailingStops() periodically; when price has risen
    // enough that a 1% trail sits above the resting stop, it cancels and
    // re-places.
    struct TrailingState {
        std::string ticker;
        double entry_price{0.0};
        double high_water{0.0};
        std::string stop_order_id;
        int qty{0};
        double units{0.0};      // crypto: fractional size; 0 for equities
        bool fractional{false}; // crypto positions use stop_limit, not stop
    };

    // Registers a position after a successful bracket entry.
    void trackPosition(const std::string& ticker, double entry_price,
                       int qty, const std::string& stop_order_id);

    // Crypto variant: fractional size, and the protective order is a
    // stop_limit rather than a stop (crypto supports no plain stop).
    void trackCryptoPosition(const std::string& ticker, double entry_price,
                             double units, const std::string& stop_order_id);

    // Drops a position (exited or manually closed).
    void forgetPosition(const std::string& ticker);

    // Re-evaluates every tracked position against `current_price`.
    // Returns the number of stop legs that were moved.
    int updateTrailingStops(const std::string& ticker, double current_price);

    // Exposed for logging/tests.
    const std::vector<TrailingState>& tracked() const { return positions_; }

    // Live risk parameters. Same numbers the Python backtest uses, so the two
    // stay comparable -- change them in one place or they drift apart.
    static constexpr double kTrailingStopPct = 0.01;  // 1% trail from peak
    static constexpr double kTakeProfitPct   = 0.03;  // +3% target
    static constexpr double kMaxPositionFrac = 0.05;  // 5% of equity per trade

private:
    std::string api_key_;
    std::string secret_key_;
    std::string base_host_;
    bool paper_;

    std::vector<TrailingState> positions_;

    // One HTTPS round trip. Returns the raw response body, plus the status
    // code through `out_status`. Empty body + status 0 means the connection
    // failed before any HTTP response arrived.
    std::string request(const std::string& method,
                        const std::string& path,
                        const std::string& json_body,
                        int& out_status);

    // Same, against an explicit host. The market-data API lives on
    // data.alpaca.markets, not the trading host.
    std::string request_to(const std::string& host,
                           const std::string& method,
                           const std::string& path,
                           const std::string& json_body,
                           int& out_status);

    // Pulls a top-level string field out of a flat-ish JSON response without
    // pulling in a JSON dependency. Sufficient for id/status/error fields;
    // NOT a general parser and not used for anything nested.
    static std::string jsonStringField(const std::string& json, const std::string& key);
    static double jsonNumberField(const std::string& json, const std::string& key);
};
