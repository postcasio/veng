#include <VengGraph/MaterialCompile.h>

#include <Veng/Assert.h>
#include <Veng/Reflection/TypeId.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <unordered_map>

namespace VengGraph
{
    using Json = nlohmann::json;
    using Veng::TypeIdOf;

    namespace
    {
        // The Slang type name for a numeric leaf pin type.
        Veng::string SlangTypeName(const PinType& type)
        {
            if (type.Type == TypeIdOf<Veng::f32>())
            {
                return "float";
            }
            if (type.Type == TypeIdOf<Veng::vec2>())
            {
                return "float2";
            }
            if (type.Type == TypeIdOf<Veng::vec3>())
            {
                return "float3";
            }
            return "float4";
        }

        // A zero literal of a numeric leaf pin type.
        Veng::string ZeroLiteral(const PinType& type)
        {
            if (type.Type == TypeIdOf<Veng::f32>())
            {
                return "0.0";
            }
            if (type.Type == TypeIdOf<Veng::vec2>())
            {
                return "float2(0,0)";
            }
            if (type.Type == TypeIdOf<Veng::vec3>())
            {
                return "float3(0,0,0)";
            }
            return "float4(0,0,0,0)";
        }

        // The default EmittedValue for an unconnected input pin: the surface UV for a
        // TextureSample UV pin, else a zero literal of the pin's type.
        EmittedValue DefaultForPin(const PinDesc& pin)
        {
            if (pin.Name == TextureSampleUVPin && pin.Type.Type == TypeIdOf<Veng::vec2>())
            {
                return EmittedValue{.Expr = "input.v_UV", .Type = pin.Type, .IsConst = false};
            }
            return EmittedValue{.Expr = ZeroLiteral(pin.Type), .Type = pin.Type, .IsConst = true};
        }
    }

    EmittedValue AppendTemp(EmitContext& ctx, const Veng::string& name, const PinType& type,
                            const Veng::string& expr)
    {
        ctx.Body += fmt::format("    {} {} = {};\n", SlangTypeName(type), name, expr);
        return EmittedValue{.Expr = name, .Type = type, .IsConst = false};
    }

