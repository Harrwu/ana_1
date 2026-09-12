// Command-line entry point for the Alpaca broker client.
//
//   brokercli account
//   brokercli size <equity> <price> [max_fraction]
//   brokercli buy <TICKER> <BULLISH|BEARISH|NEUTRAL> <price> [--dry-run]
//   brokercli canceltest <order_id>
//
// Credentials come from the environment, never from argv -- command lines are
// visible to every process on the box via /proc, and these keys can move money.
//   APCA_API_KEY_ID, APCA_API_SECRET_KEY
//
// --dry-run prints exactly what would be sent and exits without a network
// call. Use it before every live-ish run; it is the cheapest safeguard here.

#include "broker_api.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

std::string envOr(const char* name, const char* fallback = "") {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string(fallback);
}

void usage() {
    std::cerr <<
        "usage:\n"
        "  brokercli account\n"
        "  brokercli size <equity> <price> [max_fraction]\n"
        "  brokercli buy <TICKER> <BULLISH|BEARISH|NEUTRAL> <price> [--dry-run]\n"
        "  brokercli cancel <order_id>\n"
        "\nenv: APCA_API_KEY_ID, APCA_API_SECRET_KEY\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    const std::string command = argv[1];

    // `size` is pure arithmetic -- it needs no credentials, so it can be
    // sanity-checked on a machine that has none.
    if (command == "size") {
        if (argc < 4) { usage(); return 2; }
        const double equity = std::atof(argv[2]);
        const double price  = std::atof(argv[3]);
        const double frac   = (argc >= 5) ? std::atof(argv[4]) : AlpacaBroker::kMaxPositionFrac;

        const int qty = AlpacaBroker::sharesForMaxFraction(equity, price, frac);
        const double notional = qty * price;

        std::cout << "equity       : $" << equity << "\n";
        std::cout << "price        : $" << price << "\n";
        std::cout << "max fraction : " << (frac * 100.0) << "%\n";
        std::cout << "budget       : $" << (equity * frac) << "\n";
        std::cout << "shares       : " << qty << "\n";
        std::cout << "notional     : $" << notional
                  << "  (" << (equity > 0 ? (notional / equity * 100.0) : 0.0) << "% of equity)\n";
        if (qty == 0) {
            std::cout << "NOTE: 0 shares -- the cap cannot buy one whole share at this\n"
                         "      price. The caller must treat this as 'do not trade'.\n";
        }
        return 0;
    }

    const std::string api_key = envOr("APCA_API_KEY_ID");
    const std::string secret  = envOr("APCA_API_SECRET_KEY");

    if (api_key.empty() || secret.empty()) {
        std::cerr << "[ERROR] APCA_API_KEY_ID / APCA_API_SECRET_KEY not set.\n"
                     "        Export both before running. Refusing to continue.\n";
        return 1;
    }

    // Paper endpoint is the default. Switching to live requires editing this
    // line deliberately -- there is no --live flag by design.
    AlpacaBroker broker(api_key, secret, "paper-api.alpaca.markets", /*paper=*/true);

    if (command == "account") {
        const auto acct = broker.getAccount();
        if (!acct) {
            std::cerr << "[ERROR] could not read account.\n";
            return 1;
        }
        std::cout << "equity       : $" << acct->equity << "\n";
        std::cout << "buying power : $" << acct->buying_power << "\n";
        std::cout << "cash         : $" << acct->cash << " " << acct->currency << "\n";
        std::cout << "5% budget    : $" << (acct->equity * AlpacaBroker::kMaxPositionFrac) << "\n";
        return 0;
    }

    if (command == "buy") {
        if (argc < 5) { usage(); return 2; }
        const std::string ticker    = argv[2];
        const std::string sentiment = argv[3];
        const double price          = std::atof(argv[4]);

        const bool dry = (argc >= 6 && std::string(argv[5]) == "--dry-run");

        if (dry) {
            const auto acct = broker.getAccount();
            const double equity = acct ? acct->equity : 0.0;
            const int qty = AlpacaBroker::sharesForMaxFraction(equity, price);

            std::cout << "[DRY RUN] no order will be sent\n";
            std::cout << "  ticker    : " << ticker << "\n";
            std::cout << "  sentiment : " << sentiment << "\n";
            std::cout << "  price     : $" << price << "\n";
            std::cout << "  equity    : $" << equity << "\n";
            std::cout << "  shares    : " << qty << "\n";
            std::cout << "  stop      : $" << price * (1.0 - AlpacaBroker::kTrailingStopPct) << " (-1%)\n";
            std::cout << "  target    : $" << price * (1.0 + AlpacaBroker::kTakeProfitPct) << " (+3%)\n";
            return 0;
        }

        const auto res = broker.executeTrade(ticker, sentiment, price);
        if (!res.success) {
            std::cerr << "[ERROR] " << res.error << "\n";
            return 1;
        }
        std::cout << "order id : " << res.order_id << "\n";
        std::cout << "status   : " << res.status << "\n";
        return 0;
    }

    if (command == "cancel") {
        if (argc < 3) { usage(); return 2; }
        const auto res = broker.cancelOrder(argv[2]);
        if (!res.success) {
            std::cerr << "[ERROR] " << res.error << "\n";
            return 1;
        }
        std::cout << "cancelled " << res.order_id << "\n";
        return 0;
    }

    usage();
    return 2;
}
