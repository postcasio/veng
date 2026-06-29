#include <VengGraph/MaterialCatalog.h>

#include <Veng/Assert.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Asset/AssetHandle.h>

#include <fmt/format.h>

#include <cstddef>

namespace VengGraph
{
    namespace
    {
        using Veng::TypeIdOf;

        // Param property POD: one vec4 value covering f32/vec2/vec3/vec4 arities; the
        // emit-fn reads the pin type to pick the emitted arity.
        struct ParamProps
        {
            Veng::vec4 Value{0.0f, 0.0f, 0.0f, 0.0f};
        };

        // TextureSample property POD: the sampled texture handle.
        struct TextureSampleProps
        {
            Veng::AssetHandle<Veng::Texture> Texture;
        };

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
        // carry one. Each names node-unique provisional MaterialParams fields from the
        // walk's NodeKey and contributes them to ctx.ParamFields so the generated
        // source is compilable; Plan 02 materializes the final struct + field names. ---

        const Veng::TypeId vec4Type = TypeIdOf<Veng::vec4>();

        // A TextureSample samples the bindless texture and sampler named by its two
        // handle slots, with the UV input defaulting to the surface UV when unconnected.
        emit.Emitters[types.TextureSample.Value] = [vec4Type](std::span<const EmittedValue> inputs,
                                                              std::span<const std::byte>,
                                                              EmitContext& ctx) -> EmittedValue
        {
            const Veng::string textureField = fmt::format("{}_Texture", ctx.NodeKey);
            const Veng::string samplerField = fmt::format("{}_Sampler", ctx.NodeKey);
            ctx.ParamFields.push_back(EmittedParamField{.Name = textureField, .SlangType = "uint"});
            ctx.ParamFields.push_back(EmittedParamField{.Name = samplerField, .SlangType = "uint"});

            const Veng::string uv = inputs.empty() ? Veng::string("input.v_UV") : inputs[0].Expr;
            const Veng::string expr =
                fmt::format("g_Textures[NonUniformResourceIndex(p.{})].Sample("
                            "g_Samplers[NonUniformResourceIndex(p.{})], {})",
                            textureField, samplerField, uv);
            return EmittedValue{.Expr = expr, .Type = ValuePin(vec4Type), .IsConst = false};
        };

        // A Param emits a uniform read of its param-block field; the
        // const/exposed/engine-bound provenance arrives in Plan 02.
        emit.Emitters[types.Param.Value] = [vec4Type](std::span<const EmittedValue>,
                                                      std::span<const std::byte>,
                                                      EmitContext& ctx) -> EmittedValue
        {
            const Veng::string field = ctx.NodeKey;
            ctx.ParamFields.push_back(EmittedParamField{.Name = field, .SlangType = "float4"});
            return EmittedValue{
                .Expr = fmt::format("p.{}", field), .Type = ValuePin(vec4Type), .IsConst = false};
        };

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
