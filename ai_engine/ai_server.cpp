#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <sstream>
#include <mutex>
#include <chrono>
#include <format>
#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "broker_api.h"

std::mutex csv_lock;

// Guards the shared AlpacaBroker.
//
// The broker owns mutable state: the positions_ vector (read and written by
// trackPosition/updateTrailingStops) and the API key material. Detached
// worker threads each handle a different article, so without this two
// concurrent BULLISH verdicts could interleave writes to positions_ and
// corrupt it.
//
// Scope note: this is NOT protecting a socket. Each request() call constructs
// its own TlsSocket_, so there is no shared socket for threads to interleave
// on -- the concern in the brief was aimed at the wrong object. The real
// shared mutable state is the broker's position book.
std::mutex broker_lock;

// Set once at startup. Null when credentials are missing, in which case the
// server still scores sentiment and writes the CSV but places no orders.
std::unique_ptr<AlpacaBroker> broker;

// Account-level risk state, guarded by broker_lock.
//
// executeTrade's 5% cap is PER ORDER. An article matching several tickers can
// produce several bullish verdicts in one batch, each individually under 5%,
// summing to far more of the account than intended. This tracks cumulative
// deployment so the cap means what it says at the portfolio level too.
double deployed_fraction = 0.0;
constexpr double kMaxPortfolioFraction = 0.25;  // placeholder, tune to taste

std::mutex orders_lock;

// Appends one line to orders.log. Separate lock from csv_lock so a slow
// broker submission can't block signal logging.
void logOrder(const std::string& line) {
    std::lock_guard<std::mutex> lock(orders_lock);
    std::ofstream out("orders.log", std::ios::app);
    if (out.is_open()) {
        out << line << "\n";
        out.flush();
    }
}

// RFC3339-ish UTC timestamp, matching the format used for CSV rows.
std::string nowStamp() {
    auto const now = std::chrono::system_clock::now();
    return std::format("{0:%Y-%m-%d %H:%M:%S}", now);
}

// 1. JSON Sanitizer for the Ollama payload
std::string escapeJSON(const std::string& input) {
    std::string output;
    output.reserve(input.length());
    for (char c : input) {
        switch (c) {
            case '"':  output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n";  break;
            case '\r': output += "\\r";  break;
            case '\t': output += "\\t";  break;
            default:   output += c;
        }
    }
    return output;
}

// Pulls the first BULLISH / BEARISH / NEUTRAL word out of whatever the model
// said, uppercased. Anything else (extra punctuation, a stray sentence, the
// model refusing to answer, etc) collapses to NEUTRAL rather than corrupting
// the CSV row -- backtest.py does an exact string match on this column, so
// it must always be one of these three words.
std::string sanitizeSentiment(const std::string& raw) {
    size_t start = raw.find_first_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    if (start == std::string::npos) return "NEUTRAL";

    size_t end = raw.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", start);
    std::string word = (end == std::string::npos)
        ? raw.substr(start)
        : raw.substr(start, end - start);

    std::transform(word.begin(), word.end(), word.begin(), ::toupper);

    if (word == "BULLISH" || word == "BEARISH" || word == "NEUTRAL")
        return word;

    return "NEUTRAL";
}

// A single ticker's verdict.
struct TickerScore {
    std::string ticker;
    std::string sentiment;  // always BULLISH / BEARISH / NEUTRAL
};

// Splits the wire payload's ticker field into individual symbols.
//
// Accepts BOTH separators: the cashtag extractor emits comma-separated
// ("AAPL,MSFT") but hand-written test payloads often use spaces. Whitespace
// around each symbol is trimmed and empties are dropped, so "AAPL, MSFT" and
// "AAPL MSFT" both yield two tickers.
std::vector<std::string> splitTickers(const std::string& raw) {
    std::vector<std::string> out;
    std::string current;

    auto flush = [&]() {
        while (!current.empty() &&
               std::isspace(static_cast<unsigned char>(current.front()))) {
            current.erase(current.begin());
        }
        while (!current.empty() &&
               std::isspace(static_cast<unsigned char>(current.back()))) {
            current.pop_back();
        }
        if (!current.empty()) out.push_back(current);
        current.clear();
    };

    for (char c : raw) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush();
        } else {
            current += c;
        }
    }
    flush();

    return out;
}

