#include "domain/building.hpp"
#include "domain/fractional_number.hpp"
#include "domain/game_data.hpp"
#include "domain/graph_item_resolve.hpp"
#include "domain/json.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/node_data_resolver.hpp"
#include "domain/pin.hpp"
#include "app/production_app.hpp"
#include "domain/rate_solver.hpp"
#include "domain/recipe.hpp"
#include "domain/vehicle_route.hpp"
#include "infra/sav_import.hpp"
#include "infra/sav_import_service.hpp"
#include "infra/sav_runner.hpp"
#include "app/utils.hpp"

#if !defined(__EMSCRIPTEN__)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
#endif
#include <cctype>

// For InputText with std::string
#include <misc/cpp/imgui_stdlib.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <queue>
#include <stdexcept>
#include <unordered_set>

#define WITH_SPOILERS_OPTIONS 0
#define DEBUG_PROPAGATION 0

static const ImVec4 lock_purple = ImVec4(0.32f, 0.16f, 0.35f, 0.54f);

#if DEBUG_PROPAGATION
static std::unordered_set<const Pin*> graph_update_pins;
static std::unordered_set<const Pin*> graph_update_multi_pins_constrained;
#endif

ProductionApp::ProductionApp()
    : editor_backend(std::make_unique<NodeEditorBackend>())
    , graph(*editor_backend)
{
#if defined(__EMSCRIPTEN__)
    file_store = std::make_unique<WebFileStore>();
#else
    file_store = std::make_unique<DiskFileStore>();
#endif

    last_time_saved_session = 0.0;

    config.SettingsFile = nullptr;
    config.EnableSmoothZoom = true;

    context = ax::NodeEditor::CreateEditor(&config);

    popup_opened = false;
    new_node_pin = nullptr;

    recipe_filter = "";

    somersloop_texture_id = LoadTextureFromFile("icons/Wat_1_64.png");

    error_time = 0.0f;

    last_clicked_recipe = "";
    next_clicked_recipe = 0;
    last_clicked_item = "";
    next_clicked_item = 0;
    next_clicked_somersloop = 0;

    session_serializer = std::make_unique<SessionSerializer>(graph, *editor_backend, SAVE_VERSION);
    settings_store = std::make_unique<SettingsStore>(*file_store, std::string(settings_file));
    {
        std::vector<const Recipe*> alts;
        for (const auto& r : Data::Recipes()) alts.push_back(r.get());
        settings_store->Load(settings, alts);
    }

    save_watcher.Reconfigure(settings.sav_watch_dir, settings.sav_watch_world);
    RefreshDiscoveredWorlds();
    if (settings.sav_watch_enabled)
    {
        save_watcher.Start();
    }
}

ProductionApp::~ProductionApp()
{
    ax::NodeEditor::DestroyEditor(context);

    // Save current state
    // Destructor is not called in emscripten, we're using emscripten_set_beforeunload_callback in main.cpp instead
#if !defined(__EMSCRIPTEN__)
    SaveSession();
#endif
}


/******************************************************\
*             Non render related functions             *
\******************************************************/
void ProductionApp::SaveSession()
{
    file_store->Save(session_file.data(), Serialize());
}

void ProductionApp::LoadSession()
{
    // Load session file if it exists
    const std::optional<std::string> content = file_store->Load(session_file.data());
    if (!content.has_value())
    {
        return;
    }
    Deserialize(content.value());
}


std::string ProductionApp::Serialize() const
{
    return session_serializer->Serialize();
}

void ProductionApp::Deserialize(const std::string& s)
{
    session_serializer->Deserialize(s);
}

unsigned long long int ProductionApp::GetNextId()
{
    return graph.GetNextId();
}

Pin* ProductionApp::FindPin(ax::NodeEditor::PinId id) const
{
    return graph.FindPin(id);
}

// Small node-header button that re-derives a splitter/merger's item from its
// neighbours (see ResolveOrganizerItem) and pushes it onto every pin. Useful
// after a .sav import, where an organizer can be left with a stale or missing
// item. Does nothing when no connected machine resolves to a concrete item.
static void RenderOrganizerRecalcButton(OrganizerNode* node)
{
    if (node == nullptr) return;
    if (ImGui::SmallButton("R##recalc_item"))
    {
        if (const Item* it = ResolveOrganizerItem(node))
        {
            node->ChangeItem(it);
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Recalculate item from connected machines");
    }
}

// Companion button placed to the left of the single-node recalc: re-derives the
// item and propagates it through the entire connected chain of splitters /
// mergers / storages (see RecalculateOrganizerItemChain).
static void RenderOrganizerRecalcChainButton(OrganizerNode* node)
{
    if (node == nullptr) return;
    if (ImGui::SmallButton("RC##recalc_item_chain"))
    {
        RecalculateOrganizerItemChain(node);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Recalculate item and propagate through the connected chain");
    }
}

// Render a vehicle station's plug pin: a colored connection point held outside
// ins/outs. Amber, to stand out from the white belt pins. Filled when the plug
// has at least one route link. The label sits inward of the dot on each side.
static void RenderVehiclePlug(Pin* plug)
{
    if (plug == nullptr) return;
    const ImColor plug_color(255, 170, 0);
    const bool is_output = plug->direction == ax::NodeEditor::PinKind::Output;
    const bool connected = plug->node != nullptr &&
        !static_cast<VehicleStationNode*>(plug->node)->route_links.empty();

    auto draw_dot = [&]() {
        const float radius = 0.25f * ImGui::GetTextLineHeightWithSpacing();
        const ImVec2 size(2.0f * radius, 2.0f * radius);
        if (ImGui::IsRectVisible(size))
        {
            const ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            const ImVec2 center(cursor_pos.x + radius, cursor_pos.y + radius);
            if (connected)
            {
                draw_list->AddCircleFilled(center, radius, plug_color);
            }
            else
            {
                draw_list->AddCircle(center, radius, plug_color);
            }
        }
        ImGui::Dummy(size);
    };

    ax::NodeEditor::BeginPin(plug->id, plug->direction);
    ImGui::BeginHorizontal(plug->id.AsPointer());
    {
        if (is_output)
        {
            ImGui::TextUnformatted("vehicle");
            ImGui::Spring(0.0f);
            draw_dot();
        }
        else
        {
            draw_dot();
            ImGui::Spring(0.0f);
            ImGui::TextUnformatted("vehicle");
        }
    }
    ImGui::EndHorizontal();
    ax::NodeEditor::EndPin();

    // Hovering the plug shows the route pool's per-item supply vs demand.
    if (ImGui::IsItemHovered() && plug->node != nullptr)
    {
        auto* station = static_cast<VehicleStationNode*>(plug->node);
        const std::vector<VehicleStationNode*> pool = VehicleRoute::FindPool(station);
        std::vector<VehicleRoute::ItemBalance> balances = VehicleRoute::SummarizePool(pool);
        ImGui::BeginTooltip();
        ImGui::Text("Route: %d station(s)", static_cast<int>(pool.size()));
        if (balances.empty())
        {
            ImGui::TextUnformatted("(no cargo set)");
        }
        for (auto& b : balances)
        {
            const bool mismatch = !(b.supply == b.demand);
            if (mismatch) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::Text("%s: %s in -> %s out",
                b.item != nullptr ? b.item->name.c_str() : "?",
                b.supply.GetStringFraction().c_str(),
                b.demand.GetStringFraction().c_str());
            if (mismatch) ImGui::PopStyleColor();
        }
        ImGui::EndTooltip();
    }
}

void ProductionApp::CreateLink(Pin* start, Pin* end, const bool trigger_update)
{
    graph.CreateLink(start, end, trigger_update, error_time, ax::NodeEditor::GetStyle().FlowDuration);
}

void ProductionApp::DeleteLink(const ax::NodeEditor::LinkId id)
{
    graph.DeleteLink(id);
}

void ProductionApp::DeleteNode(const ax::NodeEditor::NodeId id)
{
    graph.DeleteNode(id);
}

bool ProductionApp::UpdateNodesRate(const Pin* constraint_pin, const FractionalNumber& constraint_value)
{
    return RateSolver::Solve(nodes, links, constraint_pin, constraint_value, error_time, ax::NodeEditor::GetStyle().FlowDuration);
}

void ProductionApp::NudgeNodes()
{
    // Don't nudge item if the add node popup is open (arrow keys are used for navigation in the dropdown menu)
    if (ImGui::IsPopupOpen(add_node_popup_id.data()))
    {
        return;
    }

    ImVec2 nudge{
        -1.0f * ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) + 1.0f * ImGui::IsKeyPressed(ImGuiKey_RightArrow, false),
        -1.0f * ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) + 1.0f * ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)
    };

    if (nudge.x == 0.0f && nudge.y == 0.0f)
    {
        return;
    }

    for (const auto& n : nodes)
    {
        if (ax::NodeEditor::IsNodeSelected(n->id))
        {
            ax::NodeEditor::SetNodePosition(n->id, { n->pos.x + nudge.x, n->pos.y + nudge.y });
        }
    }
}

void ProductionApp::PullNodesPosition()
{
    for (auto& n : nodes)
    {
        n->pos = ax::NodeEditor::GetNodePosition(n->id);
    }
}

