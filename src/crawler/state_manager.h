#pragma once
#include <string>
#include <unordered_set>
#include <cstddef>

// Persists crawler state to disk so a restart doesn't re-crawl and
// re-signal URLs that have already been seen.
//
// Format is deliberately dead simple: one URL per line, plain text. That
// keeps the file greppable and recoverable by hand, and it means a
// half-written file (kill -9 mid-save) loses at most the final line rather
// than corrupting an opaque binary blob.
//
// Concurrency: the caller owns the visited_url set and its mutex. This class
// does no locking of its own -- it is a serializer, not a shared resource.
// crawlEngine holds queue_mutex_ while calling into it.
class StateManager {
private:
    std::string path_;
    size_t save_count_{0};
    size_t last_saved_size_{0};

    // Saves are skipped while the set is growing slowly, so a long crawl
    // doesn't fsync thousands of times. Any save where the set has grown by
    // at least this many entries is forced through.
    size_t save_threshold_{50};

public:
    // path defaults to history.txt in the working directory.
    explicit StateManager(std::string path = "history.txt");

    // Reads the file into `out`. Missing file is NOT an error -- it just means
    // a first run, and we return 0. Returns the number of URLs loaded.
    //
    // Malformed/short lines are skipped rather than aborting the load: a
    // partially-written final line from a previous crash shouldn't cost you
    // the other 10,000 URLs in the file.
    size_t load(std::unordered_set<std::string>& out);

    // Writes every URL in `visited` to disk. Written to a temp file and then
    // renamed over the target, so an interrupted save can never leave a
    // truncated history.txt in its place. Returns true on success.
    bool save(const std::unordered_set<std::string>& visited);

    // Save-if-it-is-time: writes only when the set has grown by
    // save_threshold_ since the last write, or when `force` is set. Intended
    // to be called from the crawl loop; call with force=true when the queue
    // has drained so a clean shutdown/restart resumes from the full set.
    bool maybeSave(const std::unordered_set<std::string>& visited, bool force);

    size_t saveCount() const { return save_count_; }
    const std::string& path() const { return path_; }
};