// True when `t` looks like a tradable symbol: 1-6 chars, letters/digits, with
// an optional single '.' or '-' share-class separator (BRK.B, RDS-A). Guards
// the CSV against a model that returns a word like "Article" or "Overall" in
// the ticker slot -- an unvalidated value there would write a junk row.
bool isPlausibleTicker(const std::string& t) {
    if (t.empty() || t.size() > 6) return false;

    bool seen_separator = false;
    for (char c : t) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) continue;
        if ((c == '.' || c == '-') && !seen_separator) {
            seen_separator = true;
            continue;
        }
        return false;
    }
    // Must contain at least one letter (rules out pure numbers).
    for (char c : t) {
        if (std::isalpha(static_cast<unsigned char>(c))) return true;
    }
    return false;
}

// Parses the model's per-ticker response into (ticker, sentiment) pairs.
//
// Expected shape is one verdict per line, "AAPL:BULLISH". Tolerates the model
// wrapping lines in bullets/quotes and accepts a bare ticker with the
// sentiment on the same line after whitespace, because small models drift
// from the requested format.
//
// A ticker mentioned in the request but MISSING from the response is filled
// in as NEUTRAL by the caller rather than dropped -- dropping it would
// silently lose a signal, and the whole point of per-ticker scoring is that
// every ticker gets a verdict.
std::vector<TickerScore> parseTickerScores(const std::string& model_output,
                                           const std::vector<std::string>& requested) {
    std::vector<TickerScore> scores;

    auto trim = [](std::string s) {
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
        size_t end = s.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
        return s.substr(start, end - start);
    };

    std::string line;
    std::istringstream stream(model_output);

    while (std::getline(stream, line)) {
        // Strip list markers / quotes the model may have added.
        while (!line.empty() &&
               (line.front() == '-' || line.front() == '*' || line.front() == '"' ||
                line.front() == '\'' || line.front() == '`' || line.front() == ' ')) {
            line.erase(line.begin());
        }
        for (char& c : line) {
            if (c == '"' || c == '\'' || c == '`' || c == '*' || c == '.' || c == '#') c = ' ';
        }

        // "AAPL : BULLISH" -> ticker "AAPL", sentiment " BULLISH "
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string ticker = trim(line.substr(0, colon));
        std::string verdict = trim(line.substr(colon + 1));

        // A verdict line may itself be "BULLISH (reason)". Take the first word.
        const size_t sp = verdict.find_first_of(" \t");
        if (sp != std::string::npos) verdict = verdict.substr(0, sp);

        std::transform(ticker.begin(), ticker.end(), ticker.begin(), ::toupper);

        if (!isPlausibleTicker(ticker)) continue;

        const std::string sentiment = sanitizeSentiment(verdict);

        // First verdict for a ticker wins; ignore later duplicates.
        bool already = false;
        for (const auto& s : scores) {
            if (s.ticker == ticker) { already = true; break; }
        }
        if (!already) scores.push_back({ticker, sentiment});
    }

    // Backfill anything the model omitted so no requested ticker is lost.
    for (const auto& t : requested) {
        bool present = false;
        for (const auto& s : scores) {
            if (s.ticker == t) { present = true; break; }
        }
        if (!present) scores.push_back({t, "NEUTRAL"});
    }

    // If the model returned nothing usable at all, fall back to NEUTRAL for
    // every requested ticker rather than writing no row.
    if (scores.empty()) {
        for (const auto& t : requested) scores.push_back({t, "NEUTRAL"});
    }

    return scores;
}

