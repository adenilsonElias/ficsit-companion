#include "domain/json.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/node_data_resolver.hpp"
#include "domain/pin.hpp"

#include <imgui_node_editor.h>

namespace
{
    // Truck/Train stations always carry 2 cargo + 1 fuel inputs and 2 cargo outputs.
    constexpr size_t kCargoIns = 2;
    constexpr size_t kCargoOuts = 2;
}

ax::NodeEditor::PinKind VehicleStationNode::PlugDirectionFor(Mode m)
{
    return m == Mode::Load ? ax::NodeEditor::PinKind::Output : ax::NodeEditor::PinKind::Input;
}

VehicleStationNode::VehicleStationNode(const ax::NodeEditor::NodeId id, LogisticsNode::Kind logistics_kind,
    const std::function<unsigned long long int()>& id_generator)
    // kCargoIns + 1 fuel inlet (last input), kCargoOuts cargo outputs.
    : LogisticsNode(id, logistics_kind, kCargoIns + 1, kCargoOuts, id_generator),
      mode(Mode::Load)
{
    plug = std::make_unique<Pin>(id_generator(), PlugDirectionFor(mode), this, nullptr);
}

VehicleStationNode::VehicleStationNode(const ax::NodeEditor::NodeId id, LogisticsNode::Kind logistics_kind,
    Mode mode, size_t cargo_in, size_t cargo_out,
    const std::function<unsigned long long int()>& id_generator)
    // cargo_in cargo inputs + 1 fuel inlet (last input), cargo_out cargo outputs.
    : LogisticsNode(id, logistics_kind, cargo_in + 1, cargo_out, id_generator),
      mode(mode)
{
    plug = std::make_unique<Pin>(id_generator(), PlugDirectionFor(mode), this, nullptr);
}

VehicleStationNode::VehicleStationNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator,
    const Json::Value& serialized, const INodeDataResolver& resolver)
    : LogisticsNode(id, id_generator, serialized, resolver)
{
    mode = static_cast<Mode>(serialized.contains("mode") ? serialized["mode"].get<int>() : 0);
    plug = std::make_unique<Pin>(id_generator(), PlugDirectionFor(mode), this, nullptr);
    // route_links are reconstructed by the session loader after all nodes exist.
}

VehicleStationNode::~VehicleStationNode()
{
}

void VehicleStationNode::SetMode(Mode m, const std::function<unsigned long long int()>& id_generator)
{
    if (mode == m && plug != nullptr)
    {
        return;
    }
    mode = m;
    route_links.clear();
    plug = std::make_unique<Pin>(id_generator(), PlugDirectionFor(mode), this, nullptr);
}

Json::Value VehicleStationNode::Serialize() const
{
    Json::Value serialized = LogisticsNode::Serialize();
    serialized["mode"] = static_cast<int>(mode);
    return serialized;
}
