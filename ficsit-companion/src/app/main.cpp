#include <chrono>
#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif

#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_opengl3.h>

#include <SDL.h>
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <SDL_opengles2.h>
#else
#include <SDL_opengl.h>
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/html5.h> // emscripten_set_beforeunload_callback
#else
// STB_IMAGE_IMPLEMENTATION is already defined in utils.cpp
#include "third_party/stb_image.h"
#if NDEBUG && defined(_WIN32)
#include <windows.h>
#endif
#endif

#include "app/production_app.hpp"
#include "app/vehicle_map_app.hpp"
#include "app/factory_snapshot_app.hpp"
#include "domain/game_data.hpp"
#include "infra/save_source.hpp"
#include "infra/save_watcher.hpp"
#include "infra/sav_runner.hpp"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>
#include <filesystem>

#if defined(__EMSCRIPTEN__)
// Open the .sav file picker, run the bundled save parser, and write the wrapper
// JSON to /_internal_sav_json. The parser is loaded from /sav_import/web_loader.js
// (preloaded at build time). This is the single web import path, shared by every
// tool (it replaces the modeler's old per-tool "Import .sav" button).
EM_ASYNC_JS(void, waitForSavFileInput, (), {
    var done = false;
    var input = document.createElement("input");
    input.type = "file";
    input.accept = ".sav,.json";
    input.onchange = async function(event) {
        var file = event.target.files[0];
        if (!file) { done = true; return; }
        try {
            if (file.name.toLowerCase().endsWith(".json")) {
                var text = await file.text();
                FS.writeFile("/_internal_sav_json", text);
            } else {
                if (!window.__ficsitParseSav) {
                    var script = document.createElement("script");
                    script.src = "sav_import/web_loader.js";
                    await new Promise(function(res, rej) {
                        script.onload = res;
                        script.onerror = function() { rej(new Error("web_loader.js not bundled")); };
                        document.body.appendChild(script);
                    });
                }
                var buf = await file.arrayBuffer();
                var json = await window.__ficsitParseSav(buf);
                FS.writeFile("/_internal_sav_json", json);
            }
        } catch (e) {
            FS.writeFile("/_internal_sav_json", JSON.stringify({ error: String(e) }));
        }
        done = true;
    };
    input.addEventListener("cancel", (event) => { done = true; });
    input.click();

    while (!done) {
        await new Promise(resolve => setTimeout(resolve, 100));
    }
});
#endif

/// @brief Bundles the three top-level tools and the shared .sav load config.
/// The tab bar in Render() switches between them; each is a self-contained
/// BaseApp. A single LoadSav() call runs the wrapper once and feeds all three.
struct AppHost
{
    ProductionApp production;
    VehicleMapApp vehicle;
    FactorySnapshotApp snapshot;
    int active = 0;

    SaveSource source;
    SaveWatcher watcher;

    std::string load_error;
    std::string load_status;

    // Full paths of .sav files found in source.sav_watch_dir, newest first.
    // Populated by RefreshDiscoveredSaves() and shown in the load dropdown.
    std::vector<std::string> discovered_saves;

    static constexpr const char* kSourceFile = "saved/save_source.json";

    // Rescan the configured save folder for .sav files, newest modified first.
    void RefreshDiscoveredSaves()
    {
        discovered_saves.clear();
#if !defined(__EMSCRIPTEN__)
        std::error_code ec;
        if (source.sav_watch_dir.empty() ||
            !std::filesystem::is_directory(source.sav_watch_dir, ec))
        {
            return;
        }
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
        for (const auto& entry : std::filesystem::directory_iterator(source.sav_watch_dir, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".sav") continue;
            std::error_code tec;
            const auto t = std::filesystem::last_write_time(entry.path(), tec);
            if (tec) continue;
            found.emplace_back(t, entry.path());
        }
        std::sort(found.begin(), found.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& f : found) discovered_saves.push_back(f.second.string());
#endif
    }

    BaseApp* Active()
    {
        switch (active)
        {
            case 1:  return &vehicle;
            case 2:  return &snapshot;
            default: return &production;
        }
    }