void ProductionApp::GroupSelectedNodes()
{
    std::vector<std::unique_ptr<Node>> selected_nodes;
    std::vector<std::unique_ptr<Link>> kept_links;

    auto process_link = [&](const Link* link) {
        if (link == nullptr)
        {
            return;
        }
        for (auto it = links.begin(); it != links.end(); ++it)
        {
            if (it->get() == link)
            {
                // If this link is between two group nodes, move it in the group
                if (ax::NodeEditor::IsNodeSelected((*it)->start->node->id) &&
                    ax::NodeEditor::IsNodeSelected((*it)->end->node->id))
                {
                    ax::NodeEditor::DeleteLink((*it)->id);
                    kept_links.emplace_back(std::move(*it));
                    links.erase(it);
                }
                // Else just remove it
                // TODO: can we keep the connections between the group and external nodes?
                // it should be doable if it's a "simple" link, but what about multiple links
                // to different pins with the same items ?
                else
                {
                    DeleteLink((*it)->id);
                }
                return;
            }
        }
    };

    for (auto it = nodes.begin(); it != nodes.end();)
    {
        if (ax::NodeEditor::IsNodeSelected((*it)->id))
        {
            for (const auto& p : (*it)->ins)
            {
                process_link(p->link);
            }
            for (const auto& p : (*it)->outs)
            {
                process_link(p->link);
            }
            ax::NodeEditor::DeleteNode((*it)->id);
            selected_nodes.emplace_back(std::move(*it));
            it = nodes.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (selected_nodes.empty())
    {
        return;
    }

    // Get the top left corner of this group
    ImVec2 min_pos(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());

    for (const auto& n : selected_nodes)
    {
        min_pos.x = std::min(min_pos.x, n->pos.x);
        min_pos.y = std::min(min_pos.y, n->pos.y);
    }

    // Offset all nodes in the group to store relative positions
    for (auto& n : selected_nodes)
    {
        n->pos = ImVec2(
            n->pos.x - min_pos.x,
            n->pos.y - min_pos.y
        );
    }

    nodes.emplace_back(std::make_unique<GroupNode>(GetNextId(), std::bind(&ProductionApp::GetNextId, this), std::move(selected_nodes), std::move(kept_links)));
    nodes.back()->pos = min_pos;
    ax::NodeEditor::SetNodePosition(nodes.back()->id, min_pos);
    ax::NodeEditor::SelectNode(nodes.back()->id, false);
}

void ProductionApp::UngroupSelectedNode()
{
    // Get the selected node
    GroupNode* group_node = nullptr;
    for (const auto& n : nodes)
    {
        if (ax::NodeEditor::IsNodeSelected(n->id))
        {
            // Should not happen because we check before calling the function but just in case...
            if (!n->IsGroup())
            {
                continue;
            }
            group_node = static_cast<GroupNode*>(n.get());
        }
    }
    // Should not happen because we check before calling the function but just in case...
    if (group_node == nullptr)
    {
        return;
    }

    const size_t num_node_before_add = nodes.size();
    const Json::Value serialized = group_node->Serialize();

    // Recreate the nodes of the group in the main graph. Map each serialized
    // index to its slot in `nodes` (or -1 on deserialize failure) so a single
    // failing node doesn't desync every subsequent link lookup.
    std::vector<int> serialized_index_to_main_index;
    serialized_index_to_main_index.reserve(serialized["nodes"].get_array().size());
    int deserialize_failures = 0;
    for (auto& n : serialized["nodes"].get_array())
    {
        try
        {
            nodes.emplace_back(Node::Deserialize(GetNextId(), std::bind(&ProductionApp::GetNextId, this), n, GameDataResolver()));
            serialized_index_to_main_index.push_back(static_cast<int>(nodes.size() - 1));
        }
        catch (const std::exception&)
        {
            serialized_index_to_main_index.push_back(-1);
            deserialize_failures += 1;
            continue;
        }
        // For nodes without a rate stored, remultiply the current rates by the node global rate
        if (nodes.back()->IsOrganizer() || nodes.back()->IsSink() || nodes.back()->IsLogistics())
        {
            for (auto& p : nodes.back()->ins)
            {
                p->current_rate *= group_node->current_rate;
            }
            for (auto& p : nodes.back()->outs)
            {
                p->current_rate *= group_node->current_rate;
            }
        }

        // Offset the new node with the group node position
        nodes.back()->pos.x += group_node->pos.x;
        nodes.back()->pos.y += group_node->pos.y;
        ax::NodeEditor::SetNodePosition(nodes.back()->id, nodes.back()->pos);
        ax::NodeEditor::SelectNode(nodes.back()->id, true);
    }

    // Recreate the internal links via the index map so out-of-range indices
    // (from a failed deserialize) are silently skipped instead of crashing.
    int links_created = 0;
    int links_skipped = 0;
    for (const auto& l : serialized["links"].get_array())
    {
        const int s_idx = l["start"]["node"].get<int>();
        const int e_idx = l["end"]["node"].get<int>();
        if (s_idx < 0 || s_idx >= static_cast<int>(serialized_index_to_main_index.size()) ||
            e_idx < 0 || e_idx >= static_cast<int>(serialized_index_to_main_index.size()) ||
            serialized_index_to_main_index[s_idx] == -1 ||
            serialized_index_to_main_index[e_idx] == -1)
        {
            links_skipped += 1;
            continue;
        }
        Node* start_node = nodes[serialized_index_to_main_index[s_idx]].get();
        Node* end_node = nodes[serialized_index_to_main_index[e_idx]].get();
        const int start_pin = l["start"]["pin"].get<int>();
        const int end_pin = l["end"]["pin"].get<int>();
        if (start_pin < 0 || start_pin >= static_cast<int>(start_node->outs.size()) ||
            end_pin < 0 || end_pin >= static_cast<int>(end_node->ins.size()))
        {
            links_skipped += 1;
            continue;
        }
        CreateLink(start_node->outs[start_pin].get(), end_node->ins[end_pin].get(), false);
        links_created += 1;
    }

    if (deserialize_failures > 0 || links_skipped > 0)
    {
        fprintf(stderr, "[ungroup] %d node(s) failed to deserialize, %d link(s) skipped, %d link(s) created\n",
            deserialize_failures, links_skipped, links_created);
    }

    // Post-pass: now that every internal link is reconstructed, walk through
    // each freshly-deserialized extractor and let it inherit its resource
    // from whatever lies downstream. The first link pass can't do this for
    // chains like Miner -> Splitter -> Smelter because the splitter's
    // outbound link wasn't created yet when we processed the miner->splitter
    // link. With every link in place the chain walker can succeed.
    for (int mapped : serialized_index_to_main_index)
    {
        if (mapped < 0 || mapped >= static_cast<int>(nodes.size())) continue;
        Node* n = nodes[mapped].get();
        if (!n->IsExtractor() || n->outs.empty() || n->outs[0]->link == nullptr) continue;
        ExtractorNode* ex = static_cast<ExtractorNode*>(n);
        if (ex->resource != nullptr) continue;
        if (const Item* it = ResolveItemThroughChain(n->outs[0]->link->end);
            it != nullptr)
        {
            ex->ChangeResource(it, std::bind(&ProductionApp::GetNextId, this));
        }
    }

    // Delete old group node
    DeleteNode(group_node->id);
}

void ProductionApp::DuplicateSelectedNodes()
{
    // Duplicate nodes
    std::vector<std::unique_ptr<Node>> new_nodes;
    std::unordered_map<const Node*, const Node*> original_to_copy;
    for (const auto& n : nodes)
    {
        if (!ax::NodeEditor::IsNodeSelected(n->id))
        {
            continue;
        }

        // Don't add the new nodes directly as we are looping through it
        new_nodes.emplace_back(Node::Deserialize(GetNextId(), std::bind(&ProductionApp::GetNextId, this), n->Serialize(), GameDataResolver()));
        ax::NodeEditor::SetNodePosition(new_nodes.back()->id, ImVec2(n->pos.x + 100.0f, n->pos.y + 100.0f));
        original_to_copy[n.get()] = new_nodes.back().get();
    }
    // Insert new nodes in the graph once the loop is done
    nodes.insert(nodes.end(), std::make_move_iterator(new_nodes.begin()), std::make_move_iterator(new_nodes.end()));

    // Duplicate links between two selected nodes
    std::vector<std::pair<Pin*, Pin*>> new_links;
    for (const auto& l : links)
    {
        if (ax::NodeEditor::IsNodeSelected(l->start->node->id) && ax::NodeEditor::IsNodeSelected(l->end->node->id))
        {
            int index_start = -1;
            for (int i = 0; i < l->start->node->outs.size(); ++i)
            {
                if (l->start->node->outs[i].get() == l->start)
                {
                    index_start = i;
                    break;
                }
            }
            int index_end = -1;
            for (int i = 0; i < l->end->node->ins.size(); ++i)
            {
                if (l->end->node->ins[i].get() == l->end)
                {
                    index_end = i;
                    break;
                }
            }

            // Should always be true but just in case
            if (index_start != -1 && index_end != -1)
            {
                // Don't create link now as we loop through links
                new_links.push_back({ original_to_copy.at(l->start->node)->outs[index_start].get(), original_to_copy.at(l->end->node)->ins[index_end].get() });
            }
        }
    }

    for (const auto& [s, e] : new_links)
    {
        CreateLink(s, e, false);
    }

    // Swap selection between old nodes and new nodes
    for (const auto& [o, c] : original_to_copy)
    {
        ax::NodeEditor::DeselectNode(o->id);
        ax::NodeEditor::SelectNode(c->id, true);
    }
}


/******************************************************\
*              Rendering related functions             *
\******************************************************/
void ProductionApp::RenderImpl()
{
    error_time = std::max(error_time - ImGui::GetIO().DeltaTime, 0.0f);

    ax::NodeEditor::SetCurrentEditor(context);
    ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_Flow, error_time > 0.0f ? ImColor(255, 0, 0) : ImColor(255, 255, 0));
    ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_FlowMarker, error_time > 0.0f ? ImColor(255, 0, 0) : ImColor(255, 255, 0));
    ax::NodeEditor::PushStyleVar(ax::NodeEditor::StyleVar_SelectedNodeBorderWidth, 5.0f);

    if (settings.left_panel_folded)
    {

        ImGui::BeginChild("#left_panel", ImVec2(ImGui::CalcTextSize(">>").x + 2.0f * ImGui::GetStyle().FramePadding.x, 0.0f), false, ImGuiWindowFlags_NoNavInputs);
        if (ImGui::Button(">>"))
        {
            settings.left_panel_folded = false;
            settings_store->Save(settings);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", "Expand left panel");
        }
        ImGui::EndChild();
    }
    else
    {
        ImGui::BeginChild("#left_panel", ImVec2(0.2f * ImGui::GetWindowSize().x, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);
        RenderLeftPanel();
        ImGui::EndChild();
    }

    ImGui::SameLine();

    ax::NodeEditor::Begin("Graph", ImGui::GetContentRegionAvail());

    // First frame
    if (ImGui::IsWindowAppearing())
    {
        // Try to load session
        LoadSession();
        last_time_saved_session = ImGui::GetTime();
    }

    DrainPendingImports();

    if (ImGui::GetTime() - last_time_saved_session > 30.0)
    {
        // We need to update last_time_saved_session here because SaveSession needs
        // to be callable even without a valid ImGui context
        SaveSession();
        last_time_saved_session = ImGui::GetTime();
    }

    DeleteNodesLinks();
    DragLink();

    NudgeNodes();
    RenderNodes();
    RenderLinks();

    AddNewNode();
    CustomKeyControl();

    ax::NodeEditor::End();
    ax::NodeEditor::PopStyleVar();
    ax::NodeEditor::PopStyleColor();
    ax::NodeEditor::PopStyleColor();
    RenderResourceFlowWindow();

    // We manually copy the pos of each node at each frame to make
    // sure they are available for serialization
    PullNodesPosition();

    ax::NodeEditor::SetCurrentEditor(nullptr);

    // Render the tooltips after we exited the NodeEditor context so
    // we are in the main window coordinates system instead of the
    // one from the graph view
    RenderTooltips();
}

#if defined(__EMSCRIPTEN__)
EM_ASYNC_JS(void, waitForFileInput, (), {
    var fileReady = false;
    var input = document.createElement("input");
    input.type = "file";
    input.accept = ".fcs";
    input.onchange = async function(event) {
        var file = event.target.files[0];
        if (file) {
            var reader = new FileReader();
            reader.onload = function() {
                var data = new Uint8Array(reader.result);
                FS.writeFile("/_internal_load_file", data);
                fileReady = true;
            };
            reader.readAsArrayBuffer(file);
        }
        else { fileReady = true; }
    };
    input.addEventListener("cancel", (event) => { fileReady = true; });
    input.click();

    // Wait until the file is ready
    while (!fileReady) {
        await new Promise(resolve => setTimeout(resolve, 100));
    }
});

// Open the .sav file picker, run the bundled save parser, and write the
// wrapper JSON to /_internal_sav_json. The parser is loaded from
// /sav_import/web_loader.js (preloaded at build time). If the loader is not
// available the user is told to run tools/sav_import/wrapper.js manually.
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
                // Pre-parsed wrapper output — pass straight through.
                var text = await file.text();
                FS.writeFile("/_internal_sav_json", text);
            } else {
                // Lazy-load the bundled parser. We expect web_loader.js to
                // attach a function `window.__ficsitParseSav(arrayBuffer)`
                // that returns a Promise<string> of wrapper JSON.
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

void ProductionApp::RenderLeftPanel()
{
    ImGui::BeginDisabled(ImGui::IsPopupOpen("##ControlsPopup"));
    if (ImGui::Button("Show controls list"))
    {
        ImGui::OpenPopup("##ControlsPopup");
    }
    ImGui::EndDisabled();
    if (ImGui::Button(settings.show_resource_flow ? "Hide Resource Flow" : "Show Resource Flow"))
    {
        settings.show_resource_flow = !settings.show_resource_flow;
        settings_store->Save(settings);
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), 0, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopup("##ControlsPopup", ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_ChildWindow))
    {
        RenderControlsPopup();
    }

    // If web version, add an option to load from disk and download
#if defined(__EMSCRIPTEN__)
    ImGui::SameLine();
    if (ImGui::Button("Export"))
    {
        const std::string path = "production_chain.fcs";
        const std::string content = Serialize();
        EM_ASM({
            var filename = UTF8ToString($0);
            var content = UTF8ToString($1);
            var blob = new Blob([content], { type: "text/plain" });
            var link = document.createElement("a");
            link.href = URL.createObjectURL(blob);
            link.download = filename;
            document.body.appendChild(link);
            link.click();
            document.body.removeChild(link);
        }, path.c_str(), content.c_str());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", "Export current production chain to disk");
    }
    ImGui::SameLine();
    if (ImGui::Button("Import"))
    {
        waitForFileInput();
        if (std::filesystem::exists("_internal_load_file"))
        {
            std::ifstream f("_internal_load_file", std::ios::in);
            const std::string content = std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            f.close();
            Deserialize(content);
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", "Import a production chain from disk");
    }
    ImGui::SameLine();
    if (ImGui::Button("Import .sav"))
    {
        waitForSavFileInput();
        if (std::filesystem::exists("_internal_sav_json"))
        {
            std::ifstream f("_internal_sav_json", std::ios::in);
            const std::string content = std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            f.close();
            std::filesystem::remove("_internal_sav_json");
            if (!content.empty())
            {
                ImportSavFromJson(content);
            }
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", "Import a Satisfactory save file (.sav) or pre-parsed wrapper JSON");
    }
#endif
    ImGui::SameLine();
    const float fold_button_size = ImGui::CalcTextSize("<<").x + 2.0f * ImGui::GetStyle().FramePadding.x;
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > fold_button_size)
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - fold_button_size);
    }
    if (ImGui::Button("<<"))
    {
        settings.left_panel_folded = true;
        settings_store->Save(settings);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", "Fold left panel");
    }

    RenderSavImportSection();

    const float save_load_buttons_width = ImGui::CalcTextSize("Save").x + ImGui::CalcTextSize("Load").x + ImGui::GetStyle().FramePadding.x * 4;
    const float input_text_width = ImGui::GetContentRegionAvail().x - save_load_buttons_width - ImGui::GetStyle().ItemSpacing.x * 2;

    ImGui::PushItemWidth(input_text_width);
    if (ImGui::InputTextWithHint("##save_text", "Name to save/load...", &save_name))
    {
        for (auto& [filename, match] : file_suggestions)
        {
            match = filename.find(save_name);
        }
    }
    ImGui::PopItemWidth();

    // Autocomplete with save present locally
    const bool save_name_active = ImGui::IsItemActive();
    if (ImGui::IsItemActivated())
    {
        ImGui::OpenPopup("##AutocompletePopup");
    }
    {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y));
        ImGui::SetNextWindowSizeConstraints({ ImGui::GetItemRectSize().x , 0.0f }, { ImGui::GetItemRectSize().x, ImGui::GetTextLineHeightWithSpacing() * 10.0f });
        if (ImGui::BeginPopup("##AutocompletePopup", ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_ChildWindow))
        {
            if (ImGui::IsWindowAppearing())
            {
                file_suggestions.clear();
                // Retrieve existing saved files
#if !defined(__EMSCRIPTEN__)
                if (!std::filesystem::is_directory(save_folder))
                {
                    std::filesystem::create_directory(save_folder);
                }
                for (const auto& f : std::filesystem::recursive_directory_iterator(save_folder))
                {
                    if (f.is_regular_file())
                    {
                        std::string path = f.path().string();
                        path = path.substr(0, path.size() - 4);
                        path = path.substr(save_folder.size() + 1);
                        file_suggestions.emplace_back(path, path.find(save_name));
                    }
                }
#else
                int names_size = 0;
                // Get all existing keys in localStorage starting with save_folder
                // return a pointer to string array and save array length in names_size
                // TODO/CHECK: are pointers always guaranteed to be 32 bits?
                char** names = static_cast<char**>(EM_ASM_PTR({
                    const keys = Object.keys(localStorage).filter(k => k.startsWith(UTF8ToString($0)));
                    var length = keys.length;
                    var buffer = _malloc(length * 4);
                    for (var i = 0; i < length; ++i)
                    {
                        var key = keys[i];
                        var key_length = lengthBytesUTF8(key) + 1;
                        var key_ptr = _malloc(key_length + 1);
                        stringToUTF8(key, key_ptr, key_length);
                        setValue(buffer + i * 4, key_ptr, "i32");
                    }
                    setValue($1, length, "i32");
                    return buffer;
                }, save_folder.data(), &names_size));

                for (int i = 0; i < names_size; ++i)
                {
                    std::string filename = std::string(names[i]).substr(save_folder.size() + 1);
                    filename = filename.substr(0, filename.size() - 4);
                    file_suggestions.emplace_back(filename, filename.find(save_name));
                    free(static_cast<void*>(names[i]));
                }
                free(static_cast<void*>(names));
#endif
            }

            if (file_suggestions.size() == 0)
            {
                ImGui::CloseCurrentPopup();
            }

            std::stable_sort(file_suggestions.begin(), file_suggestions.end(), [](const std::pair<std::string, size_t>& a, const std::pair<std::string, size_t>& b) { return a.second < b.second; });

            std::vector<std::string> removed;
            for (const auto& s : file_suggestions)
            {
                ImGui::PushID(s.first.c_str());
                if (ImGui::Button("X"))
                {
                    removed.push_back(s.first);
                }
                ImGui::PopID();
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", "Delete this file");
                }
                ImGui::SameLine();
                if (ImGui::Selectable(s.first.c_str()))
                {
                    save_name = s.first;
                    ImGui::CloseCurrentPopup();
                }
                // Add tooltip for long names
                if (ImGui::IsItemHovered() && ImGui::CalcTextSize(s.first.c_str()).x > ImGui::GetWindowWidth())
                {
                    ImGui::SetTooltip("%s", s.first.c_str());
                }
            }

            // Delete files that have been flagged by clicking on the button
            for (const auto& s : removed)
            {
                file_suggestions.erase(std::remove_if(file_suggestions.begin(), file_suggestions.end(),
                    [&](const std::pair<std::string, size_t>& p) {
                        return p.first == s;
                    }), file_suggestions.end());
                file_store->Remove(std::string(save_folder) + "/" + s + ".fcs");
            }

            if (!save_name_active && !ImGui::IsWindowFocused())
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(save_name.empty());
    if (ImGui::Button("Save"))
    {
        // Save current state using provided name
        file_store->Save(std::string(save_folder) + "/" + save_name + ".fcs", Serialize());
        save_name = "";
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", "Save current production chain");
    }
    ImGui::SameLine();

    ImGui::BeginDisabled(file_suggestions.end() == std::find_if(file_suggestions.begin(), file_suggestions.end(), [&](const std::pair<std::string, size_t>& p) { return p.first == save_name; }));
    if (ImGui::Button("Load"))
    {
        const std::optional<std::string> content = file_store->Load(std::string(save_folder) + "/" + save_name + ".fcs");
        if (content.has_value())
        {
            Deserialize(content.value());
        }
        save_name = "";
    }
    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", "Load current production chain");
    }


    // Gather all statistics for the left panel
    std::map<const Item*, FractionalNumber, ItemPtrCompare> inputs;
    std::map<const Item*, FractionalNumber, ItemPtrCompare> outputs;
    std::map<const Item*, FractionalNumber, ItemPtrCompare> intermediates;
    std::map<std::string, FractionalNumber> total_machines;
    FractionalNumber all_machines;
    std::map<std::string, FractionalNumber> built_machines;
    FractionalNumber all_built_machines;
    std::map<std::string, std::map<const Recipe*, FractionalNumber, RecipePtrCompare>> detailed_machines;
    std::map<std::string, std::map<const Item*, FractionalNumber, ItemPtrCompare>> detailed_machine_outputs;
    FractionalNumber total_sink_points;
    std::map<const Item*, FractionalNumber> detailed_sink_points;
    FractionalNumber total_power;
    std::map<const Recipe*, FractionalNumber> detailed_power;
    bool has_variable_power = false;
    FractionalNumber num_somersloop;

    // Gather all craft node stats (ins/outs/machines/power)
    for (const auto& n : nodes)
    {
        if (n->IsCraft())
        {
            for (const auto& p : n->ins)
            {
                inputs[p->item] += p->current_rate;
            }
            for (const auto& p : n->outs)
            {
                outputs[p->item] += p->current_rate;
            }

            const CraftNode* node = static_cast<const CraftNode*>(n.get());
            total_machines[node->recipe->building->name] += node->current_rate;
            all_machines += node->current_rate;
            detailed_machines[node->recipe->building->name][node->recipe] += node->current_rate;
            built_machines[node->recipe->building->name] += node->built ? node->current_rate : 0;
            all_built_machines += node->built ? node->current_rate : 0;
            total_power += settings.power_equal_clocks ? node->same_clock_power : node->last_underclock_power;
            detailed_power[node->recipe] += settings.power_equal_clocks ? node->same_clock_power : node->last_underclock_power;
            has_variable_power |= node->recipe->building->variable_power;
            num_somersloop += node->num_somersloop * static_cast<int>(std::ceil((node->current_rate / FractionalNumber(5, 2)).GetValue()));
        }
        else if (n->IsGroup())
        {
            const GroupNode* node = static_cast<const GroupNode*>(n.get());

            for (const auto& [k, v] : node->inputs)
            {
                inputs[k] += v;
            }
            for (const auto& [k, v] : node->outputs)
            {
                outputs[k] += v;
            }

            total_power += settings.power_equal_clocks ? node->same_clock_power : node->last_underclock_power;
            has_variable_power |= node->variable_power;
            for (const auto& [k, v] : node->total_machines)
            {
                total_machines[k] += v;
                all_machines += v;
            }
            for (const auto& [k, v] : node->built_machines)
            {
                built_machines[k] += v;
                all_built_machines += v;
            }
            for (const auto& [k, v] : node->detailed_machines)
            {
                for (const auto& [k2, v2] : v)
                {
                    detailed_machines[k][k2] += v2;
                }
            }
            for (const auto& [k, v] : node->detailed_machine_outputs)
            {
                for (const auto& [k2, v2] : v)
                {
                    detailed_machine_outputs[k][k2] += v2;
                }
            }
            for (const auto& [k, v] : (settings.power_equal_clocks ? node->detailed_power_same_clock : node->detailed_power_last_underclock))
            {
                detailed_power[k] += v;
            }

            for (const auto& [k, v] : node->detailed_sinked_points)
            {
                total_sink_points += v;
                detailed_sink_points[k] += v;
            }
            num_somersloop += node->num_somersloop;
        }
        else if (n->IsSink())
        {
            for (const auto& p : n->ins)
            {
                if (p->item != nullptr)
                {
                    inputs[p->item] += p->current_rate;
                    total_sink_points += p->current_rate * p->item->sink_value;
                    detailed_sink_points[p->item] += p->current_rate * p->item->sink_value;
                }
            }
        }
        else if (n->IsExtractor())
        {
            const ExtractorNode* node = static_cast<const ExtractorNode*>(n.get());
            const Building* building = node->GetBuilding();
            for (const auto& p : n->outs)
            {
                if (p->item != nullptr)
                {
                    outputs[p->item] += p->current_rate;
                    if (building != nullptr)
                    {
                        detailed_machine_outputs[building->name][p->item] += p->current_rate;
                    }
                }
            }
            if (building != nullptr)
            {
                total_machines[building->name] += node->current_rate;
                all_machines += node->current_rate;
                total_power += settings.power_equal_clocks ? node->same_clock_power : node->last_underclock_power;
                has_variable_power |= building->variable_power;
            }
        }
        else if (n->IsLogistics())
        {
            const LogisticsNode* node = static_cast<const LogisticsNode*>(n.get());
            total_machines[node->GetDisplayName()] += 1;
            all_machines += 1;
            built_machines[node->GetDisplayName()] += 0;
        }
    }

    std::map<std::string, int> min_number_machines;
    for (const auto& [machine, map] : detailed_machines)
    {
        for (const auto& [r, n] : map)
        {
            min_number_machines[machine] += static_cast<int>(std::ceil(n.GetValue()));
        }
    }
    for (const auto& [machine, n] : total_machines)
    {
        if (min_number_machines.find(machine) == min_number_machines.end())
        {
            min_number_machines[machine] = static_cast<int>(std::ceil(n.GetValue()));
        }
    }


    ImGui::SeparatorText("Settings");
    // Display all settings here
    if (ImGui::Button("Unlock all alt recipes"))
    {
        settings.unlocked_alts = {};
        for (const auto& r : Data::Recipes())
        {
            if (r->alternate)
            {
                settings.unlocked_alts[r.get()] = true;
            }
        }
        settings_store->Save(settings);
    }

    if (ImGui::GetContentRegionAvail().x - ImGui::GetItemRectSize().x > ImGui::CalcTextSize("Reset alt recipes").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x)
    {
        ImGui::SameLine();
    }
    if (ImGui::Button("Reset alt recipes"))
    {
        settings.unlocked_alts = {};
        for (const auto& r : Data::Recipes())
        {
            if (r->alternate)
            {
                settings.unlocked_alts[r.get()] = false;
            }
        }
        settings_store->Save(settings);
    }

    if (ImGui::Checkbox("Show somersloop", &settings.show_somersloop))
    {
        settings_store->Save(settings);
    }
    if (settings.show_somersloop || num_somersloop.GetNumerator() != 0)
    {
        ImGui::SameLine();
        const float somersloop_width = ImGui::CalcTextSize(num_somersloop.GetStringFraction().c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float somersloop_display_size = somersloop_width + ImGui::GetTextLineHeightWithSpacing();
        const float available = ImGui::GetContentRegionAvail().x;
        if (available > somersloop_display_size)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - somersloop_display_size);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
        ImGui::SetNextItemWidth(somersloop_width);
        ImGui::BeginDisabled();
        ImGui::InputText("##total_num_somersloop", &num_somersloop.GetStringFraction(), ImGuiInputTextFlags_CharsDecimal);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", "Minimum number of somersloop required for the whole graph\n(assuming 250% overclock per machine)");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                FocusNextSomersloop();
            }
        }
        ImGui::SameLine();
        ImGui::Image((void*)(intptr_t)somersloop_texture_id, ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing()));
        if (ImGui::IsItemClicked())
        {
            FocusNextSomersloop();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", "Minimum number of somersloop required for the whole graph\n(assuming 250% overclock per machine)");
        }
        ImGui::PopStyleVar();
    }
