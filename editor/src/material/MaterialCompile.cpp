#include "material/MaterialCompile.h"

#include <Veng/Assert.h>
#include <Veng/Reflection/TypeId.h>

#include <nlohmann/json.hpp>

#include <cstring>

namespace VengEditor
{
    using Json = nlohmann::json;
    using Veng::TypeIdOf;

    namespace
    {
        // The component count a vecN/float leaf type encodes; 0 for a non-numeric
        // leaf. uint shares f32's arity (1) — they differ only in the emitted JSON
        // type, decided by the Param node's authored representation.
        Veng::u32 ArityOf(Veng::TypeId type)
        {
            if (type == TypeIdOf<Veng::f32>()) return 1;
            if (type == TypeIdOf<Veng::vec2>()) return 2;
            if (type == TypeIdOf<Veng::vec3>()) return 3;
            if (type == TypeIdOf<Veng::vec4>()) return 4;
            return 0;
        }

        Veng::string VecTypeName(Veng::u32 arity)
        {
            switch (arity)
            {
                case 1: return "float";
                case 2: return "vec2";
                case 3: return "vec3";
                case 4: return "vec4";
                default: return "float";
            }
        }

        // The link feeding an input pin, or nullptr when the pin is unconnected.
        const Link* IncomingLink(const NodeGraph& graph, NodeId node, Veng::u16 pin)
        {
            for (const Link& link : graph.Links())
                if (link.To.Node == node && link.To.Pin == pin)
                    return &link;
            return nullptr;
        }

        // Reads a Param node's authored value (its vec4 property) into a 4-element
        // buffer; absent components are zero.
        void ReadParamValue(const NodeGraph& graph, NodeId node, Veng::f32 out[4])
        {
            const std::span<const std::byte> bytes = graph.PropertyBytes(node);
            Veng::f32 v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (bytes.size() >= sizeof(v))
                std::memcpy(v, bytes.data(), sizeof(v));
            for (Veng::usize i = 0; i < 4; ++i)
                out[i] = v[i];
        }

        // Reads a TextureSample node's Texture property AssetId (the leading u64).
        Veng::u64 ReadTextureId(const NodeGraph& graph, NodeId node)
        {
            const std::span<const std::byte> bytes = graph.PropertyBytes(node);
            Veng::u64 id = 0;
            if (bytes.size() >= sizeof(id))
                std::memcpy(&id, bytes.data(), sizeof(id));
            return id;
        }

        // The output pin type of a feeding node, by catalog lookup.
        PinType OutputPinType(const NodeGraph& graph, const NodeCatalog& catalog,
                              const Link& link)
        {
            const NodeType* type = catalog.Find(graph.GetTypeOf(link.From.Node));
            VE_ASSERT(type != nullptr && link.From.Pin < type->Outputs.size(),
                      "CompileMaterialGraph: a link names an out-of-range output pin");
            return type->Outputs[link.From.Pin].Type;
        }
    }

    Veng::Result<Veng::vector<CompiledField>> CompileMaterialGraph(
        const NodeGraph& graph, const NodeCatalog& catalog,
        const MaterialShaderInterface& shader)
    {
        const NodeType* textureSample = catalog.Find(TextureSampleTypeName);
        const NodeType* param = catalog.Find(ParamTypeName);
        const NodeType* outputType = catalog.Find(MaterialOutputTypeName);
        if (textureSample == nullptr || param == nullptr || outputType == nullptr)
            return std::unexpected("CompileMaterialGraph: the catalog lacks the material node types");

        // Find the single MaterialOutput node. TopoOrder ends with the output;
        // searching the live set is simpler and order-independent.
        NodeId outputNode{};
        bool found = false;
        for (NodeId node : graph.Nodes())
        {
            if (graph.GetTypeOf(node) == outputType->Id)
            {
                if (found)
                    return std::unexpected("CompileMaterialGraph: more than one MaterialOutput node");
                outputNode = node;
                found = true;
            }
        }
        if (!found)
            return std::unexpected("CompileMaterialGraph: no MaterialOutput node in the graph");

        Veng::vector<CompiledField> fields;

        // Each MaterialOutput input pin maps to one declared param/texture field.
        for (Veng::usize pin = 0; pin < outputType->Inputs.size(); ++pin)
        {
            const PinDesc& input = outputType->Inputs[pin];
            const Link* link = IncomingLink(graph, outputNode, static_cast<Veng::u16>(pin));
            if (link == nullptr)
                continue; // unconnected → omitted, importer keeps the default

            const NodeTypeId fromType = graph.GetTypeOf(link->From.Node);

            if (fromType == textureSample->Id)
            {
                CompiledField texture;
                texture.Name = input.Name;
                texture.Type = "texture";
                texture.TextureId = ReadTextureId(graph, link->From.Node);
                fields.push_back(std::move(texture));

                // The paired sampler, emitted implicitly by convention <Name>Sampler.
                CompiledField sampler;
                sampler.Name = input.Name + "Sampler";
                sampler.Type = "sampler";
                sampler.SamplerTexture = input.Name;
                fields.push_back(std::move(sampler));
                continue;
            }

            // A numeric value source (a Param, or any node whose output pin is a
            // scalar/vector leaf) feeds a param field. The emitted arity is the
            // target field's; coercion is read from the source output pin's type.
            const Veng::u32 targetArity = ArityOf(input.Type.Type);
            const PinType sourceType = OutputPinType(graph, catalog, *link);
            const Veng::u32 sourceArity = ArityOf(sourceType.Type);

            if (targetArity == 0 || sourceArity == 0)
            {
                return std::unexpected(
                    "CompileMaterialGraph: output pin '" + input.Name + "' is fed by an unsupported node");
            }

            Veng::f32 value[4];
            ReadParamValue(graph, link->From.Node, value);

            CompiledField field;
            field.Name = input.Name;
            field.Type = VecTypeName(targetArity);

            // Coercion: a scalar source splats across the target's components; a
            // wider source truncates to the target arity. An equal-arity source
            // copies straight through.
            if (sourceArity == 1 && targetArity > 1)
            {
                for (Veng::u32 i = 0; i < targetArity; ++i)
                    field.Values.push_back(value[0]);
            }
            else
            {
                for (Veng::u32 i = 0; i < targetArity; ++i)
                    field.Values.push_back(value[i]);
            }

            fields.push_back(std::move(field));
        }

        return fields;
    }