    void LoadConfig()
    {
#if !defined(__EMSCRIPTEN__)
        std::ifstream f(kSourceFile, std::ios::binary);
        if (!f) return;
        std::ostringstream ss;
        ss << f.rdbuf();
        source.Deserialize(ss.str());
#endif
    }

    void SaveConfig()
    {
#if !defined(__EMSCRIPTEN__)
        std::error_code ec;
        std::filesystem::create_directories("saved", ec);
        std::ofstream f(kSourceFile, std::ios::binary | std::ios::trunc);
        if (f) f << source.Serialize();
#endif
    }

    void SaveAll()
    {
        production.SaveSession();
        vehicle.SaveSession();
        snapshot.SaveSession();
        SaveConfig();
    }

    // Hand the same wrapper JSON to the save-consuming tools, built with the
    // shared options. The production planner no longer ingests saves (importing
    // a save lives in the Factory Snapshot tool now).
    void DistributeJson(const std::string& json)
    {
        const SavImport::BuildOptions options = source.ToBuildOptions();
        vehicle.LoadFromWrapperJson(json, options);
        snapshot.LoadFromWrapperJson(json, options);
    }

    // Run the wrapper once and hand the same JSON to every tool.
    void LoadSav(const std::string& sav_path)
    {
        load_error.clear();
        load_status.clear();
#if defined(__EMSCRIPTEN__)
        (void)sav_path;
        load_error = "Loading .sav is not supported on the web build yet";
#else
        // A directory can't be parsed as a file (the wrapper would fail with
        // "EISDIR ... read"). When given a folder, resolve to the newest .sav in
        // it — honoring the world filter — exactly like the "Latest" button.
        std::string file_path = sav_path;
        std::error_code ec;
        if (std::filesystem::is_directory(file_path, ec))
        {
            SaveSource in_dir = source;
            in_dir.sav_watch_dir = file_path;
            const std::optional<std::string> latest = in_dir.FindLatestSav();
            if (!latest.has_value())
            {
                load_error = source.sav_watch_world.empty()
                    ? "No .sav files in that folder"
                    : "No .sav files for world '" + source.sav_watch_world + "' in that folder";
                return;
            }
            file_path = latest.value();
        }

        std::string err;
        const std::string json = SavRunner::RunSavWrapper(file_path, source.node_executable_path, err);
        if (!err.empty() || json.empty())
        {
            load_error = err.empty() ? "Parser produced no output" : err;
            return;
        }
        DistributeJson(json);
        source.last_sav_path = file_path;
        load_status = "Loaded " + file_path;
        SaveConfig();
#endif
    }

#if defined(__EMSCRIPTEN__)
    // Web import: pick a .sav/.json via the browser, parse it, feed all tools.
    void LoadSavFromWebPicker()
    {
        load_error.clear();
        load_status.clear();
        waitForSavFileInput();
        if (!std::filesystem::exists("_internal_sav_json"))
        {
            return; // user cancelled the picker
        }
        std::ifstream f("_internal_sav_json", std::ios::in);
        const std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        std::filesystem::remove("_internal_sav_json");
        if (content.empty())
        {
            load_error = "Parser produced no output";
            return;
        }
        DistributeJson(content);
        load_status = "Loaded save";
    }
#endif

    void LoadLatestSav()
    {
        load_error.clear();
        load_status.clear();
#if defined(__EMSCRIPTEN__)
        load_error = "Loading .sav is not supported on the web build yet";
#else
        const std::optional<std::string> latest = source.FindLatestSav();
        if (!latest.has_value())
        {
            load_error = source.sav_watch_dir.empty()
                ? "Save folder is empty"
                : "No matching .sav files in folder";
            return;
        }
        LoadSav(latest.value());
#endif
    }

    void PollWatch()
    {
        std::vector<std::string> pending;
        watcher.TakePending(pending);
        // Only the latest detected save matters; earlier events are superseded.
        if (!pending.empty()) LoadSav(pending.back());
    }
};

