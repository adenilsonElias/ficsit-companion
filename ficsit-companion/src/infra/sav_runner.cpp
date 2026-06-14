#include "infra/sav_runner.hpp"

#if !defined(__EMSCRIPTEN__)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
#endif

namespace SavRunner
{
#if !defined(__EMSCRIPTEN__)
    static std::string QuoteArgForShell(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('"');
        for (char c : s)
        {
            if (c == '"' || c == '\\')
            {
                out.push_back('\\');
            }
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    }
#endif

    std::string RunSavWrapper(const std::string& sav_path,
                              const std::string& node_exe,
                              std::string& err)
    {
#if defined(__EMSCRIPTEN__)
        (void)sav_path;
        (void)node_exe;
        err = "Desktop parser is not available on the web build";
        return std::string();
#else
        const std::string node = node_exe.empty() ? std::string("node") : node_exe;

        // wrapper.js is shipped next to the executable (post-build copy step copies
        // the tools/ directory into the runtime location, alongside assets/).
        const std::filesystem::path wrapper_path = std::filesystem::path("tools") / "sav_import" / "wrapper.js";
        if (!std::filesystem::exists(wrapper_path))
        {
            err = "wrapper.js not found at " + wrapper_path.string() + " (did the post-build copy step run?)";
            return std::string();
        }

        // Capture stderr separately into a tempfile so wrapper.js error messages
        // (e.g. "[sav-import] save file not found", parser exceptions) surface to
        // the user instead of being silently discarded.
        std::error_code stderr_ec;
        const std::filesystem::path stderr_path =
            std::filesystem::temp_directory_path(stderr_ec) /
            ("ficsit-companion-sav-import-" + std::to_string(
#if defined(_WIN32)
                static_cast<unsigned long long>(_getpid())
#else
                static_cast<unsigned long long>(getpid())
#endif
            ) + ".err");

        std::string cmd =
            QuoteArgForShell(node) + " " +
            QuoteArgForShell(wrapper_path.string()) + " " +
            QuoteArgForShell(sav_path) + " 2>" +
            QuoteArgForShell(stderr_path.string());

#if defined(_WIN32)
        // _popen on Windows wraps the command with cmd.exe /S /C "<cmd>". When the
        // outer command itself begins with a quote, cmd.exe strips the outer pair
        // of quotes, which corrupts our carefully quoted argv. Wrap the whole
        // thing in an extra pair so the stripping leaves our intended command.
        cmd = "\"" + cmd + "\"";
        FILE* pipe = _popen(cmd.c_str(), "rb");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        if (pipe == nullptr)
        {
            err = "Failed to spawn parser process (is Node.js installed?)";
            return std::string();
        }

        std::ostringstream out;
        char buffer[4096];
        while (size_t n = std::fread(buffer, 1, sizeof(buffer), pipe))
        {
            out.write(buffer, static_cast<std::streamsize>(n));
        }
#if defined(_WIN32)
        const int exit_code = _pclose(pipe);
#else
        const int exit_code = pclose(pipe);
#endif

        std::string stderr_text;
        {
            std::ifstream f(stderr_path, std::ios::binary);
            if (f)
            {
                std::ostringstream ss;
                ss << f.rdbuf();
                stderr_text = ss.str();
            }
        }
        std::error_code rm_ec;
        std::filesystem::remove(stderr_path, rm_ec);

        if (exit_code != 0)
        {
            // Trim trailing whitespace/newlines from the captured stderr.
            while (!stderr_text.empty() &&
                   (stderr_text.back() == '\n' || stderr_text.back() == '\r' ||
                    stderr_text.back() == ' ' || stderr_text.back() == '\t'))
            {
                stderr_text.pop_back();
            }
            if (stderr_text.empty())
            {
                err = "Parser exited with code " + std::to_string(exit_code);
            }
            else
            {
                err = "Parser exited with code " + std::to_string(exit_code) +
                      ": " + stderr_text;
            }
            return std::string();
        }
        return out.str();
#endif
    }
}
