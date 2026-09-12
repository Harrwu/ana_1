#pragma once
#include "tls_socket.h"
#include "tcp_socket.h"
#include "profile.h"
#include "state_manager.h"
#include <queue>
#include <unordered_set>
#include <memory>
#include <map>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

class crawlEngine {
private:
    // What a single processUrl() call accomplished, for the completion line.
    struct UrlOutcome {
        int links_found{0};
        bool dispatched{false};
    };

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::unordered_set<std::string> visited_url{};
    std::queue<std::string> url_queue{};
    std::map<std::string, std::unique_ptr<SiteProfile>> profile_registry;

    unsigned int worker_count_;

    // Persists visited_url across restarts. load() runs once in the
    // constructor; maybeSave() is called from the workers.
    StateManager state_;

    // Tracks how many workers are idle-blocked on an empty queue, so we know
    // when the crawl is genuinely drained (and therefore a safe point to
    // force a full save) rather than just momentarily quiet.
    unsigned int idle_workers_{0};
    std::atomic<bool> shutting_down_{false};

    // Per-domain throttle. Without this, all N workers can hit the same host
    // simultaneously -- which is what gets a UA rate-limited or banned, and
    // what EDGAR explicitly forbids (10 req/s ceiling).
    // Keyed on host, value is the earliest time the next request may fire.
    std::mutex rate_mutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> next_allowed_;

    // Owned worker threads. A member (not a local in run()) so stop() can
    // signal and join them -- previously run() owned them and had no way to
    // shut them down, so the only exit was killing the process.
    std::vector<std::thread> workers_;

    // Set by stop(). Checked by workers to distinguish "shut down" from
    // "queue temporarily empty".
    std::atomic<bool> stop_requested_{false};

    // Latches the drain announcement so the milestone prints once per drain
    // episode rather than once per idle worker.
    std::atomic<bool> drain_announced_{false};

    // Workers currently processing a URL. Guarded by queue_mutex_. The drain
    // milestone requires this to be 0 AND the queue to be empty -- checking
    // only idle_workers_ was unreliable because workers cycle asynchronously.
    unsigned int in_flight_{0};

    // Latches the final history flush so it happens exactly once no matter
    // how many of run()/stop()/the destructor reach the shutdown path.
    std::atomic<bool> shutdown_finalized_{false};

    // Performs the one-time final save and prints the stopped line.
    // Idempotent; called from run() after joining.
    void finishShutdown();

    // Blocks until this host's per-profile interval has elapsed, then claims
    // the next slot. Returns immediately when the host is already clear.
    void waitForRateLimit(const std::string& host, double min_interval_seconds);

    // One worker thread's main loop: pop a URL, fetch it, parse it, dispatch
    // to the AI server, follow any links it finds. Blocks on queue_cv_ when
    // the queue is empty instead of the old sleep_for(5min) -- wakes up
    // immediately when any thread adds a new URL.
    void workerLoop();

    // The actual fetch+parse+dispatch logic for a single URL. Was previously
    // inlined in run()'s while loop with a shared `sock_` member; now takes
    // its own local socket so multiple workers can run this concurrently
    // without stepping on each other.
    UrlOutcome processUrl(const std::string& url);

    // Atomically checks visited_url and enqueues if new. Use this instead of
    // the old "if (visited_url.insert(x).second) addUrl(x);" pattern when
    // called from inside a worker thread -- that pattern has a race window
    // between the check and the push once multiple threads are running.
    void tryEnqueue(const std::string& url);

public:
    // worker_count defaults to a small pool since crawling is I/O-bound
    // (waiting on sockets, not CPU) -- it's fine, often better, to run more
    // workers than you have CPU cores. Tune this to taste.
    //
    // state_path is where the visited-URL history is persisted. History is
    // loaded here, so a restart resumes instead of re-crawling everything.
    explicit crawlEngine(unsigned int worker_count = 8,
                         const std::string& state_path = "history.txt");

    // Thread-safe: can be called from main() to seed initial URLs, or from
    // inside a worker while the pool is already running.
    void addUrl(const std::string& url);

    // Starts worker_count_ threads and blocks the calling thread joining them.
    // The join completes once stop() has been called.
    void run();

    // Requests shutdown: sets the flag, wakes every worker blocked on the
    // queue, then joins them. Safe to call from another thread (e.g. a signal
    // handler or a watchdog) or after run() returns. Idempotent.
    void stop();

    // Number of URLs known so far (visited + seeded). For logging/tests.
    size_t visitedCount() const { return visited_url.size(); }
};
