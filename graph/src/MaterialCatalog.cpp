#include <VengGraph/MaterialCatalog.h>

#include <Veng/Assert.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Asset/AssetHandle.h>

#include <fmt/format.h>

#include <cstddef>
#include <cstring>

namespace VengGraph
{
    namespace
    {
        using Veng::TypeIdOf;

        // Param property POD: a vec4 value covering f32/vec2/vec3/vec4 arities (the
        // emit-fn reads the pin type to pick the emitted arity), the provenance selecting
        // how the value reaches the shader (const-fold / exposed / engine-bound), and an
        // optional authored Name the generated MaterialParams field takes (empty → the
        // node key). An engine-bound param the engine writes by name needs the exact name.
        struct ParamProps
        {
            Veng::vec4 Value{0.0f, 0.0f, 0.0f, 0.0f};
            ParamProvenance Provenance = ParamProvenance::Const;
            NodeName Name;
        };

        // TextureSample property POD: the sampled texture handle plus an optional authored
        // Name the generated handle field takes (empty → the node key). An engine-bound
        // texture (id 0) the engine writes by name needs the exact name (e.g. "Hdr").
        struct TextureSampleProps
        {
            Veng::AssetHandle<Veng::Texture> Texture;
            NodeName Name;
        };

        // The authored field name in a NodeName buffer, or an empty string when unset.
        Veng::string NameOf(const NodeName& name)
        {
            const Veng::usize length = ::strnlen(name.Chars, NodeNameCapacity);
            return Veng::string(name.Chars, length);
        }

        PinType ValuePin(Veng::TypeId id)
        {
            return PinType{.Kind = PinType::Kind::Value, .Type = id};
        }

        // The component count a vecN/float leaf type encodes; 0 for a non-numeric leaf.
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
    }

    const NodeEmitFn* MaterialEmitTable::Find(NodeTypeId id) const
    {
        const auto it = Emitters.find(id.Value);
        return it != Emitters.end() ? &it->second : nullptr;
    }

    Veng::vector<DomainOutputPin> DomainOutputContract(Veng::MaterialDomain domain)
    {
        switch (domain)
        {
        case Veng::MaterialDomain::PostProcess:
            return {
                DomainOutputPin{.Name = OutputColorPin, .Type = ValuePin(TypeIdOf<Veng::vec4>())},
            };
        case Veng::MaterialDomain::Surface:
            return {
                DomainOutputPin{.Name = OutputAlbedoPin, .Type = ValuePin(TypeIdOf<Veng::vec4>())},
                DomainOutputPin{.Name = OutputNormalPin, .Type = ValuePin(TypeIdOf<Veng::vec3>())},
            };
        }
        VE_ASSERT(false, "DomainOutputContract: unhandled MaterialDomain {}",
                  static_cast<Veng::u32>(domain));
    }

    Veng::string CoerceExpr(const Veng::string& expr, const PinType& from, const PinType& to)
    {
        if (from.Type == to.Type)
        {
            return expr;
        }

        const Veng::u32 fromArity = ArityOf(from.Type);
        const Veng::u32 toArity = ArityOf(to.Type);

        // f32 → vecN: splat the scalar across the destination's components.
        if (fromArity == 1 && toArity >= 2)
        {
            switch (toArity)
            {
            case 2:
                return fmt::format("({}).xx", expr);
            case 3:
                return fmt::format("({}).xxx", expr);
            case 4:
                return fmt::format("({}).xxxx", expr);
            default:
                break;
            }
        }

        // vecN → lower-arity vecN: truncate the trailing components.
        if (fromArity > toArity && toArity >= 2)
        {
            switch (toArity)
            {
            case 2:
                return fmt::format("({}).xy", expr);
            case 3:
                return fmt::format("({}).xyz", expr);
            default:
                break;
            }
        }

        return expr;
    }

