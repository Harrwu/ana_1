#include "broker_api.h"
#include "tls_socket.h"
#include "builder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

// Escapes a value for embedding in a JSON string literal. Tickers and
// client-order-ids are alnum in practice, but a stray quote would otherwise
// produce a malformed body that the broker rejects with an opaque 400.
std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

// Formats a double with fixed decimals, locale-independent. std::format's
// default float formatting can emit scientific notation on small values
// (e.g. 1e-05), which the broker rejects as an invalid price.
std::string money(double value, int decimals = 2) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(decimals);
    ss << value;
    return ss.str();
}

}  // namespace

AlpacaBroker::AlpacaBroker(std::string api_key,
                           std::string secret_key,
                           std::string base_host,
                           bool paper)
    : api_key_(std::move(api_key)),
      secret_key_(std::move(secret_key)),
      base_host_(std::move(base_host)),
      paper_(paper) {}

// ---------------------------------------------------------------------------
// HTTP plumbing
// ---------------------------------------------------------------------------
std::string AlpacaBroker::request(const std::string& method,
                                  const std::string& path,
                                  const std::string& json_body,
                                  int& out_status) {
    return request_to(base_host_, method, path, json_body, out_status);
}

std::string AlpacaBroker::request_to(const std::string& host,
                                     const std::string& method,
                                     const std::string& path,
                                     const std::string& json_body,
                                     int& out_status) {
    out_status = 0;

    TlsSocket_ sock;
    if (sock.connect(host, 443) != Tcpsock_::rType::CONNECT_) {
        std::cerr << "[BROKER] TLS connect to " << host << ":443 failed\n";
        return {};
    }

    ReqBuild req(host, "close");
    req.SetMethod(method);
    req.SetPath(path);
    req.addHead("Host", host);
    req.addHead("APCA-API-KEY-ID", api_key_);
    req.addHead("APCA-API-SECRET-KEY", secret_key_);
    req.addHead("Accept", "application/json");
    req.addHead("User-Agent", "ana_1-bot/1.0");

    if (!json_body.empty()) {
        req.SetBody(json_body, "application/json");
    }

    if (sock.send(req.build_()) != Tcpsock_::rType::CONNECT_) {
        std::cerr << "[BROKER] send failed for " << method << " " << path << "\n";
        return {};
    }

    std::string raw;
    char buffer[4096];
    ssize_t bytes = 0;
    while ((bytes = sock.recv(buffer, sizeof(buffer))) > 0) {
        raw.append(buffer, bytes);
    }
    sock.Close();

    if (raw.empty()) return {};

    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        std::cerr << "[BROKER] malformed HTTP response (no header terminator)\n";
        return {};
    }

    const std::string headers = raw.substr(0, header_end);
    std::string body = raw.substr(header_end + 4);

    // "HTTP/1.1 200 OK" -> 200
    {
        const size_t sp = headers.find(' ');
        if (sp != std::string::npos) {
            out_status = std::atoi(headers.c_str() + sp + 1);
        }
    }

    // De-chunk if needed.
    std::string lower_headers = headers;
    std::transform(lower_headers.begin(), lower_headers.end(),
                   lower_headers.begin(), ::tolower);

    if (lower_headers.find("transfer-encoding: chunked") != std::string::npos) {
        std::string dechunked;
        size_t pos = 0;
        while (pos < body.size()) {
            const size_t line_end = body.find("\r\n", pos);
            if (line_end == std::string::npos) break;

            const size_t chunk_size = std::strtoul(body.c_str() + pos, nullptr, 16);
            if (chunk_size == 0) break;

            const size_t data_start = line_end + 2;
            if (data_start + chunk_size > body.size()) break;

            dechunked.append(body, data_start, chunk_size);
            pos = data_start + chunk_size + 2;  // skip trailing CRLF
        }
        body = std::move(dechunked);
    }

    return body;
}

// ---------------------------------------------------------------------------
// Tiny JSON field readers. Not a parser -- sufficient for flat responses.
// ---------------------------------------------------------------------------
std::string AlpacaBroker::jsonStringField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    ++pos;

    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;

    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;

    std::string value;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                default:  value += json[pos];
            }
        } else {
            value += json[pos];
        }
        ++pos;
    }
    return value;
}

