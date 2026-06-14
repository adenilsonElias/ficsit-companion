#include "infra/file_store.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

std::optional<std::string> DiskFileStore::Load(const std::string& path) const
{
    // Load file if it exists
    if (std::filesystem::exists(path))
    {
        std::ifstream f(path, std::ios::in);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return std::nullopt;
}

void DiskFileStore::Save(const std::string& path, const std::string& content)
{
    std::ofstream f(path, std::ios::out);
    f << content;
    f.close();
}

void DiskFileStore::Remove(const std::string& path)
{
    if (std::filesystem::exists(path))
    {
        std::filesystem::remove(path);
    }
}

#if defined(__EMSCRIPTEN__)
std::optional<std::string> WebFileStore::Load(const std::string& path) const
{
    // Read from localStorage
    char* content_raw = static_cast<char*>(EM_ASM_PTR({
        var str = localStorage.getItem(UTF8ToString($0)) || "";
        var length = lengthBytesUTF8(str) + 1;
        var str_wasm = _malloc(length);
        stringToUTF8(str, str_wasm, length);
        return str_wasm;
    }, path.c_str()));
    const std::string content(content_raw);
    free(static_cast<void*>(content_raw));
    return content.empty() ? std::nullopt : std::optional<std::string>(content);
}

void WebFileStore::Save(const std::string& path, const std::string& content)
{
    EM_ASM({ localStorage.setItem(UTF8ToString($0), UTF8ToString($1)); }, path.c_str(), content.c_str());
}

void WebFileStore::Remove(const std::string& path)
{
    EM_ASM({
        localStorage.removeItem(UTF8ToString($0));
    }, path.c_str());
}
#endif

std::optional<std::string> InMemoryFileStore::Load(const std::string& path) const
{
    auto it = files.find(path);
    if (it == files.end()) return std::nullopt;
    return it->second;
}

void InMemoryFileStore::Save(const std::string& path, const std::string& content)
{
    files[path] = content;
}

void InMemoryFileStore::Remove(const std::string& path)
{
    files.erase(path);
}
