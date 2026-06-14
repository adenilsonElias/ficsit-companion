#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#include <filesystem>
#endif

/// @brief Polling-based watcher for new/modified .sav files in a directory.
///
/// On each scan (every poll_interval_ms), enumerates *.sav files under
/// watch_dir, and reports any file whose name starts with "<world>_" (or any
/// file if world is empty) and whose last_write_time is newer than the value
/// recorded on the previous scan. Reports are pushed under a mutex; the main
/// thread drains them each frame via TakePending().
///
/// First scan after Start() / Reconfigure() baselines the directory without
/// emitting events, so existing files are not re-imported.
///
/// No-op on Emscripten (browser builds have no filesystem to watch).
class SaveWatcher
{
public:
    SaveWatcher();
    ~SaveWatcher();

    /// @brief Start the watcher thread. Safe to call multiple times.
    void Start();
    /// @brief Stop the watcher thread.
    void Stop();
    /// @brief Update directory / world filter at runtime. Resets baseline.
    /// @param dir Directory to scan
    /// @param world World prefix to filter on (empty = all)
    void Reconfigure(const std::string& dir, const std::string& world);

    /// @brief Move all pending detected file paths into the caller's vector.
    /// @param out The vector to append into (existing contents preserved).
    void TakePending(std::vector<std::string>& out);

    bool IsRunning() const { return running.load(); }

private:
    std::atomic<bool> running{ false };

#if !defined(__EMSCRIPTEN__)
    void Loop();
    void Rebaseline();

    std::thread worker;
    std::atomic<bool> reset_baseline{ true };
    std::condition_variable cv;
    std::mutex cv_mutex;

    std::mutex config_mutex;
    std::string watch_dir;
    std::string world_prefix;

    std::mutex pending_mutex;
    std::vector<std::string> pending;

    std::unordered_map<std::string, std::filesystem::file_time_type> seen;

    static constexpr int poll_interval_ms = 2000;
#endif
};
