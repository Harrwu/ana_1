#pragma once
#include <string>
#include <vector>
#include <cstring>

// Abstract Base Class
class SiteProfile {
public:
    virtual ~SiteProfile() = default;
    virtual bool isArticleLink(const std::string& url) const = 0;
    virtual std::string extractNewsText(const std::string& raw_html) const = 0;

    // Per-profile request identity. SEC EDGAR *requires* a descriptive
    // User-Agent carrying a contact address and returns 403 to generic ones,
    // so a single global UA cannot serve every profile. Everything else
    // inherits the original string and behaviour is unchanged.
    virtual std::string userAgent() const {
        return "OptionsScraper/1.0";
    }

    // Minimum seconds between two requests to this profile's host, enforced
    // by crawlEngine's per-domain limiter. 0 would mean "no throttling", but
    // no profile uses that: hammering a news site gets the UA blocked, and
    // EDGAR publishes a hard ceiling (10 req/s) that exceeding gets you
    // banned for 10 minutes.
    virtual double minSecondsBetweenRequests() const {
        return 1.0;
    }
};

// Inline helper so it doesn't trigger multiple-definition linker errors.
// Pulls text out of every <p>...</p> pair it finds in whatever HTML it's given.
// NOTE: this has no idea what "the article" is -- it just grabs paragraphs.
// If you feed it a whole page, you get nav bars, sidebars, related-article
// teasers, etc mixed in with the real article text. Use extractDivByClass()
// below to narrow the HTML down to just the article container first.
inline std::string genericParagraphExtractor(const std::string& raw_html) {
    std::string article_text = "";
    size_t search_pos = 0;

    while (true) {
        size_t p_start = raw_html.find("<p", search_pos);
        if (p_start == std::string::npos) break;

        size_t p_end = raw_html.find("</p>", p_start);
        if (p_end == std::string::npos) break;

        size_t text_start = raw_html.find(">", p_start);
        if (text_start == std::string::npos || text_start > p_end) {
            search_pos = p_end + 4;
            continue;
        }
        text_start += 1;

        std::string raw_paragraph = raw_html.substr(text_start, p_end - text_start);

        // Fast inner-tag stripper (handles <b>, <a>, <span> etc inside a <p>)
        std::string clean_paragraph = "";
        bool in_tag = false;
        for (char c : raw_paragraph) {
            if (c == '<') in_tag = true;
            else if (c == '>') in_tag = false;
            else if (!in_tag) clean_paragraph += c;
        }

        if (clean_paragraph.length() > 20) {
            article_text += clean_paragraph + "\n\n";
        }

        search_pos = p_end + 4;
    }

    return article_text;
}

// Finds a <div class="...className..."> and returns everything between it and
// its properly-matched closing </div>, tracking nesting depth so it doesn't
// stop at the first nested </div> it sees. Returns "" if the class isn't found
// or the HTML is too malformed to find a matching close tag.
inline std::string extractDivByClass(const std::string& html, const std::string& className) {
    size_t class_pos = html.find("class=\"" + className);
    if (class_pos == std::string::npos) {
        // Also allow the class name to appear anywhere in a multi-class
        // attribute, e.g. class="foo caas-body bar"
        class_pos = html.find(" " + className);
        if (class_pos == std::string::npos) return "";
    }

    size_t tag_start = html.rfind("<div", class_pos);
    if (tag_start == std::string::npos) return "";

    size_t tag_open_end = html.find(">", tag_start);
    if (tag_open_end == std::string::npos) return "";
    tag_open_end += 1;

    size_t pos = tag_open_end;
    int depth = 1;

    while (depth > 0) {
        size_t next_open = html.find("<div", pos);
        size_t next_close = html.find("</div>", pos);

        if (next_close == std::string::npos) {
            // Malformed / truncated HTML -- return what we found so far
            // rather than nothing.
            return html.substr(tag_open_end, html.length() - tag_open_end);
        }

        if (next_open != std::string::npos && next_open < next_close) {
            depth += 1;
            pos = next_open + 4;
        } else {
            depth -= 1;
            pos = next_close + 6;
        }
    }

    size_t content_end = pos - 6;  // right before the matching </div>
    return html.substr(tag_open_end, content_end - tag_open_end);
}