    Veng::string WriteMaterialVmat(const Veng::vector<CompiledField>& fields,
                                   const MaterialShaderInterface& shader)
    {
        Json out = Json::object();

        Json shaders = Json::object();
        shaders["vertex"] = shader.VertexShader.Value;
        shaders["fragment"] = shader.FragmentShader.Value;
        out["shaders"] = std::move(shaders);

        Json fieldArray = Json::array();
        for (const CompiledField& field : fields)
        {
            Json entry = Json::object();
            entry["name"] = field.Name;
            entry["type"] = field.Type;

            if (field.Type == "texture")
            {
                entry["id"] = field.TextureId;
            }
            else if (field.Type == "sampler")
            {
                entry["texture"] = field.SamplerTexture;
            }
            else if (field.Type == "uint")
            {
                entry["value"] = field.UintValue;
            }
            else if (field.Type == "float")
            {
                entry["value"] = field.Values.empty() ? 0.0f : field.Values[0];
            }
            else
            {
                Json values = Json::array();
                for (Veng::f32 v : field.Values)
                    values.push_back(v);
                entry["value"] = std::move(values);
            }

            fieldArray.push_back(std::move(entry));
        }
        out["fields"] = std::move(fieldArray);

        return out.dump(2);
    }

    NodeGraph BuildGraphFromMaterial(const MaterialShaderInterface& shader,
                                     const NodeCatalog& catalog,
                                     const MaterialNodeTypes& types)
    {
        NodeGraph graph(
            MaterialCanConnect,
            [&catalog](NodeTypeId id) { return catalog.ShapeOf(id); },
            [&catalog](NodeTypeId id) {
                const NodeType* type = catalog.Find(id);
                return type != nullptr ? type->PropertySize : Veng::usize{0};
            });

        const NodeType* outputType = catalog.Find(types.MaterialOutput);
        VE_ASSERT(outputType != nullptr, "BuildGraphFromMaterial: catalog lacks MaterialOutput");

        const NodeId outputNode = graph.AddNode(types.MaterialOutput);
        graph.MoveNode(outputNode, Veng::vec2{600.0f, 0.0f});

        // One feeder node per MaterialOutput input pin, laid out in a left column.
        Veng::f32 row = 0.0f;
        constexpr Veng::f32 RowStride = 160.0f;

        for (Veng::usize pin = 0; pin < outputType->Inputs.size(); ++pin)
        {
            const PinDesc& input = outputType->Inputs[pin];

            // The matching field by name carries the Kind and (for textures) the id.
            const Veng::MaterialField* field = nullptr;
            for (const Veng::MaterialField& f : shader.Fields)
            {
                if (f.Name == input.Name)
                {
                    field = &f;
                    break;
                }
            }
            VE_ASSERT(field != nullptr,
                      "BuildGraphFromMaterial: an output pin has no matching field");

            if (field->Kind == Veng::MaterialField::FieldKind::TextureHandle)
            {
                const NodeId sample = graph.AddNode(types.TextureSample);
                graph.MoveNode(sample, Veng::vec2{0.0f, row});

                const NodeType* sampleType = catalog.Find(types.TextureSample);
                VE_ASSERT(sampleType != nullptr && !sampleType->Properties.empty(),
                          "BuildGraphFromMaterial: TextureSample lacks its Texture property");
                const Veng::u64 id = field->TextureId;
                const std::byte* idBytes = reinterpret_cast<const std::byte*>(&id);
                graph.SetProperty(sample, sampleType->Properties[0],
                                  std::span<const std::byte>(idBytes, sizeof(id)));

                const PinRef from{sample, 0}; // Color
                const PinRef to{outputNode, static_cast<Veng::u16>(pin)};
                const Veng::VoidResult result = graph.Connect(from, to);
                VE_ASSERT(static_cast<bool>(result),
                          "BuildGraphFromMaterial: failed to wire a texture pin");
            }
            else // Param
            {
                const NodeId paramNode = graph.AddNode(types.Param);
                graph.MoveNode(paramNode, Veng::vec2{0.0f, row});

                const PinRef from{paramNode, 0}; // Value
                const PinRef to{outputNode, static_cast<Veng::u16>(pin)};
                const Veng::VoidResult result = graph.Connect(from, to);
                VE_ASSERT(static_cast<bool>(result),
                          "BuildGraphFromMaterial: failed to wire a param pin");
            }

            row += RowStride;
        }

        return graph;
    }
}