bool Render(SDL_Window* window, AppHost* host)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT ||
            (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)
            )
        )
        {
#if defined(__EMSCRIPTEN__)
            emscripten_cancel_main_loop();
#endif
            return false;
        }
    }

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    // If no user interaction, go down to 5 FPS to save some CPU
    const std::chrono::steady_clock::time_point end = start + std::chrono::milliseconds(host->Active()->HasRecentInteraction() ? 16 : 200);

    // Init imgui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);

    ImGui::Begin("Ficsit Companion", NULL,
        ImGuiWindowFlags_NoNavInputs |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse
    );

    // Shared .sav load bar: one load feeds every tool. This is the single import
    // path for the whole app — the modeler and the other tools no longer have
    // their own load controls; they receive the parsed save from here.
    {
#if defined(__EMSCRIPTEN__)
        ImGui::TextUnformatted("Save:");
        ImGui::SameLine();
        if (ImGui::Button("Import .sav..."))
        {
            host->LoadSavFromWebPicker();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", "Import a Satisfactory save file (.sav) or pre-parsed wrapper JSON");
        }
#else
        // Enter the game's save folder, then pick a save from the dropdown.
        ImGui::TextUnformatted("Save folder:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::InputText("##sav_folder", &host->source.sav_watch_dir))
        {
            host->RefreshDiscoveredSaves();
            host->watcher.Reconfigure(host->source.sav_watch_dir, host->source.sav_watch_world);
            host->SaveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", "Folder containing your Satisfactory .sav files\n(e.g. %LOCALAPPDATA%\\FactoryGame\\Saved\\SaveGames\\<id>)");
        }

        ImGui::SameLine();
        const std::string preview = host->source.last_sav_path.empty()
            ? std::string("Select a save...")
            : std::filesystem::path(host->source.last_sav_path).filename().string();
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("##save_select", preview.c_str()))
        {
            host->RefreshDiscoveredSaves(); // catch new autosaves while open
            if (host->discovered_saves.empty())
            {
                ImGui::TextDisabled("No .sav files in this folder");
            }
            for (const std::string& path : host->discovered_saves)
            {
                const std::string name = std::filesystem::path(path).filename().string();
                const bool selected = (path == host->source.last_sav_path);
                if (ImGui::Selectable(name.c_str(), selected))
                {
                    host->LoadSav(path);
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button("Reload"))
        {
            // Re-import the current save (or the newest one if none picked yet).
            if (!host->source.last_sav_path.empty()) host->LoadSav(host->source.last_sav_path);
            else host->LoadLatestSav();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", "Re-import the selected save (or the newest one) after playing more");
        }

        ImGui::SameLine();
        if (ImGui::Checkbox("Auto-watch", &host->source.sav_watch_enabled))
        {
            if (host->source.sav_watch_enabled)
            {
                host->watcher.Reconfigure(host->source.sav_watch_dir, host->source.sav_watch_world);
                host->watcher.Start();
            }
            else
            {
                host->watcher.Stop();
            }
            host->SaveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", "Automatically re-import whenever the game writes a new save in this folder");
        }
#endif
        if (!host->load_error.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", host->load_error.c_str());
        }
        else if (!host->load_status.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", host->load_status.c_str());
        }

        // Second row: shared import options (apply to every tool's next import).
        ImGui::TextUnformatted("Import:");
        ImGui::SameLine();
        int layout_mode = host->source.layout_mode == SavImport::LayoutMode::World ? 1 : 0;
        if (ImGui::RadioButton("Compact", layout_mode == 0))
        {
            host->source.layout_mode = SavImport::LayoutMode::Compact;
            host->SaveConfig();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("World", layout_mode == 1))
        {
            host->source.layout_mode = SavImport::LayoutMode::World;
            host->SaveConfig();
        }
        if (host->source.layout_mode == SavImport::LayoutMode::World)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetTextLineHeightWithSpacing() * 6.0f);
            if (ImGui::InputFloat("World spacing", &host->source.world_spacing_scale, 0.01f, 0.05f, "%.3f"))
            {
                if (host->source.world_spacing_scale <= 0.0f)
                {
                    host->source.world_spacing_scale = SavImport::kPositionScale;
                }
                host->SaveConfig();
            }
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Connect vehicle routes", &host->source.connect_vehicle_routes))
        {
            host->SaveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", "Wire truck/train routes as links between stations (loader \xE2\x86\x92 unloader).");
        }
#if !defined(__EMSCRIPTEN__)
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputText("node path", &host->source.node_executable_path))
        {
            host->SaveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", "Path to the Node.js executable used to run the .sav parser (leave empty to use 'node' from PATH).");
        }
#endif
    }
    host->PollWatch();

    if (ImGui::BeginTabBar("##tools", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("Production Planner")) { host->active = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Vehicle Map")) { host->active = 1; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Factory Snapshot")) { host->active = 2; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    host->Active()->Render();

    ImGui::End();

    // Render ImGui
    ImGui::Render();
    glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x), static_cast<int>(ImGui::GetIO().DisplaySize.y));
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);

