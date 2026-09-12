#include "engine.h"
#include <memory>
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

namespace {

// The handler only sets a flag. Calling crawlEngine::stop() directly from a
// signal handler is not safe: stop() takes mutexes and joins threads, and
// both async-signal-unsafe operations. Setting an atomic bool and letting a
// watcher thread do the real work keeps the handler minimal.
std::atomic<bool> g_stop_requested{false};

void handleSignal(int) {
    g_stop_requested.store(true);
}

}  // namespace

int main() {
    auto crawl = std::make_unique<crawlEngine>();

    std::cout << "Starting Trading Crawler Engine...\n";

    // 1. Seed the queue with high-value RSS feeds
    // Yahoo Finance RSS
    crawl->addUrl("https://finance.yahoo.com/news/rss");

    // PR Newswire (Earnings/Mergers)
    crawl->addUrl("https://www.prnewswire.com/rss/news-releases-list.rss");

    // CNBC Top News RSS
    crawl->addUrl("https://www.cnbc.com/id/100003114/device/rss/rss.html");

    // Ctrl-C now triggers a clean shutdown: stop() wakes the workers, joins
    // them, and force-saves history.txt. Without this the process died
    // abruptly and any URLs visited since the last checkpoint were lost.
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::cout << "[SYSTEM] Press Ctrl-C for a clean shutdown "
                 "(history will be flushed).\n";

    // Watcher: polls the signal flag, then performs the actual shutdown.
    std::thread watcher([&crawl] {
        while (!g_stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::cout << "\n[SYSTEM] Interrupt received. Shutting down...\n";
        crawl->stop();
    });

    // 2. Start the continuous autonomous loop. Returns once stop() is called.
    crawl->run();

    if (watcher.joinable()) watcher.join();

    std::cout << "[SYSTEM] Exited cleanly.\n";
    return 0;
}
