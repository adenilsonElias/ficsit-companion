#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <imgui_node_editor.h>

#include "domain/fractional_number.hpp"
#include "domain/json.hpp"
#include "app/utils.hpp"

struct Building;
struct Item;
struct Link;
struct Pin;
struct Recipe;
struct INodeDataResolver;

struct Node
{
    /// @brief Type of node, ALWAYS ADD NEW TYPES AT THE END (would break old save files otherwise)
    enum class Kind
    {
        Craft,
        CustomSplitter,
        Merger,
        Group,
        GameSplitter,
        Sink,
        Extractor,
        Logistics,
        // ALWAYS ADD NEW TYPES HERE (would break compatibility with old save files otherwise)
    };

    Node(const ax::NodeEditor::NodeId id);
    Node(const ax::NodeEditor::NodeId id, const Json::Value& serialized);
    virtual ~Node();

    virtual Kind GetKind() const = 0;
    virtual bool IsPowered() const;
    virtual bool IsCraft() const;
    virtual bool IsGroup() const;
    /// @brief Merger or Splitter
    virtual bool IsOrganizer() const;
    virtual bool IsMerger() const;
    virtual bool IsCustomSplitter() const;
    virtual bool IsGameSplitter() const;
    virtual bool IsSink() const;
    virtual bool IsExtractor() const;
    virtual bool IsLogistics() const;
    virtual Json::Value Serialize() const;

    static std::unique_ptr<Node> Deserialize(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver);

    const ax::NodeEditor::NodeId id;

    std::vector<std::unique_ptr<Pin>> ins;
    std::vector<std::unique_ptr<Pin>> outs;
    ImVec2 pos;
};

struct PoweredNode : public Node
{
    PoweredNode(const ax::NodeEditor::NodeId id);
    PoweredNode(const ax::NodeEditor::NodeId id, const Json::Value& serialized);
    virtual ~PoweredNode();

    virtual bool IsPowered() const override;
    virtual Json::Value Serialize() const override;
    virtual void UpdateRate(const FractionalNumber& new_rate) = 0;
    virtual void ComputePowerUsage() = 0;
    virtual bool HasVariablePower() const = 0;

    FractionalNumber current_rate;
    /// @brief Power requirement if all machines are at the same clock.
    /// It could be a double, but FractionalNumber already has all string operations
    FractionalNumber same_clock_power;
    /// @brief Power requirements if all machines are at 100% except the last one.
    /// It could be a double, but FractionalNumber already has all string operations
    FractionalNumber last_underclock_power;
    /// @brief Technically it could be just an int, but FractionalNumber already has all string operations
    FractionalNumber num_somersloop;
};

struct CraftNode : public PoweredNode
{
    CraftNode(const ax::NodeEditor::NodeId id, const Recipe* recipe,
        const std::function<unsigned long long int()>& id_generator);
    CraftNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver);
    virtual ~CraftNode();
    virtual Kind GetKind() const override;
    virtual bool IsCraft() const override;
    virtual Json::Value Serialize() const override;
    virtual void UpdateRate(const FractionalNumber& new_rate) override;
    virtual bool HasVariablePower() const override;
    virtual void ComputePowerUsage() override;
    void ChangeRecipe(const Recipe* recipe, const std::function<unsigned long long int()>& id_generator);

    const Recipe* recipe;
    /// @brief Custom boolean that can be used to track progress on factory building
    bool built;
};

struct GroupNode : public PoweredNode
{
    GroupNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator,
        std::vector<std::unique_ptr<Node>>&& nodes_, std::vector<std::unique_ptr<Link>>&& links_);
    GroupNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator,
        const Json::Value& serialized, const INodeDataResolver& resolver);
    virtual Kind GetKind() const override;
    virtual bool IsGroup() const override;
    virtual Json::Value Serialize() const override;
    virtual void UpdateRate(const FractionalNumber& new_rate) override;
    virtual bool HasVariablePower() const override;
    virtual void ComputePowerUsage() override;
    void PropagateRateToSubnodes();
    void SetBuiltState(const bool b);