// Defensive cap so a pathological page can't make the linear scans below
// run away. Real article pages are ~1-3 MB; anything past this is almost
// always an ad frame or a streaming blob with no prose in it.
inline constexpr size_t kMaxExtractBytes = 4u * 1024u * 1024u;

// Case-insensitive search for a bare tag name. Deliberately checks the
// character AFTER the name so "<nav" doesn't also match "<navbar>" and
// "<p" in a class="pt-3" attribute -- a whole family of false hits that
// plain find() walks straight into.
inline size_t findOpenTag(const std::string& html, const std::string& tag, size_t from) {
    const size_t n = tag.size();
    size_t pos = from;
    while ((pos = html.find(tag, pos)) != std::string::npos) {
        const char after = (pos + n < html.size()) ? html[pos + n] : '>';
        if (after == '>' || after == ' ' || after == '\t' ||
            after == '\n' || after == '\r' || after == '/') {
            return pos;
        }
        pos += n;
    }
    return std::string::npos;
}

// Removes the balanced <script>...</script> / <style>...</style> / etc blocks
// that sit inside an extracted article region. The container is expected to be
// a whole page most of the time, and these tags NEVER contain article prose --
// leaving them in is what puts minified JavaScript and CSS fragments in front
// of the sentiment model.
inline std::string stripRawBlocks(const std::string& html) {
    static const std::vector<std::string> blocked = {
        "<script", "<style", "<noscript", "<svg", "<iframe", "<template"
    };

    std::string out;
    out.reserve(html.size());
    size_t pos = 0;

    while (pos < html.size()) {
        size_t next_hit = std::string::npos;
        std::string next_tag;

        for (const auto& tag : blocked) {
            const size_t hit = findOpenTag(html, tag, pos);
            if (hit != std::string::npos && (next_hit == std::string::npos || hit < next_hit)) {
                next_hit = hit;
                next_tag = tag;
            }
        }

        if (next_hit == std::string::npos) {
            out.append(html, pos, std::string::npos);
            break;
        }

        out.append(html, pos, next_hit - pos);

        const std::string closer = "</" + next_tag.substr(1) + ">";
        const size_t close_pos = html.find(closer, next_hit);
        if (close_pos == std::string::npos) {
            // Unclosed (truncated page): drop the rest rather than dumping
            // a 200 KB JS blob into the model.
            break;
        }
        pos = close_pos + closer.size();
    }

    return out;
}

// Chops a page region down to the first outer </body> or </html>. Everything
// past that is trailing markup we never want, and it means the block stripper
// stops instead of scanning tens of KB of footer.
inline std::string trimTrailingMarkup(const std::string& html) {
    size_t cut = html.size();

    for (const char* closer : {"</body>", "</BODY>", "</html>", "</HTML>"}) {
        const size_t hit = html.find(closer);
        if (hit != std::string::npos && hit < cut) cut = hit;
    }

    return html.substr(0, cut);
}

