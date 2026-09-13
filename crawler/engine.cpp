#include "engine.h"
#include "builder.h"
#include "url_parser.h"
#include "html_parser.h"
#include "xml_parser.h"
#include "rss_parser.h"
#include "parser.h"
#include "cashtag_extractor.h"
#include <thread>
#include <vector>
#include <memory>
#include <iostream>
#include <algorithm>
#include <string>

#define ANA_CRYPTO_MODE 1

crawlEngine::crawlEngine(unsigned int worker_count, const std::string& state_path)
    : worker_count_(worker_count), state_(state_path) {

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        state_.load(visited_url);
    }

#if ANA_CRYPTO_MODE

    // ============================================================
    // CRYPTO PROFILE SET
    // ============================================================

    profile_registry["www.coindesk.com"] =
        std::make_unique<CoinDeskProfile>();

    profile_registry["cointelegraph.com"] =
        std::make_unique<CoinTelegraphProfile>();

    profile_registry["decrypt.co"] =
        std::make_unique<DecryptProfile>();

#else

    // ============================================================
    // EQUITY PROFILE SET
    // ============================================================

    profile_registry["finance.yahoo.com"] =
        std::make_unique<YahooProfile>();

    profile_registry["www.cnbc.com"] =
        std::make_unique<CnbcProfile>();

    profile_registry["www.prnewswire.com"] =
        std::make_unique<PRNewswireProfile>();

    profile_registry["www.benzinga.com"] =
        std::make_unique<BenzingaProfile>();

    profile_registry["www.wsj.com"] =
        std::make_unique<WsjProfile>();

    profile_registry["www.marketwatch.com"] =
        std::make_unique<MarketWatchProfile>();

    profile_registry["www.investing.com"] =
        std::make_unique<InvestingComProfile>();

    profile_registry["seekingalpha.com"] =
        std::make_unique<SeekingAlphaProfile>();

    profile_registry["www.marketbeat.com"] =
        std::make_unique<MarketBeatProfile>();

    profile_registry["www.bloomberg.com"] =
        std::make_unique<BloombergProfile>();

    profile_registry["www.reuters.com"] =
        std::make_unique<ReutersProfile>();

    profile_registry["www.sec.gov"] =
        std::make_unique<SecEdgarProfile>();

    profile_registry["www.barrons.com"] =
        std::make_unique<BarronsProfile>();

    profile_registry["www.nasdaq.com"] =
        std::make_unique<NasdaqProfile>();

    profile_registry["www.businesswire.com"] =
        std::make_unique<BusinessWireProfile>();

    profile_registry["www.globenewswire.com"] =
        std::make_unique<GlobeNewswireProfile>();

    profile_registry["www.ft.com"] =
        std::make_unique<FinancialTimesProfile>();

#endif
}

void crawlEngine::addUrl(const std::string& url) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    url_queue.push(url);
    queue_cv_.notify_one();
}

void crawlEngine::tryEnqueue(const std::string& url) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (visited_url.insert(url).second) {
        url_queue.push(url);
        queue_cv_.notify_one();
    }
}

void crawlEngine::run() {
    std::cout << "[SYSTEM] Trading Crawler Engine Started with "
              << worker_count_ << " workers.\n";

    workers_.reserve(worker_count_);

    for (unsigned int i = 0; i < worker_count_; ++i) {
        workers_.emplace_back(&crawlEngine::workerLoop, this);
    }

    // run() is the sole owner of joining. Workers exit once stop_requested_
    // is set, so this returns instead of blocking forever -- the previous
    // version had no exit path at all short of killing the process.
    //
    // Deliberately does NOT clear() the vector: stop() may still be running
    // (it is frequently called from another thread) and iterating it. Having
    // both paths join the same vector concurrently is a data race and a
    // double-join, which is what wedged the first version of this.
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }

    // Join completed, so no other thread can be iterating workers_ now.
    workers_.clear();

    // If run() returned because of a shutdown, make sure the final flush
    // happened even if stop() was never called (e.g. the flag was set
    // directly). finishShutdown() is idempotent.
    finishShutdown();
}

void crawlEngine::stop() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!stop_requested_.exchange(true)) {
            std::cout << "[SYSTEM] Shutdown requested. Draining "
                      << url_queue.size() << " queued URL(s)...\n";
        }
    }

    // Wake every worker: they are all parked on queue_cv_ and would otherwise
    // never observe the flag.
    queue_cv_.notify_all();

    // NOTE: intentionally does NOT join or touch workers_ here. run() owns
    // that. stop() is commonly called from a watcher/signal thread while
    // run() is blocked joining, so joining here would mean two threads
    // joining the same vector -- a data race, and a double-join.
}