    Veng::Result<GeneratedFragment> CompileMaterialGraph(const NodeGraph& graph,
                                                         const NodeCatalog& catalog,
                                                         const MaterialEmitTable& emit,
                                                         Veng::MaterialDomain domain)
    {
        const NodeType* outputType = catalog.Find(MaterialOutputTypeName);
        if (outputType == nullptr)
        {
            return std::unexpected(
                "CompileMaterialGraph: the catalog lacks the MaterialOutput node type");
        }

        // Locate the single MaterialOutput node; reject zero or multiple.
        NodeId outputNode{};
        bool haveOutput = false;
        for (const NodeId node : graph.Nodes())
        {
            if (graph.GetTypeOf(node) == outputType->Id)
            {
                if (haveOutput)
                {
                    return std::unexpected(
                        "CompileMaterialGraph: more than one MaterialOutput node");
                }
                outputNode = node;
                haveOutput = true;
            }
        }
        if (!haveOutput)
        {
            return std::unexpected("CompileMaterialGraph: no MaterialOutput node in the graph");
        }

        // A stable per-node key from creation order (preserved across a save/load
        // round-trip), so the same logical graph emits byte-identical text.
        std::unordered_map<Veng::u64, Veng::u32> creationIndex;
        const auto nodeBits = [](NodeId id)
        { return (static_cast<Veng::u64>(id.Generation) << 32) | id.Index; };
        {
            Veng::u32 i = 0;
            for (const NodeId node : graph.Nodes())
            {
                creationIndex[nodeBits(node)] = i++;
            }
        }
        const auto keyOf = [&](NodeId node)
        { return fmt::format("n{}", creationIndex.at(nodeBits(node))); };

        // The link feeding an input pin (the topology core forbids fan-in > 1), and the
        // count of links a (node, output-pin) feeds (fan-out → temp vs. inline).
        const std::span<const Link> links = graph.Links();
        std::unordered_map<Veng::u64, Veng::u32> outputUseCount;
        const auto pinBits = [&](NodeId node, Veng::u16 pin)
        { return (nodeBits(node) << 16) | pin; };
        for (const Link& link : links)
        {
            ++outputUseCount[pinBits(link.From.Node, link.From.Pin)];
        }

        const auto feederOf = [&](NodeId node, Veng::u16 pin) -> const Link*
        {
            for (const Link& link : links)
            {
                if (link.To.Node == node && link.To.Pin == pin)
                {
                    return &link;
                }
            }
            return nullptr;
        };

        // Per (node, output-pin) → its emitted value, filled as the walk visits nodes.
        std::unordered_map<Veng::u64, EmittedValue> pinValues;

        EmitContext ctx;

        // Resolve an input pin's incoming value: the upstream's emitted value coerced to
        // this pin's type, or the pin's default when unconnected.
        const auto resolveInput = [&](NodeId node, Veng::u16 pin,
                                      const PinDesc& pinDesc) -> EmittedValue
        {
            const Link* feeder = feederOf(node, pin);
            if (feeder == nullptr)
            {
                return DefaultForPin(pinDesc);
            }
            const auto it = pinValues.find(pinBits(feeder->From.Node, feeder->From.Pin));
            if (it == pinValues.end())
            {
                return DefaultForPin(pinDesc);
            }
            const EmittedValue& upstream = it->second;
            return EmittedValue{
                .Expr = CoerceExpr(upstream.Expr, upstream.Type, pinDesc.Type),
                .Type = pinDesc.Type,
                .IsConst = upstream.IsConst,
            };
        };

        // Walk every value-producing node in dependency order; MaterialOutput is the
        // sink, emitted into the entry point after the walk.
        for (const NodeId node : graph.TopoOrder())
        {
            if (node == outputNode)
            {
                continue;
            }

            const NodeType* type = catalog.Find(graph.GetTypeOf(node));
            if (type == nullptr)
            {
                continue; // an unknown node type contributes nothing
            }

            const NodeEmitFn* emitFn = emit.Find(type->Id);
            if (emitFn == nullptr)
            {
                return std::unexpected(
                    fmt::format("CompileMaterialGraph: node type '{}' has no emit-fn", type->Name));
            }

            Veng::vector<EmittedValue> inputs;
            inputs.reserve(type->Inputs.size());
            for (Veng::usize i = 0; i < type->Inputs.size(); ++i)
            {
                inputs.push_back(resolveInput(node, static_cast<Veng::u16>(i), type->Inputs[i]));
            }

            ctx.NodeKey = keyOf(node);
            const EmittedValue produced = (*emitFn)(inputs, graph.PropertyBytes(node), ctx);

            // One output pin per known node type. A value used more than once becomes a
            // temp (single evaluation of a shared node); a single-use value inlines.
            VE_ASSERT(type->Outputs.size() == 1,
                      "CompileMaterialGraph: node type '{}' must have exactly one output",
                      type->Name);
            const Veng::u32 uses = outputUseCount[pinBits(node, 0)];
            EmittedValue stored = produced;
            if (uses > 1)
            {
                const Veng::string tempName =
                    fmt::format("{}_{}", keyOf(node), type->Outputs[0].Name);
                stored = AppendTemp(ctx, tempName, produced.Type, produced.Expr);
            }
            pinValues[pinBits(node, 0)] = std::move(stored);
        }

        // The MaterialOutput sink values, in domain-contract order.
        Veng::vector<EmittedValue> sinks;
        sinks.reserve(outputType->Inputs.size());
        for (Veng::usize i = 0; i < outputType->Inputs.size(); ++i)
        {
            const Link* feeder = feederOf(outputNode, static_cast<Veng::u16>(i));
            if (feeder == nullptr)
            {
                // An unconnected sink keeps its domain default, applied during assembly.
                sinks.push_back(EmittedValue{
                    .Expr = Veng::string(), .Type = outputType->Inputs[i].Type, .IsConst = true});
                continue;
            }
            sinks.push_back(
                resolveInput(outputNode, static_cast<Veng::u16>(i), outputType->Inputs[i]));
        }

        // --- Assemble the generated source ---

        Veng::string source = "#include \"Veng/material.slang\"\n\n";

        // The generated MaterialParams struct, exactly the texture + exposed/engine-bound
        // param fields the walk collected. Order them large-alignment-first (descending
        // alignment, stable within a rank) so the cooker's std140 reflection and the
        // shader's scalar-layout Load<MaterialParams> resolve identical offsets — a scalar
        // before a vec4 would land the vec at a different offset under each layout and read
        // as 0.
        Veng::vector<EmittedParamField> ordered = ctx.ParamFields;
        std::ranges::stable_sort(ordered, [](const EmittedParamField& a, const EmittedParamField& b)
                                 { return a.Alignment > b.Alignment; });

        source += "struct MaterialParams\n{\n";
        for (const EmittedParamField& field : ordered)
        {
            source += fmt::format("    {} {};\n", field.SlangType, field.Name);
        }
        source += "};\n\n";
        source += "MaterialParams LoadMaterialParams(uint index)\n{\n"
                  "    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);\n"
                  "}\n\n";

        const auto sinkOr = [&](Veng::usize index, const char* fallback) -> Veng::string
        {
            if (index < sinks.size() && !sinks[index].Expr.empty())
            {
                return sinks[index].Expr;
            }
            return fallback;
        };

        if (domain == Veng::MaterialDomain::Surface)
        {
            source += "[shader(\"fragment\")]\n";
            source += "GBufferOutput fsMain(SurfaceFragmentInput input)\n{\n";
            source += "    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);\n";
            source += ctx.Body;
            source += "    GBufferOutput o;\n";
            // Sink order matches DomainOutputContract(Surface): Albedo (0), Normal (1).
            source += fmt::format("    o.Albedo = {};\n", sinkOr(0, "float4(0,0,0,1)"));
            source +=
                fmt::format("    o.Normal = float4({}, 0);\n", sinkOr(1, "input.v_WorldNormal"));
            source += "    o.ORM = float4(1, 1, 0, 0);\n";
            source += "    o.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);\n";
            source += "    return o;\n}\n";
        }
        else
        {
            source += "[shader(\"fragment\")]\n";
            source += "float4 fsMain(PostProcessFragmentInput input) : SV_Target0\n{\n";
            // A PostProcess material pushes its frame-folded selector at push-constant
            // offset 0 (Material::Bind, SelectorOffset 0). The engine header's g_PC reads
            // offset 0 as FrameBase, so that field carries the selector here — a Surface
            // shader instead reads its selector from the v_MaterialIndex interpolant.
            source += "    MaterialParams p = LoadMaterialParams(g_PC.FrameBase);\n";
            source += ctx.Body;
            source += fmt::format("    return {};\n}}\n",
                                  sinkOr(0, "g_Textures[0].Sample(g_Samplers[0], input.v_UV)"));
        }

        // --- The matching .vmat field list, from the same ordered set ---
        //
        // A handle slot emits a texture/sampler row; an exposed param a float/vecN row
        // carrying its authored default; an engine-bound param no row (the engine writes
        // it by name). The order matches the struct so the row order reads naturally,
        // though the cook matches each row to the reflected struct by name.
        Veng::vector<CompiledField> generatedFields;
        for (const EmittedParamField& field : ordered)
        {
            switch (field.Kind)
            {
            case EmittedFieldKind::TextureHandle:
                generatedFields.push_back(CompiledField{
                    .Name = field.Name, .Type = "texture", .TextureId = field.TextureId});
                break;
            case EmittedFieldKind::SamplerHandle:
                generatedFields.push_back(CompiledField{
                    .Name = field.Name, .Type = "sampler", .SamplerTexture = field.SamplerTexture});
                break;
            case EmittedFieldKind::Param:
            {
                // Engine-bound: a struct field with no authored value, not a field-list row.
                if (field.Default.empty())
                {
                    break;
                }
                const char* type = field.ComponentCount == 1   ? "float"
                                   : field.ComponentCount == 2 ? "vec2"
                                   : field.ComponentCount == 3 ? "vec3"
                                                               : "vec4";
                generatedFields.push_back(
                    CompiledField{.Name = field.Name, .Type = type, .Values = field.Default});
                break;
            }
            }
        }

        return GeneratedFragment{
            .Source = std::move(source), .Domain = domain, .Fields = std::move(generatedFields)};
    }

    Veng::string WriteMaterialVmat(const Veng::vector<CompiledField>& fields,
                                   const MaterialShaderInterface& shader,
                                   Veng::MaterialDomain domain)
    {
        Json out = Json::object();

        switch (domain)
        {
        case Veng::MaterialDomain::Surface:
            out["domain"] = "surface";
            break;
        case Veng::MaterialDomain::PostProcess:
            out["domain"] = "postprocess";
            break;
        }

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
}