// Strips <nav>/<header>/<footer>/<aside>/<form> blocks, which on real news
// pages carry the nav tree, the masthead, the "Recommended Stories" rail and
// the footer link farm. None of it is article prose and all of it bleeds
// tickers into the cashtag extractor.
//
// These five are non-nesting in practice, so a plain first-closer search is
// enough -- there is no div-style nesting to track. Stops and keeps the
// remainder on an unclosed tag, which beats silently eating the whole page.
inline std::string stripBoilerplateBlocks(const std::string& html) {
    static const std::vector<std::string> blocked = {
        "<nav", "<header", "<footer", "<aside", "<form"
    };

    std::string out;
    out.reserve(html.size());
    size_t pos = 0;

    while (pos < html.size()) {
        size_t next_hit = std::string::npos;
        std::string next_tag;

        for (const auto& tag : blocked) {
            const size_t hit = findOpenTag(html, tag, pos);
            if (hit != std::string::npos && (next_hit == std::string::npos || hit < next_hit)) {
                next_hit = hit;
                next_tag = tag;
            }
        }

        if (next_hit == std::string::npos) {
            out.append(html, pos, std::string::npos);
            break;
        }

        out.append(html, pos, next_hit - pos);

        const std::string closer = "</" + next_tag.substr(1) + ">";
        const size_t block_end = html.find(closer, next_hit);

        if (block_end == std::string::npos) {
            // Unclosed block: stop stripping and keep the remainder rather
            // than deleting everything after it.
            out.append(html, next_hit, std::string::npos);
            break;
        }

        pos = block_end + closer.size();
    }

    return out;
}

// Replaces the handful of named/numeric entities that actually show up in
// body text. A full entity table is out of scope -- HTMLParser::extract()
// already does a pass over the page, and this is the cheap cleanup for the
// common punctuation so "&amp;" and "&#x27;" don't land inside a headline
// the model is asked to judge.
inline void decodeCommonEntities(std::string& text) {
    struct Entity { const char* from; const char* to; };
    static const Entity table[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&#x27;", "'"}, {"&#39;", "'"},
        {"&apos;", "'"}, {"&nbsp;", " "}, {"&mdash;", "-"},
        {"&ndash;", "-"}, {"&hellip;", "..."}, {"&#x2F;", "/"},
        {"&#47;", "/"}, {"&rsquo;", "'"}, {"&lsquo;", "'"},
        {"&ldquo;", "\""}, {"&rdquo;", "\""}, {"&pound;", "GBP "},
    };

    for (const auto& e : table) {
        size_t pos = 0;
        while ((pos = text.find(e.from, pos)) != std::string::npos) {
            text.replace(pos, std::strlen(e.from), e.to);
            pos += std::strlen(e.to);
        }
    }
}

// Splits a page region into <p ...>...</p> candidates and returns the text of
// each, one per line. Mirrors genericParagraphExtractor's inner-tag stripping
// so the two stay consistent.
inline std::string paragraphsAsLines(const std::string& html) {
    std::string out;
    size_t search_pos = 0;

    while (true) {
        const size_t p_start = findOpenTag(html, "<p", search_pos);
        if (p_start == std::string::npos) break;

        const size_t p_end = html.find("</p>", p_start);
        if (p_end == std::string::npos) break;

        size_t text_start = html.find('>', p_start);
        if (text_start == std::string::npos || text_start > p_end) {
            search_pos = p_end + 4;
            continue;
        }
        text_start += 1;

        const std::string raw_paragraph = html.substr(text_start, p_end - text_start);

        std::string clean;
        bool in_tag = false;
        for (char c : raw_paragraph) {
            if (c == '<') in_tag = true;
            else if (c == '>') in_tag = false;
            else if (!in_tag) clean += c;
        }

        if (!clean.empty()) {
            decodeCommonEntities(clean);
            out += clean;
            out += '\n';
        }

        search_pos = p_end + 4;
    }

    return out;
}

// Strips every remaining tag out of a region, leaving text lines. Used as the
// last-ditch fallback when a page carries its body in <div>s rather than <p>s.
inline std::string stripAllTags(const std::string& html) {
    std::string out;
    out.reserve(html.size());

    bool in_tag = false;
    for (char c : html) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) out += c;
    }

    decodeCommonEntities(out);
    return out;
}