double AlpacaBroker::jsonNumberField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0.0;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return 0.0;
    ++pos;

    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size()) return 0.0;

    if (json[pos] == '"') ++pos;  // some numeric fields come back as strings

    return std::strtod(json.c_str() + pos, nullptr);
}

// ---------------------------------------------------------------------------
// Symbol classification
// ---------------------------------------------------------------------------
bool AlpacaBroker::isCryptoSymbol(const std::string& ticker) {
    // Modern Alpaca crypto pairs use a slash: BTC/USD. That alone is decisive.
    if (ticker.find('/') != std::string::npos) return true;

    // The legacy form drops the slash (BTCUSD), which is ambiguous with an
    // equity symbol, so it only matches against a known quote-currency
    // suffix -- and the base must be alphabetic so "123USD" can't slip in.
    static const char* quote_suffixes[] = {"USD", "USDT", "USDC", "BTC", "ETH"};
    for (const char* suffix : quote_suffixes) {
        const std::string s = suffix;
        if (ticker.size() > s.size() + 1 &&
            ticker.compare(ticker.size() - s.size(), s.size(), s) == 0) {
            const std::string base = ticker.substr(0, ticker.size() - s.size());
            if (std::all_of(base.begin(), base.end(),
                            [](unsigned char c) { return std::isalpha(c); })) {
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Latest price
// ---------------------------------------------------------------------------
std::optional<AlpacaBroker::Quote> AlpacaBroker::getLatestPrice(const std::string& ticker) {
    if (ticker.empty()) return std::nullopt;

    // --- Crypto -------------------------------------------------------------
    //
    // Different endpoint AND a different response shape from equities, so this
    // branches before the equity path rather than sharing it.
    //
    // The pair is passed through VERBATIM, slash included: /v1beta3 expects
    // "BTC/USD". Stripping or substituting the slash gets a 404.
    if (isCryptoSymbol(ticker)) {
        const std::string path =
            "/v1beta3/crypto/us/latest/quotes?symbols=" + ticker;

        int status = 0;
        const std::string body =
            request_to("data.alpaca.markets", "GET", path, "", status);

        if (status != 200 || body.empty()) {
            std::cerr << "[BROKER] getLatestPrice(" << ticker
                      << ") crypto lookup failed (http " << status << "): "
                      << body.substr(0, 200) << "\n";
            return std::nullopt;
        }

        // Shape: {"quotes":{"BTC/USD":{"ap":..,"bp":..,"t":".."}}}
        // Scoped to the "quotes" object; the symbol key contains a slash, so
        // the flat field readers are anchored rather than scanning the body.
        const size_t quotes_pos = body.find("\"quotes\"");
        const std::string scope = (quotes_pos == std::string::npos)
            ? body
            : body.substr(quotes_pos);

        const double ask = jsonNumberField(scope, "ap");
        const double bid = jsonNumberField(scope, "bp");

        if (ask <= 0.0 && bid <= 0.0) {
            std::cerr << "[BROKER] getLatestPrice(" << ticker
                      << "): crypto quote had no usable bid/ask\n";
            return std::nullopt;
        }

        Quote quote;
        // Crypto exposes quotes, never a trade bar, so the mid is the price.
        // Fall back to whichever single side exists if one is missing.
        quote.price = (ask > 0.0 && bid > 0.0) ? (ask + bid) / 2.0
                                               : (ask > 0.0 ? ask : bid);
        quote.timestamp = jsonStringField(scope, "t");
        quote.from_quote = true;

        if (quote.price <= 0.0) return std::nullopt;
        quote.valid = true;
        return quote;
    }

    // --- Equities -----------------------------------------------------------
    //
    // feed=iex is pinned, not defaulted. On a free/paper account the default
    // already resolves to iex, but if the account is ever upgraded the default
    // silently becomes "sip" -- changing both the data and the rate limits.
    // An explicit feed=sip without the subscription fails with 42210000.
    const std::string path =
        "/v2/stocks/" + ticker + "/bars/latest?feed=iex";

    int status = 0;
    const std::string body = request_to("data.alpaca.markets", "GET", path, "", status);

    if (status != 200 || body.empty()) {
        std::cerr << "[BROKER] getLatestPrice(" << ticker << ") failed (http "
                  << status << "): " << body.substr(0, 200) << "\n";
        return std::nullopt;
    }

    // Response shape: {"bars":{"AAPL":{"c":123.45,"h":...,"t":"2026-..."}}}
    // The bar is nested under "bars" keyed by symbol, so plain top-level
    // field lookup would miss. Anchor the search inside the "bars" object.
    const size_t bars_pos = body.find("\"bars\"");
    const std::string scope = (bars_pos == std::string::npos)
        ? body
        : body.substr(bars_pos);

    Quote quote;
    quote.price     = jsonNumberField(scope, "c");   // close (i.e. last trade)
    quote.timestamp = jsonStringField(scope, "t");

    if (quote.price <= 0.0) {
        // Fall back to a quote mid if the bar endpoint gave nothing usable.
        // Not all symbols have a latest trade on the IEX feed at all hours.
        const std::string qpath =
            "/v2/stocks/" + ticker + "/quotes/latest?feed=iex";
        int qstatus = 0;
        const std::string qbody =
            request_to("data.alpaca.markets", "GET", qpath, "", qstatus);

        if (qstatus == 200 && !qbody.empty()) {
            const size_t qbars = qbody.find("\"quote\"");
            const std::string qscope = (qbars == std::string::npos)
                ? qbody
                : qbody.substr(qbars);

            const double bid = jsonNumberField(qscope, "bp");
            const double ask = jsonNumberField(qscope, "ap");

            if (bid > 0.0 && ask > 0.0) {
                quote.price = (bid + ask) / 2.0;
                quote.from_quote = true;
                quote.timestamp = jsonStringField(qscope, "t");
            }
        }
    }

    if (quote.price <= 0.0) {
        std::cerr << "[BROKER] getLatestPrice(" << ticker
                  << "): no usable price on iex feed\n";
        return std::nullopt;
    }

    quote.valid = true;
    return quote;
}

bool AlpacaBroker::isQuoteFresh(const Quote& quote, int max_age_seconds) {
    if (!quote.valid || quote.timestamp.empty()) return false;

    // Timestamps come back as RFC3339 UTC, e.g. "2026-09-10T14:31:00.123456789Z".
    // Only the date/time portion is needed; fractional seconds and the Z are
    // stripped before parsing.
    std::string ts = quote.timestamp;
    if (const size_t dot = ts.find('.'); dot != std::string::npos) {
        const size_t z = ts.find('Z', dot);
        ts = ts.substr(0, dot) + (z != std::string::npos ? "Z" : "");
    }

    std::tm tm{};
    std::istringstream ss(ts);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return false;

    const std::time_t quote_time = timegm(&tm);  // parse as UTC, not local
    if (quote_time <= 0) return false;

    const std::time_t now = std::time(nullptr);
    const double age = std::difftime(now, quote_time);

    return age <= static_cast<double>(max_age_seconds);
}

// ---------------------------------------------------------------------------
// Account
// ---------------------------------------------------------------------------
std::optional<AlpacaBroker::Account> AlpacaBroker::getAccount() {
    int status = 0;
    const std::string body = request("GET", "/v2/account", "", status);

    if (status != 200 || body.empty()) {
        std::cerr << "[BROKER] getAccount failed (http " << status << "): "
                  << body.substr(0, 200) << "\n";
        return std::nullopt;
    }

    Account acct;
    acct.equity       = jsonNumberField(body, "equity");
    acct.buying_power = jsonNumberField(body, "buying_power");
    acct.cash         = jsonNumberField(body, "cash");
    acct.currency     = jsonStringField(body, "currency");

    if (acct.equity <= 0.0) {
        std::cerr << "[BROKER] account returned non-positive equity; refusing to size\n";
        return std::nullopt;
    }
    return acct;
}

// ---------------------------------------------------------------------------
// Sizing
// ---------------------------------------------------------------------------
int AlpacaBroker::sharesForMaxFraction(double equity, double price, double max_fraction) {
    if (equity <= 0.0 || price <= 0.0 || max_fraction <= 0.0) return 0;

    const double budget = equity * max_fraction;
    const int shares = static_cast<int>(std::floor(budget / price));

    // 0 means the 5% budget can't even buy one share (e.g. a $800 stock on a
    // $10k account with a 5% cap). That is a "skip", never a "round up".
    return shares < 0 ? 0 : shares;
}

double AlpacaBroker::unitsForMaxFraction(double equity, double price,
                                         double max_fraction,
                                         bool fractional,
                                         double min_notional) {
    if (equity <= 0.0 || price <= 0.0 || max_fraction <= 0.0) return 0.0;

    const double budget = equity * max_fraction;

    if (!fractional) {
        // Delegate so the whole-share rule has exactly one implementation and
        // the two entry points cannot drift apart.
        return static_cast<double>(sharesForMaxFraction(equity, price, max_fraction));
    }

    // Floor to 8 decimals. Alpaca rejects quantities carrying more precision
    // than the pair supports, and that precision varies per coin, so this is
    // deliberately conservative.
    const double raw = budget / price;
    const double floored = std::floor(raw * 1e8) / 1e8;

    // Unlike equities, a sub-1.0 size is a legitimate position here -- the
    // whole-share "0 means skip" rule does not transfer. The guard is
    // economic: skip only when the order is too small to be worth placing.
    if (floored * price < min_notional) return 0.0;

    return floored;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
AlpacaBroker::OrderResult AlpacaBroker::executeTrade(const std::string& ticker,
                                                     const std::string& sentiment,
                                                     double price) {
    OrderResult result;

    // Only a bullish read has a trade path. Note crypto cannot be sold short
    // on Alpaca either, so there is no bearish counterpart to add here.
    if (sentiment != "BULLISH") {
        result.error = "sentiment is not BULLISH; no order placed";
        return result;
    }

    if (price <= 0.0) {
        result.error = "invalid price; cannot size position";
        return result;
    }

    const auto account = getAccount();
    if (!account) {
        result.error = "could not fetch account for position sizing";
        return result;
    }

    // --- CRYPTO PATH -------------------------------------------------------
    //
    // order_class=simple ONLY. A bracket returns
    // 42210000 ("crypto orders not allowed for advanced order_class"), so
    // take_profit / stop_loss / order_class are dropped entirely.
    //
    // time_in_force must be gtc or ioc; "day" is not supported for crypto,
    // which is also why a 24/7 runner needs no session boundary.
    //
    // CONSEQUENCE, and it is real: a plain market order carries NO
    // broker-side stop. Between this fill and the stop_limit placed by
    // updateTrailingStops(), the position is unprotected. If the process dies
    // in that window, nothing closes it.
    if (isCryptoSymbol(ticker)) {
        const double units = unitsForMaxFraction(
            account->equity, price, kMaxPositionFrac, /*fractional=*/true);

        if (units <= 0.0) {
            result.error = "5% equity cap is below the minimum crypto order at $" +
                           money(price) + " (equity $" + money(account->equity) + ")";
            std::cerr << "[BROKER] skipping " << ticker << ": " << result.error << "\n";
            return result;
        }

        const std::string qty = money(units, 8);  // matches the 8dp flooring above

        const std::string payload = std::string(R"JSON({
            "symbol": ")JSON") + jsonEscape(ticker) + R"JSON(",
            "qty": ")JSON" + qty + R"JSON(",
            "side": "buy",
            "type": "market",
            "time_in_force": "gtc"
        })JSON";

        std::cout << "[BROKER] " << (paper_ ? "PAPER " : "LIVE! ") << "BUY "
                  << qty << " " << ticker << " @ ~$" << money(price)
                  << " [CRYPTO: market/gtc, no broker-side bracket]\n";

        int status = 0;
        const std::string body = request("POST", "/v2/orders", payload, status);

        result.http_status = status;
        result.order_id = jsonStringField(body, "id");
        result.status   = jsonStringField(body, "status");

        if (status == 200 || status == 201) {
            result.success = true;

            // Register for software-managed protection. No stop leg exists
            // yet -- updateTrailingStops() places the stop_limit.
            trackCryptoPosition(ticker, price, units, "");

            std::cout << "[BROKER] accepted: id=" << result.order_id
                      << " status=" << result.status
                      << " (protection pending: no broker-side stop yet)\n";
        } else {
            result.error = jsonStringField(body, "message");
            if (result.error.empty()) result.error = body.substr(0, 300);
            std::cerr << "[BROKER] REJECTED (http " << status << "): "
                      << result.error << "\n";
        }

        return result;
    }

    // --- EQUITY PATH (bracket) ---------------------------------------------
    const int qty = sharesForMaxFraction(account->equity, price, kMaxPositionFrac);
    if (qty <= 0) {
        result.error = "5% equity cap buys no whole shares at $" + money(price) +
                       " (equity $" + money(account->equity) + ")";
        std::cerr << "[BROKER] skipping " << ticker << ": " << result.error << "\n";
        return result;
    }

    const double stop_price = price * (1.0 - kTrailingStopPct);
    const double take_price = price * (1.0 + kTakeProfitPct);

    // Bracket = market entry + hard stop + take profit, all broker-side.
    // NOTE: the double braces around the inner objects are just raw string
    // literal delimiters -- the JSON itself is what matters.
    const std::string payload = std::string(R"JSON({
        "symbol": ")JSON") + jsonEscape(ticker) + R"JSON(",
        "qty": ")JSON" + std::to_string(qty) + R"JSON(",
        "side": "buy",
        "type": "market",
        "time_in_force": "day",
        "order_class": "bracket",
        "take_profit": { "limit_price": ")JSON" + money(take_price) + R"JSON(" },
        "stop_loss":   { "stop_price": ")JSON" + money(stop_price) + R"JSON(" }
    })JSON";

    std::cout << "[BROKER] " << (paper_ ? "PAPER " : "LIVE! ") << "BUY " << qty
              << " " << ticker << " @ ~$" << money(price)
              << " (stop $" << money(stop_price)
              << " / target $" << money(take_price) << ")\n";

    int status = 0;
    const std::string body = request("POST", "/v2/orders", payload, status);

    result.http_status = status;
    result.order_id = jsonStringField(body, "id");
    result.status   = jsonStringField(body, "status");

    if (status == 200 || status == 201) {
        result.success = true;

        // Register for the software-side trailing stop, using the bracket's
        // stop leg id where we can read it, else the parent id.
        const std::string stop_id = jsonStringField(body, "stop_order_id");
        trackPosition(ticker, price, qty, stop_id.empty() ? result.order_id : stop_id);

        std::cout << "[BROKER] accepted: id=" << result.order_id
                  << " status=" << result.status << "\n";
    } else {
        result.error = jsonStringField(body, "message");
        if (result.error.empty()) result.error = body.substr(0, 300);
        std::cerr << "[BROKER] REJECTED (http " << status << "): "
                  << result.error << "\n";
    }

    return result;
}

AlpacaBroker::OrderResult AlpacaBroker::cancelOrder(const std::string& order_id) {
    OrderResult result;
    if (order_id.empty()) {
        result.error = "empty order id";
        return result;
    }

    int status = 0;
    const std::string body = request("DELETE", "/v2/orders/" + order_id, "", status);
    result.http_status = status;

    // Alpaca returns 204 No Content on a successful cancel.
    if (status == 204 || status == 200) {
        result.success = true;
        result.order_id = order_id;
    } else {
        result.error = jsonStringField(body, "message");
        if (result.error.empty()) result.error = "cancel failed";
    }
    return result;
}

// ---------------------------------------------------------------------------
// Software-side trailing stop
//
// Alpaca offers no broker-side trailing stop for crypto at all, and for
// equities it cannot be a bracket leg and does not trigger outside regular
// hours. The trail is therefore maintained here against the high-water mark.
// ---------------------------------------------------------------------------
void AlpacaBroker::trackPosition(const std::string& ticker, double entry_price,
                                 int qty, const std::string& stop_order_id) {
    for (auto& p : positions_) {
        if (p.ticker == ticker) {
            p.entry_price = entry_price;
            p.high_water = std::max(p.high_water, entry_price);
            p.qty = qty;
            p.units = 0.0;
            p.fractional = false;
            p.stop_order_id = stop_order_id;
            return;
        }
    }
    positions_.push_back({ticker, entry_price, entry_price, stop_order_id, qty, 0.0, false});
}

void AlpacaBroker::trackCryptoPosition(const std::string& ticker, double entry_price,
                                       double units, const std::string& stop_order_id) {
    for (auto& p : positions_) {
        if (p.ticker == ticker) {
            p.entry_price = entry_price;
            p.high_water = std::max(p.high_water, entry_price);
            p.units = units;
            p.qty = 0;
            p.fractional = true;
            p.stop_order_id = stop_order_id;
            return;
        }
    }
    positions_.push_back({ticker, entry_price, entry_price, stop_order_id, 0, units, true});
}

void AlpacaBroker::forgetPosition(const std::string& ticker) {
    positions_.erase(
        std::remove_if(positions_.begin(), positions_.end(),
                       [&](const TrailingState& p) { return p.ticker == ticker; }),
        positions_.end());
}

int AlpacaBroker::updateTrailingStops(const std::string& ticker, double current_price) {
    int moved = 0;

    for (auto& p : positions_) {
        if (p.ticker != ticker) continue;
        // Skip anything with no size -- a position that failed to register.
        if (p.fractional ? (p.units <= 0.0) : (p.qty <= 0)) continue;

        if (current_price > p.high_water) p.high_water = current_price;

        const double new_stop = p.high_water * (1.0 - kTrailingStopPct);
        const double entry_stop = p.entry_price * (1.0 - kTrailingStopPct);

        // Only ratchet UP. A trailing stop that moves down is not a stop; it
        // is a way to widen risk after the fact.
        if (new_stop <= entry_stop * 1.0001) continue;

        std::cout << "[BROKER] trailing " << p.ticker << " stop "
                  << (p.fractional ? "(stop_limit) " : "")
                  << "up to $" << money(new_stop) << " (peak $"
                  << money(p.high_water) << ")\n";

        if (!p.stop_order_id.empty()) {
            cancelOrder(p.stop_order_id);
        }

        std::string payload;

        if (p.fractional) {
            // CRYPTO: stop_limit is the ONLY supported stop type -- plain
            // "stop" and "trailing_stop" are rejected. The limit is set 2%
            // below the stop so a sharp wick doesn't leave the order
            // unfilled, at the cost of a worse fill if it gaps.
            const double limit = new_stop * 0.98;

            payload = std::string(R"JSON({
                "symbol": ")JSON") + jsonEscape(p.ticker) + R"JSON(",
                "qty": ")JSON" + money(p.units, 8) + R"JSON(",
                "side": "sell",
                "type": "stop_limit",
                "time_in_force": "gtc",
                "stop_price": ")JSON" + money(new_stop) + R"JSON(",
                "limit_price": ")JSON" + money(limit) + R"JSON("
            })JSON";
        } else {
            payload = std::string(R"JSON({
                "symbol": ")JSON") + jsonEscape(p.ticker) + R"JSON(",
                "qty": ")JSON" + std::to_string(p.qty) + R"JSON(",
                "side": "sell",
                "type": "stop",
                "time_in_force": "day",
                "stop_price": ")JSON" + money(new_stop) + R"JSON("
            })JSON";
        }

        int status = 0;
        const std::string body = request("POST", "/v2/orders", payload, status);

        if (status == 200 || status == 201) {
            p.stop_order_id = jsonStringField(body, "id");
            ++moved;
        } else {
            std::cerr << "[BROKER] trailing stop replace failed (http " << status
                      << "): " << body.substr(0, 200) << "\n";
            // Leave stop_order_id pointing at the cancelled order so the next
            // pass retries rather than believing a stop is resting.
        }
    }

    return moved;
}