#if WITH_SPOILERS_OPTIONS
    if (ImGui::Checkbox("Show 1.0 new recipes", &settings.show_spoilers))
    {
        settings_store->Save(settings);
    }
#endif
    if (ImGui::Checkbox("Show build progress", &settings.show_build_progress))
    {
        settings_store->Save(settings);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", "If set, will add a checkmark on craft nodes and overall build progress bars");
    }
    if (ImGui::Checkbox("Show debug IDs", &settings.show_debug_ids))
    {
        settings_store->Save(settings);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", "Show node, pin, and link ids for reporting problematic imported connections");
    }
    if (settings.show_build_progress && all_machines.GetNumerator() != 0 && (all_built_machines / all_machines).GetNumerator() != 0)
    {
        if (ImGui::GetContentRegionAvail().x - ImGui::GetItemRectSize().x > ImGui::CalcTextSize("Reset progress").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x)
        {
            ImGui::SameLine();
        }
        if (ImGui::Button("Reset progress"))
        {
            for (auto& n : nodes)
            {
                if (n->IsCraft())
                {
                    static_cast<CraftNode*>(n.get())->built = false;
                }
                else if (n->IsGroup())
                {
                    static_cast<GroupNode*>(n.get())->SetBuiltState(false);
                }
            }
        }
    }


    if (settings.show_build_progress)
    {
        ImGui::SeparatorText("Build Progress");

        // Progress bar color
        ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_PlotHistogram, ImVec4(0, 0.5, 0, 1));
        // No visible color change when hovered/click
        ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        const bool display_build_details = ImGui::TreeNodeEx("##build_progress", ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();

        // Displayed over the TreeNodeEx element (same line)
        ImGui::SameLine();
        ImGui::ProgressBar(all_machines.GetNumerator() == 0 ? 0.0f : static_cast<float>((all_built_machines / all_machines).GetValue()));
        float max_machine_name_width = 0.0f;
        for (auto& [machine, f] : built_machines)
        {
            max_machine_name_width = std::max(max_machine_name_width, ImGui::CalcTextSize(machine.c_str()).x);
        }
        // Detailed list of recipes if the tree node is open
        if (display_build_details)
        {
            ImGui::Indent();
            for (auto& [machine, f] : built_machines)
            {
                ImGui::TextUnformatted(machine.c_str());
                ImGui::SameLine();
                ImGui::Dummy(ImVec2(max_machine_name_width - ImGui::CalcTextSize(machine.c_str()).x, 0.0f));
                ImGui::SameLine();
                ImGui::ProgressBar((f / total_machines[machine]).GetValue());
            }

            ImGui::Unindent();
            ImGui::TreePop();
        }
        ImGui::PopStyleColor();
    }

    ImGui::SeparatorText(has_variable_power ? "Average Power Consumption" : "Power Consumption");
    if (total_power.GetNumerator() != 0)
    {
        if (ImGui::Checkbox("Compute power with equal clocks", &settings.power_equal_clocks))
        {
            settings_store->Save(settings);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s",
                "If set, the power will be calculated assuming all machines in a node are set at the same clock rate\n"
                "Otherwise, it will be calculated with machines at 100% and one last machine underclocked");
        }

        const float power_width = ImGui::CalcTextSize("000000.00").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        std::vector<std::pair<const Recipe*, FractionalNumber>> sorted_detailed_power(detailed_power.begin(), detailed_power.end());
        std::stable_sort(sorted_detailed_power.begin(), sorted_detailed_power.end(), [](const auto& a, const auto& b) {
            return a.second.GetValue() > b.second.GetValue();
        });

        // No visible color change when hovered/click
        ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        const bool display_power_details = ImGui::TreeNodeEx("##power", ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();

        // Displayed over the TreeNodeEx element (same line)
        ImGui::SameLine();
        total_power.RenderInputText("##power", true, false, power_width);
        ImGui::SameLine();
        ImGui::Text("%sMW", has_variable_power ? "~" : "");
        // Detailed list of recipes if the tree node is open
        if (display_power_details)
        {
            ImGui::Indent();
            for (auto& [recipe, p] : sorted_detailed_power)
            {
                p.RenderInputText("##power", true, false, power_width);
                ImGui::SameLine();
                ImGui::Text("%sMW", recipe->building->variable_power ? "~" : "");
                ImGui::SameLine();
                recipe->Render();
                if (ImGui::IsItemClicked())
                {
                    FocusNextRecipe(recipe->name);
                }
            }

            ImGui::Unindent();
            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("Sink points");
    if (total_sink_points.GetNumerator() != 0)
    {
        const float sink_points_width = ImGui::CalcTextSize("00000000.00").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        std::vector<std::pair<const Item*, FractionalNumber>> sorted_sink_points(detailed_sink_points.begin(), detailed_sink_points.end());
        std::stable_sort(sorted_sink_points.begin(), sorted_sink_points.end(), [](const auto& a, const auto& b) {
            return a.second.GetValue() > b.second.GetValue();
            });

        // No visible color change when hovered/click
        ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        const bool display_sink_points_details = ImGui::TreeNodeEx("##sink_points", ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();

        // Displayed over the TreeNodeEx element (same line)
        ImGui::SameLine();
        total_sink_points.RenderInputText("##sink_points", true, true, sink_points_width);
        ImGui::SameLine();
        ImGui::TextUnformatted("Points");
        // Detailed list of items if the tree node is open
        if (display_sink_points_details)
        {
            ImGui::Indent();
            for (auto& [item, p] : sorted_sink_points)
            {
                p.RenderInputText("##sink_points", true, true, sink_points_width);
                ImGui::SameLine();
                ImGui::Image((void*)(intptr_t)item->icon_gl_index, ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing()));
                if (ImGui::IsItemClicked())
                {
                    FocusNextItem(item->name);
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(item->name.c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip("%s", item->name.c_str());
                }
                if (ImGui::IsItemClicked())
                {
                    FocusNextItem(item->name);
                }
            }
            ImGui::Unindent();
            ImGui::TreePop();
        }
    }

    const float rate_width = ImGui::CalcTextSize("0000.000").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SeparatorText("Machines");
    for (auto& [machine, n] : total_machines)
    {
        if (n.GetNumerator() == 0)
        {
            continue;
        }
        // No visible color change when hovered/click
        ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        const bool display_details = ImGui::TreeNodeEx(("##" + machine).c_str(), ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();

        // Displayed over the TreeNodeEx element (same line)
        ImGui::SameLine();
        n.RenderInputText("##rate", true, true, rate_width);
        ImGui::SameLine();
        ImGui::Text("(%i)", min_number_machines[machine]);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", "Minimum number of machines at 100%");
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(machine.c_str());

        // Detailed list of recipes if the tree node is open
        if (display_details)
        {
            ImGui::Indent();
            for (auto& [recipe, n2] : detailed_machines[machine])
            {
                n2.RenderInputText("##rate", true, true, rate_width);
                ImGui::SameLine();
                ImGui::Text("(%i)", static_cast<int>(std::ceil(n2.GetValue())));
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", "Minimum number of machines at 100%");
                }
                ImGui::SameLine();

                recipe->Render();
                if (ImGui::IsItemClicked())
                {
                    FocusNextRecipe(recipe->name);
                }
            }
            for (auto& [item, n2] : detailed_machine_outputs[machine])
            {
                if (item == nullptr)
                {
                    continue;
                }
                ImGui::PushID(item);
                n2.RenderInputText("##rate", true, true, rate_width);
                ImGui::SameLine();
                ImGui::Image((void*)(intptr_t)item->icon_gl_index, ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing()));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip("%s", item->name.c_str());
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(item->name.c_str());
                if (ImGui::IsItemClicked())
                {
                    FocusNextItem(item->name);
                }
                ImGui::PopID();
            }

            ImGui::Unindent();

            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("Inputs");
    for (auto& [item, n] : inputs)
    {
        if (n.GetNumerator() == 0)
        {
            continue;
        }
        if (const auto out_it = outputs.find(item); out_it != outputs.end())
        {
            // More output than input, don't display this in inputs
            if (out_it->second > n)
            {
                continue;
            }
            // Equal, just add it to intermediate
            else if (out_it->second == n)
            {
                outputs.erase(out_it);
                intermediates[item] += n;
                continue;
            }
            // More input than output, display the diff and add the common part to intermediate
            else
            {
                n = n - out_it->second;
                intermediates[item] += out_it->second;
                outputs.erase(out_it);
            }
        }
        n.RenderInputText("##rate", true, true, rate_width);
        ImGui::SameLine();
        ImGui::Image((void*)(intptr_t)item->icon_gl_index, ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing()));
        if (ImGui::IsItemClicked())
        {
            FocusNextItem(item->name);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(item->name.c_str());
        if (ImGui::IsItemClicked())
        {
            FocusNextItem(item->name);
        }
    }

    ImGui::SeparatorText("Outputs");
    for (auto& [item, n] : outputs)
    {
        if (n.GetNumerator() == 0)
        {
            continue;
        }
        if (const auto in_it = inputs.find(item); in_it != inputs.end())
        {
            // More input than output, skip this
            if (in_it->second > n)
            {
                continue;
            }
            // Equal should not happen as it's removed from output in input loop
            // More output than input, display the diff and add the common part to intermediate
            else
            {
                n = n - in_it->second;
                intermediates[item] += in_it->second;
                inputs.erase(in_it);
            }
        }
        n.RenderInputText("##rate", true, true, rate_width);
        ImGui::SameLine();
        ImGui::Image((void*)(intptr_t)item->icon_gl_index, ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing()));
        if (ImGui::IsItemClicked())
        {
            FocusNextItem(item->name);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(item->name.c_str());
        if (ImGui::IsItemClicked())
        {
            FocusNextItem(item->name);
        }
    }

    ImGui::SeparatorText("Intermediates");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", "Items both produced and consumed in the production chain");
    }
    for (auto& [item, n] : intermediates)
    {
        if (n.GetNumerator() == 0)
        {
            continue;
        }
        n.RenderInputText("##rate", true, true, rate_width);
        ImGui::SameLine();
        ImGui::Image((void*)(intptr_t)item->icon_gl_index, ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing()));
        if (ImGui::IsItemClicked())
        {
            FocusNextItem(item->name);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(item->name.c_str());
        if (ImGui::IsItemClicked())
        {
            FocusNextItem(item->name);
        }
    }
}

void ProductionApp::RenderResourceFlowWindow()
{
    if (!settings.show_resource_flow)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);
    bool open = settings.show_resource_flow;
    if (ImGui::Begin("Resource Flow", &open))
    {
        // Status filter buttons
        const auto filter_button = [this](const char* label, ResourceFlowFilter value) {
            const bool active = resource_flow_filter == value;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(label)) resource_flow_filter = value;
            if (active) ImGui::PopStyleColor();
        };
        filter_button("All", ResourceFlowFilter::All);
        ImGui::SameLine();
        filter_button("Deficit", ResourceFlowFilter::Deficit);
        ImGui::SameLine();
        filter_button("Surplus", ResourceFlowFilter::Surplus);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", resource_flow_search.c_str());
        if (ImGui::InputTextWithHint("##resource_flow_search", "Search...", buf, sizeof(buf)))
        {
            resource_flow_search = buf;
        }

        // Non-const so the cached GetStringFloat() strings can be built per row.
        ResourceFlowReport report = BuildResourceFlowReport(nodes);

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##resource_flow_table", 5, flags))
        {
            ImGui::TableSetupColumn("Item");
            ImGui::TableSetupColumn("Produced/min");
            ImGui::TableSetupColumn("Consumed/min");
            ImGui::TableSetupColumn("Net/min");
            ImGui::TableSetupColumn("Status");
            ImGui::TableHeadersRow();

            const float h = ImGui::GetTextLineHeightWithSpacing();
            for (ResourceFlowRow& row : report.rows)
            {
                if (!RowPassesFilter(row, resource_flow_filter, resource_flow_search))
                {
                    continue;
                }

                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                if (row.item != nullptr && row.item->icon_gl_index != 0)
                {
                    ImGui::Image((void*)(intptr_t)row.item->icon_gl_index, ImVec2(h, h));
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(row.item != nullptr ? row.item->name.c_str() : "(unknown)");

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.produced.GetStringFloat().c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.consumed.GetStringFloat().c_str());

                ImGui::TableNextColumn();
                const ImVec4 green(0.4f, 0.85f, 0.4f, 1.0f);
                const ImVec4 red(0.9f, 0.4f, 0.4f, 1.0f);
                const std::string net_str =
                    (row.status == ResourceFlowStatus::Surplus ? "+" : "") +
                    row.net.GetStringFloat();
                if (row.status == ResourceFlowStatus::Surplus) ImGui::TextColored(green, "%s", net_str.c_str());
                else if (row.status == ResourceFlowStatus::Deficit) ImGui::TextColored(red, "%s", net_str.c_str());
                else ImGui::TextUnformatted(net_str.c_str());

                ImGui::TableNextColumn();
                switch (row.status)
                {
                case ResourceFlowStatus::Surplus:  ImGui::TextColored(green, "Surplus"); break;
                case ResourceFlowStatus::Deficit:  ImGui::TextColored(red, "Deficit"); break;
                case ResourceFlowStatus::Balanced: ImGui::TextUnformatted("Balanced"); break;
                }
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        ImGui::Text("%zu surplus  -  %zu deficit  -  %zu balanced",
            report.surplus_count, report.deficit_count, report.balanced_count);
        if (report.skipped_null_item_pins > 0)
        {
            ImGui::TextDisabled("%zu pin(s) with no item skipped", report.skipped_null_item_pins);
        }
    }
    ImGui::End();

    // The window's [x] close button toggles the persisted setting.
    if (open != settings.show_resource_flow)
    {
        settings.show_resource_flow = open;
        settings_store->Save(settings);
    }
}

void ProductionApp::RenderNodes()
{
    const float zoom_level = ax::NodeEditor::GetCurrentZoom();
    const float rate_width = ImGui::CalcTextSize("000.000").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float somersloop_width = ImGui::CalcTextSize("4").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    // Vector that will be reused to sort pins for all nodes (instead of creating two per nodes)
    std::vector<size_t> sorted_pin_indices(4);
    auto sort_pin_indices = [&](const std::vector<std::unique_ptr<Pin>>& pins) {
        // Make sure there is enough elements in the vector
        const size_t N = pins.size();
        if (N > sorted_pin_indices.size())
        {
            sorted_pin_indices.resize(N);
        }
        for (size_t i = 0; i < N; ++i)
        {
            sorted_pin_indices[i] = i;
        }
        // Don't need to use stable sort as pins don't usually have the exact same Y coordinates
        std::sort(sorted_pin_indices.begin(), sorted_pin_indices.begin() + N, [&](const size_t i1, const size_t i2) {
            const std::unique_ptr<Pin>& p1 = pins[i1];
            const std::unique_ptr<Pin>& p2 = pins[i2];
            const Link* l1 = p1->link;
            const Link* l2 = p2->link;

            const bool p1_is_above = l1 != nullptr && (p1->direction == ax::NodeEditor::PinKind::Input ? l1->start : l1->end)->node->pos.y < p1->node->pos.y;
            const bool p2_is_above = l2 != nullptr && (p2->direction == ax::NodeEditor::PinKind::Input ? l2->start : l2->end)->node->pos.y < p2->node->pos.y;

            // Both are linked and above
            if (p1_is_above && p2_is_above)
            {
                return (p1->direction == ax::NodeEditor::PinKind::Input && l1->start->node->pos.y < l2->start->node->pos.y) ||
                    (p1->direction == ax::NodeEditor::PinKind::Output && l1->end->node->pos.y < l2->end->node->pos.y);
            }

            // p1 is above, p2 is either unlinked or below
            if (p1_is_above)
            {
                return true;
            }

            // p2 is above, p1 is either unlinked or below
            if (p2_is_above)
            {
                return false;
            }

            // No link, keep default order
            if (l1 == nullptr && l2 == nullptr)
            {
                return i1 < i2;
            }

            // p1 isn't linked, p2 is and we know it's not above, so p1 < p2
            if (l1 == nullptr)
            {
                return true;
            }

            // p2 isn't linked, p1 is and we know it's not above, so p2 < p1
            if (l2 == nullptr)
            {
                return false;
            }

            // Both are linked and below
            return (p1->direction == ax::NodeEditor::PinKind::Input && l1->start->node->pos.y < l2->start->node->pos.y) ||
                (p1->direction == ax::NodeEditor::PinKind::Output && l1->end->node->pos.y < l2->end->node->pos.y);
        });
    };

    for (const auto& node : nodes)
    {
        int node_pushed_style = 0;
        if (node->IsOrganizer() && !static_cast<OrganizerNode*>(node.get())->IsBalanced() ||
            node->IsGroup() && static_cast<GroupNode*>(node.get())->loading_error)
        {
            ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_NodeBorder, ImColor(255, 0, 0));
            node_pushed_style += 1;
        }
        if (settings.show_build_progress && (
            (node->IsCraft() && static_cast<CraftNode*>(node.get())->built) ||
            (node->IsGroup() && static_cast<GroupNode*>(node.get())->built_machines == static_cast<GroupNode*>(node.get())->total_machines)
        ))
        {
            ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_NodeBorder, ImColor(0, 255, 0));
            node_pushed_style += 1;
        }
        ax::NodeEditor::BeginNode(node->id);
        ImGui::PushID(node->id.AsPointer());
        ImGui::BeginVertical("node");
        {
            if (zoom_level < 1.5f)
            {
                ImGui::BeginHorizontal("header");
                {
                    switch (node->GetKind())
                    {
                    case Node::Kind::Craft:
                    {
                        CraftNode* craft_node = static_cast<CraftNode*>(node.get());
                        ImGui::Spring(1.0f);
                        ImGui::TextUnformatted(craft_node->recipe->display_name.c_str());
                        ImGui::Spring(1.0f);
                        if (settings.show_build_progress)
                        {
                            ImGui::PushStyleVar(ImGuiStyleVar_::ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                            ImGui::Checkbox("##craft_built", &craft_node->built);
                            ImGui::PopStyleVar();
                        }
                    }
                    break;
                    case Node::Kind::Merger:
                        ImGui::TextUnformatted("Merger");
                        ImGui::Spring(0.0f);
                        RenderOrganizerRecalcChainButton(static_cast<OrganizerNode*>(node.get()));
                        RenderOrganizerRecalcButton(static_cast<OrganizerNode*>(node.get()));
                        break;
                    case Node::Kind::CustomSplitter:
                        ImGui::TextUnformatted("Splitter*");
                        ImGui::Spring(0.0f);
                        RenderOrganizerRecalcChainButton(static_cast<OrganizerNode*>(node.get()));
                        RenderOrganizerRecalcButton(static_cast<OrganizerNode*>(node.get()));
                        break;
                    case Node::Kind::GameSplitter:
                        ImGui::TextUnformatted("Splitter");
                        ImGui::Spring(0.0f);
                        RenderOrganizerRecalcChainButton(static_cast<OrganizerNode*>(node.get()));
                        RenderOrganizerRecalcButton(static_cast<OrganizerNode*>(node.get()));
                        break;
                    case Node::Kind::Sink:
                        ImGui::TextUnformatted("Sink");
                        break;
                    case Node::Kind::Logistics:
                    {
                        const LogisticsNode* logistics_node = static_cast<const LogisticsNode*>(node.get());
                        ImGui::TextUnformatted(logistics_node->GetDisplayName());
                        break;
                    }
                    case Node::Kind::Group:
                    {
                        GroupNode* group_node = static_cast<GroupNode*>(node.get());
                        ImGui::Spring(1.0f);
                        ImGui::TextUnformatted("Group");
                        ImGui::Spring(0.0f);
                        ImGui::SetNextItemWidth(std::max(ImGui::CalcTextSize(group_node->name.c_str()).x, ImGui::CalcTextSize("Name...").x) + ImGui::GetStyle().FramePadding.x * 4.0f);
                        ImGui::InputTextWithHint("##name", "Name...", &group_node->name);
                        ImGui::Spring(1.0f);
                        if (settings.show_build_progress)
                        {
                            ImGui::PushStyleVar(ImGuiStyleVar_::ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                            bool is_built = group_node->built_machines == group_node->total_machines;
                            if (ImGui::Checkbox("##group_built", &is_built))
                            {
                                group_node->SetBuiltState(is_built);
                            }
                            ImGui::PopStyleVar();
                        }
                        break;
                    }
                    case Node::Kind::Extractor:
                    {
                        ExtractorNode* ex = static_cast<ExtractorNode*>(node.get());
                        const Building* b = ex->GetBuilding();
                        ImGui::Spring(1.0f);
                        ImGui::TextUnformatted(b != nullptr ? b->name.c_str() : "Extractor");
                        ImGui::Spring(1.0f);
                        break;
                    }
                    }
                }
                ImGui::EndHorizontal();

                if (settings.show_debug_ids)
                {
                    ImGui::TextDisabled("node:%s", std::to_string(node->id.Get()).c_str());
                }

                // spacing between header and content
                ImGui::Spring(0, ImGui::GetStyle().ItemSpacing.y * 2.0f);
            }

            ImGui::BeginHorizontal("content");
            {
                ImGui::Spring(0.0f, 0.0f);
                ImGui::BeginVertical("inputs", ImVec2(0, 0), 0.0f); // Align elements on the left of the column
                {
                    // Set where the link will connect to (left center)
                    ax::NodeEditor::PushStyleVar(ax::NodeEditor::StyleVar_PivotAlignment, ImVec2(0, 0.5f));
                    ax::NodeEditor::PushStyleVar(ax::NodeEditor::StyleVar_PivotSize, ImVec2(0, 0));

                    std::optional<int> removed_input_idx = std::nullopt;
                    sort_pin_indices(node->ins);
                    for (int idx = 0; idx < node->ins.size(); ++idx)
                    {
                        const auto& p = node->ins[sorted_pin_indices[idx]];
                        ax::NodeEditor::BeginPin(p->id, p->direction);
                        ImGui::BeginHorizontal(p->id.AsPointer());
                        {
                            const float radius = 0.2f * ImGui::GetTextLineHeightWithSpacing();
                            const ImVec2 size(2.0f * radius, 2.0f * radius);
                            // Draw circle
                            if (ImGui::IsRectVisible(size))
                            {
                                const ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
                                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                                const ImVec2 center(
                                    cursor_pos.x + radius,
                                    cursor_pos.y + radius
                                );
                                if (p->link == nullptr)
                                {
                                    draw_list->AddCircle(center, radius, ImColor(1.0f, 1.0f, 1.0f));
                                }
                                else
                                {
                                    draw_list->AddCircleFilled(center, radius, ImColor(1.0f, 1.0f, 1.0f));
                                }
                            }
                            ImGui::Dummy(size);
                            ImGui::Spring(0.0f);
                            if (zoom_level < 1.5f)
                            {
                                if (settings.show_debug_ids)
                                {
                                    ImGui::TextDisabled("pin:%s", std::to_string(p->id.Get()).c_str());
                                    ImGui::Spring(0.0f);
                                }
                                if (node->IsMerger() || node->IsSink())
                                {
                                    ImGui::BeginDisabled(node->ins.size() == 1);
                                    if (ImGui::Button("x"))
                                    {
                                        // We can't remove it directly as we are currently looping through the pins
                                        removed_input_idx = idx;
                                    }
                                    ImGui::EndDisabled();
                                }
                                ImGui::Spring(0.0f);
                                if (p->GetLocked())
                                {
                                    ImGui::PushStyleColor(ImGuiCol_FrameBg, lock_purple);
                                }
                                p->current_rate.RenderInputText("##rate", p->GetLocked(), false, rate_width);
                                if (p->GetLocked())
                                {
                                    ImGui::PopStyleColor();
                                }
                                if (ImGui::IsItemDeactivatedAfterEdit())
                                {
                                    try
                                    {
                                        const FractionalNumber new_rate(p->current_rate.GetStringFloat());
                                        if (new_rate.GetNumerator() < 0)
                                        {
                                            throw std::invalid_argument("Negative rate in pin");
                                        }
                                        if (!UpdateNodesRate(p.get(), new_rate))
                                        {
                                            p->current_rate = FractionalNumber(p->current_rate.GetNumerator(), p->current_rate.GetDenominator());
                                        }
                                    }
                                    // User entered an invalid string
                                    catch (const std::invalid_argument&)
                                    {
                                        p->current_rate = FractionalNumber(p->current_rate.GetNumerator(), p->current_rate.GetDenominator());
                                    }
                                    // Wrong equations during update process
                                    catch (const std::runtime_error&)
                                    {
                                        p->current_rate = FractionalNumber(p->current_rate.GetNumerator(), p->current_rate.GetDenominator());
                                        fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
                                    }
                                }
                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                {
                                    frame_tooltips.push_back(p->current_rate.GetStringFraction());
                                }
#if DEBUG_PROPAGATION
                                {
                                    const bool is_in_graph = graph_update_pins.find(p.get()) != graph_update_pins.end();
                                    const bool is_multi = graph_update_multi_pins_constrained.find(p.get()) != graph_update_multi_pins_constrained.end();
                                    // Dark green if multi pin constrained, green if relevant to the graph update
                                    // Orange if multi pin constrained and error, yellow if relevant to the graph update and error
                                    if (is_in_graph && is_multi)
                                    {
                                        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImColor(p->error ? 255 : 0, 125, 0), 0.0f, ImDrawFlags_None, 1.0f);
                                    }
                                    else if (is_in_graph)
                                    {
                                        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImColor(p->error ? 255 : 0, 255, 0), 0.0f, ImDrawFlags_None, 1.0f);
                                    }
                                }
#else
                                if (p->error)
                                {
                                    // Draw red rectangle around pin
                                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImColor(255, 0, 0), 0.0f, ImDrawFlags_None, 1.0f);
                                }
#endif
                                if (node->IsPowered() ||
                                    ((node->IsSink() || node->IsLogistics()) && p->item != nullptr))
                                {
                                    ImGui::Spring(0.0f);
                                    ImGui::Image((void*)(intptr_t)p->item->icon_gl_index, ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing()));
                                    ImGui::Spring(0.0f);
                                    ImGui::TextUnformatted(p->item->new_line_name.c_str());
                                    ImGui::Spring(0.0f);
                                }
                            }
                            else if (zoom_level < 3.5f && (node->IsPowered() || node->IsLogistics()) && p->item != nullptr)
                            {
                                ImGui::Image((void*)(intptr_t)p->item->icon_gl_index, ImVec2(ImGui::GetTextLineHeightWithSpacing() * (0.5f + zoom_level), ImGui::GetTextLineHeightWithSpacing() * (0.5f + zoom_level)));
                                ImGui::Spring(0.0f);
                            }
                        }
                        ImGui::EndHorizontal();
                        ax::NodeEditor::EndPin();

                        ImGui::Spring(0.0f);
                    }
                    // Vehicle station plug on the input side (Unload mode).
                    if (node->IsLogistics())
                    {
                        LogisticsNode* lg = static_cast<LogisticsNode*>(node.get());
                        if (lg->logistics_kind == LogisticsNode::Kind::TruckStation ||
                            lg->logistics_kind == LogisticsNode::Kind::TrainStation)
                        {
                            VehicleStationNode* v = static_cast<VehicleStationNode*>(lg);
                            if (v->plug && v->plug->direction == ax::NodeEditor::PinKind::Input)
                            {
                                RenderVehiclePlug(v->plug.get());
                                ImGui::Spring(0.0f);
                            }
                        }
                    }
                    ax::NodeEditor::PopStyleVar();
                    ax::NodeEditor::PopStyleVar();
                    if (zoom_level < 1.5f && (node->IsMerger() || node->IsSink()))
                    {
                        ImGui::BeginHorizontal("add_input_+_buttton");
                        ImGui::Spring(1.0f, 0.0f);
                        if (ImGui::Button("+"))
                        {
                            node->ins.emplace_back(std::make_unique<Pin>(
                                GetNextId(),
                                ax::NodeEditor::PinKind::Input,
                                node.get(),
                                node->IsMerger() ? static_cast<MergerNode*>(node.get())->item : nullptr, // Merger or Sink
                                node->IsMerger() && node->outs[0]->GetLocked()
                            ));
                        }
                        ImGui::Spring(1.0f, 0.0f);
                        ImGui::EndHorizontal();
                        if (removed_input_idx.has_value())
                        {
                            const int idx = removed_input_idx.value();
                            if (node->ins[sorted_pin_indices[idx]]->link != nullptr)
                            {
                                DeleteLink(node->ins[sorted_pin_indices[idx]]->link->id);
                            }
                            node->ins.erase(node->ins.begin() + sorted_pin_indices[idx]);
                            if (node->IsMerger()) // Sink doesn't have any output to update, nor lock pins to update
                            {
                                FractionalNumber new_output;
                                size_t num_unlocked = 0;
                                for (const auto& p : node->ins)
                                {
                                    new_output += p->current_rate;
                                    num_unlocked += !p->GetLocked();
                                }
                                // Output can't change if it's locked
                                if (node->outs[0]->GetLocked())
                                {
                                    new_output = node->outs[0]->current_rate;
                                }
                                const FractionalNumber old_output = node->outs[0].get()->current_rate;
                                // We need to set the current rate to the new sum, otherwise balancing would
                                // be performed on the old ratios (including the deleted pin)
                                node->outs[0].get()->current_rate = new_output;
                                try
                                {
                                    if (!UpdateNodesRate(node->outs[0].get(), new_output))
                                    {
                                        node->outs[0].get()->current_rate = old_output;
                                    }
                                }
                                // Wrong equations during update process
                                catch (const std::runtime_error&)
                                {
                                    node->outs[0].get()->current_rate = old_output;
                                    fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
                                    error_time = ax::NodeEditor::GetStyle().FlowDuration;
                                }

                                // Update lock state
                                if (num_unlocked == 0)
                                {
                                    node->outs[0]->SetLocked(true);
                                }
                                // If output is locked and there is only one remaining input, it should be locked now
                                else if (num_unlocked == 1 && node->outs[0]->GetLocked())
                                {
                                    for (const auto& p : node->ins)
                                    {
                                        p->SetLocked(true);
                                    }
                                }
                            }
                        }
                    }
                    ImGui::Spring(1.0f, 0.0f);
                }
                ImGui::EndVertical();

                if (zoom_level < 3.5f)
                {
                    ImGui::Spring(1.0f);
                }
                else if (node->IsPowered() && node->outs.size() > 0 && node->outs[0]->item != nullptr)
                {
                    ImGui::Spring(0.0f);
                    ImGui::Image((void*)(intptr_t)node->outs[0]->item->icon_gl_index, ImVec2(ImGui::GetTextLineHeightWithSpacing() * (0.5f + zoom_level), ImGui::GetTextLineHeightWithSpacing() * (0.5f + zoom_level)));
                    ImGui::Spring(0.0f);
                }

                ImGui::BeginVertical("outputs", ImVec2(0, 0), 1.0f); // Align all elements on the right of the column
                {
                    // Set where the link will connect to (right center)
                    ax::NodeEditor::PushStyleVar(ax::NodeEditor::StyleVar_PivotAlignment, ImVec2(1.0f, 0.5f));
                    ax::NodeEditor::PushStyleVar(ax::NodeEditor::StyleVar_PivotSize, ImVec2(0, 0));
                    std::optional<int> removed_output_idx = std::nullopt;
                    sort_pin_indices(node->outs);
                    for (int idx = 0; idx < node->outs.size(); ++idx)
                    {
                        const auto& p = node->outs[sorted_pin_indices[idx]];
                        ax::NodeEditor::BeginPin(p->id, p->direction);
                        ImGui::BeginHorizontal(p->id.AsPointer());
                        {
                            if (zoom_level < 1.5f)
                            {
                                if ((node->IsPowered() || node->IsLogistics()) && p->item != nullptr)
                                {
                                    ImGui::Spring(0.0f);
                                    ImGui::TextUnformatted(p->item->new_line_name.c_str());
                                    ImGui::Spring(0.0f);
                                    ImGui::Image((void*)(intptr_t)p->item->icon_gl_index, ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing()));
                                }
                                ImGui::Spring(0.0f);
                                if (p->GetLocked())
                                {
                                    ImGui::PushStyleColor(ImGuiCol_FrameBg, lock_purple);
                                }
                                p->current_rate.RenderInputText("##rate", p->GetLocked(), false, rate_width);
                                if (p->GetLocked())
                                {
                                    ImGui::PopStyleColor();
                                }
                                if (ImGui::IsItemDeactivatedAfterEdit())
                                {
                                    try
                                    {
                                        const FractionalNumber new_rate(p->current_rate.GetStringFloat());
                                        if (new_rate.GetNumerator() < 0)
                                        {
                                            throw std::invalid_argument("Negative rate in pin");
                                        }
                                        if (!UpdateNodesRate(p.get(), new_rate))
                                        {
                                            // Revert to previous value
                                            p->current_rate = FractionalNumber(p->current_rate.GetNumerator(), p->current_rate.GetDenominator());
                                        }
                                    }
                                    // User entered an invalid string
                                    catch (const std::invalid_argument&)
                                    {
                                        p->current_rate = FractionalNumber(p->current_rate.GetNumerator(), p->current_rate.GetDenominator());
                                    }
                                    // Wrong equations during update process
                                    catch (const std::runtime_error&)
                                    {
                                        p->current_rate = FractionalNumber(p->current_rate.GetNumerator(), p->current_rate.GetDenominator());
                                        fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
                                        error_time = ax::NodeEditor::GetStyle().FlowDuration;
                                    }
                                }
                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                {
                                    frame_tooltips.push_back(p->current_rate.GetStringFraction());
                                }
#if DEBUG_PROPAGATION
                                {
                                    const bool is_in_graph = graph_update_pins.find(p.get()) != graph_update_pins.end();
                                    const bool is_multi = graph_update_multi_pins_constrained.find(p.get()) != graph_update_multi_pins_constrained.end();
                                    // Dark green if multi pin constrained, green if relevant to the graph update
                                    // Orange if multi pin constrained and error, yellow if relevant to the graph update and error
                                    if (is_in_graph && is_multi)
                                    {
                                        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImColor(p->error ? 255 : 0, 125, 0), 0.0f, ImDrawFlags_None, 1.0f);
                                    }
                                    else if (is_in_graph)
                                    {
                                        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImColor(p->error ? 255 : 0, 255, 0), 0.0f, ImDrawFlags_None, 1.0f);
                                    }
                                }
#else
                                if (p->error)
                                {
                                    // Draw red rectangle around pin
                                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImColor(255, 0, 0), 0.0f, ImDrawFlags_None, 1.0f);
                                }
#endif
                                if (settings.show_debug_ids)
                                {
                                    ImGui::Spring(0.0f);
                                    ImGui::TextDisabled("pin:%s", std::to_string(p->id.Get()).c_str());
                                }
                                ImGui::Spring(0.0f);
                                if (node->IsCustomSplitter() || node->IsGameSplitter())
                                {
                                    ImGui::BeginDisabled(node->outs.size() == 1);
                                    if (ImGui::Button("x"))
                                    {
                                        // We can't remove it directly as we are currently looping through the pins
                                        removed_output_idx = idx;
                                    }
                                    ImGui::EndDisabled();
                                }
                            }
                            else if (zoom_level < 3.5f && (node->IsPowered() || node->IsLogistics()) && p->item != nullptr)
                            {
                                ImGui::Image((void*)(intptr_t)p->item->icon_gl_index, ImVec2(ImGui::GetTextLineHeightWithSpacing() * (0.5f + zoom_level), ImGui::GetTextLineHeightWithSpacing() * (0.5f + zoom_level)));
                            }
                            ImGui::Spring(0.0f);
                            const float radius = 0.2f * ImGui::GetTextLineHeightWithSpacing();
                            const ImVec2 size(2.0f * radius, 2.0f * radius);
                            // Draw circle
                            if (ImGui::IsRectVisible(size))
                            {
                                const ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
                                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                                const ImVec2 center(
                                    cursor_pos.x + radius,
                                    cursor_pos.y + radius
                                );
                                if (p->link == nullptr)
                                {
                                    draw_list->AddCircle(center, radius, ImColor(1.0f, 1.0f, 1.0f));
                                }
                                else
                                {
                                    draw_list->AddCircleFilled(center, radius, ImColor(1.0f, 1.0f, 1.0f));
                                }
                            }
                            ImGui::Dummy(size);
                        }
                        ImGui::EndHorizontal();
                        ax::NodeEditor::EndPin();

                        ImGui::Spring(0.0f);
                    }
                    // Vehicle station plug on the output side (Load mode).
                    if (node->IsLogistics())
                    {
                        LogisticsNode* lg = static_cast<LogisticsNode*>(node.get());
                        if (lg->logistics_kind == LogisticsNode::Kind::TruckStation ||
                            lg->logistics_kind == LogisticsNode::Kind::TrainStation)
                        {
                            VehicleStationNode* v = static_cast<VehicleStationNode*>(lg);
                            if (v->plug && v->plug->direction == ax::NodeEditor::PinKind::Output)
                            {
                                RenderVehiclePlug(v->plug.get());
                                ImGui::Spring(0.0f);
                            }
                        }
                    }
                    ax::NodeEditor::PopStyleVar();
                    ax::NodeEditor::PopStyleVar();
                    if (zoom_level < 1.5f && (node->IsCustomSplitter() || node->IsGameSplitter()))
                    {
                        OrganizerNode* org_node = static_cast<OrganizerNode*>(node.get());
                        ImGui::BeginHorizontal("add_output_+_buttton");
                        ImGui::Spring(1.0f, 0.0f);
                        if (ImGui::Button("+"))
                        {
                            org_node->outs.emplace_back(std::make_unique<Pin>(
                                GetNextId(),
                                ax::NodeEditor::PinKind::Output,
                                org_node,
                                org_node->item,
                                node->ins[0]->GetLocked()
                            ));
                            if (node->IsGameSplitter())
                            {
                                try
                                {
                                    if (!UpdateNodesRate(node->ins[0].get(), node->ins[0]->current_rate))
                                    {
                                        // Not sure what to do if it fails? Recreate the deleted pin?
                                    }
                                }
                                // Wrong equations during update process
                                catch (const std::runtime_error&)
                                {
                                    fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
                                    error_time = ax::NodeEditor::GetStyle().FlowDuration;
                                }
                            }
                        }
                        ImGui::Spring(1.0f, 0.0f);
                        ImGui::EndHorizontal();
                        if (removed_output_idx.has_value())
                        {
                            const int idx = removed_output_idx.value();
                            if (node->outs[sorted_pin_indices[idx]]->link != nullptr)
                            {
                                DeleteLink(node->outs[sorted_pin_indices[idx]]->link->id);
                            }
                            node->outs.erase(node->outs.begin() + sorted_pin_indices[idx]);
                            if (node->IsCustomSplitter())
                            {
                                FractionalNumber new_input;
                                size_t num_unlocked = 0;
                                for (const auto& p : node->outs)
                                {
                                    new_input += p->current_rate;
                                    num_unlocked += !p->GetLocked();
                                }
                                // Input can't change if it's locked
                                if (node->ins[0]->GetLocked())
                                {
                                    new_input = node->ins[0]->current_rate;
                                }
                                const FractionalNumber old_input = node->ins[0].get()->current_rate;
                                // We need to set the current rate to the new sum, otherwise balancing would
                                // be performed on the old ratios (including the deleted pin)
                                node->ins[0].get()->current_rate = new_input;
                                try
                                {
                                    if (!UpdateNodesRate(node->ins[0].get(), new_input))
                                    {
                                        node->ins[0].get()->current_rate = old_input;
                                    }
                                }
                                // Wrong equations during update process
                                catch (const std::runtime_error&)
                                {
                                    fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
                                    error_time = ax::NodeEditor::GetStyle().FlowDuration;
                                }

                                // Update lock state
                                if (num_unlocked == 0)
                                {
                                    node->ins[0]->SetLocked(true);
                                }
                                // If input is locked and there is only one remaining output, it should be locked now
                                else if (num_unlocked == 1 && node->ins[0]->GetLocked())
                                {
                                    for (const auto& p : node->outs)
                                    {
                                        p->SetLocked(true);
                                    }
                                }
                            }
                            else // GameSplitter
                            {
                                try
                                {
                                    if (!UpdateNodesRate(node->ins[0].get(), node->ins[0]->current_rate))
                                    {
                                        // Not sure what to do if it fails? Recreate the deleted pin?
                                    }
                                }
                                // Wrong equations during update process
                                catch (const std::runtime_error&)
                                {
                                    fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
                                    error_time = ax::NodeEditor::GetStyle().FlowDuration;
                                }
                            }
                        }
                    }
                    ImGui::Spring(1.0f, 0.0f);
                }
                ImGui::EndVertical();
            }
            ImGui::EndHorizontal();

            if (zoom_level < 1.5f)
            {
                ImGui::BeginHorizontal("bottom");
                {
                    if (node->IsPowered())
                    {
                        ImGui::Spring(0.0f);
                        PoweredNode* powered_node = static_cast<PoweredNode*>(node.get());
                        const bool is_locked =
                            (node->ins.size() > 0 && node->ins[0]->GetLocked()) ||
                            (node->outs.size() > 0 && node->outs[0]->GetLocked());
                        if (is_locked)
                        {
                            ImGui::PushStyleColor(ImGuiCol_FrameBg, lock_purple);
                        }
                        (settings.power_equal_clocks ? powered_node->same_clock_power : powered_node->last_underclock_power).RenderInputText("##power", true, false);
                        if (is_locked)
                        {
                            ImGui::PopStyleColor();
                        }
                        ImGui::Spring(0.0f);
                        ImGui::Text("%sMW", powered_node->HasVariablePower() ? "~" : "");
                        if (powered_node->HasVariablePower() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            frame_tooltips.push_back("Average power");
                        }
                        ImGui::Spring(1.0f);
                        if (is_locked)
                        {
                            ImGui::PushStyleColor(ImGuiCol_FrameBg, lock_purple);
                        }
                        powered_node->current_rate.RenderInputText("##rate", is_locked, false, rate_width);
                        if (is_locked)
                        {
                            ImGui::PopStyleColor();
                        }
                        if (ImGui::IsItemDeactivatedAfterEdit())
                        {
                            const FractionalNumber old_rate = FractionalNumber(powered_node->current_rate.GetNumerator(), powered_node->current_rate.GetDenominator());
                            try
                            {
                                const FractionalNumber new_rate(powered_node->current_rate.GetStringFloat());
                                if (new_rate.GetNumerator() < 0)
                                {
                                    throw std::invalid_argument("Negative rate in node");
                                }
                                powered_node->UpdateRate(new_rate);
                                // Update from inputs if there is one, else from output
                                if (powered_node->ins.size() > 0)
                                {
                                    if (!UpdateNodesRate(powered_node->ins[0].get(), powered_node->ins[0]->current_rate))
                                    {
                                        powered_node->UpdateRate(old_rate);
                                    }
                                }
                                else if (powered_node->outs.size() > 0)
                                {
                                    if (!UpdateNodesRate(powered_node->outs[0].get(), powered_node->outs[0]->current_rate))
                                    {
                                        powered_node->UpdateRate(old_rate);
                                    }
                                }
                            }
                            // User entered an invalid string, reset node rate
                            catch (const std::invalid_argument&)
                            {
                                powered_node->UpdateRate(old_rate);
                            }
                            // Wrong equations during update process
                            catch (const std::runtime_error&)
                            {
                                powered_node->UpdateRate(old_rate);
                                fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
                                error_time = ax::NodeEditor::GetStyle().FlowDuration;
                            }
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            frame_tooltips.push_back(powered_node->current_rate.GetStringFraction());
                        }
                        if (node->IsGroup())
                        {
                            GroupNode* group_node = static_cast<GroupNode*>(node.get());
                            ImGui::Spring(0.0f);
                            // Override settings if it's not 0 (for example if the production chain is imported)
                            if (!settings.show_somersloop && group_node->num_somersloop.GetNumerator() == 0)
                            {
                                ImGui::Spring(1.0f);
                            }
                            else
                            {
                                ImGui::Spring(1.0f);
                                ImGui::SetNextItemWidth(ImGui::CalcTextSize(group_node->num_somersloop.GetStringFraction().c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f);
                                ImGui::BeginDisabled();
                                ImGui::InputText("##somersloop", &group_node->num_somersloop.GetStringFraction(), ImGuiInputTextFlags_CharsDecimal);
                                ImGui::EndDisabled();
                                ImGui::Spring(0.0f);
                                ImGui::Image((void*)(intptr_t)somersloop_texture_id, ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing()));
                                ImGui::Spring(0.0f);
                            }
                        }
                        else if (node->IsExtractor())
                        {
                            ExtractorNode* ex = static_cast<ExtractorNode*>(node.get());
                            const Building* b = ex->GetBuilding();
                            ImGui::Spring(0.0f);
                            ImGui::TextUnformatted(b != nullptr ? b->name.c_str() : "Extractor");
                            ImGui::Spring(1.0f);

                            // Mk-level selector for solid miners only. We use a
                            // row of SmallButtons rather than ImGui::Combo here
                            // because Combo opens a popup, and popups inside
                            // the node-editor canvas don't follow the canvas
                            // transform — they render in screen space at the
                            // wrong location.
                            const bool is_miner =
                                ex->extractor_kind == ExtractorNode::Kind::MinerMk1 ||
                                ex->extractor_kind == ExtractorNode::Kind::MinerMk2 ||
                                ex->extractor_kind == ExtractorNode::Kind::MinerMk3;
                            if (is_miner)
                            {
                                static const char* mk_labels[] = { "Mk1", "Mk2", "Mk3" };
                                const int cur_mk = static_cast<int>(ex->extractor_kind);
                                for (int m = 0; m < 3; ++m)
                                {
                                    if (m > 0) ImGui::Spring(0.0f, 0.0f);
                                    const bool selected = (cur_mk == m);
                                    if (selected)
                                    {
                                        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                                    }
                                    if (ImGui::SmallButton(mk_labels[m]) && !selected)
                                    {
                                        ex->ChangeKind(static_cast<ExtractorNode::Kind>(m));
                                        if (ex->outs.size() > 0)
                                        {
                                            if (!UpdateNodesRate(ex->outs[0].get(), ex->outs[0]->current_rate))
                                            {
                                                // Soft-fail.
                                            }
                                        }
                                    }
                                    if (selected)
                                    {
                                        ImGui::PopStyleColor();
                                    }
                                }
                                ImGui::Spring(0.0f);
                            }

                            // Purity selector (skipped for Water Extractor).
                            const bool supports_purity =
                                ex->extractor_kind != ExtractorNode::Kind::WaterExtractor;
                            if (supports_purity)
                            {
                                static const char* purity_labels[] = { "Imp", "Nor", "Pur" };
                                const int cur_p = static_cast<int>(ex->purity);
                                for (int pp = 0; pp < 3; ++pp)
                                {
                                    if (pp > 0) ImGui::Spring(0.0f, 0.0f);
                                    const bool selected = (cur_p == pp);
                                    if (selected)
                                    {
                                        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                                    }
                                    if (ImGui::SmallButton(purity_labels[pp]) && !selected)
                                    {
                                        ex->ChangePurity(static_cast<ExtractorNode::Purity>(pp));
                                        if (ex->outs.size() > 0)
                                        {
                                            if (!UpdateNodesRate(ex->outs[0].get(), ex->outs[0]->current_rate))
                                            {
                                                // Soft-fail.
                                            }
                                        }
                                    }
                                    if (selected)
                                    {
                                        ImGui::PopStyleColor();
                                    }
                                }
                                ImGui::Spring(0.0f);
                            }

                        }
                        else if (node->IsCraft())
                        {
                            CraftNode* craft_node = static_cast<CraftNode*>(node.get());
                            ImGui::Spring(0.0f);
                            ImGui::TextUnformatted(craft_node->recipe->building->name.c_str());
                            if (// Override settings if it's not 0 (for example if the production chain is imported)
                                (!settings.show_somersloop && craft_node->num_somersloop.GetNumerator() == 0) ||
                                // Don't display somersloop if this building can't have one
                                craft_node->recipe->building->somersloop_mult.GetNumerator() == 0 ||
                                // Don't display somersloop for power generators
                                craft_node->recipe->building->power < 0.0
                            )
                            {
                                ImGui::Spring(1.0f);
                            }
                            else
                            {
                                ImGui::Spring(1.0f);
                                ImGui::SetNextItemWidth(somersloop_width);
                                if (is_locked)
                                {
                                    ImGui::PushStyleColor(ImGuiCol_FrameBg, lock_purple);
                                    ImGui::BeginDisabled();
                                }
                                ImGui::InputText("##somersloop", &craft_node->num_somersloop.GetStringFraction(), ImGuiInputTextFlags_CharsDecimal);
                                if (is_locked)
                                {
                                    ImGui::EndDisabled();
                                    ImGui::PopStyleColor();
                                }
                                if (ImGui::IsItemDeactivatedAfterEdit())
                                {
                                    const FractionalNumber old_num_somersloop = FractionalNumber(craft_node->num_somersloop.GetNumerator(), craft_node->num_somersloop.GetDenominator());
                                    try
                                    {
                                        FractionalNumber new_num_somersloop = FractionalNumber(craft_node->num_somersloop.GetStringFraction());
                                        // Only integer somersloop allowed
                                        if (new_num_somersloop.GetDenominator() != 1 || new_num_somersloop.GetNumerator() < 0)
                                        {
                                            throw std::invalid_argument("somersloop num can only be positive whole integers");
                                        }
                                        // Check we don't try to boost more than 2x
                                        // We know numerator is > 0 as otherwise somersloop input is not displayed, so it's ok to invert the fraction
                                        if (new_num_somersloop > 1 / craft_node->recipe->building->somersloop_mult)
                                        {
                                            new_num_somersloop = 1 / craft_node->recipe->building->somersloop_mult;
                                        }
                                        craft_node->num_somersloop = new_num_somersloop;
                                        craft_node->UpdateRate(craft_node->current_rate);
                                        if (craft_node->ins.size() > 0)
                                        {
                                            if (!UpdateNodesRate(craft_node->ins[0].get(), craft_node->ins[0]->current_rate))
                                            {
                                                craft_node->num_somersloop = old_num_somersloop;
                                                craft_node->UpdateRate(craft_node->current_rate);
                                            }
                                        }
                                        else if (craft_node->outs.size() > 0)
                                        {
                                            if (!UpdateNodesRate(craft_node->outs[0].get(), craft_node->outs[0]->current_rate))
                                            {
                                                craft_node->num_somersloop = old_num_somersloop;
                                                craft_node->UpdateRate(craft_node->current_rate);
                                            }
                                        }
                                    }
                                    // User entered an invalid string for somersloop, reset somersloop num (node rate wasn't changed)
                                    catch (const std::invalid_argument&)
                                    {
                                        craft_node->num_somersloop = old_num_somersloop;
                                    }
                                    // Wrong equations during update process, reset somersloop num and node rate
                                    catch (const std::runtime_error&)
                                    {
                                        craft_node->num_somersloop = old_num_somersloop;
                                        craft_node->UpdateRate(craft_node->current_rate);
                                        fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
                                        error_time = ax::NodeEditor::GetStyle().FlowDuration;
                                    }
                                }
                                ImGui::Spring(0.0f);
                                ImGui::Image((void*)(intptr_t)somersloop_texture_id, ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing()));
                                ImGui::Spring(0.0f);
                            }
                        }
                    }
                    else if (node->IsOrganizer())
                    {
                        ImGui::Spring(1.0f);
                        OrganizerNode* org_node = static_cast<OrganizerNode*>(node.get());
                        if (org_node->item != nullptr)
                        {
                            ImGui::Spring(0.0f);
                            ImGui::TextUnformatted(org_node->item->name.c_str());
                            ImGui::Spring(0.0f);
                            ImGui::Image((void*)(intptr_t)org_node->item->icon_gl_index, ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing()));
                            ImGui::Spring(0.0f);
                        }
                        ImGui::Spring(1.0f);
                    }
                    else if (node->IsSink())
                    {
                        ImGui::Spring(1.0f);
                        FractionalNumber sum_sink;
                        for (const auto& i : node->ins)
                        {
                            if (i->item != nullptr)
                            {
                                sum_sink += i->current_rate * i->item->sink_value;
                            }
                        }
                        sum_sink.RenderInputText("##points", true, false, 0.0f);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            frame_tooltips.push_back(sum_sink.GetStringFraction());
                        }
                        ImGui::Spring(0.0f);
                        ImGui::TextUnformatted("points");
                        ImGui::Spring(1.0f);
                    }
                    else if (node->IsLogistics())
                    {
                        LogisticsNode* logistics_node = static_cast<LogisticsNode*>(node.get());
                        ImGui::Spring(1.0f);
                        ImGui::TextUnformatted(logistics_node->GetDisplayName());
                        ImGui::Spring(1.0f);
                        if (logistics_node->logistics_kind == LogisticsNode::Kind::TruckStation ||
                            logistics_node->logistics_kind == LogisticsNode::Kind::TrainStation)
                        {
                            VehicleStationNode* v = static_cast<VehicleStationNode*>(logistics_node);
                            auto switch_mode = [&](VehicleStationNode::Mode m) {
                                if (v->mode == m) return;
                                // Drop the plug's route links before recreating the plug.
                                std::vector<ax::NodeEditor::LinkId> ids;
                                for (Link* rl : v->route_links) ids.push_back(rl->id);
                                for (auto id : ids) DeleteLink(id);
                                v->SetMode(m, std::bind(&ProductionApp::GetNextId, this));
                            };
                            if (zoom_level < 1.5f)
                            {
                                if (ImGui::RadioButton("Load", v->mode == VehicleStationNode::Mode::Load))
                                {
                                    switch_mode(VehicleStationNode::Mode::Load);
                                }
                                ImGui::Spring(0.0f);
                                if (ImGui::RadioButton("Unload", v->mode == VehicleStationNode::Mode::Unload))
                                {
                                    switch_mode(VehicleStationNode::Mode::Unload);
                                }
                                ImGui::Spring(1.0f);
                            }
                        }
                    }
                }
                ImGui::EndHorizontal();
            }
        }
        ImGui::EndVertical();
        ImGui::PopID();
        ax::NodeEditor::EndNode();
        for (int i = 0; i < node_pushed_style; ++i)
        {
            ax::NodeEditor::PopStyleColor();
        }
    }
}