// 2. Query Ollama on 127.0.0.1:11434
//
// tickers: the article's matched symbols, comma-separated (e.g. "AAPL,MSFT")
// text:    the article body
//
// Returns the model's raw per-ticker response text; parsing happens in
// parseTickerScores() so the two concerns stay separable and testable.
std::string query_ollama(const std::string& tickers, const std::string& text) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(11434);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return "";
    }

    std::string safe_text = escapeJSON(text);
    if (safe_text.length() > 4000) {
        safe_text = safe_text.substr(0, 4000);
    }

    // Per-ticker scoring instead of one overall verdict.
    //
    // The previous prompt asked for a single word for the whole group. That
    // dilutes: an article strongly bullish on NVDA and neutral on MSFT
    // averaged to one label that described neither. Asking for one line per
    // ticker keeps each verdict independent.
    //
    // "num_predict" is scaled with the ticker count -- 6 tokens is enough for
    // one word but truncates a multi-line reply mid-list, which would drop
    // verdicts and silently mark them NEUTRAL. "temperature": 0 keeps decoding
    // greedy so the same article always scores the same way.
    const int token_budget = 12 + 12 * static_cast<int>(std::count(tickers.begin(), tickers.end(), ',') + 1);

    std::string body = R"({
        "model": "llama3.1",
        "prompt": "You are a financial news sentiment classifier. For EACH stock ticker listed, decide the sentiment THIS article conveys about that specific ticker, judged independently of the others. Respond with one line per ticker, in exactly this format and nothing else: TICKER:SENTIMENT where SENTIMENT is BULLISH, BEARISH, or NEUTRAL. Tickers: )" + tickers + R"(. Article: )" + safe_text + R"(",
        "stream": false,
        "options": {
            "num_predict": )" + std::to_string(token_budget) + R"(,
            "temperature": 0
        }
    })";

    std::string req = "POST /api/generate HTTP/1.1\r\n"
                      "Host: 127.0.0.1:11434\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(body.length()) + "\r\n"
                      "Connection: close\r\n\r\n" + body;

    send(sock, req.c_str(), req.length(), 0);

    std::string response{};
    char buffer[4096];
    ssize_t bytes = 0;
    while ((bytes = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, bytes);
    }
    close(sock);

    // Pull the "response" field. Note the JSON body escapes newlines as \n,
    // so unescape them or every verdict lands on one line and only the first
    // parses.
    size_t key_pos = response.find("\"response\":\"");
    if (key_pos == std::string::npos) return "";

    key_pos += 12;

    std::string raw;
    bool escaped = false;
    for (size_t i = key_pos; i < response.size(); ++i) {
        const char c = response[i];
        if (escaped) {
            switch (c) {
                case 'n':  raw += '\n'; break;
                case 't':  raw += '\t'; break;
                case 'r':  raw += '\r'; break;
                case '"':  raw += '"';  break;
                case '\\': raw += '\\'; break;
                default:   raw += c;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            break;
        } else {
            raw += c;
        }
    }

    return raw;
}

