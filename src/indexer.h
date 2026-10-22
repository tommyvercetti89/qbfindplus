// QBFind C++ - concurrent file indexer, streaming persistent cache and live watcher.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "core.h"

namespace qb {

class Indexer {
public:
    Indexer();
    ~Indexer();
    Indexer(const Indexer&) = delete;
    Indexer& operator=(const Indexer&) = delete;

    void setExcludes(std::vector<std::wstring> folded);
    // Loads cache if fresh, else starts a full scan.
    void init();
    // Full rescan (drops current entries).
    void rescan();
    void stop();

    bool scanning() const { return scanning_.load(); }
    int64_t count() const { return count_.load(); }

    // Runs fn with the entries locked for reading (scan/watcher writers wait).
    template <class F>
    void withEntries(F&& fn) const {
        std::shared_lock<std::shared_mutex> lk(mu_);
        fn(entries_);
    }

    // Called (from worker threads) when the entry set changed; must be cheap and thread-safe.
    std::function<void()> onChanged;

private:
    void scanMain(uint64_t id);
    void worker(uint64_t id);
    void pushJob(std::wstring dir);
    void flush(uint64_t id, std::vector<Entry>& batch);
    void walkDir(uint64_t id, const std::wstring& dir, std::vector<Entry>& batch);
    Entry build(const std::wstring& path, int64_t size, int64_t mtime, bool isDir) const;
    int priorityOf(const std::wstring& lowerPath) const;
    bool excluded(const std::wstring& name) const;

    void initPriority();
    void saveCache();
    bool loadCache();

    void startWatchers();
    void stopWatchers();
    void watchLoop(uint64_t gen, std::wstring root, void* handle);
    void applyChanges(uint64_t gen, std::vector<std::wstring>& removed, std::vector<Entry>& added);

    mutable std::shared_mutex mu_;
    std::vector<Entry> entries_;
    std::atomic<int64_t> count_{0};
    std::atomic<bool> scanning_{false};
    std::atomic<uint64_t> scanId_{0};

    std::mutex ctlMu_;  // serialises init/rescan/stop
    std::thread scanThread_;

    // job queue
    std::mutex qMu_;
    std::condition_variable qCv_;
    std::deque<std::wstring> queue_;
    int64_t pending_ = 0;  // queued + in-flight directories

    std::vector<std::wstring> excludes_;
    std::vector<std::wstring> priorityRoots_, userRoots_, noisyRoots_, quickRoots_;

    std::mutex wMu_;
    std::vector<void*> watchHandles_;
    std::vector<std::thread> watchThreads_;
    std::atomic<uint64_t> watchGen_{0};
};

}  // namespace qb