void ProductionApp::RenderLinks()
{
    for (const auto& link : links)
    {
        ImColor link_color;
        if (link->start->current_rate != link->end->current_rate)
        {
            link_color = ImColor(1.0f, 0.0f, 0.0f); // Red
        }
        else if (link->end->node->IsSink() && (
            link->start->item == nullptr || link->end->item == nullptr ||
            link->start->item->sink_value == 0 || link->end->item->sink_value == 0)
        )
        {
            link_color = ImColor(1.0f, 0.5f, 0.0f); // Orange
        }
        else
        {
            link_color = ImColor(0.0f, 1.0f, 0.0f); // Green
        }

        ax::NodeEditor::Link(link->id, link->start_id, link->end_id, link_color);
        if (link->flow.has_value())
        {
            ax::NodeEditor::Flow(link->id, link->flow.value());
            link->flow = std::nullopt;
        }
    }

    if (settings.show_debug_ids)
    {
        const ax::NodeEditor::LinkId hovered_link = ax::NodeEditor::GetHoveredLink();
        if (hovered_link)
        {
            for (const auto& link : links)
            {
                if (link->id == hovered_link)
                {
                    frame_tooltips.push_back("link:" + std::to_string(link->id.Get()) +
                        "\nfrom pin:" + std::to_string(link->start_id.Get()) +
                        "\nto pin:" + std::to_string(link->end_id.Get()));
                    break;
                }
            }
        }
    }
}