// Candidate container classes, tried before the generic scan. These are the
// recurring CMS wrappers across the sites in profile_registry; a hit here is
// cheap and high-signal, so it's worth checking first.
inline const std::vector<std::string>& commonArticleClasses() {
    static const std::vector<std::string> classes = {
        // Yahoo / Yahoo-family (Barchart etc. render inside Yahoo's shell)
        "caas-body", "article-wrap", "canvas-body",
        // General CMS wrappers seen across finance sites
        "article-body", "articleBody", "article__body", "article-content",
        "entry-content", "post-content", "story-body", "storybody",
        "content-body", "main-content", "body-content", "ArticleBody",
        "paywall-content", "caas-art-content",
    };
    return classes;
}

// ---------------------------------------------------------------- scoring ---
// A block is "prose" if it has real text and not much markup. This is the
// whole heuristic: news bodies are long, mostly-text, low-anchor; nav rails
// and footers are long but almost entirely <a> tags.
inline size_t countSubstring(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t count = 0;
    size_t pos = haystack.find(needle, 0);
    while (pos != std::string::npos) {
        ++count;
        pos = haystack.find(needle, pos + needle.size());
    }
    return count;
}

// Score one candidate region: paragraph text minus the cost of every link.
// An <a> tag contributes ~0 sentiment text but eats bytes, so each one is a
// penalty -- that is what pushes a nav rail below the real article body.
inline long long scoreText(const std::string& paragraph_text, const std::string& region_html) {
    long long score = static_cast<long long>(paragraph_text.size());
    score -= static_cast<long long>(countSubstring(region_html, "<a ")) * 60;
    score -= static_cast<long long>(countSubstring(region_html, "<li")) * 40;
    return score;
}

// ------------------------------------------------------------- extractor ---
// Pulls the article body out of a full page: drops script/style/iframe blobs,
// strips nav/header/footer/aside/form, bounds the region to <body>, then picks
// the best-scoring candidate container. This is the function every profile
// routes through, so a single fix here covers all of them.
//
// Returns "" when nothing plausible is found, which lets each profile fall
// back to its own site-specific extraction. That ordering matters: this is a
// general heuristic, so a profile that knows the exact container class should
// still get the chance to win with higher precision.
inline std::string extractArticleBody(const std::string& raw_html) {
    if (raw_html.empty()) return "";

    // 1. Work on a bounded copy. A pathological page shouldn't cost us a
    //    linear scan over hundreds of MB.
    std::string html = raw_html.size() > kMaxExtractBytes
        ? raw_html.substr(0, kMaxExtractBytes)
        : raw_html;

    // 2. Remove non-prose tag neighbourhoods outright. Order matters: script
    //    and style first (their contents can contain "<nav" as a string
    //    literal and would otherwise confuse the block stripper), then the
    //    layout chrome.
    html = trimTrailingMarkup(html);
    html = stripRawBlocks(html);
    html = stripBoilerplateBlocks(html);

    if (html.size() < 200) return "";

    // 3. Score every known container class and every candidate region, then
    //    keep the best. Taking the first class hit would be wrong on pages
    //    like Yahoo's, where "article-wrap" matches the shell that contains
    //    the nav as well as the body -- scoring picks the real prose.
    std::string best_text;
    long long best_score = 0;

    auto consider = [&](const std::string& region_html) {
        if (region_html.size() < 200) return;
        const std::string text = paragraphsAsLines(region_html);
        if (text.size() < 200) return;

        const long long score = scoreText(text, region_html);
        if (score > best_score) {
            best_score = score;
            best_text = text;
        }
    };

    for (const auto& className : commonArticleClasses()) {
        consider(extractDivByClass(html, className));
    }

    // 4. Generic pass over the whole cleaned region as a competing candidate.
    //    On pages with no matching class this is the only candidate; on pages
    //    where a class matched too broadly, scoring decides between them.
    consider(html);

    if (best_text.size() >= 200) return best_text;

    // 5. Some sites carry their body in <div>s rather than <p>s. Only used
    //    when the paragraph passes found nothing at all.
    const std::string loose = stripAllTags(html);
    return loose.size() >= 200 ? loose : "";
}

