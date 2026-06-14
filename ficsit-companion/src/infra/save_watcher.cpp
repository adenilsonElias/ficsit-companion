#include "infra/save_watcher.hpp"

#if !defined(__EMSCRIPTEN__)

#include <chrono>
#include <system_error>

SaveWatcher::SaveWatcher() = default;

SaveWatcher::~SaveWatcher()
{
    Stop();
}

void SaveWatcher::Start()
{
    if (running.load())
    {
        return;
    }
    running.store(true);
    reset_baseline.store(true);
    worker = std::thread(&SaveWatcher::Loop, this);
}

void SaveWatcher::Stop()
{
    if (!running.load())
    {
        return;
    }
    running.store(false);
    cv.notify_all();
    if (worker.joinable())
    {
        worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending.clear();
    }
    seen.clear();
}

void SaveWatcher::Reconfigure(const std::string& dir, const std::string& world)
{
    {
        std::lock_guard<std::mutex> lock(config_mutex);
        watch_dir = dir;
        world_prefix = world;
    }
    reset_baseline.store(true);
    cv.notify_all();
}

void SaveWatcher::TakePending(std::vector<std::string>& out)
{
    std::lock_guard<std::mutex> lock(pending_mutex);
    if (pending.empty())
    {
        return;
    }
    out.insert(out.end(), pending.begin(), pending.end());
    pending.clear();
}

void SaveWatcher::Rebaseline()
{
    seen.clear();
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(config_mutex);
        dir = watch_dir;
    }
    if (dir.empty())
    {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
    {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".sav") continue;
        std::error_code time_ec;
        auto t = std::filesystem::last_write_time(entry.path(), time_ec);
        if (time_ec) continue;
        seen[entry.path().string()] = t;
    }
}

void SaveWatcher::Loop()
{
    while (running.load())
    {
        if (reset_baseline.exchange(false))
        {
            Rebaseline();
        }

        std::string dir;
        std::string prefix;
        {
            std::lock_guard<std::mutex> lock(config_mutex);
            dir = watch_dir;
            prefix = world_prefix;
        }

        if (!dir.empty())
        {
            std::error_code ec;
            if (std::filesystem::is_directory(dir, ec))
            {
                std::vector<std::string> new_paths;
                for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
                {
                    if (ec) break;
                    if (!entry.is_regular_file()) continue;
                    if (entry.path().extension() != ".sav") continue;
                    const std::string filename = entry.path().filename().string();
                    if (!prefix.empty())
                    {
                        const std::string needle = prefix + "_";
                        if (filename.size() < needle.size() ||
                            filename.compare(0, needle.size(), needle) != 0)
                        {
                            continue;
                        }
                    }
                    std::error_code time_ec;
                    auto t = std::filesystem::last_write_time(entry.path(), time_ec);
                    if (time_ec) continue;
                    const std::string path_str = entry.path().string();
                    auto it = seen.find(path_str);
                    if (it == seen.end() || it->second != t)
                    {
                        seen[path_str] = t;
                        new_paths.push_back(path_str);
                    }
                }
                if (!new_paths.empty())
                {
                    std::lock_guard<std::mutex> lock(pending_mutex);
                    for (auto& p : new_paths)
                    {
                        pending.push_back(std::move(p));
                    }
                }
            }
        }

        std::unique_lock<std::mutex> lock(cv_mutex);
        cv.wait_for(lock, std::chrono::milliseconds(poll_interval_ms),
            [this]() { return !running.load() || reset_baseline.load(); });
    }
}

#else // __EMSCRIPTEN__

SaveWatcher::SaveWatcher() = default;
SaveWatcher::~SaveWatcher() = default;
void SaveWatcher::Start() {}
void SaveWatcher::Stop() {}
void SaveWatcher::Reconfigure(const std::string&, const std::string&) {}
void SaveWatcher::TakePending(std::vector<std::string>&) {}

#endif
