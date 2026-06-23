#pragma once
#include <map>
#include <optional>
#include <string>

/// @brief Abstraction over text file persistence so logic is testable and the
/// desktop/web (localStorage) split lives behind one seam.
class IFileStore
{
public:
    virtual ~IFileStore() = default;
    virtual std::optional<std::string> Load(const std::string& path) const = 0;
    virtual void Save(const std::string& path, const std::string& content) = 0;
    virtual void Remove(const std::string& path) = 0;
};

class DiskFileStore : public IFileStore
{
public:
    std::optional<std::string> Load(const std::string& path) const override;
    void Save(const std::string& path, const std::string& content) override;
    void Remove(const std::string& path) override;
};

#if defined(__EMSCRIPTEN__)
class WebFileStore : public IFileStore
{
public:
    std::optional<std::string> Load(const std::string& path) const override;
    void Save(const std::string& path, const std::string& content) override;
    void Remove(const std::string& path) override;
};
#endif

class InMemoryFileStore : public IFileStore
{
public:
    std::optional<std::string> Load(const std::string& path) const override;
    void Save(const std::string& path, const std::string& content) override;
    void Remove(const std::string& path) override;
    std::map<std::string, std::string> files;
};