void crawlEngine::finishShutdown() {
    // Idempotent: only the first caller performs the save.
    if (shutdown_finalized_.exchange(true)) return;

    // Final flush. The drained-path save only fires when the queue empties
    // naturally; a stop with items still queued would otherwise lose them.
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        state_.maybeSave(visited_url, /*force=*/true);
    }

    std::cout << "[SYSTEM] Crawler stopped. " << visited_url.size()
              << " URLs persisted to " << state_.path() << "\n";
}

void crawlEngine::workerLoop() {
    while (true) {
        std::string current_url;
        bool drained = false;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);

            // Count ourselves as idle while blocked.
            ++idle_workers_;

            queue_cv_.wait(lock, [this] {
                return stop_requested_ || !url_queue.empty();
            });

            --idle_workers_;

            if (url_queue.empty()) {
                if (stop_requested_) return;
                drained = true;
            } else {
                current_url = url_queue.front();
                url_queue.pop();

                // Claim the URL: from here until the matching decrement below,
                // this worker counts as in-flight. The drain check requires
                // in_flight_ == 0, which is what makes the milestone reliable.
                ++in_flight_;
            }
        }

        if (drained) {
            // Queue is empty for THIS worker. That alone does not mean the
            // crawl is drained -- another worker may still be mid-URL and
            // about to enqueue more links. The real condition is: queue empty
            // AND nobody processing.
            //
            // The previous version checked `idle_workers_ + 1 >= worker_count_`,
            // which required every other worker to be parked at the same
            // instant. Workers cycle through this branch asynchronously, so
            // that alignment almost never happened and the milestone silently
            // never fired.
            bool announced = false;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (url_queue.empty() && in_flight_ == 0) {
                    state_.maybeSave(visited_url, /*force=*/true);
                    announced = !drain_announced_.exchange(true);
                }
            }

            if (announced) {
                std::cout << "[CRAWLER MILESTONE] Queue fully drained. All "
                          << worker_count_ << " workers idle. Visited URLs saved to "
                          << state_.path() << " (" << visited_url.size() << " URLs).\n";
            }
            continue;
        }

        // A URL is being processed: clear the "already announced" latch so the
        // next drain reports again.
        drain_announced_ = false;

        const UrlOutcome outcome = processUrl(current_url);

        // Release the in-flight claim and decide whether the crawl just
        // finished. Done under the lock, then announced outside it.
        bool drained_now = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            --in_flight_;
            if (url_queue.empty() && in_flight_ == 0) {
                state_.maybeSave(visited_url, /*force=*/true);
                drained_now = !drain_announced_.exchange(true);
            }
        }

        std::cout << "[CRAWLER DONE] Processed: " << current_url
                  << " | Extracted Links: " << outcome.links_found
                  << " | Dispatched to AI: " << (outcome.dispatched ? "YES" : "NO")
                  << "\n";

        if (drained_now) {
            std::cout << "[CRAWLER MILESTONE] Queue fully drained. All "
                      << worker_count_ << " workers idle. Visited URLs saved to "
                      << state_.path() << " (" << visited_url.size() << " URLs).\n";
        }
    }
}

void crawlEngine::waitForRateLimit(const std::string& host, double min_interval_seconds) {
    if (min_interval_seconds <= 0.0) return;

    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(min_interval_seconds));

    std::unique_lock<std::mutex> lock(rate_mutex_);

    const auto now = std::chrono::steady_clock::now();
    auto it = next_allowed_.find(host);

    if (it == next_allowed_.end() || it->second <= now) {
        // Host is clear: claim the slot and go. Reserving the slot even on the
        // fast path is what stops two workers from both seeing "clear" and
        // firing together.
        next_allowed_[host] = now + interval;
        return;
    }

    // Someone else holds the slot. Work out how long is left, then sleep
    // without the lock so other workers can claim their own hosts meanwhile.
    const auto wait_until = it->second;
    next_allowed_[host] = wait_until + interval;
    lock.unlock();

    const auto delay = wait_until - std::chrono::steady_clock::now();
    if (delay > std::chrono::steady_clock::duration::zero()) {
        std::this_thread::sleep_for(delay);
    }
}