// Tries a list of candidate container class names in order and returns the
// first non-empty match. If none of them hit, returns "" so the caller can
// decide how to fall back.
inline std::string extractFirstMatchingContainer(const std::string& html,
                                                   const std::vector<std::string>& candidateClasses) {
    for (const auto& className : candidateClasses) {
        std::string body = extractDivByClass(html, className);
        if (!body.empty()) return body;
    }
    return "";
}

// 1. Yahoo Finance
class YahooProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("finance.yahoo.com/news") != std::string::npos ||
               url.find("finance.yahoo.com/m") != std::string::npos;
    }

    std::string extractNewsText(const std::string& raw_html) const override {
        // Yahoo's article body has used a few different class names over the
        // years as their frontend has changed. The shared heuristic below
        // already scores "caas-body" / "article-wrap" / "canvas-body" (they
        // are in commonArticleClasses) and picks the best-scoring one, so it
        // supersedes the old hand-rolled attempt here.
        //
        // The old version fell back to `body = raw_html` when none of the
        // classes matched, which fed the entire page -- nav, "Recommended
        // Stories", the footer link farm -- into the AI. The sidebar tickers
        // in that chrome (BTC-USD, RKLB, SMR, TTAN) were being extracted as
        // real article tickers. The heuristic bounds the region instead.
        std::string body = extractArticleBody(raw_html);
        if (body.empty()) body = genericParagraphExtractor(raw_html);
        return body;
    }
};

// 2. CNBC
class CnbcProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        // FIX: this previously checked for the literal string
        // "[www.cnbc.com/20](https://www.cnbc.com/20)" (Markdown syntax that
        // leaked into the string literal), which never matches a real URL.
        return url.find("www.cnbc.com/20") != std::string::npos &&
               url.find("/video/") == std::string::npos;
    }

    std::string extractNewsText(const std::string& raw_html) const override {
        // Route through the shared body heuristic: it strips script/style
        // blobs and nav/header/footer/aside chrome before extracting, which
        // is what keeps sidebar tickers out of the cashtag matcher. Falls
        // back to the unbounded paragraph pass if the heuristic finds
        // nothing, so a page it mis-handles still yields text.
        std::string body = extractArticleBody(raw_html);
        if (body.empty()) body = genericParagraphExtractor(raw_html);
        return body;
    }
};

// 3. PR Newswire
class PRNewswireProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        // FIX: same Markdown-leak bug as CNBC above.
        return url.find("www.prnewswire.com/news-releases") != std::string::npos;
    }

    std::string extractNewsText(const std::string& raw_html) const override {
        // Route through the shared body heuristic: it strips script/style
        // blobs and nav/header/footer/aside chrome before extracting, which
        // is what keeps sidebar tickers out of the cashtag matcher. Falls
        // back to the unbounded paragraph pass if the heuristic finds
        // nothing, so a page it mis-handles still yields text.
        std::string body = extractArticleBody(raw_html);
        if (body.empty()) body = genericParagraphExtractor(raw_html);
        return body;
    }
};

// 4. Benzinga
class BenzingaProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        // FIX: same Markdown-leak bug as CNBC above.
        return url.find("www.benzinga.com/markets") != std::string::npos ||
               url.find("www.benzinga.com/news") != std::string::npos;
    }

    std::string extractNewsText(const std::string& raw_html) const override {
        // Route through the shared body heuristic: it strips script/style
        // blobs and nav/header/footer/aside chrome before extracting, which
        // is what keeps sidebar tickers out of the cashtag matcher. Falls
        // back to the unbounded paragraph pass if the heuristic finds
        // nothing, so a page it mis-handles still yields text.
        std::string body = extractArticleBody(raw_html);
        if (body.empty()) body = genericParagraphExtractor(raw_html);
        return body;
    }
};