void ProductionApp::DragLink()
{
    if (ax::NodeEditor::BeginCreate())
    {
        ax::NodeEditor::PinId input_pin_id = 0, output_pin_id = 0;
        if (ax::NodeEditor::QueryNewLink(&input_pin_id, &output_pin_id))
        {
            if (input_pin_id && output_pin_id)
            {
                Pin* start_pin = FindPin(input_pin_id);
                Pin* end_pin = FindPin(output_pin_id);
                // If either side is a fuel-only pin (Truck/Train station's
                // dedicated fuel inlet), the item flowing on the other side
                // must be one of the recognized fuel items, or null (lets the
                // user wire from an organizer chain whose item is still
                // unresolved — the actual item check fires once one end is
                // typed).
                const bool start_is_fuel_pin = IsFuelPin(start_pin);
                const bool end_is_fuel_pin = IsFuelPin(end_pin);
                bool fuel_pin_rejects = false;
                if (start_is_fuel_pin || end_is_fuel_pin)
                {
                    const Pin* other = start_is_fuel_pin ? end_pin : start_pin;
                    if (other != nullptr && other->item != nullptr && !IsFuelItem(other->item))
                    {
                        fuel_pin_rejects = true;
                    }
                }
                // Vehicle plugs are multi-link, untyped, and only connect to each
                // other (a Load-side output plug to an Unload-side input plug).
                const bool start_is_plug = IsVehiclePlug(start_pin);
                const bool end_is_plug = IsVehiclePlug(end_pin);
                const bool both_plugs = start_is_plug && end_is_plug;
                const bool one_plug = start_is_plug != end_is_plug;
                if (start_pin == nullptr ||
                    end_pin == nullptr ||
                    start_pin == end_pin ||
                    start_pin->direction == end_pin->direction ||
                    start_pin->node == end_pin->node ||
                    one_plug ||                                  // belt<->plug not allowed
                    (!both_plugs && (start_pin->link != nullptr || end_pin->link != nullptr)) ||
                    (!both_plugs && start_pin->item != nullptr && end_pin->item != nullptr && start_pin->item != end_pin->item) ||
                    (!both_plugs && start_pin->GetLocked() && end_pin->GetLocked() && start_pin->current_rate != end_pin->current_rate) ||
                    fuel_pin_rejects
                )
                {
                    ax::NodeEditor::RejectNewItem(ImColor(255, 0, 0), 2.0f);
                }
                else if (ax::NodeEditor::AcceptNewItem(ImColor(128, 255, 128), 4.0f))
                {
                    // If we are dragging from a default initialized 0 pin of an organizer node
                    // or if end pin is locked
                    // pull value instead of pushing it
                    if (((start_pin->node->IsOrganizer() || start_pin->node->IsSink() || start_pin->node->IsLogistics()) && start_pin->current_rate.GetNumerator() == 0) ||
                        end_pin->GetLocked()
                    )
                    {
                        CreateLink(end_pin, start_pin, true);
                    }
                    else
                    {
                        CreateLink(start_pin, end_pin, true);
                    }
                }
            }
        }

        input_pin_id = 0;
        if (ax::NodeEditor::QueryNewNode(&input_pin_id))
        {
            Pin* input_pin = FindPin(input_pin_id);
            // Plugs only connect to other plugs, so don't spawn a (belt) node from one.
            if (input_pin == nullptr || input_pin->link != nullptr || IsVehiclePlug(input_pin))
            {
                ax::NodeEditor::RejectNewItem(ImColor(255, 0, 0), 2.0f);
            }
            else if (ax::NodeEditor::AcceptNewItem())
            {
                new_node_pin = input_pin;
                ax::NodeEditor::Suspend();
                ImGui::OpenPopup(add_node_popup_id.data());
                ax::NodeEditor::Resume();
            }
        }
    }
    ax::NodeEditor::EndCreate();
}