crawlEngine::UrlOutcome crawlEngine::processUrl(const std::string& current_url) {
    UrlOutcome outcome;

    UrlParts url_parts = parseUrl_(current_url);

    auto it = profile_registry.find(url_parts.host);
    if (it == profile_registry.end())
        return outcome;

    SiteProfile* active_profile = it->second.get();

    // Respect this profile's per-domain interval before opening a socket.
    waitForRateLimit(url_parts.host, active_profile->minSecondsBetweenRequests());

    std::cout << "\n[CRAWLER] Visiting: " << current_url << "\n";

    int port = (url_parts.scheme == "https") ? 443 : 80;

    // Local to this call -- each worker gets its own socket instead of
    // sharing one `sock_` member across threads.
    std::unique_ptr<Tcpsock_> sock_;

    if (port == 443)
        sock_ = std::make_unique<TlsSocket_>();
    else
        sock_ = std::make_unique<Tcpsock_>();

    if (sock_->connect(url_parts.host, port) != Tcpsock_::rType::CONNECT_)
        return outcome;

    auto buildReq = std::make_unique<ReqBuild>(url_parts.host, "close");
    buildReq->SetMethod("GET");
    buildReq->SetPath(url_parts.path.empty() ? "/" : url_parts.path);

    // Per-profile UA. SEC EDGAR 403s a generic one; everything else inherits
    // the original OptionsScraper/1.0 from SiteProfile::userAgent().
    buildReq->addHead("User-Agent", active_profile->userAgent());
    buildReq->addHead("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    buildReq->addHead("Accept-Language", "en-US,en;q=0.9");

    if (sock_->send(buildReq->build_()) != Tcpsock_::rType::CONNECT_)
        return outcome;

    std::string main_payload{};
    char buffer[4096] = {0};
    ssize_t bytes_read = 0;

    while ((bytes_read = sock_->recv(buffer, sizeof(buffer))) > 0) {
        main_payload.append(buffer, bytes_read);
        std::fill(std::begin(buffer), std::end(buffer), '\0');
    }

    if (bytes_read < 0 || main_payload.empty())
        return outcome;

    Parse_ parse(main_payload);
    parse.extract();
    Response res = parse.getResponse();

    std::unique_ptr<TextParser> main_parser = nullptr;
    bool isXml = false;

    std::string pure_body = res.getBody();
    std::string content_type = res.getHeader("Content-Type");
    std::string location = res.getHeader("Location");

    if (!location.empty()) {
        std::cout << "[ROUTER] Following Redirect -> " << location << "\n";
        tryEnqueue(location);
        outcome.links_found = 1;
        return outcome;
    }

    if (pure_body.find("<rss") != std::string::npos ||
        pure_body.find("<feed") != std::string::npos) {
        isXml = true;
        main_parser = std::make_unique<RssParse_>(pure_body);
    }
    else if (content_type.find("xml") != std::string::npos ||
             pure_body.find("<?xml") != std::string::npos ||
             pure_body.find("<urlset>") != std::string::npos) {
        isXml = true;
        main_parser = std::make_unique<XmlParse_>(pure_body);
    }
    else if (content_type.find("html") != std::string::npos ||
             pure_body.find("<!doctype html>") != std::string::npos ||
             pure_body.find("<!DOCTYPE html>") != std::string::npos ||
             pure_body.find("<html") != std::string::npos) {
        main_parser = std::make_unique<HtmlParse_>(pure_body);

        std::string article_text = active_profile->extractNewsText(pure_body);

        if (!article_text.empty()) {
            std::cout << "\n";
            std::cout << "[DEBUG] URL: " << current_url << "\n";
            std::cout << "[DEBUG] Article length: " << article_text.length() << "\n";
            std::cout << "[DEBUG] First 1000 chars:\n"
                      << article_text.substr(0, 1000)
                      << "\n";

            std::string tickers = extractCashtags(article_text);

            std::cout << "[TICKER MATCH] " << tickers << "\n";
            std::cout << "\n";

            std::string wire_payload = tickers + "|" + article_text;

            // A fresh connection per dispatch -- ai_server.cpp already
            // spawns a detached thread per incoming connection, so multiple
            // workers dispatching concurrently is already handled on that
            // side. Just make sure OLLAMA_NUM_PARALLEL is set high enough
            // (see PERFORMANCE.md) or these will queue up on the GPU anyway.
            auto ai_client = std::make_unique<Tcpsock_>();

            if (ai_client->connect("127.0.0.1", 58888) == Tcpsock_::rType::CONNECT_) {
                ai_client->send(wire_payload);
                outcome.dispatched = true;
                std::cout << "[DISPATCH] Sent " << tickers << " -> AI Server\n";
            }
            else {
                std::cout << "[ERROR] Could not connect to AI server on 127.0.0.1:58888\n";
            }
        }
    }

    if (main_parser) {
        main_parser->extract();

        if (isXml) {
            std::vector<std::string> links = main_parser->getResLink();

            for (const auto& x : links) {
                if (active_profile->isArticleLink(x)) {
                    tryEnqueue(x);
                    ++outcome.links_found;
                }
            }
        }
    }

    return outcome;
}