// 3. Thread Worker: Processes each article independently
void process_sentiment(int client_sock) {
    const auto batch_start = std::chrono::steady_clock::now();

    std::string payload{};
    char buffer[8192];
    ssize_t bytes_read = 0;

    while ((bytes_read = recv(client_sock, buffer, sizeof(buffer), 0)) > 0) {
        payload.append(buffer, bytes_read);
    }
    close(client_sock); // Hang up immediately so crawler resumes instantly

    if (payload.empty()) return;

    size_t delim_pos = payload.find('|');
    if (delim_pos == std::string::npos) return;

    std::string ticker_field = payload.substr(0, delim_pos);
    std::string article_text = payload.substr(delim_pos + 1);

    // The crawler sends comma-separated cashtags; splitTickers also accepts
    // whitespace so hand-written test payloads work.
    const std::vector<std::string> tickers = splitTickers(ticker_field);
    if (tickers.empty()) return;

    std::cout << "[AI BATCH START] " << tickers.size() << " ticker(s) for article ("
              << article_text.length() << " chars): " << ticker_field << "\n";

    const std::string model_output = query_ollama(ticker_field, article_text);
    std::vector<TickerScore> scores = parseTickerScores(model_output, tickers);

    const std::string timestamp = nowStamp();

    for (const auto& s : scores) {
        std::cout << "[RESULT] " << timestamp << " | " << s.ticker
                  << " -> " << s.sentiment << "\n";
    }

    // 4. Thread-Safe CSV Logging
    //
    // One row PER TICKER, not one row with a combined "AAPL:BULLISH,MSFT:X"
    // cell. backtest.py reads the last column as a single sentiment
    // (parts[-1]) and compares it to 'BULLISH' exactly, so a combined cell
    // would never match and every multi-ticker signal would be dropped.
    // Splitting into rows keeps the existing CSV schema and parser intact.
    {
        std::lock_guard<std::mutex> lock(csv_lock);
        std::ofstream csv_file("trading_signals.csv", std::ios::app);
        if (csv_file.is_open()) {
            // Format: Timestamp, Ticker, Sentiment
            for (const auto& s : scores) {
                csv_file << timestamp << "," << s.ticker << "," << s.sentiment << "\n";
            }
            csv_file.flush();
        }
    }

    // 5. Execution trigger.
    //
    // Every BULLISH verdict resolves a price and submits a bracket order.
    // Scoring and logging have already completed above, so a broker failure
    // degrades to "signal recorded, no trade" rather than losing the signal.
    int traded = 0;
    int bearish = 0;
    int neutral = 0;

    for (auto& s : scores) {
        if (s.sentiment == "BEARISH") { bearish ++; continue; }
        if (s.sentiment == "NEUTRAL") { neutral ++; continue; }
        if (s.ticker == "MACRP") continue;

        if (s.ticker == "BTC" || s.ticker == "ETH" || s.ticker == "SOL" || s.ticker == "DOGE") {
            s.ticker += "/USD";
        }

        // --- BULLISH ---

        // Everything touching the shared broker happens under one lock,
        // including the price lookup: getLatestPrice is a plain method on the
        // same object and would otherwise race with trackPosition on the
        // position book.
        std::lock_guard<std::mutex> lock(broker_lock);

        if (!broker) {
            const std::string msg =
                timestamp + " | " + s.ticker + " | SKIP | no broker configured "
                "(set APCA_API_KEY_ID / APCA_API_SECRET_KEY)";
            std::cout << "[AI BROKER] " << msg << "\n";
            logOrder(msg);
            continue;
        }

        if (deployed_fraction >= kMaxPortfolioFraction) {
            const std::string msg =
                timestamp + " | " + s.ticker + " | SKIP | portfolio cap reached ("
                + std::to_string(static_cast<int>(deployed_fraction * 100))
                + "% deployed, limit "
                + std::to_string(static_cast<int>(kMaxPortfolioFraction * 100)) + "%)";
            std::cout << "[AI BROKER] " << msg << "\n";
            logOrder(msg);
            continue;
        }

        const auto quote = broker->getLatestPrice(s.ticker);
        if (!quote) {
            const std::string msg = timestamp + " | " + s.ticker +
                " | SKIP | no price available from data.alpaca.markets";
            std::cout << "[AI BROKER] " << msg << "\n";
            logOrder(msg);
            continue;
        }

        if (!AlpacaBroker::isQuoteFresh(*quote)) {
            const std::string msg = timestamp + " | " + s.ticker +
                " | SKIP | stale quote (" + quote->timestamp + ")";
            std::cout << "[AI BROKER] " << msg << "\n";
            logOrder(msg);
            continue;
        }

        const auto result = broker->executeTrade(s.ticker, s.sentiment, quote->price);

        if (result.success) {
            const std::string src = quote->from_quote ? "mid" : "last";
            const std::string msg =
                timestamp + " | " + s.ticker + " | FILLED | id=" + result.order_id
                + " | status=" + result.status
                + " | entry~$" + std::format("{:.2f}", quote->price)
                + " | stop~$" + std::format("{:.2f}",
                      quote->price * (1.0 - AlpacaBroker::kTrailingStopPct))
                + " | target~$" + std::format("{:.2f}",
                      quote->price * (1.0 + AlpacaBroker::kTakeProfitPct))
                + " | price_src=" + src;
            std::cout << "[AI BROKER] " << msg << "\n";
            logOrder(msg);

            // Account for the deployment. executeTrade floors to whole shares,
            // so recompute the actual fraction from what it bought rather than
            // assuming it spent the full 5%.
            const auto acct = broker->getAccount();
            if (acct && acct->equity > 0.0) {
                const int qty = AlpacaBroker::sharesForMaxFraction(
                    acct->equity, quote->price);
                deployed_fraction += (qty * quote->price) / acct->equity;
            }
            ++traded;
        } else {
            const std::string msg =
                timestamp + " | " + s.ticker + " | REJECTED | " + result.error
                + " | http=" + std::to_string(result.http_status);
            std::cout << "[AI BROKER] " << msg << "\n";
            logOrder(msg);
        }
    }

    // 6. Batch completion signal.
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - batch_start).count();

    std::cout << "[AI BATCH COMPLETE] Finished analyzing "
              << scores.size() << "/" << tickers.size()
              << " tickers for article (Elapsed: " << elapsed_ms << " ms).\n";
    std::cout << "[AI STATUS] Summary: " << traded << " BULLISH (Traded), "
              << bearish << " BEARISH, " << neutral << " NEUTRAL.\n";
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[ERROR] Socket creation failed.\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    address.sin_port = htons(58888);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "[ERROR] Bind failed on 127.0.0.1:58888.\n";
        return 1;
    }

    if (listen(server_fd, 50) < 0) {
        std::cerr << "[ERROR] Listen failed.\n";
        return 1;
    }

    // Ensure CSV header exists
    {
        std::ifstream check("trading_signals.csv");
        if (!check.good()) {
            std::ofstream init("trading_signals.csv");
            init << "Timestamp,Ticker,Sentiment\n";
        }
    }

    // Broker initialization. Credentials come from the environment, never
    // from argv -- command lines are readable via /proc and these keys can
    // move money.
    //
    // Missing credentials are NOT fatal: the server still scores sentiment and
    // logs signals, it just doesn't trade. That keeps the AI pipeline useful
    // (and testable) on a machine with no broker access.
    {
        const char* key_id = std::getenv("APCA_API_KEY_ID");
        const char* secret = std::getenv("APCA_API_SECRET_KEY");

        if (key_id && secret && *key_id && *secret) {
            // Paper endpoint, hardcoded. Switching to live means editing this
            // line deliberately -- there is no environment toggle, because
            // that would make going live a config change rather than a
            // decision.
            broker = std::make_unique<AlpacaBroker>(
                key_id, secret, "paper-api.alpaca.markets", /*paper=*/true);
        }else {
            std::cout << "\n";
            std::cout << "[AI SERVER] Notice APCA_API_KEY_ID | APCA_API_SECRET_KEY Not found in Enviroment\n";
            std::cout << "[AI SERVER] Procced [y/n]: ";
            std::string usrIn; std::getline(std::cin, usrIn);
            {
                std::string passIn;
                for (char &c: usrIn){
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    passIn += c;
                }
                usrIn = passIn;
            }

            if (usrIn == "yes" || usrIn == "y"){
                std::cout << "[AI SERVER] Exiting On User Command\n";
                close(server_fd); return 0;
            }else std::cout << "[AI SERVER] Continuing\n";
        }
    }

    std::cout << "\n";
    std::cout << "[AI SERVER] Listening on 127.0.0.1:58888\n";
    std::cout << "[AI SERVER] Logging to trading_signals.csv\n";
    if (broker) {
        std::cout << "[AI SERVER] Broker: PAPER paper-api.alpaca.markets\n";
        std::cout << "[AI SERVER] Order log: orders.log\n";
        std::cout << "[AI SERVER] Per-order cap: 5% of equity | "
                     "portfolio cap: " << static_cast<int>(kMaxPortfolioFraction * 100)
                  << "%\n";
    }
    std::cout << "\n";

    while (true) {
        int client_sock = accept(server_fd, nullptr, nullptr);
        if (client_sock >= 0) {
            std::thread(process_sentiment, client_sock).detach();
        }
    }

    close(server_fd);
    return 0;
}