    MaterialNodeTypes RegisterMaterialNodeTypes(NodeCatalog& catalog, MaterialEmitTable& emit,
                                                Veng::MaterialDomain domain)
    {
        MaterialNodeTypes types;

        // --- TextureSample: UV in (defaulting to the surface UV), Color (vec4) out ---
        {
            NodeType type;
            type.Name = TextureSampleTypeName;
            type.Inputs = {
                PinDesc{.Name = TextureSampleUVPin, .Type = ValuePin(TypeIdOf<Veng::vec2>())},
            };
            type.Outputs = {
                PinDesc{.Name = TextureSampleColorPin, .Type = ValuePin(TypeIdOf<Veng::vec4>())},
            };
            type.Properties = {
                Veng::FieldDescriptor{
                    .Name = TextureSampleTextureProperty,
                    .Type = TypeIdOf<Veng::AssetHandle<Veng::Texture>>(),
                    .Class = Veng::FieldClass::AssetHandle,
                    .Offset = offsetof(TextureSampleProps, Texture),
                },
                Veng::FieldDescriptor{
                    .Name = NodeNameProperty,
                    .Type = TypeIdOf<Veng::string>(),
                    .Class = Veng::FieldClass::String,
                    .Offset = offsetof(TextureSampleProps, Name),
                    // A node-property string is a fixed inline char buffer, not a Veng::string
                    // object; the inspector's String widget reinterprets the field as a real
                    // string, so the name is authored in the graph JSON and hidden from the UI.
                    .Hidden = true,
                },
            };
            type.PropertySize = sizeof(TextureSampleProps);
            types.TextureSample = catalog.Register(std::move(type));
        }

        // --- Param: typed Value (vec4) out, vec4 Value property ---
        {
            NodeType type;
            type.Name = ParamTypeName;
            type.Outputs = {
                PinDesc{.Name = ParamValuePin, .Type = ValuePin(TypeIdOf<Veng::vec4>())},
            };
            type.Properties = {
                Veng::FieldDescriptor{
                    .Name = ParamValueProperty,
                    .Type = TypeIdOf<Veng::vec4>(),
                    .Class = Veng::FieldClass::Vector,
                    .Offset = offsetof(ParamProps, Value),
                },
                Veng::FieldDescriptor{
                    .Name = ParamProvenanceProperty,
                    .Type = TypeIdOf<ParamProvenance>(),
                    .Class = Veng::FieldClass::Enum,
                    .Offset = offsetof(ParamProps, Provenance),
                },
                Veng::FieldDescriptor{
                    .Name = NodeNameProperty,
                    .Type = TypeIdOf<Veng::string>(),
                    .Class = Veng::FieldClass::String,
                    .Offset = offsetof(ParamProps, Name),
                    // The fixed inline name buffer is authored in the graph JSON, not the
                    // inspector — the String widget reinterprets the field as a Veng::string.
                    .Hidden = true,
                },
            };
            type.PropertySize = sizeof(ParamProps);
            types.Param = catalog.Register(std::move(type));
        }

        // --- MaterialOutput: one input pin per domain output-contract sink ---
        // Surface's sinks are the g-buffer channels (Albedo + Normal); PostProcess's
        // is the single final Color. The sinks express the domain's fixed output
        // contract.
        {
            NodeType type;
            type.Name = MaterialOutputTypeName;
            for (const DomainOutputPin& sink : DomainOutputContract(domain))
            {
                type.Inputs.push_back(PinDesc{.Name = sink.Name, .Type = sink.Type});
            }
            type.PropertySize = 0;
            types.MaterialOutput = catalog.Register(std::move(type));
        }

        // --- Emit-fns. MaterialOutput produces no value of its own (the walk
        // special-cases it into the entry point), so only the value-producing types
        // carry one. Each names node-unique MaterialParams fields from the walk's
        // NodeKey and contributes them (with provenance) to ctx.ParamFields, from which
        // the walk emits the final struct + matching .vmat field list. ---

        const Veng::TypeId vec4Type = TypeIdOf<Veng::vec4>();

        // A TextureSample samples the bindless texture and sampler named by its two
        // handle slots, with the UV input defaulting to the surface UV when unconnected.
        // Its texture handle slot carries the node's authored texture AssetId; the
        // paired sampler slot references it by name.
        emit.Emitters[types.TextureSample.Value] =
            [vec4Type](std::span<const EmittedValue> inputs, std::span<const std::byte> props,
                       EmitContext& ctx) -> Veng::vector<EmittedValue>
        {
            // TextureSampleProps holds an AssetHandle (non-trivial), so read the two fields
            // out of the raw bytes by offset rather than memcpy'ing the whole struct.
            Veng::u64 textureId = 0;
            if (props.size() >= offsetof(TextureSampleProps, Texture) + sizeof(Veng::u64))
            {
                std::memcpy(&textureId, props.data() + offsetof(TextureSampleProps, Texture),
                            sizeof(textureId));
            }
            NodeName nameBuffer;
            if (props.size() >= offsetof(TextureSampleProps, Name) + sizeof(NodeName))
            {
                std::memcpy(&nameBuffer, props.data() + offsetof(TextureSampleProps, Name),
                            sizeof(nameBuffer));
            }

            // An authored name lets an engine-bound texture take the exact field name the
            // engine writes by (e.g. "Hdr" → "HdrSampler"); an unnamed node keys off the
            // walk's node key so two textures never collide.
            const Veng::string authored = NameOf(nameBuffer);
            const Veng::string textureField =
                authored.empty() ? fmt::format("{}_Texture", ctx.NodeKey) : authored;
            const Veng::string samplerField = authored.empty()
                                                  ? fmt::format("{}_Sampler", ctx.NodeKey)
                                                  : fmt::format("{}Sampler", authored);

            ctx.ParamFields.push_back(EmittedParamField{.Name = textureField,
                                                        .SlangType = "uint",
                                                        .Kind = EmittedFieldKind::TextureHandle,
                                                        .Alignment = 4,
                                                        .ComponentCount = 1,
                                                        .IsUint = true,
                                                        .TextureId = textureId});
            ctx.ParamFields.push_back(EmittedParamField{.Name = samplerField,
                                                        .SlangType = "uint",
                                                        .Kind = EmittedFieldKind::SamplerHandle,
                                                        .Alignment = 4,
                                                        .ComponentCount = 1,
                                                        .IsUint = true,
                                                        .SamplerTexture = textureField});

            const Veng::string uv = inputs.empty() ? Veng::string("input.v_UV") : inputs[0].Expr;
            const Veng::string expr =
                fmt::format("g_Textures[NonUniformResourceIndex(p.{})].Sample("
                            "g_Samplers[NonUniformResourceIndex(p.{})], {})",
                            textureField, samplerField, uv);
            return {EmittedValue{.Expr = expr, .Type = ValuePin(vec4Type), .IsConst = false}};
        };

        // A Param's provenance decides how its value reaches the shader: a const Param
        // folds its authored value inline (no field); an exposed Param emits a p.<Name>
        // read backed by a generated field carrying the authored default; an engine-bound
        // Param emits the same read but contributes no default (the engine writes it).
        emit.Emitters[types.Param.Value] =
            [vec4Type](std::span<const EmittedValue>, std::span<const std::byte> props,
                       EmitContext& ctx) -> Veng::vector<EmittedValue>
        {
            ParamProps p;
            if (props.size() >= sizeof(ParamProps))
            {
                std::memcpy(&p, props.data(), sizeof(p));
            }

            if (p.Provenance == ParamProvenance::Const)
            {
                const Veng::string literal = fmt::format("float4({}, {}, {}, {})", p.Value.x,
                                                         p.Value.y, p.Value.z, p.Value.w);
                return {EmittedValue{.Expr = literal, .Type = ValuePin(vec4Type), .IsConst = true}};
            }

            // An exposed/engine-bound param takes its authored name (so the engine can write
            // it by name), falling back to the walk's node key when unnamed.
            const Veng::string authored = NameOf(p.Name);
            const Veng::string field = authored.empty() ? ctx.NodeKey : authored;
            EmittedParamField emitted{.Name = field,
                                      .SlangType = "float4",
                                      .Kind = EmittedFieldKind::Param,
                                      .Alignment = 16,
                                      .ComponentCount = 4,
                                      .IsUint = false};
            if (p.Provenance == ParamProvenance::Exposed)
            {
                emitted.Default = {p.Value.x, p.Value.y, p.Value.z, p.Value.w};
            }
            ctx.ParamFields.push_back(std::move(emitted));
            return {EmittedValue{
                .Expr = fmt::format("p.{}", field), .Type = ValuePin(vec4Type), .IsConst = false}};
        };

        RegisterMathNodeTypes(catalog, emit);
        return types;
    }

    bool MaterialCanConnect(const PinType& from, const PinType& to)
    {
        if (from.Kind != PinType::Kind::Value || to.Kind != PinType::Kind::Value)
        {
            return false;
        }

        // Exact-type identity always connects.
        if (from.Type == to.Type)
        {
            return true;
        }

        const Veng::TypeId f32Type = TypeIdOf<Veng::f32>();
        const Veng::TypeId vec2Type = TypeIdOf<Veng::vec2>();
        const Veng::TypeId vec3Type = TypeIdOf<Veng::vec3>();
        const Veng::TypeId vec4Type = TypeIdOf<Veng::vec4>();

        // f32 → vecN: splat the scalar across the destination's components.
        if (from.Type == f32Type &&
            (to.Type == vec2Type || to.Type == vec3Type || to.Type == vec4Type))
        {
            return true;
        }

        // vec4 → vec3 / vec2: truncate the trailing components.
        if (from.Type == vec4Type && (to.Type == vec3Type || to.Type == vec2Type))
        {
            return true;
        }

        return false;
    }
}
