#include "material/MaterialCatalog.h"

#include <Veng/Assert.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Asset/AssetHandle.h>

#include <cstddef>

namespace VengEditor
{
    namespace
    {
        using Veng::TypeIdOf;

        // The Param node's property POD: one typed leaf value. Authored as a vec4
        // so it holds any of f32/vec2/vec3/vec4 (the compiler reads the live pin
        // type to pick the emitted arity) and a uint alias for an integer param.
        struct ParamProps
        {
            Veng::vec4 Value{0.0f, 0.0f, 0.0f, 0.0f};
        };

        // The TextureSample node's property POD: the sampled texture handle.
        struct TextureSampleProps
        {
            Veng::AssetHandle<Veng::Texture> Texture;
        };

        PinType ValuePin(Veng::TypeId id) { return PinType{PinType::Kind::Value, id}; }
    }

    Veng::vector<DomainOutputPin> DomainOutputContract(Veng::MaterialDomain domain)
    {
        switch (domain)
        {
            case Veng::MaterialDomain::PostProcess:
                return {
                    DomainOutputPin{OutputColorPin, ValuePin(TypeIdOf<Veng::vec4>())},
                };
            case Veng::MaterialDomain::Surface:
                return {
                    DomainOutputPin{OutputAlbedoPin, ValuePin(TypeIdOf<Veng::vec4>())},
                    DomainOutputPin{OutputNormalPin, ValuePin(TypeIdOf<Veng::vec3>())},
                };
        }
        VE_ASSERT(false, "DomainOutputContract: unhandled MaterialDomain {}",
                  static_cast<Veng::u32>(domain));
    }

    MaterialNodeTypes RegisterMaterialNodeTypes(NodeCatalog& catalog,
                                                const MaterialShaderInterface& shader,
                                                Veng::MaterialDomain domain)
    {
        MaterialNodeTypes types;

        // --- TextureSample: optional UV in, Color (vec4) out, Texture property ---
        {
            NodeType type;
            type.Name = TextureSampleTypeName;
            type.Inputs = {
                PinDesc{TextureSampleUVPin, ValuePin(TypeIdOf<Veng::vec2>())},
            };
            type.Outputs = {
                PinDesc{TextureSampleColorPin, ValuePin(TypeIdOf<Veng::vec4>())},
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

        // --- Param: typed Value out, Value property ---
        {
            NodeType type;
            type.Name = ParamTypeName;
            type.Outputs = {
                PinDesc{ParamValuePin, ValuePin(TypeIdOf<Veng::vec4>())},
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
        // contract, not the loaded shader's fields.
        {
            NodeType type;
            type.Name = MaterialOutputTypeName;
            for (const DomainOutputPin& sink : DomainOutputContract(domain))
                type.Inputs.push_back(PinDesc{sink.Name, sink.Type});
            type.PropertySize = 0;
            types.MaterialOutput = catalog.Register(std::move(type));
        }

        return types;
    }

    bool MaterialCanConnect(const PinType& from, const PinType& to)
    {
        if (from.Kind != PinType::Kind::Value || to.Kind != PinType::Kind::Value)
            return false;

        // Exact-type identity always connects.
        if (from.Type == to.Type)
            return true;

        const Veng::TypeId f32Type = TypeIdOf<Veng::f32>();
        const Veng::TypeId vec2Type = TypeIdOf<Veng::vec2>();
        const Veng::TypeId vec3Type = TypeIdOf<Veng::vec3>();
        const Veng::TypeId vec4Type = TypeIdOf<Veng::vec4>();

        // f32 → vecN: splat the scalar across the destination's components.
        if (from.Type == f32Type
            && (to.Type == vec2Type || to.Type == vec3Type || to.Type == vec4Type))
            return true;

        // vec4 → vec3 / vec2: truncate the trailing components.
        if (from.Type == vec4Type && (to.Type == vec3Type || to.Type == vec2Type))
            return true;

        return false;
    }
}