// 5. Wall Street Journal
class WsjProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        // FIX: same Markdown-leak bug as CNBC above.
        return url.find("www.wsj.com/articles") != std::string::npos ||
               url.find("www.wsj.com/finance") != std::string::npos;
    }

    std::string extractNewsText(const std::string& raw_html) const override {
        // Route through the shared body heuristic: it strips script/style
        // blobs and nav/header/footer/aside chrome before extracting, which
        // is what keeps sidebar tickers out of the cashtag matcher. Falls
        // back to the unbounded paragraph pass if the heuristic finds
        // nothing, so a page it mis-handles still yields text.
        std::string body = extractArticleBody(raw_html);
        if (body.empty()) body = genericParagraphExtractor(raw_html);
        return body;
    }
};

// --- New profiles below. Confidence varies -- see the note on each one.
// Run the crawler against a real URL from each site and check the
// [DEBUG] output in your terminal; if isArticleLink() is finding 0 links,
// or extractNewsText() is pulling in obvious sidebar junk, that's the
// signal to come back and adjust the pattern here.

// 6. MarketWatch
// Confidence: high. MarketWatch article URLs have used the "/story/"
// segment for a long time, e.g. www.marketwatch.com/story/some-headline-...
class MarketWatchProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.marketwatch.com/story/") != std::string::npos;
    }

    std::string extractNewsText(const std::string& raw_html) const override {
        // Route through the shared body heuristic: it strips script/style
        // blobs and nav/header/footer/aside chrome before extracting, which
        // is what keeps sidebar tickers out of the cashtag matcher. Falls
        // back to the unbounded paragraph pass if the heuristic finds
        // nothing, so a page it mis-handles still yields text.
        std::string body = extractArticleBody(raw_html);
        if (body.empty()) body = genericParagraphExtractor(raw_html);
        return body;
    }
};

// 7. Investing.com
// Confidence: medium-high. Article URLs live under "/news/", e.g.
// www.investing.com/news/stock-market-news/some-headline-...
// They also publish a lot of syndicated/aggregated content (see the
// Wikipedia note on this if you search it) -- worth spot-checking that
// what you're pulling in is original reporting, not a re-post.
class InvestingComProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.investing.com/news/") != std::string::npos;
    }

    std::string extractNewsText(const std::string& raw_html) const override {
        // Route through the shared body heuristic: it strips script/style
        // blobs and nav/header/footer/aside chrome before extracting, which
        // is what keeps sidebar tickers out of the cashtag matcher. Falls
        // back to the unbounded paragraph pass if the heuristic finds
        // nothing, so a page it mis-handles still yields text.
        std::string body = extractArticleBody(raw_html);
        if (body.empty()) body = genericParagraphExtractor(raw_html);
        return body;
    }
};

// 8. Seeking Alpha
// Confidence: medium. Article/news URLs generally live under "/news/" or
// "/article/". Seeking Alpha also puts a lot of its content behind a
// paywall/login -- expect the crawler to sometimes get a truncated or
// paywall-teaser body rather than the full article. Verify against a
// live URL before relying on this one.
class SeekingAlphaProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("seekingalpha.com/news/") != std::string::npos ||
               url.find("seekingalpha.com/article/") != std::string::npos;
    }

    std::string extractNewsText(const std::string& raw_html) const override {
        // Route through the shared body heuristic: it strips script/style
        // blobs and nav/header/footer/aside chrome before extracting, which
        // is what keeps sidebar tickers out of the cashtag matcher. Falls
        // back to the unbounded paragraph pass if the heuristic finds
        // nothing, so a page it mis-handles still yields text.
        std::string body = extractArticleBody(raw_html);
        if (body.empty()) body = genericParagraphExtractor(raw_html);
        return body;
    }
};