private:
    void CreateInsOuts(const std::function<unsigned long long int()>& id_generator);
    void UpdateDetails();

public:
    std::vector<std::unique_ptr<Node>> nodes;
    /// @brief The rate of the node when this group was created.
    /// Required cause in case the group rate is set to 0 the info is lost otherwise
    std::vector<FractionalNumber> nodes_base_rate;
    std::vector<std::unique_ptr<Link>> links;

    std::string name;
    /// @brief Cached value to avoid looping through all the nodes everytime
    bool variable_power;
    std::map<std::string, FractionalNumber> total_machines;
    std::map<std::string, FractionalNumber> built_machines;
    std::map<std::string, std::map<const Recipe*, FractionalNumber>> detailed_machines;
    std::map<std::string, std::map<const Item*, FractionalNumber, ItemPtrCompare>> detailed_machine_outputs;
    std::map<const Recipe*, FractionalNumber> detailed_power_same_clock;
    std::map<const Recipe*, FractionalNumber> detailed_power_last_underclock;
    std::map<const Item*, FractionalNumber, ItemPtrCompare> inputs;
    std::map<const Item*, FractionalNumber, ItemPtrCompare> outputs;
    std::map<const Item*, FractionalNumber> detailed_sinked_points;
    bool loading_error;
};

struct OrganizerNode : public Node
{
    OrganizerNode(const ax::NodeEditor::NodeId id, const Item* item = nullptr);
    OrganizerNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver);
    virtual ~OrganizerNode();
    virtual bool IsOrganizer() const override;
    virtual Json::Value Serialize() const override;

    void ChangeItem(const Item* item);
    void RemoveItemIfNotForced();
    virtual bool IsBalanced() const;

    const Item* item;
};

struct CustomSplitterNode : public OrganizerNode
{
    CustomSplitterNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Item* item = nullptr);
    CustomSplitterNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver);
    virtual ~CustomSplitterNode();
    virtual bool IsCustomSplitter() const override;

    virtual Kind GetKind() const override;
};

struct MergerNode : public OrganizerNode
{
    MergerNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Item* item = nullptr);
    MergerNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver);
    virtual ~MergerNode();
    virtual bool IsMerger() const override;

    virtual Kind GetKind() const override;
};

struct GameSplitterNode : public OrganizerNode
{
    GameSplitterNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Item* item = nullptr);
    GameSplitterNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver);
    virtual ~GameSplitterNode();
    virtual bool IsGameSplitter() const override;

    virtual Kind GetKind() const override;
    virtual bool IsBalanced() const override;
};

struct SinkNode : public Node
{
    SinkNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Item* item = nullptr);
    SinkNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver);
    virtual ~SinkNode();
    virtual bool IsSink() const override;

    virtual Kind GetKind() const override;
    virtual Json::Value Serialize() const override;
};

struct LogisticsNode : public Node
{
    enum class Kind : int
    {
        Storage = 0,
        IndustrialStorage = 1,
        TruckStation = 2,
        TrainStation = 3,
        // ALWAYS ADD NEW TYPES HERE (appending preserves saved kind indices)
        DimensionalDepot = 4,
    };

    LogisticsNode(const ax::NodeEditor::NodeId id, LogisticsNode::Kind logistics_kind,
        size_t input_count, size_t output_count,
        const std::function<unsigned long long int()>& id_generator);
    LogisticsNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator,
        const Json::Value& serialized, const INodeDataResolver& resolver);
    virtual ~LogisticsNode();
    virtual bool IsLogistics() const override;

    virtual Node::Kind GetKind() const override;
    virtual Json::Value Serialize() const override;
    const char* GetDisplayName() const;

    LogisticsNode::Kind logistics_kind;
};

/// @brief Truck/Train station with a Load/Unload mode and a vehicle "plug"
/// (a Pin held outside ins/outs) that links station-to-station to form routes.
/// Belt layout is inherited from LogisticsNode (2 cargo-in + 1 fuel-in, 2 cargo-out).
struct VehicleStationNode : public LogisticsNode
{
    enum class Mode : int { Load = 0, Unload = 1 };

