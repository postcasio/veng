#include <VengGraph/NodeGraphSerialize.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Reflection/TypeId.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace VengGraph
{
    using Json = nlohmann::json;

    namespace
    {
        // Maps a builtin leaf field to a component count and element repr for JSON:
        // Scalar → 1 element, vecN → N, quat → 4. Enum and AssetHandle are handled
        // separately. The node-property restriction to builtin leaves keeps this switch
        // closed and registry-free.
        struct LeafLayout
        {
            enum class Repr : Veng::u8
            {
                F32,
                I32,
                U32
            };
            Repr Component = Repr::F32;
            Veng::usize Count = 0; // component count; 0 = not a numeric leaf
        };

        LeafLayout NumericLayoutOf(Veng::TypeId type)
        {
            using Veng::TypeIdOf;
            if (type == TypeIdOf<Veng::f32>())
            {
                return {.Component = LeafLayout::Repr::F32, .Count = 1};
            }
            if (type == TypeIdOf<Veng::i32>())
            {
                return {.Component = LeafLayout::Repr::I32, .Count = 1};
            }
            if (type == TypeIdOf<Veng::u32>())
            {
                return {.Component = LeafLayout::Repr::U32, .Count = 1};
            }
            if (type == TypeIdOf<Veng::vec2>())
            {
                return {.Component = LeafLayout::Repr::F32, .Count = 2};
            }
            if (type == TypeIdOf<Veng::vec3>())
            {
                return {.Component = LeafLayout::Repr::F32, .Count = 3};
            }
            if (type == TypeIdOf<Veng::vec4>())
            {
                return {.Component = LeafLayout::Repr::F32, .Count = 4};
            }
            if (type == TypeIdOf<Veng::quat>())
            {
                return {.Component = LeafLayout::Repr::F32, .Count = 4};
            }
            return {};
        }

        Veng::usize ComponentSize(LeafLayout::Repr repr)
        {
            switch (repr)
            {
            case LeafLayout::Repr::F32:
                return sizeof(Veng::f32);
            case LeafLayout::Repr::I32:
                return sizeof(Veng::i32);
            case LeafLayout::Repr::U32:
                return sizeof(Veng::u32);
            }
            return 0;
        }

        // Reads one numeric component out of raw bytes as JSON.
        Json ComponentToJson(const std::byte* p, LeafLayout::Repr repr)
        {
            switch (repr)
            {
            case LeafLayout::Repr::F32:
            {
                Veng::f32 v = 0.0f;
                std::memcpy(&v, p, sizeof(v));
                return v;
            }
            case LeafLayout::Repr::I32:
            {
                Veng::i32 v = 0;
                std::memcpy(&v, p, sizeof(v));
                return v;
            }
            case LeafLayout::Repr::U32:
            {
                Veng::u32 v = 0;
                std::memcpy(&v, p, sizeof(v));
                return v;
            }
            }
            return nullptr;
        }

        void JsonToComponent(const Json& value, std::byte* p, LeafLayout::Repr repr)
        {
            switch (repr)
            {
            case LeafLayout::Repr::F32:
            {
                Veng::f32 v = value.is_number() ? value.get<Veng::f32>() : 0.0f;
                std::memcpy(p, &v, sizeof(v));
                break;
            }
            case LeafLayout::Repr::I32:
            {
                Veng::i32 v = value.is_number() ? value.get<Veng::i32>() : 0;
                std::memcpy(p, &v, sizeof(v));
                break;
            }
            case LeafLayout::Repr::U32:
            {
                Veng::u32 v = value.is_number() ? value.get<Veng::u32>() : 0u;
                std::memcpy(p, &v, sizeof(v));
                break;
            }
            }
        }

        // Serializes one property field's bytes to JSON, by FieldClass.
        Json PropertyValueToJson(const std::byte* fieldPtr, const Veng::FieldDescriptor& field)
        {
            switch (field.Class)
            {
            case Veng::FieldClass::Scalar:
            {
                const LeafLayout layout = NumericLayoutOf(field.Type);
                VE_ASSERT(layout.Count == 1, "node property '{}': unsupported scalar type",
                          field.Name);
                return ComponentToJson(fieldPtr, layout.Component);
            }
            case Veng::FieldClass::Vector:
            case Veng::FieldClass::Quaternion:
            {
                const LeafLayout layout = NumericLayoutOf(field.Type);
                VE_ASSERT(layout.Count > 0, "node property '{}': unsupported vector type",
                          field.Name);
                const Veng::usize size = ComponentSize(layout.Component);
                Json array = Json::array();
                for (Veng::usize i = 0; i < layout.Count; ++i)
                {
                    array.push_back(ComponentToJson(fieldPtr + i * size, layout.Component));
                }
                return array;
            }
            case Veng::FieldClass::Enum:
            {
                // The enum's underlying integer; serialized as a number.
                Veng::i32 v = 0;
                std::memcpy(&v, fieldPtr, sizeof(v));
                return v;
            }
            case Veng::FieldClass::AssetHandle:
            {
                // The leading u64 AssetId; an invalid (zero) id serializes as
                // null — "no asset".
                Veng::u64 id = 0;
                std::memcpy(&id, fieldPtr, sizeof(id));
                if (id == 0)
                {
                    return nullptr;
                }
                return id;
            }
            case Veng::FieldClass::String:
            {
                // A node-property string is a fixed NodeNameCapacity char buffer (a
                // null-terminated, NUL-padded name); the empty string serializes as null.
                const char* chars = reinterpret_cast<const char*>(fieldPtr);
                const Veng::usize length = ::strnlen(chars, NodeNameCapacity);
                if (length == 0)
                {
                    return nullptr;
                }
                return Veng::string(chars, length);
            }
            default:
                VE_ASSERT(false, "node property '{}': FieldClass not permitted on a node",
                          field.Name);
                return nullptr;
            }
        }

        // Deserializes one JSON value into a property field's bytes by FieldClass.
        // A missing or typeless value leaves the zero-initialised default.
        void JsonToPropertyValue(const Json& value, std::byte* fieldPtr,
                                 const Veng::FieldDescriptor& field)
        {
            switch (field.Class)
            {
            case Veng::FieldClass::Scalar:
            {
                const LeafLayout layout = NumericLayoutOf(field.Type);
                if (layout.Count == 1)
                {
                    JsonToComponent(value, fieldPtr, layout.Component);
                }
                break;
            }
            case Veng::FieldClass::Vector:
            case Veng::FieldClass::Quaternion:
            {
                const LeafLayout layout = NumericLayoutOf(field.Type);
                if (layout.Count == 0 || !value.is_array())
                {
                    break;
                }
                const Veng::usize size = ComponentSize(layout.Component);
                const Veng::usize count = std::min<Veng::usize>(layout.Count, value.size());
                for (Veng::usize i = 0; i < count; ++i)
                {
                    JsonToComponent(value[i], fieldPtr + i * size, layout.Component);
                }
                break;
            }
            case Veng::FieldClass::Enum:
            {
                Veng::i32 v = value.is_number() ? value.get<Veng::i32>() : 0;
                std::memcpy(fieldPtr, &v, sizeof(v));
                break;
            }
            case Veng::FieldClass::AssetHandle:
            {
                // null/non-number leaves id zero ("no asset"); rehydration to a live
                // handle is the panel's job.
                Veng::u64 id = value.is_number_unsigned() ? value.get<Veng::u64>() : 0;
                std::memcpy(fieldPtr, &id, sizeof(id));
                break;
            }
            case Veng::FieldClass::String:
            {
                // Copy into the fixed NodeNameCapacity buffer, truncating and always
                // leaving the trailing byte NUL; a non-string value leaves the zero buffer.
                if (value.is_string())
                {
                    const Veng::string text = value.get<Veng::string>();
                    const Veng::usize count =
                        std::min<Veng::usize>(text.size(), NodeNameCapacity - 1);
                    std::memcpy(fieldPtr, text.data(), count);
                    fieldPtr[count] = std::byte{0};
                }
                break;
            }
            default:
                break;
            }
        }

        // Byte size of a node-property field, derived registry-free from FieldClass/TypeId.
        Veng::usize PropertyFieldSize(const Veng::FieldDescriptor& field)
        {
            switch (field.Class)
            {
            case Veng::FieldClass::Scalar:
            case Veng::FieldClass::Vector:
            case Veng::FieldClass::Quaternion:
            {
                const LeafLayout layout = NumericLayoutOf(field.Type);
                return layout.Count * ComponentSize(layout.Component);
            }
            case Veng::FieldClass::Enum:
                return sizeof(Veng::i32);
            case Veng::FieldClass::AssetHandle:
                return sizeof(Veng::u64);
            case Veng::FieldClass::String:
                return NodeNameCapacity;
            default:
                return 0;
            }
        }

        const PinDesc* FindPin(const Veng::vector<PinDesc>& pins, Veng::string_view name)
        {
            for (const PinDesc& pin : pins)
            {
                if (pin.Name == name)
                {
                    return &pin;
                }
            }
            return nullptr;
        }

        Veng::u16 PinIndex(const Veng::vector<PinDesc>& pins, Veng::string_view name)
        {
            for (Veng::usize i = 0; i < pins.size(); ++i)
            {
                if (pins[i].Name == name)
                {
                    return static_cast<Veng::u16>(i);
                }
            }
            return static_cast<Veng::u16>(pins.size());
        }
    }

    Veng::string WriteNodeGraph(const NodeGraph& graph, const NodeCatalog& catalog)
    {
        Json out = Json::object();
        out["version"] = NodeGraphFormatVersion;

        Json nodes = Json::array();

        // Links reference nodes by position in this array, not by runtime NodeId.
        Veng::vector<NodeId> order;
        for (const NodeId node : graph.Nodes())
        {
            order.push_back(node);
        }

        const auto serialIndexOf = [&](NodeId node) -> Veng::usize
        {
            for (Veng::usize i = 0; i < order.size(); ++i)
            {
                if (order[i] == node)
                {
                    return i;
                }
            }
            return order.size();
        };

        for (const NodeId node : order)
        {
            const NodeType* type = catalog.Find(graph.GetTypeOf(node));
            VE_ASSERT(type != nullptr, "WriteNodeGraph: a live node has an unknown type");

            Json entry = Json::object();
            entry["type"] = type->Name; // stable catalog name, not the runtime id

            const Veng::vec2 pos = graph.PositionOf(node);
            entry["position"] = Json::array({pos.x, pos.y});

            if (!type->Properties.empty())
            {
                Json props = Json::object();
                const std::span<const std::byte> bytes = graph.PropertyBytes(node);
                for (const Veng::FieldDescriptor& field : type->Properties)
                {
                    props[field.Name] = PropertyValueToJson(bytes.data() + field.Offset, field);
                }
                entry["properties"] = std::move(props);
            }

            nodes.push_back(std::move(entry));
        }

        out["nodes"] = std::move(nodes);

        Json links = Json::array();
        for (const Link& link : graph.Links())
        {
            const NodeType* fromType = catalog.Find(graph.GetTypeOf(link.From.Node));
            const NodeType* toType = catalog.Find(graph.GetTypeOf(link.To.Node));
            VE_ASSERT(fromType != nullptr && toType != nullptr,
                      "WriteNodeGraph: a link endpoint has an unknown type");
            VE_ASSERT(link.From.Pin < fromType->Outputs.size() &&
                          link.To.Pin < toType->Inputs.size(),
                      "WriteNodeGraph: a link names an out-of-range pin");

            Json entry = Json::object();
            entry["from_node"] = serialIndexOf(link.From.Node);
            entry["from_pin"] = fromType->Outputs[link.From.Pin].Name;
            entry["to_node"] = serialIndexOf(link.To.Node);
            entry["to_pin"] = toType->Inputs[link.To.Pin].Name;
            links.push_back(std::move(entry));
        }

        out["links"] = std::move(links);
        return out.dump(4);
    }

    NodeGraphReadOutcome ReadNodeGraph(Veng::string_view json, NodeGraph& dest,
                                       const NodeCatalog& catalog)
    {
        // JSON_NOEXCEPTION: a parse error yields a discarded value, not a throw.
        const Json in = Json::parse(json, nullptr, false);
        if (in.is_discarded() || !in.is_object())
        {
            return NodeGraphReadOutcome::Malformed;
        }

        Veng::i32 version = NodeGraphFormatVersion;
        if (in.contains("version") && in["version"].is_number_integer())
        {
            version = in["version"].get<Veng::i32>();
        }

        // Refuse newer documents outright; a degraded parse must not overwrite the author's data.
        if (version > NodeGraphFormatVersion)
        {
            return NodeGraphReadOutcome::VersionTooNew;
        }

        // serialIndex -> spawned NodeId; entries stay null (IsValid == false) for
        // dropped nodes (unknown type), so incident links are also dropped.
        Veng::vector<NodeId> spawned;
        Veng::vector<const NodeType*> spawnedTypes;

        if (in.contains("nodes") && in["nodes"].is_array())
        {
            for (const Json& entry : in["nodes"])
            {
                spawned.push_back(NodeId{});
                spawnedTypes.push_back(nullptr);

                if (!entry.is_object() || !entry.contains("type") || !entry["type"].is_string())
                {
                    Veng::Log::Warn("ReadNodeGraph: dropping a malformed node entry");
                    continue;
                }

                const Veng::string typeName = entry["type"].get<Veng::string>();
                const NodeType* type = catalog.Find(typeName);
                if (type == nullptr)
                {
                    Veng::Log::Warn("ReadNodeGraph: dropping node of unknown type '{}'", typeName);
                    continue;
                }

                const NodeId node = dest.AddNode(type->Id);
                spawned.back() = node;
                spawnedTypes.back() = type;

                if (entry.contains("position") && entry["position"].is_array() &&
                    entry["position"].size() == 2)
                {
                    const Json& p = entry["position"];
                    dest.MoveNode(node, Veng::vec2{p[0].get<Veng::f32>(), p[1].get<Veng::f32>()});
                }

                if (entry.contains("properties") && entry["properties"].is_object())
                {
                    const Json& props = entry["properties"];
                    for (const Veng::FieldDescriptor& field : type->Properties)
                    {
                        if (!props.contains(field.Name))
                        {
                            continue; // omitted field keeps its default
                        }

                        // Decode into a scratch buffer, then route through SetProperty.
                        Veng::vector<std::byte> scratch(PropertyFieldSize(field), std::byte{0});
                        if (scratch.empty())
                        {
                            continue;
                        }
                        JsonToPropertyValue(props[field.Name], scratch.data(), field);
                        dest.SetProperty(node, field, scratch);
                    }
                }
            }
        }

        if (in.contains("links") && in["links"].is_array())
        {
            for (const Json& entry : in["links"])
            {
                if (!entry.is_object() || !entry.contains("from_node") ||
                    !entry.contains("to_node") || !entry.contains("from_pin") ||
                    !entry.contains("to_pin"))
                {
                    Veng::Log::Warn("ReadNodeGraph: dropping a malformed link entry");
                    continue;
                }

                const Veng::usize fromIdx = entry["from_node"].get<Veng::usize>();
                const Veng::usize toIdx = entry["to_node"].get<Veng::usize>();
                if (fromIdx >= spawned.size() || toIdx >= spawned.size())
                {
                    Veng::Log::Warn("ReadNodeGraph: dropping a link with an out-of-range endpoint");
                    continue;
                }

                const NodeId fromNode = spawned[fromIdx];
                const NodeId toNode = spawned[toIdx];
                const NodeType* fromType = spawnedTypes[fromIdx];
                const NodeType* toType = spawnedTypes[toIdx];
                if (!dest.IsValid(fromNode) || !dest.IsValid(toNode) || fromType == nullptr ||
                    toType == nullptr)
                {
                    // An endpoint node was dropped (unknown type) — its links go too.
                    Veng::Log::Warn("ReadNodeGraph: dropping a link to a dropped node");
                    continue;
                }

                const Veng::string fromPin = entry["from_pin"].get<Veng::string>();
                const Veng::string toPin = entry["to_pin"].get<Veng::string>();
                if (FindPin(fromType->Outputs, fromPin) == nullptr ||
                    FindPin(toType->Inputs, toPin) == nullptr)
                {
                    Veng::Log::Warn("ReadNodeGraph: dropping a link naming an unknown pin");
                    continue;
                }

                const PinRef from{.Node = fromNode, .Pin = PinIndex(fromType->Outputs, fromPin)};
                const PinRef to{.Node = toNode, .Pin = PinIndex(toType->Inputs, toPin)};
                if (const Veng::VoidResult result = dest.Connect(from, to); !result)
                {
                    Veng::Log::Warn("ReadNodeGraph: dropping an invalid link: {}", result.error());
                }
            }
        }

        return NodeGraphReadOutcome::Loaded;
    }
}
