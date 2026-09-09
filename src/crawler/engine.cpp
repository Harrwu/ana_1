#include "engine.h"
#include "builder.h"
#include "url_parser.h"
#include "html_parser.h"
#include "xml_parser.h"
#include "rss_parser.h"
#include "parser.h"
#include <thread>
#include <chrono>
#include <memory>
#include <iostream>
#include <algorithm>
#include <regex>
#include <set>
#include <vector>
#include <string>
#include <cctype>

static std::string toLowerCopy(const std::string& input) {
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

static std::string toUpperCopy(const std::string& input) {
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return result;
}

static std::string decodeDollarEntities(const std::string& text) {
    std::string result = text;

    auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.length(), to);
            pos += to.length();
        }
    };

    replaceAll(result, "&#36;", "$");
    replaceAll(result, "&#36", "$");
    replaceAll(result, "&#x24;", "$");
    replaceAll(result, "&#X24;", "$");
    replaceAll(result, "&#x24", "$");
    replaceAll(result, "&#X24", "$");
    replaceAll(result, "&dollar;", "$");

    return result;
}

static bool isWordCharacter(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

static bool containsWholePhrase(const std::string& lowerText, const std::string& lowerPhrase) {
    if (lowerPhrase.empty())
        return false;

    size_t searchPos = 0;

    while (true) {
        size_t pos = lowerText.find(lowerPhrase, searchPos);

        if (pos == std::string::npos)
            return false;

        bool validBefore = true;
        bool validAfter = true;

        if (pos > 0)
            validBefore = !isWordCharacter(lowerText[pos - 1]);

        size_t endPos = pos + lowerPhrase.length();

        if (endPos < lowerText.length())
            validAfter = !isWordCharacter(lowerText[endPos]);

        if (validBefore && validAfter)
            return true;

        searchPos = pos + 1;
    }
}

static std::string extractCashtags(const std::string& text) {
    const std::string decodedText = decodeDollarEntities(text);
    std::set<std::string> detectedTickers;

    static const std::regex cashtagRegex(
        R"(\$([A-Za-z]{1,5}(?:\.[A-Za-z]{1,2})?)(?![A-Za-z0-9_]))"
    );

    for (std::sregex_iterator it(decodedText.begin(), decodedText.end(), cashtagRegex);
         it != std::sregex_iterator();
         ++it) {
        std::string ticker = toUpperCopy((*it)[1].str());
        detectedTickers.insert(ticker);
    }

    const std::string lowerText = toLowerCopy(decodedText);

    static const std::vector<std::pair<std::string, std::string>> companies = {
        {"apple", "AAPL"},
        {"apple inc", "AAPL"},

        {"microsoft", "MSFT"},
        {"microsoft corporation", "MSFT"},

        {"nvidia", "NVDA"},
        {"nvidia corporation", "NVDA"},

        {"amazon", "AMZN"},
        {"amazon.com", "AMZN"},
        {"amazon web services", "AMZN"},

        {"alphabet", "GOOGL"},
        {"google", "GOOGL"},

        {"meta platforms", "META"},
        {"facebook", "META"},
        {"instagram", "META"},
        {"whatsapp", "META"},

        {"tesla", "TSLA"},
        {"tesla motors", "TSLA"},

        {"netflix", "NFLX"},

        {"advanced micro devices", "AMD"},
        {"amd", "AMD"},

        {"intel", "INTC"},
        {"intel corporation", "INTC"},

        {"broadcom", "AVGO"},
        {"broadcom inc", "AVGO"},

        {"qualcomm", "QCOM"},

        {"micron", "MU"},
        {"micron technology", "MU"},

        {"arm holdings", "ARM"},

        {"marvell technology", "MRVL"},

        {"applied materials", "AMAT"},

        {"lam research", "LRCX"},

        {"kla corporation", "KLAC"},

        {"texas instruments", "TXN"},

        {"analog devices", "ADI"},

        {"asml holding", "ASML"},
        {"asml", "ASML"},

        {"taiwan semiconductor", "TSM"},
        {"taiwan semiconductor manufacturing", "TSM"},
        {"tsmc", "TSM"},

        {"oracle", "ORCL"},

        {"salesforce", "CRM"},

        {"palantir", "PLTR"},
        {"palantir technologies", "PLTR"},

        {"cisco systems", "CSCO"},
        {"cisco", "CSCO"},

        {"adobe", "ADBE"},

        {"servicenow", "NOW"},

        {"accenture", "ACN"},

        {"intuit", "INTU"},

        {"snowflake", "SNOW"},

        {"crowdstrike", "CRWD"},
        {"crowdstrike holdings", "CRWD"},

        {"palo alto networks", "PANW"},

        {"fortinet", "FTNT"},

        {"cloudflare", "NET"},

        {"datadog", "DDOG"},

        {"mongodb", "MDB"},

        {"atlassian", "TEAM"},

        {"workday", "WDAY"},

        {"hubspot", "HUBS"},

        {"zoom video", "ZM"},

        {"docusign", "DOCU"},

        {"twilio", "TWLO"},

        {"okta", "OKTA"},

        {"zscaler", "ZS"},

        {"gitlab", "GTLB"},

        {"applovin", "APP"},

        {"jpmorgan", "JPM"},
        {"jpmorgan chase", "JPM"},

        {"bank of america", "BAC"},

        {"wells fargo", "WFC"},

        {"goldman sachs", "GS"},

        {"morgan stanley", "MS"},

        {"citigroup", "C"},
        {"citi", "C"},

        {"blackrock", "BLK"},

        {"blackstone", "BX"},

        {"charles schwab", "SCHW"},
        {"schwab", "SCHW"},

        {"american express", "AXP"},
        {"amex", "AXP"},

        {"coinbase", "COIN"},

        {"robinhood", "HOOD"},
        {"robinhood markets", "HOOD"},

        {"sofi technologies", "SOFI"},

        {"affirm", "AFRM"},

        {"block inc", "XYZ"},
        {"square", "XYZ"},

        {"eli lilly", "LLY"},
        {"lilly", "LLY"},

        {"johnson & johnson", "JNJ"},
        {"johnson and johnson", "JNJ"},

        {"unitedhealth", "UNH"},
        {"unitedhealth group", "UNH"},

        {"merck", "MRK"},
        {"merck & co", "MRK"},

        {"pfizer", "PFE"},

        {"abbvie", "ABBV"},

        {"amgen", "AMGN"},

        {"moderna", "MRNA"},

        {"gilead sciences", "GILD"},

        {"bristol myers squibb", "BMY"},

        {"thermo fisher scientific", "TMO"},
        {"thermo fisher", "TMO"},

        {"danaher", "DHR"},

        {"cvs health", "CVS"},

        {"novo nordisk", "NVO"},

        {"astrazeneca", "AZN"},

        {"novartis", "NVS"},

        {"sanofi", "SNY"},

        {"glaxosmithkline", "GSK"},

        {"walmart", "WMT"},

        {"costco", "COST"},
        {"costco wholesale", "COST"},

        {"target", "TGT"},

        {"home depot", "HD"},

        {"lowe's", "LOW"},
        {"lowes", "LOW"},

        {"nike", "NKE"},

        {"starbucks", "SBUX"},

        {"mcdonald's", "MCD"},
        {"mcdonalds", "MCD"},

        {"coca-cola", "KO"},
        {"coca cola", "KO"},

        {"pepsico", "PEP"},
        {"pepsi", "PEP"},

        {"procter & gamble", "PG"},
        {"procter and gamble", "PG"},

        {"chipotle", "CMG"},
        {"chipotle mexican grill", "CMG"},

        {"domino's", "DPZ"},
        {"dominos", "DPZ"},

        {"lululemon", "LULU"},

        {"tjx companies", "TJX"},

        {"ross stores", "ROST"},

        {"dollar general", "DG"},

        {"dollar tree", "DLTR"},

        {"best buy", "BBY"},

        {"ford motor", "F"},
        {"ford motor company", "F"},

        {"general motors", "GM"},

        {"toyota motor", "TM"},
        {"toyota", "TM"},

        {"ferrari", "RACE"},

        {"rivian automotive", "RIVN"},
        {"rivian", "RIVN"},

        {"lucid motors", "LCID"},
        {"lucid group", "LCID"},

        {"carvana", "CVNA"},

        {"autozone", "AZO"},

        {"o'reilly automotive", "ORLY"},

        {"boeing", "BA"},

        {"lockheed martin", "LMT"},

        {"raytheon", "RTX"},
        {"raytheon technologies", "RTX"},

        {"northrop grumman", "NOC"},

        {"general dynamics", "GD"},

        {"kratos defense", "KTOS"},

        {"exxon mobil", "XOM"},
        {"exxon", "XOM"},

        {"chevron", "CVX"},

        {"conocophillips", "COP"},

        {"occidental petroleum", "OXY"},

        {"marathon petroleum", "MPC"},

        {"schlumberger", "SLB"},

        {"eog resources", "EOG"},

        {"nextera energy", "NEE"},

        {"duke energy", "DUK"},

        {"vistra", "VST"},

        {"at&t", "T"},
        {"at&t inc", "T"},

        {"verizon", "VZ"},

        {"t-mobile", "TMUS"},
        {"tmobile", "TMUS"},

        {"comcast", "CMCSA"},

        {"disney", "DIS"},
        {"walt disney", "DIS"},

        {"warner bros discovery", "WBD"},
        {"warner bros. discovery", "WBD"},

        {"spotify", "SPOT"},

        {"reddit", "RDDT"},

        {"pinterest", "PINS"},

        {"snapchat", "SNAP"},

        {"shopify", "SHOP"},

        {"ebay", "EBAY"},

        {"etsy", "ETSY"},

        {"mercadolibre", "MELI"},
        {"mercado libre", "MELI"},

        {"uber technologies", "UBER"},
        {"uber", "UBER"},

        {"airbnb", "ABNB"},

        {"doordash", "DASH"},

        {"booking holdings", "BKNG"},
        {"booking.com", "BKNG"},

        {"expedia", "EXPE"},

        {"electronic arts", "EA"},

        {"take-two interactive", "TTWO"},
        {"take two", "TTWO"},

        {"roblox", "RBLX"},

        {"unity software", "U"},

        {"gamestop", "GME"},

        {"draftkings", "DKNG"},

        {"flutter entertainment", "FLUT"},

        {"amc entertainment", "AMC"},

        {"caterpillar", "CAT"},

        {"john deere", "DE"},
        {"deere", "DE"},

        {"honeywell", "HON"},

        {"general electric", "GE"},
        {"ge aerospace", "GE"},

        {"3m company", "MMM"},

        {"union pacific", "UNP"},

        {"united parcel service", "UPS"},

        {"fedex", "FDX"},

        {"waste management", "WM"},

        {"american tower", "AMT"},

        {"crown castle", "CCI"},

        {"prologis", "PLD"},

        {"realty income", "O"},

        {"simon property group", "SPG"},

        {"alibaba", "BABA"},

        {"baidu", "BIDU"},

        {"jd.com", "JD"},

        {"pdd holdings", "PDD"},

        {"tencent", "TCEHY"},

        {"li auto", "LI"},

        {"xpeng", "XPEV"},

        {"byd company", "BYDDY"},

        {"sea limited", "SE"},

        {"grab", "GRAB"},

        {"nu holdings", "NU"},

        {"sony", "SONY"},

        {"nintendo", "NTDOY"},

        {"nokia", "NOK"},

        {"ericsson", "ERIC"},

        {"hsbc", "HSBC"},

        {"ubs", "UBS"},

        {"barclays", "BCS"},

        {"deutsche bank", "DB"},

        {"rio tinto", "RIO"},

        {"bhp", "BHP"},

        {"vale", "VALE"},

        {"shell", "SHEL"},

        {"totalenergies", "TTE"}
    };

    for (const auto& [name, ticker] : companies) {
        if (!ticker.empty() && containsWholePhrase(lowerText, name))
            detectedTickers.insert(ticker);
    }

    std::string result;

    for (const auto& ticker : detectedTickers) {
        if (!result.empty())
            result += ",";

        result += ticker;
    }
    
    if (result.empty()) return "MACRO";
    

    return result;
}

crawlEngine::crawlEngine() {
    profile_registry["finance.yahoo.com"] = std::make_unique<YahooProfile>();
    profile_registry["www.cnbc.com"] = std::make_unique<CnbcProfile>();
    profile_registry["www.prnewswire.com"] = std::make_unique<PRNewswireProfile>();
    profile_registry["www.benzinga.com"] = std::make_unique<BenzingaProfile>();
    profile_registry["www.wsj.com"] = std::make_unique<WsjProfile>();
}

void crawlEngine::run() {
    std::cout << "[SYSTEM] Trading Crawler Engine Started.\n";

    while (true) {
        if (url_queue.empty()) {
            std::cout << "[SYSTEM] Queue empty. Sleeping for 5 minutes...\n";
            std::this_thread::sleep_for(std::chrono::minutes(5));
            continue;
        }

        std::string current_url = url_queue.front();
        url_queue.pop();

        UrlParts url_parts = parseUrl_(current_url);

        auto it = profile_registry.find(url_parts.host);
        if (it == profile_registry.end())
            continue;

        SiteProfile* active_profile = it->second.get();

        std::cout << "\n[CRAWLER] Visiting: " << current_url << "\n";

        int port = (url_parts.scheme == "https") ? 443 : 80;

        if (port == 443)
            sock_ = std::make_unique<TlsSocket_>();
        else
            sock_ = std::make_unique<Tcpsock_>();

        if (sock_->connect(url_parts.host, port) != Tcpsock_::rType::CONNECT_)
            continue;

        auto buildReq = std::make_unique<ReqBuild>(url_parts.host, "close");
        buildReq->SetMethod("GET");
        buildReq->SetPath(url_parts.path.empty() ? "/" : url_parts.path);
        buildReq->addHead("User-Agent", "OptionsScraper/1.0");

        if (sock_->send(buildReq->build_()) == Tcpsock_::rType::CONNECT_) {
            std::string main_payload{};
            char buffer[4096] = {0};
            ssize_t bytes_read = 0;

            while ((bytes_read = sock_->recv(buffer, sizeof(buffer))) > 0) {
                main_payload.append(buffer, bytes_read);
                std::fill(std::begin(buffer), std::end(buffer), '\0');
            }

            if (bytes_read >= 0 && !main_payload.empty()) {
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

                    if (visited_url.insert(location).second)
                        addUrl(location);

                    continue;
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
                        std::cout << "\n================ TICKER DEBUG ================\n";
                        std::cout << "[DEBUG] URL: " << current_url << "\n";
                        std::cout << "[DEBUG] Article length: " << article_text.length() << "\n";
                        std::cout << "[DEBUG] First 1000 chars:\n"
                                  << article_text.substr(0, 1000)
                                  << "\n";

                        std::string tickers = extractCashtags(article_text);

                        std::cout << "[TICKER MATCH] " << tickers << "\n";
                        std::cout << "===============================================\n";

                        std::string wire_payload = tickers + "|" + article_text;

                        auto ai_client = std::make_unique<Tcpsock_>();

                        if (ai_client->connect("127.0.0.1", 58888) == Tcpsock_::rType::CONNECT_) {
                            ai_client->send(wire_payload);
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
                                if (visited_url.insert(x).second)
                                    addUrl(x);
                            }
                        }
                    }
                }
            }
        }
    }
}