    VehicleStationNode(const ax::NodeEditor::NodeId id, LogisticsNode::Kind logistics_kind,
        const std::function<unsigned long long int()>& id_generator);
    /// @brief Build with an explicit mode and cargo pin counts (used by the
    /// .sav importer, which derives counts from observed belt ports). Allocates
    /// `cargo_in` cargo inputs + 1 fuel inlet (last input) + `cargo_out` cargo
    /// outputs, and a plug whose direction matches `mode`.
    VehicleStationNode(const ax::NodeEditor::NodeId id, LogisticsNode::Kind logistics_kind,
        Mode mode, size_t cargo_in, size_t cargo_out,
        const std::function<unsigned long long int()>& id_generator);
    VehicleStationNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator,
        const Json::Value& serialized, const INodeDataResolver& resolver);
    virtual ~VehicleStationNode();

    virtual Json::Value Serialize() const override;

    /// @brief Recreate the plug with the direction implied by the new mode and
    /// drop existing route links (the caller must delete the Link objects first).
    void SetMode(Mode m, const std::function<unsigned long long int()>& id_generator);
    static ax::NodeEditor::PinKind PlugDirectionFor(Mode m);

    Mode mode;
    /// @brief Vehicle plug. Lives outside ins/outs; its Pin::link stays unused
    /// (route links are tracked in route_links instead).
    std::unique_ptr<Pin> plug;
    /// @brief Non-owning index of route (plug<->plug) links touching this plug.
    /// The Link objects are owned by GraphModel::links.
    std::vector<Link*> route_links;
};

/// @brief Resource extractor: Miner Mk1/2/3, Water Extractor, Oil Extractor.
/// Has no inputs and a single output whose rate is base × purity × current_rate
/// (current_rate doubles as the clock multiplier, same convention CraftNode uses).
struct ExtractorNode : public PoweredNode
{
    enum class Kind : int
    {
        MinerMk1 = 0,
        MinerMk2 = 1,
        MinerMk3 = 2,
        WaterExtractor = 3,
        OilExtractor = 4,
        // ALWAYS ADD NEW TYPES HERE (would break compatibility with old save files otherwise)
    };

    enum class Purity : int
    {
        Impure = 0,
        Normal = 1,
        Pure = 2,
    };

    /// @brief Place a new extractor node. `resource` may be null (the user will
    /// pick one via the UI); the node still places and saves cleanly.
    ExtractorNode(const ax::NodeEditor::NodeId id, ExtractorNode::Kind extractor_kind,
        const Item* resource, ExtractorNode::Purity purity,
        const std::function<unsigned long long int()>& id_generator);
    ExtractorNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver);
    virtual ~ExtractorNode();

    virtual Node::Kind GetKind() const override;
    virtual bool IsExtractor() const override;
    virtual Json::Value Serialize() const override;
    virtual void UpdateRate(const FractionalNumber& new_rate) override;
    virtual bool HasVariablePower() const override;
    virtual void ComputePowerUsage() override;

    /// @brief Swap which kind of extractor this is (Mk1<->Mk3, Miner<->Oil, ...).
    /// Reuses the existing output pin if the resource is unchanged; otherwise
    /// the caller is responsible for recreating it via ChangeResource.
    void ChangeKind(ExtractorNode::Kind extractor_kind);
    /// @brief Change which item this extractor produces. Recreates the output pin.
    void ChangeResource(const Item* resource, const std::function<unsigned long long int()>& id_generator);
    /// @brief Change the resource node's purity (does not apply to Water).
    void ChangePurity(ExtractorNode::Purity purity);

    /// @brief Building (from Data::Buildings()) backing this extractor's kind.
    /// Used by power computation and UI display.
    const Building* GetBuilding() const;
    /// @brief Base rate in items/min (or m³/min) at clock 100% / Normal purity.
    FractionalNumber GetBaseRate() const;
    /// @brief Purity multiplier (1 for kinds that don't use purity, like Water).
    FractionalNumber GetPurityMultiplier() const;

    ExtractorNode::Kind extractor_kind;
    const Item* resource;
    ExtractorNode::Purity purity;
};
