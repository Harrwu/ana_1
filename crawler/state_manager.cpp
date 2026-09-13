#include "state_manager.h"

#include <fstream>
#include <iostream>
#include <filesystem>
#include <system_error>

StateManager::StateManager(std::string path) : path_(std::move(path)) {}

size_t StateManager::load(std::unordered_set<std::string>& out) {
    std::ifstream in(path_);
    if (!in.is_open()) {
        // First run. Not an error worth shouting about.
        std::cout << "[STATE] No " << path_
                  << " found -- starting with empty history.\n";
        return 0;
    }

    // Reserve up front-ish. We can cheaply estimate from file size (avg URL
    // ~60 bytes) instead of doing a full count pass over the file.
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path_, ec);
    if (!ec && bytes > 0) out.reserve(out.size() + bytes / 60 + 1);

    std::string line;
    size_t loaded = 0;
    size_t skipped = 0;

    while (std::getline(in, line)) {
        // CRLF safety: a file touched by an editor on Windows would otherwise
        // load every URL with a trailing '\r' and never match a real one.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line.size() < 8 || line.find("://") == std::string::npos) {
            // Not a URL. Skip the garbage line but keep the rest of the file.
            ++skipped;
            continue;
        }

        if (out.insert(line).second) ++loaded;
    }

    last_saved_size_ = out.size();

    std::cout << "[STATE] Loaded " << loaded << " URLs from " << path_;
    if (skipped) std::cout << " (" << skipped << " malformed lines skipped)";
    std::cout << "\n";

    return loaded;
}

bool StateManager::save(const std::unordered_set<std::string>& visited) {
    // Write to a sibling temp file first, then rename over the target.
    // rename() within a filesystem is atomic, so a crash mid-write leaves the
    // previous good history.txt intact instead of a half-written one.
    const std::string tmp_path = path_ + ".tmp";

    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[STATE] ERROR: cannot open " << tmp_path
                      << " for writing -- history NOT saved.\n";
            return false;
        }

        for (const auto& url : visited) {
            out << url << '\n';
        }
        out.flush();

        // Check the stream actually survived the flush before we clobber the
        // real file with it (e.g. disk full mid-write).
        if (!out.good()) {
            std::cerr << "[STATE] ERROR: write to " << tmp_path
                      << " failed (disk full?) -- history NOT saved.\n";
            out.close();
            std::filesystem::remove(tmp_path);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, path_, ec);
    if (ec) {
        std::cerr << "[STATE] ERROR: rename " << tmp_path << " -> " << path_
                  << " failed: " << ec.message() << "\n";
        std::filesystem::remove(tmp_path);
        return false;
    }

    last_saved_size_ = visited.size();
    ++save_count_;
    return true;
}

bool StateManager::maybeSave(const std::unordered_set<std::string>& visited, bool force) {
    const size_t grown = visited.size() - last_saved_size_;

    if (!force && grown < save_threshold_) return false;

    if (save(visited)) {
        std::cout << "[STATE] Saved " << visited.size() << " URLs to " << path_
                  << " (save #" << save_count_ << ")\n";
        return true;
    }
    return false;
}
