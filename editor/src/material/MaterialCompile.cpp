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
            if (type == TypeIdOf<Veng::f32>())
            {
                return 1;
            }
            if (type == TypeIdOf<Veng::vec2>())
            {
                return 2;
            }
            if (type == TypeIdOf<Veng::vec3>())
            {
                return 3;
            }
            if (type == TypeIdOf<Veng::vec4>())
            {
                return 4;
            }
            return 0;
        }

        Veng::string VecTypeName(Veng::u32 arity)
        {
            switch (arity)
            {
            case 1:
                return "float";
            case 2:
                return "vec2";
            case 3:
                return "vec3";
            case 4:
                return "vec4";
            default:
                return "float";
            }
        }

        // A param field's leaf type, derived from its byte Size. A uint param also
        // has Size 4, so it shares the f32 leaf; the emitted JSON arity is the same.
        Veng::TypeId ParamFieldType(Veng::u32 size)
        {
            switch (size)
            {
            case 8:
                return TypeIdOf<Veng::vec2>();
            case 12:
                return TypeIdOf<Veng::vec3>();
            case 16:
                return TypeIdOf<Veng::vec4>();
            default:
                return TypeIdOf<Veng::f32>();
            }
        }

        // Reads a Param node's authored value (its vec4 property) into a 4-element
        // buffer; absent components are zero.
        void ReadParamValue(const NodeGraph& graph, NodeId node, Veng::f32 out[4])
        {
            const std::span<const std::byte> bytes = graph.PropertyBytes(node);
            Veng::f32 v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (bytes.size() >= sizeof(v))
            {
                std::memcpy(v, bytes.data(), sizeof(v));
            }
            for (Veng::usize i = 0; i < 4; ++i)
            {
                out[i] = v[i];
            }
        }

        // Reads a TextureSample node's Texture property AssetId (the leading u64).
        Veng::u64 ReadTextureId(const NodeGraph& graph, NodeId node)
        {
            const std::span<const std::byte> bytes = graph.PropertyBytes(node);
            Veng::u64 id = 0;
            if (bytes.size() >= sizeof(id))
            {
                std::memcpy(&id, bytes.data(), sizeof(id));
            }
            return id;
        }

        Veng::string DomainKey(Veng::MaterialDomain domain)
        {
            switch (domain)
            {
            case Veng::MaterialDomain::Surface:
                return "surface";
            case Veng::MaterialDomain::PostProcess:
                return "postprocess";
            }
            VE_ASSERT(false, "WriteMaterialVmat: unhandled MaterialDomain {}",
                      static_cast<Veng::u32>(domain));
        }
    }

    Veng::Result<Veng::vector<CompiledField>>
    CompileMaterialGraph(const NodeGraph& graph, const NodeCatalog& catalog,
                         const MaterialShaderInterface& shader, Veng::MaterialDomain domain)
    {
        (void)domain; // the domain shapes the output node's sinks, not the bound field set

        const NodeType* textureSample = catalog.Find(TextureSampleTypeName);
        const NodeType* param = catalog.Find(ParamTypeName);
        const NodeType* outputType = catalog.Find(MaterialOutputTypeName);
        if (textureSample == nullptr || param == nullptr || outputType == nullptr)
        {
            return std::unexpected(
                "CompileMaterialGraph: the catalog lacks the material node types");
        }

        bool outputCount = false;
        for (const NodeId node : graph.Nodes())
        {
            if (graph.GetTypeOf(node) == outputType->Id)
            {
                if (outputCount)
                {
                    return std::unexpected(
                        "CompileMaterialGraph: more than one MaterialOutput node");
                }
                outputCount = true;
            }
        }
        if (!outputCount)
        {
            return std::unexpected("CompileMaterialGraph: no MaterialOutput node in the graph");
        }

        // Collect feeders in node-creation order so the synthesized graph round-trips
        // its source field list; the domain output node is the contract endpoint only.
        Veng::vector<NodeId> textureFeeders;
        Veng::vector<NodeId> paramFeeders;
        for (const NodeId node : graph.Nodes())
        {
            const NodeTypeId nodeType = graph.GetTypeOf(node);
            if (nodeType == textureSample->Id)
            {
                textureFeeders.push_back(node);
            }
            else if (nodeType == param->Id)
            {
                paramFeeders.push_back(node);
            }
        }

        Veng::vector<CompiledField> fields;
        Veng::usize textureCursor = 0;
        Veng::usize paramCursor = 0;

        for (const Veng::MaterialField& shaderField : shader.Fields)
        {
            switch (shaderField.Kind)
            {
            case Veng::MaterialField::FieldKind::TextureHandle:
            {
                CompiledField texture;
                texture.Name = shaderField.Name;
                texture.Type = "texture";
                if (textureCursor < textureFeeders.size())
                {
                    texture.TextureId = ReadTextureId(graph, textureFeeders[textureCursor++]);
                }
                fields.push_back(std::move(texture));
                break;
            }
            case Veng::MaterialField::FieldKind::SamplerHandle:
            {
                // A sampler binds the texture it samples; the cooked field carries
                // no pairing, so reconstruct it by the <Texture>Sampler convention.
                CompiledField sampler;
                sampler.Name = shaderField.Name;
                sampler.Type = "sampler";
                const Veng::string suffix = "Sampler";
                if (shaderField.Name.size() > suffix.size() &&
                    shaderField.Name.compare(shaderField.Name.size() - suffix.size(), suffix.size(),
                                             suffix) == 0)
                {
                    sampler.SamplerTexture =
                        shaderField.Name.substr(0, shaderField.Name.size() - suffix.size());
                }
                fields.push_back(std::move(sampler));
                break;
            }
            case Veng::MaterialField::FieldKind::Param:
            {
                const Veng::u32 targetArity = ArityOf(ParamFieldType(shaderField.Size));
                if (targetArity == 0)
                {
                    return std::unexpected("CompileMaterialGraph: param field '" +
                                           shaderField.Name + "' has an unsupported size");
                }

                Veng::f32 value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (paramCursor < paramFeeders.size())
                {
                    ReadParamValue(graph, paramFeeders[paramCursor++], value);
                }

                CompiledField field;
                field.Name = shaderField.Name;
                field.Type = VecTypeName(targetArity);
                for (Veng::u32 i = 0; i < targetArity; ++i)
                {
                    field.Values.push_back(value[i]);
                }
                fields.push_back(std::move(field));
                break;
            }
            }
        }

        return fields;
    }

    Veng::string WriteMaterialVmat(const Veng::vector<CompiledField>& fields,
                                   const MaterialShaderInterface& shader,
                                   Veng::MaterialDomain domain)
    {
        Json out = Json::object();

        out["domain"] = DomainKey(domain);

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
                for (const Veng::f32 v : field.Values)
                {
                    values.push_back(v);
                }
                entry["value"] = std::move(values);
            }

            fieldArray.push_back(std::move(entry));
        }
        out["fields"] = std::move(fieldArray);

        return out.dump(2);
    }

    NodeGraph BuildGraphFromMaterial(const MaterialShaderInterface& shader,
                                     const NodeCatalog& catalog, const MaterialNodeTypes& types)
    {
        NodeGraph graph(
            MaterialCanConnect, [&catalog](NodeTypeId id) { return catalog.ShapeOf(id); },
            [&catalog](NodeTypeId id)
            {
                const NodeType* type = catalog.Find(id);
                return type != nullptr ? type->PropertySize : Veng::usize{0};
            });

        const NodeType* outputType = catalog.Find(types.MaterialOutput);
        VE_ASSERT(outputType != nullptr, "BuildGraphFromMaterial: catalog lacks MaterialOutput");

        const NodeId outputNode = graph.AddNode(types.MaterialOutput);
        graph.MoveNode(outputNode, Veng::vec2{600.0f, 0.0f});

        // One feeder node per non-sampler material field, laid out in a left column.
        // A feeder wires into the next free domain output sink it can connect to;
        // sinks are the domain contract (Albedo/Normal or Color), so a feeder list
        // longer than the sink set leaves the surplus feeders unconnected — they
        // still carry their authored value into compile, which binds by field order.
        Veng::f32 row = 0.0f;
        constexpr Veng::f32 RowStride = 160.0f;
        Veng::usize nextSink = 0;

        const auto wireToSink = [&](NodeId feeder, const PinType& outPin)
        {
            while (nextSink < outputType->Inputs.size())
            {
                const auto sink = static_cast<Veng::u16>(nextSink++);
                if (!MaterialCanConnect(outPin, outputType->Inputs[sink].Type))
                {
                    continue;
                }
                const Veng::VoidResult result = graph.Connect(
                    PinRef{.Node = feeder, .Pin = 0}, PinRef{.Node = outputNode, .Pin = sink});
                if (result)
                {
                    return;
                }
            }
        };

        for (const Veng::MaterialField& field : shader.Fields)
        {
            if (field.Kind == Veng::MaterialField::FieldKind::SamplerHandle)
            {
                continue; // a sampler is paired to its texture, never its own node
            }

            if (field.Kind == Veng::MaterialField::FieldKind::TextureHandle)
            {
                const NodeId sample = graph.AddNode(types.TextureSample);
                graph.MoveNode(sample, Veng::vec2{0.0f, row});

                const NodeType* sampleType = catalog.Find(types.TextureSample);
                VE_ASSERT(sampleType != nullptr && !sampleType->Properties.empty(),
                          "BuildGraphFromMaterial: TextureSample lacks its Texture property");
                const Veng::u64 id = field.TextureId;
                const auto* idBytes = reinterpret_cast<const std::byte*>(&id);
                graph.SetProperty(sample, sampleType->Properties[0],
                                  std::span<const std::byte>(idBytes, sizeof(id)));

                wireToSink(sample, sampleType->Outputs[0].Type);
            }
            else // Param
            {
                const NodeId paramNode = graph.AddNode(types.Param);
                graph.MoveNode(paramNode, Veng::vec2{0.0f, row});

                const NodeType* paramType = catalog.Find(types.Param);
                VE_ASSERT(paramType != nullptr && !paramType->Outputs.empty(),
                          "BuildGraphFromMaterial: Param lacks its Value output");
                wireToSink(paramNode, paramType->Outputs[0].Type);
            }

            row += RowStride;
        }

        return graph;
    }
}