#if !defined(__EMSCRIPTEN__)
    std::this_thread::sleep_until(end);
#else
    while (std::chrono::steady_clock::now() < end)
    {
        emscripten_sleep(1);
    }
#endif

    return true;
}

int main(int argc, char* argv[])
{
#if NDEBUG && defined(_WIN32)
    // Hide console on Windows except if asked not to from the command line
    const std::vector<std::string> args(argv, argv + argc);
    HWND console = GetConsoleWindow();
    if (std::find(args.begin(), args.end(), "--show-console") == args.end())
    {
        ShowWindow(console, SW_HIDE);
    }
    else
    {
        ShowWindow(console, SW_SHOWDEFAULT);
    }
#endif

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
    {
        printf("Error %s\n", SDL_GetError());
        return -1;
    }

    // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100
    const char* glsl_version = "#version 100";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(__APPLE__)
    // GL 3.2 Core + GLSL 150
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    // From 2.0.18: Enable native IME.
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_Window* window = SDL_CreateWindow(
        "Ficsit Companion",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1600, 900,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s", SDL_GetError());
        return -1;
    }

#if !defined(__EMSCRIPTEN__)
    // Set window icon
    int width, height, channels;
    unsigned char* img = stbi_load("icon.png", &width, &height, &channels, 4);
    if (img == nullptr)
    {
        printf("Warning, error loading window icon");
    }
    SDL_Surface* icon = nullptr;
    if (img != nullptr)
    {
        icon = SDL_CreateRGBSurfaceWithFormatFrom(img, width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32);
        if (icon == nullptr)
        {
            printf("Warning, error creating window icon");
        }
        else
        {
            SDL_SetWindowIcon(window, icon);
        }
    }
#endif

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    // imgui: setup context
    // ---------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Don't save window layout in ini file
    ImGui::GetIO().IniFilename = nullptr;
    // Enable Keyboard Navigation
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Style
    ImGui::StyleColorsDark();

    // Setup platform/renderer
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    Data::LoadData("satisfactory");

    AppHost host;
    host.LoadConfig();
    host.RefreshDiscoveredSaves();
    host.watcher.Reconfigure(host.source.sav_watch_dir, host.source.sav_watch_world);
    if (host.source.sav_watch_enabled) host.watcher.Start();

#if !defined(__EMSCRIPTEN__)
    while (Render(window, &host))
    {

    }
#else
    // Write to localStorage when quitting
    emscripten_set_beforeunload_callback(static_cast<void*>(&host), [](int event_type, const void* reserved, void* user_data) {
        // Save all tools' sessions to disk
        static_cast<AppHost*>(user_data)->SaveAll();
        // return empty string does not trigger the popup asking if we *really* want to quit
        return "";
    });

    struct WindowHost { SDL_Window* window; AppHost* host; };
    WindowHost arg{ window, &host };
    emscripten_set_main_loop_arg([](void* arg) {
        WindowHost* wh = static_cast<WindowHost*>(arg);
        Render(wh->window, wh->host);
    }, &arg, 0, true);
#endif

    // ImGui cleaning
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

#if !defined(__EMSCRIPTEN__)
    SDL_FreeSurface(icon);
    if (img != nullptr)
    {
        stbi_image_free(img);
    }
#endif
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
