#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <mutex>
#include <chrono>
#include <format>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

std::mutex csv_lock;

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

// 2. Query Ollama on 127.0.0.1:11434
// Update signature to accept BOTH tickers and text
std::string query_ollama(const std::string& tickers, const std::string& text) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "ERROR";

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(11434);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return "CONN_FAILED";
    }

    std::string safe_text = escapeJSON(text);
    if (safe_text.length() > 4000) {
        safe_text = safe_text.substr(0, 4000);
    }

    // The upgraded multi-ticker prompt!
    std::string body = R"({
        "model": "llama3.1",
        "prompt": "Analyze the financial sentiment for these specific companies: )" + tickers + R"(. Reply ONLY with a comma-separated list in the format TICKER:SENTIMENT. Example: AAPL:BULLISH, MSFT:BEARISH. Text: )" + safe_text + R"(",
        "stream": false
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

    size_t key_pos = response.find("\"response\":\"");
    if (key_pos != std::string::npos) {
        key_pos += 12;
        size_t end_pos = response.find("\"", key_pos);
        if (end_pos != std::string::npos) {
            std::string sentiment = response.substr(key_pos, end_pos - key_pos);
            while (!sentiment.empty() && (sentiment.back() == '\n' || sentiment.back() == '\r' || sentiment.back() == '.' || sentiment.back() == ' ')) {
                sentiment.pop_back();
            }
            return sentiment;
        }
    }
    return "UNKNOWN";
}

// 3. Thread Worker: Processes each article independently
void process_sentiment(int client_sock) {
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

    std::string ticker = payload.substr(0, delim_pos);
    std::string article_text = payload.substr(delim_pos + 1);

    std::cout << "[INCOMING] Ticker: " << ticker << " (" << article_text.length() << " chars). Querying AI...\n";

    std::string sentiment = query_ollama(article_text, ticker);

    // Timestamp for the CSV record
    auto const now = std::chrono::system_clock::now();
    std::string timestamp = std::format("{0:%Y-%m-%d %H:%M:%S}", now);

    std::cout << "[RESULT] " << timestamp << " | " << ticker << " -> " << sentiment << "\n";

    // 4. Thread-Safe CSV Logging
    {
        std::lock_guard<std::mutex> lock(csv_lock);
        std::ofstream csv_file("trading_signals.csv", std::ios::app);
        if (csv_file.is_open()) {
            // Format: Timestamp, Ticker, Sentiment
            csv_file << timestamp << "," << ticker << "," << sentiment << "\n";
            csv_file.flush();
        }
    }
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

    std::cout << "=================================================\n";
    std::cout << "[AI SERVER] Listening on 127.0.0.1:58888\n";
    std::cout << "[AI SERVER] Logging to trading_signals.csv\n";
    std::cout << "=================================================\n";

    while (true) {
        int client_sock = accept(server_fd, nullptr, nullptr);
        if (client_sock >= 0) {
            std::thread(process_sentiment, client_sock).detach();
        }
    }

    close(server_fd);
    return 0;
}