void ProductionApp::DeleteNodesLinks()
{
    if (ax::NodeEditor::BeginDelete())
    {
        ax::NodeEditor::NodeId node_id = 0;
        while (ax::NodeEditor::QueryDeletedNode(&node_id))
        {
            if (ax::NodeEditor::AcceptDeletedItem())
            {
                DeleteNode(node_id);
            }
        }

        ax::NodeEditor::LinkId link_id = 0;
        while (ax::NodeEditor::QueryDeletedLink(&link_id))
        {
            if (ax::NodeEditor::AcceptDeletedItem())
            {
                DeleteLink(link_id);
            }
        }
    }
    ax::NodeEditor::EndDelete();
}

void ProductionApp::AddNewNode()
{
    ax::NodeEditor::Suspend();
    if (ax::NodeEditor::ShowBackgroundContextMenu())
    {
        new_node_pin = nullptr;
        ImGui::OpenPopup(add_node_popup_id.data());
    }
    ax::NodeEditor::Resume();

    // We can't use IsWindowAppearing to detect the first frame as
    // we need new_node_position before BeginPopup for the
    // window max size computation. popup_opened is thus needed
    if (ImGui::IsPopupOpen(add_node_popup_id.data()) && !popup_opened)
    {
        popup_opened = true;
        new_node_position = ImGui::GetMousePos();
    }

    // Callback called to clean/reset state once popup is closed
    auto on_popup_close = [&]() {
        recipe_filter = "";
        popup_opened = false;
        new_node_pin = nullptr;
    };

    ax::NodeEditor::Suspend();
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(0.0f, 0.0f),
        ImVec2(
            ImGui::GetTextLineHeightWithSpacing() * 25.0f,
            // Max height is whatever space left to the bottom of the screen, clamped between 10 and 25 lines
            std::clamp(ImGui::GetMainViewport()->Size.y - ax::NodeEditor::CanvasToScreen(new_node_position).y, ImGui::GetTextLineHeightWithSpacing() * 10.0f, ImGui::GetTextLineHeightWithSpacing() * 25.0f)
        )
    );
    enum RecipeSelectionIndex : int
    {
        None = -1,
        Merger,
        CustomSplitter,
        GameSplitter,
        Sink,
        Miner,          // resource carried in `miner_resource_selection` below
        WaterExtractor,
        OilExtractor,
        Storage,
        IndustrialStorage,
        TruckStation,
        TrainStation,
        DimensionalDepot,
        FIRST_REAL_RECIPE_INDEX,
    };
    // Solid resources the in-game Miner can extract. Listed here rather than
    // derived from satisfactory.json because the data file doesn't tag items
    // as "mineable"; this mirrors the smelter-style menu the user asked for
    // where each recipe (Iron Ingot, Copper Ingot, ...) is the entry point.
    static const char* kSolidMinerResources[] = {
        "Iron Ore",
        "Copper Ore",
        "Caterium Ore",
        "Limestone",
        "Coal",
        "Sulfur",
        "Bauxite",
        "Raw Quartz",
        "Uranium",
        "S.A.M.",
    };

    if (ImGui::BeginPopup(add_node_popup_id.data()))
    {
        int recipe_index = RecipeSelectionIndex::None;
        // Set when the user picks an entry under the "Miner" submenu — carries
        // through to the dispatch switch below to build an ExtractorNode whose
        // resource is fixed at creation time (no more auto-inherit from
        // downstream connections).
        const Item* miner_resource_selection = nullptr;
        if (ImGui::MenuItem("Merger"))
        {
            recipe_index = RecipeSelectionIndex::Merger;
        }
        if (ImGui::MenuItem("Splitter*"))
        {
            recipe_index = RecipeSelectionIndex::CustomSplitter;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", "Splitter with independant output rates");
        }
        if (ImGui::MenuItem("Splitter"))
        {
            recipe_index = RecipeSelectionIndex::GameSplitter;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", "Splitter with equals output rates");
        }
        if (ImGui::MenuItem("Sink"))
        {
            recipe_index = RecipeSelectionIndex::Sink;
        }
        if (ImGui::BeginMenu("Miner"))
        {
            for (const char* resource_name : kSolidMinerResources)
            {
                auto it = Data::Items().find(resource_name);
                if (it == Data::Items().end()) continue;
                if (ImGui::MenuItem(resource_name))
                {
                    recipe_index = RecipeSelectionIndex::Miner;
                    miner_resource_selection = it->second.get();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Water Extractor"))
        {
            recipe_index = RecipeSelectionIndex::WaterExtractor;
        }
        if (ImGui::MenuItem("Oil Extractor"))
        {
            recipe_index = RecipeSelectionIndex::OilExtractor;
        }
        if (ImGui::MenuItem("Storage Container"))
        {
            recipe_index = RecipeSelectionIndex::Storage;
        }
        if (ImGui::MenuItem("Industrial Storage"))
        {
            recipe_index = RecipeSelectionIndex::IndustrialStorage;
        }
        if (ImGui::MenuItem("Truck Station"))
        {
            recipe_index = RecipeSelectionIndex::TruckStation;
        }
        if (ImGui::MenuItem("Train Station"))
        {
            recipe_index = RecipeSelectionIndex::TrainStation;
        }
        if (ImGui::MenuItem("Dimensional Depot"))
        {
            recipe_index = RecipeSelectionIndex::DimensionalDepot;
        }
        ImGui::Separator();
        // Stores the recipe index and a "match score" to sort them in the display
        std::vector<std::pair<int, size_t>> recipe_indices;
        const std::vector<std::unique_ptr<Recipe>>& recipes = Data::Recipes();
        recipe_indices.reserve(recipes.size());
        // If this is already linked to another node
        // only display matching recipes
        if (new_node_pin != nullptr && new_node_pin->item != nullptr)
        {
            const std::string& item_name = new_node_pin->item->new_line_name;
            for (int i = 0; i < recipes.size(); ++i)
            {
                if (recipe_index != RecipeSelectionIndex::None)
                {
                    break;
                }
                const std::vector<CountedItem>& matching_pins = new_node_pin->direction == ax::NodeEditor::PinKind::Input ? recipes[i]->outs : recipes[i]->ins;
                for (int j = 0; j < matching_pins.size(); ++j)
                {
                    if (matching_pins[j].item->new_line_name == item_name)
                    {
                        recipe_indices.push_back({ i, 0 });
                        break;
                    }
                }
            }
        }
        // Otherwise display all recipes, with a filter option
        else if (recipe_index == RecipeSelectionIndex::None || (new_node_pin != nullptr && new_node_pin->item == nullptr))
        {
            if (ImGui::IsWindowAppearing())
            {
                // This automatically "click" on the input filter on the first popup frame.
                // A consequence is that it is no longer possible to reopen the menu at
                // another location without closing it first.
                // It's not necessarily a bad thing but may be a bit weird for first users.
                // Could use something to check if it's the case and reopen the popup further away ?
                // ?? ImGui::IsMouseClicked(config.ContextMenuButtonIndex) && !ImGui::IsWindowHovered() ??
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::InputTextWithHint("##recipe_filter", "Filter...", &recipe_filter);

            // If no filter, display all recipes in alphabetical order
            if (recipe_filter.empty())
            {
                for (int i = 0; i < recipes.size(); ++i)
                {
                    recipe_indices.push_back({ i, 0 });
                }
            }
            // Else display first all recipes with matching name, then matching ingredients
            else
            {
                // A recipe goes on top if it matched the search string "before" another
                // If they both matched at the same place, the alternate goes after
                auto scored_recipe_sorting = [&](const std::pair<int, size_t>& a, const std::pair<int, size_t>& b) {
                    return a.second < b.second || (a.second == b.second && !recipes[a.first]->alternate && recipes[b.first]->alternate);
                };
                for (int i = 0; i < recipes.size(); ++i)
                {
                    if (const size_t pos = recipes[i]->FindInName(recipe_filter); pos != std::string::npos)
                    {
                        recipe_indices.push_back({ i, pos });
                    }
                }
                std::stable_sort(recipe_indices.begin(), recipe_indices.end(), scored_recipe_sorting);
                const size_t num_recipe_match = recipe_indices.size();

                for (int i = 0; i < recipes.size(); ++i)
                {
                    if (recipes[i]->FindInName(recipe_filter) == std::string::npos)
                    {
                        if (const size_t pos = recipes[i]->FindInIngredients(recipe_filter); pos != std::string::npos)
                        {
                            recipe_indices.push_back({ i, pos });
                        }
                    }
                }
                std::stable_sort(recipe_indices.begin() + num_recipe_match, recipe_indices.end(), scored_recipe_sorting);
            }
        }

        ImGui::BeginTable("##recipe_selector", 3,
            ImGuiTableFlags_NoSavedSettings |
            ImGuiTableFlags_NoBordersInBody |
            ImGuiTableFlags_SizingStretchProp);
        const int col_flags =
            ImGuiTableColumnFlags_WidthStretch |
            ImGuiTableColumnFlags_NoResize |
            ImGuiTableColumnFlags_NoReorder |
            ImGuiTableColumnFlags_NoHide |
            ImGuiTableColumnFlags_NoClip |
            ImGuiTableColumnFlags_NoSort |
            ImGuiTableColumnFlags_NoHeaderWidth;
        ImGui::TableSetupColumn("##recipe_checkbox", col_flags);
        ImGui::TableSetupColumn("##recipe_names", col_flags);
        ImGui::TableSetupColumn("##items", col_flags);

        for (const auto [i, score_ignored] : recipe_indices)
        {
            if (!settings.show_spoilers && recipes[i]->is_spoiler)
            {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            if (recipes[i]->alternate && ImGui::Checkbox(("##checkbox" + recipes[i]->name).c_str(), &settings.unlocked_alts.at(recipes[i].get())))
            {
                settings_store->Save(settings);
            }
            ImGui::PopStyleVar();
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginDisabled(recipes[i]->alternate && !settings.unlocked_alts.at(recipes[i].get()));
            if (ImGui::MenuItem(recipes[i]->display_name.c_str()))
            {
                recipe_index = i + RecipeSelectionIndex::FIRST_REAL_RECIPE_INDEX;
                // Need to duplicate the EndDisabled because of the break
                ImGui::EndDisabled();
                break;
            }
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(2);

            recipes[i]->Render(false);
        }
        ImGui::EndTable();

        if (recipe_index != RecipeSelectionIndex::None)
        {
            switch (recipe_index)
            {
            case RecipeSelectionIndex::None:
                break; // Should not happen because of the if above
            case RecipeSelectionIndex::Merger:
                nodes.emplace_back(std::make_unique<MergerNode>(GetNextId(), std::bind(&ProductionApp::GetNextId, this)));
                break;
            case RecipeSelectionIndex::CustomSplitter:
                nodes.emplace_back(std::make_unique<CustomSplitterNode>(GetNextId(), std::bind(&ProductionApp::GetNextId, this)));
                break;
            case RecipeSelectionIndex::GameSplitter:
                nodes.emplace_back(std::make_unique<GameSplitterNode>(GetNextId(), std::bind(&ProductionApp::GetNextId, this)));
                break;
            case RecipeSelectionIndex::Sink:
                nodes.emplace_back(std::make_unique<SinkNode>(GetNextId(), std::bind(&ProductionApp::GetNextId, this)));
                break;
            case RecipeSelectionIndex::Storage:
                nodes.emplace_back(std::make_unique<LogisticsNode>(GetNextId(), LogisticsNode::Kind::Storage, 1, 1, std::bind(&ProductionApp::GetNextId, this)));
                break;
            case RecipeSelectionIndex::IndustrialStorage:
                nodes.emplace_back(std::make_unique<LogisticsNode>(GetNextId(), LogisticsNode::Kind::IndustrialStorage, 2, 2, std::bind(&ProductionApp::GetNextId, this)));
                break;
            case RecipeSelectionIndex::TruckStation:
                nodes.emplace_back(std::make_unique<VehicleStationNode>(GetNextId(), LogisticsNode::Kind::TruckStation, std::bind(&ProductionApp::GetNextId, this)));
                break;
            case RecipeSelectionIndex::TrainStation:
                nodes.emplace_back(std::make_unique<VehicleStationNode>(GetNextId(), LogisticsNode::Kind::TrainStation, std::bind(&ProductionApp::GetNextId, this)));
                break;
            case RecipeSelectionIndex::DimensionalDepot:
                nodes.emplace_back(std::make_unique<LogisticsNode>(GetNextId(), LogisticsNode::Kind::DimensionalDepot, 1, 0, std::bind(&ProductionApp::GetNextId, this)));
                break;
            case RecipeSelectionIndex::Miner:
            case RecipeSelectionIndex::WaterExtractor:
            case RecipeSelectionIndex::OilExtractor:
            {
                ExtractorNode::Kind ekind = ExtractorNode::Kind::MinerMk1;
                const Item* default_resource = nullptr;
                switch (recipe_index)
                {
                case RecipeSelectionIndex::Miner:
                    ekind = ExtractorNode::Kind::MinerMk1;
                    default_resource = miner_resource_selection;
                    break;
                case RecipeSelectionIndex::WaterExtractor:
                    ekind = ExtractorNode::Kind::WaterExtractor;
                    if (auto it = Data::Items().find("Water"); it != Data::Items().end()) default_resource = it->second.get();
                    break;
                case RecipeSelectionIndex::OilExtractor:
                    ekind = ExtractorNode::Kind::OilExtractor;
                    if (auto it = Data::Items().find("Crude Oil"); it != Data::Items().end()) default_resource = it->second.get();
                    break;
                }
                nodes.emplace_back(std::make_unique<ExtractorNode>(GetNextId(), ekind, default_resource, ExtractorNode::Purity::Normal, std::bind(&ProductionApp::GetNextId, this)));
                break;
            }
            default:
                nodes.emplace_back(std::make_unique<CraftNode>(GetNextId(), recipes[recipe_index - RecipeSelectionIndex::FIRST_REAL_RECIPE_INDEX].get(), std::bind(&ProductionApp::GetNextId, this)));
                break;
            }
            ax::NodeEditor::SetNodePosition(nodes.back()->id, new_node_position);
            if (new_node_pin != nullptr)
            {
                const std::vector<std::unique_ptr<Pin>>& pins = new_node_pin->direction == ax::NodeEditor::PinKind::Input ? nodes.back()->outs : nodes.back()->ins;
                int pin_index = -1;
                for (int i = 0; i < pins.size(); ++i)
                {
                    if (new_node_pin->item == nullptr || pins[i]->item == nullptr || pins[i]->item == new_node_pin->item)
                    {
                        pin_index = i;
                        break;
                    }
                }
                if (pin_index != -1)
                {
                    // If we are dragging from a default initialized 0 pin of an organizer node, pull value instead of pushing it
                    if ((new_node_pin->node->IsOrganizer() || new_node_pin->node->IsSink() || new_node_pin->node->IsLogistics()) && new_node_pin->current_rate.GetNumerator() == 0)
                    {
                        CreateLink(pins[pin_index].get(), new_node_pin, true);
                    }
                    else
                    {
                        CreateLink(new_node_pin, pins[pin_index].get(), true);
                    }
                }
            }
            on_popup_close();
        }
        ImGui::EndPopup();
    }
    else
    {
        on_popup_close();
    }
    ax::NodeEditor::Resume();
}

void ProductionApp::RenderTooltips()
{
    for (const auto& s : frame_tooltips)
    {
        ImGui::SetTooltip("%s", s.c_str());
    }
    frame_tooltips.clear();
}

void ProductionApp::RenderControlsPopup()
{
    if (ImGui::BeginTable("##controls_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
    {
        static constexpr std::array controls = {
            std::make_pair("Right click",         "Add node/Lock Pin"),
            std::make_pair("Right click + mouse", "Move view"),
            std::make_pair("Left click",          "Select node/link"),
            std::make_pair("Left click + mouse",  "Move node/link"),
            std::make_pair("Mouse wheel",         "Zoom/Unzoom"),
            std::make_pair("Del",                 "Delete selection"),
            std::make_pair("F",                   "Show selection/full graph"),
            std::make_pair("Alt",                 "Disable grid snapping"),
            std::make_pair("Arrows",              "Nudge selection"),
            std::make_pair("Ctrl + A",            "Select all nodes"),
            std::make_pair("Ctrl + D",            "Duplicate nodes"),
            std::make_pair("Ctrl + G",            "Group/Ungroup nodes"),
            std::make_pair("Ctrl + Left click",   "Add to selection"),
        };
        for (const auto [k, s] : controls)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(k);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(s).x));
            ImGui::TextUnformatted(s);
        }

        ImGui::EndTable();
    }

    if (!ImGui::IsWindowFocused())
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ProductionApp::CustomKeyControl()
{
    ImGuiIO& io = ImGui::GetIO();

    // Ctrl + A, select all
    if (!io.WantCaptureKeyboard &&
        ImGui::IsKeyPressed(ImGuiKey_A, false) &&
        io.KeyCtrl)
    {
        for (const auto& n : nodes)
        {
            ax::NodeEditor::SelectNode(n->id, true);
        }
    }

    // Ctrl + G, group/ungroup selected nodes
    if (!io.WantCaptureKeyboard &&
        ImGui::IsKeyPressed(ImGuiKey_G, false) &&
        io.KeyCtrl)
    {
        size_t num_selected = 0;
        bool group_selected = false;
        for (const auto& n : nodes)
        {
            if (ax::NodeEditor::IsNodeSelected(n->id))
            {
                num_selected += 1;
                // If we have multiple node selected, it's a group action
                if (num_selected > 1)
                {
                    break;
                }
                group_selected = n->IsGroup();
                // If we have at least one not group node, it's a group action
                if (!group_selected)
                {
                    break;
                }
            }
        }

        if (num_selected == 1 && group_selected)
        {
            UngroupSelectedNode();
        }
        else if (num_selected > 0)
        {
            GroupSelectedNodes();
        }
    }

    // Ctrl + D, duplicate selected nodes (group them and ungroup them immediately)
    if (!io.WantCaptureKeyboard &&
        ImGui::IsKeyPressed(ImGuiKey_D, false) &&
        io.KeyCtrl)
    {
        DuplicateSelectedNodes();
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_::ImGuiMouseButton_Right) &&
        io.MouseDelta.x == 0.0f &&
        io.MouseDelta.y == 0.0f)
    {
        const ax::NodeEditor::PinId hovered_pin = ax::NodeEditor::GetHoveredPin();
        const ax::NodeEditor::NodeId hovered_node = ax::NodeEditor::GetHoveredNode();
        if (hovered_pin)
        {
            Pin* pin = FindPin(hovered_pin);
            if (pin != nullptr)
            {
                pin->SetLocked(!pin->GetLocked());
            }
        }
        else if (hovered_node)
        {
            for (const auto& n : nodes)
            {
                if (n->id == hovered_node)
                {
                    const bool is_locked =
                        (n->ins.size() > 0 && n->ins[0]->GetLocked()) ||
                        (n->outs.size() > 0 && n->outs[0]->GetLocked());
                    for (const auto& p : n->ins)
                    {
                        p->SetLocked(!is_locked);
                    }
                    for (const auto& p : n->outs)
                    {
                        p->SetLocked(!is_locked);
                    }
                    break;
                }
            }
        }
    }
}

void ProductionApp::FocusNextRecipe(const std::string& recipe)
{
    if (recipe != last_clicked_recipe)
    {
        last_clicked_recipe = recipe;
        next_clicked_recipe = 0;
    }

    // Get all matching nodes
    std::vector<const Node*> matching_nodes;
    for (const auto& n : nodes)
    {
        if (n->IsCraft() && static_cast<CraftNode*>(n.get())->recipe->name == recipe)
        {
            matching_nodes.push_back(n.get());
        }
    }

    if (matching_nodes.size() == 0)
    {
        return;
    }

    ax::NodeEditor::SelectNode(matching_nodes[next_clicked_recipe % matching_nodes.size()]->id);
    ax::NodeEditor::NavigateToSelection();
    ax::NodeEditor::DeselectNode(matching_nodes[next_clicked_recipe % matching_nodes.size()]->id);
    next_clicked_recipe = (next_clicked_recipe + 1) % matching_nodes.size();
}

void ProductionApp::FocusNextItem(const std::string& item)
{
    if (item != last_clicked_item)
    {
        last_clicked_item = item;
        next_clicked_item = 0;
    }

    // Get all matching nodes
    std::vector<const Node*> matching_nodes;
    for (const auto& n : nodes)
    {
        bool has_item = false;
        for (const auto& p : n->ins)
        {
            if (p->item != nullptr && p->item->name == item)
            {
                has_item = true;
                break;
            }
        }
        if (!has_item)
        {
            for (const auto& p : n->outs)
            {
                if (p->item != nullptr && p->item->name == item)
                {
                    has_item = true;
                    break;
                }
            }
        }

        if (has_item)
        {
            matching_nodes.push_back(n.get());
        }
    }

    if (matching_nodes.size() == 0)
    {
        return;
    }

    ax::NodeEditor::SelectNode(matching_nodes[next_clicked_item % matching_nodes.size()]->id);
    ax::NodeEditor::NavigateToSelection();
    ax::NodeEditor::DeselectNode(matching_nodes[next_clicked_item % matching_nodes.size()]->id);
    next_clicked_item = (next_clicked_item + 1) % matching_nodes.size();
}

void ProductionApp::FocusNextSomersloop()
{
    // Get all matching nodes
    std::vector<const Node*> matching_nodes;
    for (const auto& n : nodes)
    {
        if (!n->IsPowered())
        {
            continue;
        }
        const PoweredNode* node = static_cast<const PoweredNode*>(n.get());
        if (node->num_somersloop.GetNumerator() > 0)
        {
            matching_nodes.push_back(n.get());
        }
    }

    if (matching_nodes.size() == 0)
    {
        return;
    }

    ax::NodeEditor::SelectNode(matching_nodes[next_clicked_somersloop % matching_nodes.size()]->id);
    ax::NodeEditor::NavigateToSelection();
    ax::NodeEditor::DeselectNode(matching_nodes[next_clicked_somersloop % matching_nodes.size()]->id);
    next_clicked_somersloop = (next_clicked_somersloop + 1) % matching_nodes.size();
}

/******************************************************\
*                  Save (.sav) import                  *
\******************************************************/

void ProductionApp::RefreshDiscoveredWorlds()
{
    discovered_worlds.clear();
#if !defined(__EMSCRIPTEN__)
    if (settings.sav_watch_dir.empty())
    {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(settings.sav_watch_dir, ec))
    {
        return;
    }
    std::vector<std::string> stems;
    for (const auto& entry : std::filesystem::directory_iterator(settings.sav_watch_dir, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".sav") continue;

        stems.push_back(entry.path().stem().string());
    }
    discovered_worlds = DiscoverWorldNames(stems);
#endif
}

std::string ProductionApp::RunSavParserDesktop(const std::string& sav_path, std::string& err)
{
    // Subprocess plumbing lives in SavRunner so the Vehicle Map tool can share it.
    return SavRunner::RunSavWrapper(sav_path, settings.node_executable_path, err);
}

void ProductionApp::ImportSavFromJson(const std::string& wrapper_json)
{
    SavImport::ParseResult parsed = SavImport::ParseWrapperJson(wrapper_json);
    if (!parsed.ok)
    {
        sav_last_error = parsed.error.empty() ? "Failed to parse wrapper JSON" : parsed.error;
        return;
    }

    SavImport::BuildOutput built;
    std::string err;
    SavImport::BuildOptions build_options;
    build_options.layout_mode = settings.sav_import_layout_mode;
    build_options.world_spacing_scale = settings.sav_import_world_spacing_scale;
    build_options.connect_vehicle_routes = settings.sav_import_connect_vehicle_routes;
    if (!SavImport::BuildGraph(parsed,
            std::bind(&ProductionApp::GetNextId, this),
            built,
            err,
            build_options))
    {
        sav_last_error = err.empty() ? "Failed to build graph" : err;
        return;
    }

    if (built.nodes.empty())
    {
        sav_last_error = "Imported save contained no convertible buildings";
        return;
    }

    // Wrap into a fresh GroupNode and append to the canvas.
    auto group = std::make_unique<GroupNode>(GetNextId(),
        std::bind(&ProductionApp::GetNextId, this),
        std::move(built.nodes),
        std::move(built.links));

    // Position the new group offset from any existing node so it does not
    // overlap user's current work. Bottom-right of the existing bounding box.
    float max_x = 0.0f;
    float max_y = 0.0f;
    bool has_node = false;
    for (const auto& n : nodes)
    {
        const ImVec2 p = ax::NodeEditor::GetNodePosition(n->id);
        if (!has_node || p.x > max_x) max_x = p.x;
        if (!has_node || p.y > max_y) max_y = p.y;
        has_node = true;
    }
    const ImVec2 placement = has_node
        ? ImVec2(max_x + 400.0f, max_y)
        : ImVec2(0.0f, 0.0f);
    group->pos = placement;

    nodes.push_back(std::move(group));
    ax::NodeEditor::SetNodePosition(nodes.back()->id, placement);

    sav_last_warnings = std::move(built.warnings);
    if (!sav_last_warnings.empty())
    {
        sav_last_error = "Imported with " + std::to_string(sav_last_warnings.size()) + " warning(s)";
    }
    else
    {
        sav_last_error.clear();
    }
    sav_last_import_time = ImGui::GetTime();
}

void ProductionApp::ImportSavFile(const std::string& sav_path)
{
    sav_last_imported_path = sav_path;

#if defined(__EMSCRIPTEN__)
    (void)sav_path;
    sav_last_error = "Web build does not import directly from a path (use Import .sav button)";
#else
    std::string err;
    const std::string json = RunSavParserDesktop(sav_path, err);
    if (!err.empty() || json.empty())
    {
        sav_last_error = err.empty() ? "Parser produced no output" : err;
        return;
    }
    ImportSavFromJson(json);
#endif
}

void ProductionApp::DrainPendingImports()
{
    std::vector<std::string> pending;
    save_watcher.TakePending(pending);
    for (const auto& path : pending)
    {
        ImportSavFile(path);
    }
}

void ProductionApp::RenderSavImportSection()
{
    if (!ImGui::CollapsingHeader("Save Import"))
    {
        return;
    }

    ImGui::TextUnformatted("Save folder:");
    if (ImGui::InputText("##sav_folder", &settings.sav_watch_dir))
    {
        settings_store->Save(settings);
        save_watcher.Reconfigure(settings.sav_watch_dir, settings.sav_watch_world);
        RefreshDiscoveredWorlds();
    }

    // World selector
    const std::string label = settings.sav_watch_world.empty()
        ? std::string("Select world to track  (all worlds)")
        : "Select world to track  [" + settings.sav_watch_world + "]";
    if (ImGui::Button(label.c_str()))
    {
        RefreshDiscoveredWorlds();
        ImGui::OpenPopup("##sav_world_popup");
    }
    if (ImGui::BeginPopup("##sav_world_popup"))
    {
        if (ImGui::MenuItem("(all worlds)", nullptr, settings.sav_watch_world.empty()))
        {
            settings.sav_watch_world.clear();
            settings_store->Save(settings);
            save_watcher.Reconfigure(settings.sav_watch_dir, settings.sav_watch_world);
        }
        ImGui::Separator();
        if (discovered_worlds.empty())
        {
            ImGui::TextDisabled("No .sav files in folder");
        }
        for (const std::string& w : discovered_worlds)
        {
            if (ImGui::MenuItem(w.c_str(), nullptr, settings.sav_watch_world == w))
            {
                settings.sav_watch_world = w;
                settings_store->Save(settings);
                save_watcher.Reconfigure(settings.sav_watch_dir, settings.sav_watch_world);
            }
        }
        ImGui::EndPopup();
    }

    int layout_mode = settings.sav_import_layout_mode == SavImport::LayoutMode::World ? 1 : 0;
    if (ImGui::RadioButton("Compact layout", layout_mode == 0))
    {
        settings.sav_import_layout_mode = SavImport::LayoutMode::Compact;
        settings_store->Save(settings);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("World layout", layout_mode == 1))
    {
        settings.sav_import_layout_mode = SavImport::LayoutMode::World;
        settings_store->Save(settings);
    }
    if (settings.sav_import_layout_mode == SavImport::LayoutMode::World)
    {
        ImGui::SetNextItemWidth(ImGui::GetTextLineHeightWithSpacing() * 6.0f);
        if (ImGui::InputFloat("World spacing", &settings.sav_import_world_spacing_scale, 0.01f, 0.05f, "%.3f"))
        {
            if (settings.sav_import_world_spacing_scale <= 0.0f)
            {
                settings.sav_import_world_spacing_scale = SavImport::kPositionScale;
            }
            settings_store->Save(settings);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", "Multiplier applied to .sav world coordinates before placing nodes");
        }
    }

    if (ImGui::Checkbox("Connect vehicle routes", &settings.sav_import_connect_vehicle_routes))
    {
        settings_store->Save(settings);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", "Wire truck/train routes as links between stations (loader \xE2\x86\x92 unloader).\nDirection is inferred from each station's belts; ambiguous legs are skipped.");
    }

#if !defined(__EMSCRIPTEN__)
    if (ImGui::Checkbox("Auto-import new saves", &settings.sav_watch_enabled))
    {
        settings_store->Save(settings);
        if (settings.sav_watch_enabled)
        {
            save_watcher.Reconfigure(settings.sav_watch_dir, settings.sav_watch_world);
            save_watcher.Start();
        }
        else
        {
            save_watcher.Stop();
        }
    }

    if (ImGui::Button("Import latest now"))
    {
        // Pick the most recently modified .sav matching the world filter and import it.
        std::error_code ec;
        if (std::filesystem::is_directory(settings.sav_watch_dir, ec))
        {
            std::filesystem::path best;
            std::filesystem::file_time_type best_time{};
            bool found = false;
            for (const auto& entry : std::filesystem::directory_iterator(settings.sav_watch_dir, ec))
            {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() != ".sav") continue;
                if (!settings.sav_watch_world.empty())
                {
                    const std::string filename = entry.path().filename().string();
                    const std::string needle = settings.sav_watch_world + "_";
                    if (filename.size() < needle.size() ||
                        filename.compare(0, needle.size(), needle) != 0)
                    {
                        continue;
                    }
                }
                std::error_code time_ec;
                auto t = std::filesystem::last_write_time(entry.path(), time_ec);
                if (time_ec) continue;
                if (!found || t > best_time)
                {
                    best = entry.path();
                    best_time = t;
                    found = true;
                }
            }
            if (found)
            {
                ImportSavFile(best.string());
            }
            else
            {
                sav_last_error = "No matching .sav files in folder";
            }
        }
        else
        {
            sav_last_error = "Save folder does not exist";
        }
    }
#endif

    if (!sav_last_imported_path.empty())
    {
        ImGui::TextDisabled("Last: %s", sav_last_imported_path.c_str());
    }
    if (!sav_last_error.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", sav_last_error.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy##sav_err"))
        {
            ImGui::SetClipboardText(sav_last_error.c_str());
        }
        if (!sav_last_warnings.empty())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("View warnings##sav_warn"))
            {
                ImGui::OpenPopup("Sav Import Warnings");
            }
        }
    }

    if (ImGui::BeginPopup("Sav Import Warnings"))
    {
        ImGui::Text("%d warning(s)", static_cast<int>(sav_last_warnings.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy all##sav_warn_all"))
        {
            std::string joined;
            joined.reserve(sav_last_warnings.size() * 64);
            for (const auto& w : sav_last_warnings)
            {
                joined += w;
                joined += '\n';
            }
            ImGui::SetClipboardText(joined.c_str());
        }
        ImGui::Separator();
        const float listing_h = ImGui::GetTextLineHeightWithSpacing() * 20.0f;
        if (ImGui::BeginChild("##sav_warn_list", ImVec2(600.0f, listing_h)))
        {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sav_last_warnings.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    ImGui::TextUnformatted(sav_last_warnings[i].c_str());
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
}