// 9. MarketBeat
// Confidence: medium. MarketBeat aggregates a high volume of short,
// ticker-tagged news items rather than long-form articles -- good for
// volume/coverage, but double check article length in your [DEBUG] output;
// if it's consistently very short, genericParagraphExtractor may be
// missing most of the body and you'll want a site-specific extractor.
class MarketBeatProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.marketbeat.com/stocks/") != std::string::npos ||
               url.find("www.marketbeat.com/articles/") != std::string::npos;
    }

    std::string extractNewsText(const std::string& raw_html) const override {
        // Route through the shared body heuristic: it strips script/style
        // blobs and nav/header/footer/aside chrome before extracting, which
        // is what keeps sidebar tickers out of the cashtag matcher. Falls
        // back to the unbounded paragraph pass if the heuristic finds
        // nothing, so a page it mis-handles still yields text.
        std::string body = extractArticleBody(raw_html);
        if (body.empty()) body = genericParagraphExtractor(raw_html);
        return body;
    }
};

// ===========================================================================
// Profiles 10-20. Target list for Phase 2.
//
// These all inherit extractArticleBody() via the shared extractNewsText body
// below, which strips nav/header/footer chrome and scores candidate containers
// before extracting prose. Writing a bespoke extractor per site is what caused
// the original whole-page bug -- the generic scorer already beats per-site
// guessing, so a site only earns a hand-written override when real output
// proves it needs one. Add container class names to commonArticleClasses()
// instead (see the block above) when a site underperforms.
//
// Most of these sites are actively hostile to crawlers (paywalls, bot
// detection, JS-rendered bodies). The class existing is NOT evidence that
// extraction works against the live site -- check the [DEBUG] output in
// engine.cpp against a real URL before trusting any of them.
// ===========================================================================

// Reusable extraction body. Every profile below uses this exact shape; it is
// factored out so the fallback contract lives in one place rather than being
// copy-pasted eleven times.
#define ANA_STANDARD_EXTRACT                                                      \
    std::string extractNewsText(const std::string& raw_html) const override {     \
        std::string body = extractArticleBody(raw_html);                          \
        if (body.empty()) body = genericParagraphExtractor(raw_html);             \
        return body;                                                              \
    }

// 10. Bloomberg
// Confidence: low-medium. Heavily JS-rendered and aggressively bot-hostile;
// /articles/ and /news/articles/ are the article paths.
class BloombergProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.bloomberg.com/news/articles/") != std::string::npos ||
               url.find("www.bloomberg.com/news/features/") != std::string::npos;
    }
    ANA_STANDARD_EXTRACT
};

// 11. Reuters
// Confidence: medium. Article URLs are /world/, /markets/, /business/ with a
// trailing slug. Reuters has been adding bot detection; expect occasional
// 401/403 rather than a parse failure.
class ReutersProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.reuters.com/markets/") != std::string::npos ||
               url.find("www.reuters.com/business/") != std::string::npos ||
               url.find("www.reuters.com/technology/") != std::string::npos;
    }
    ANA_STANDARD_EXTRACT
};

// 12. SEC EDGAR
// Confidence: high for fetching, special-cased for parsing. EDGAR is the one
// source here that is explicitly *allowed* -- it is public domain, has a
// documented API, and only asks for a descriptive User-Agent. It requires a
// contact address in the UA and a hard ceiling of 10 requests/second; going
// over gets the IP banned temporarily, hence the 8s-rate limiter.
//
// Parsing note: EDGAR filings are not news articles. An 8-K or 10-Q body is
// inline-XBRL/HTML with financial tables, so <p> extraction pulls fragments.
// There is no clean prose container to score, which is why this profile does
// NOT use the standard paragraph path -- stripAllTags() over the whole body
// keeps the sentence-shaped text and drops the table markup.
class SecEdgarProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        // Match filing index pages and the raw .htm documents themselves.
        return url.find("sec.gov/Archives/edgar/data/") != std::string::npos ||
               url.find("sec.gov/cgi-bin/browse-edgar") != std::string::npos;
    }

    std::string userAgent() const override {
        // UPDATE THIS with a real address before enabling. EDGAR blocks
        // requests whose UA looks generic or automated without contact info.
        return "ana_1-research/1.0 (contact: you@example.com)";
    }

    double minSecondsBetweenRequests() const override {
        return 0.125;  // 8 req/s, under EDGAR's documented 10 req/s ceiling
    }

    std::string extractNewsText(const std::string& raw_html) const override {
        std::string html = stripRawBlocks(trimTrailingMarkup(raw_html));
        html = stripBoilerplateBlocks(html);

        // Prefer the filing's document container when present.
        const std::string doc = extractDivByClass(html, "formContent");
        const std::string source = doc.empty() ? html : doc;

        // For filing prose, the whole-body text pass actually beats the <p>
        // scorer: table cells are <td>, not <p>, so the paragraph path finds
        // almost nothing and returns "".
        std::string text = stripAllTags(source);

        if (text.size() < 200) text = paragraphsAsLines(source);
        return text;
    }
};

// 13. CoinDesk
// Confidence: medium-high. Crypto-native sites are usually friendlier to
// scrapers than the equity press, but bodies are often React-rendered.
class CoinDeskProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.coindesk.com/markets/") != std::string::npos ||
               url.find("www.coindesk.com/business/") != std::string::npos ||
               url.find("www.coindesk.com/policy/") != std::string::npos ||
               url.find("www.coindesk.com/tech/") != std::string::npos;
    }
    double minSecondsBetweenRequests() const override { return 2.0; }
    ANA_STANDARD_EXTRACT
};

// 14. CoinTelegraph
// Confidence: medium. Article URLs are /news/<slug> and /markets/<slug>.
class CoinTelegraphProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("cointelegraph.com/news/") != std::string::npos ||
               url.find("cointelegraph.com/markets/") != std::string::npos;
    }
    double minSecondsBetweenRequests() const override { return 2.0; }
    ANA_STANDARD_EXTRACT
};

// 15. Decrypt
// Confidence: medium. Smaller outlet, /article/<slug> style paths.
class DecryptProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("decrypt.co/") != std::string::npos &&
               url.find("/article/") != std::string::npos;
    }
    ANA_STANDARD_EXTRACT
};

// 16. Barron's
// Confidence: low. Hard paywall (News Corp, same family as WSJ). Expect a
// teaser rather than a body for most article URLs -- the extraction fix makes
// that teaser *cleaner*, not *longer*.
class BarronsProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.barrons.com/articles/") != std::string::npos;
    }
    ANA_STANDARD_EXTRACT
};

// 17. Nasdaq
// Confidence: medium. /articles/ and /market-activity/ paths; mixes original
// content with redistributed wire copy.
class NasdaqProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.nasdaq.com/articles/") != std::string::npos ||
               url.find("www.nasdaq.com/market-activity/") != std::string::npos;
    }
    ANA_STANDARD_EXTRACT
};

// 18. Business Wire
// Confidence: high for access, medium for prose. Wire releases are
// consistently structured and unusually scraper-friendly, so these extract
// well -- but a press release is a company describing itself favourably,
// which is a systematic bias to be aware of when reading the sentiment.
class BusinessWireProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.businesswire.com/news/home/") != std::string::npos;
    }
    ANA_STANDARD_EXTRACT
};

// 19. GlobeNewswire
// Confidence: high for access, medium for prose. Same wire-release bias
// caveat as Business Wire above.
class GlobeNewswireProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.globenewswire.com/news-release/") != std::string::npos ||
               url.find("www.globenewswire.com/en/news-release/") != std::string::npos;
    }
    ANA_STANDARD_EXTRACT
};

// 20. Financial Times
// Confidence: low. Metred paywall -- the same URL may return full text or a
// truncated teaser depending on cookie state, which makes extraction quality
// nondeterministic run to run.
class FinancialTimesProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.ft.com/content/") != std::string::npos;
    }
    ANA_STANDARD_EXTRACT
};
