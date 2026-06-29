#include <VengGraph/MaterialCatalog.h>

#include <Veng/Assert.h>
#include <Veng/Reflection/TypeId.h>

#include <fmt/format.h>

#include <cstddef>
#include <cstring>

namespace VengGraph
{
    namespace
    {
        using Veng::TypeIdOf;

        // Constant property POD: a vec4 holding up to four authored components, plus the
        // leaf type selecting which Slang scalar/vector the node emits (and how many of the
        // components the literal carries).
        struct ConstantProps
        {
            Veng::vec4 Value{0.0f, 0.0f, 0.0f, 0.0f};
            MaterialLeafType LeafType = MaterialLeafType::Vec4;
        };

        // ScalarParam property POD: a single authored default, the provenance selecting how
        // the value reaches the shader (const-fold / exposed / engine-bound), and an optional
        // authored Name the generated field takes (empty → the node key).
        struct ScalarParamProps
        {
            Veng::f32 Value = 0.0f;
            ParamProvenance Provenance = ParamProvenance::Const;
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

        // The pin/output TypeId a leaf type maps to.
        Veng::TypeId LeafTypeId(MaterialLeafType leaf)
        {
            switch (leaf)
            {
            case MaterialLeafType::Float:
                return TypeIdOf<Veng::f32>();
            case MaterialLeafType::Vec2:
                return TypeIdOf<Veng::vec2>();
            case MaterialLeafType::Vec3:
                return TypeIdOf<Veng::vec3>();
            case MaterialLeafType::Vec4:
                return TypeIdOf<Veng::vec4>();
            case MaterialLeafType::Uint:
                return TypeIdOf<Veng::u32>();
            }
            return TypeIdOf<Veng::vec4>();
        }

        // The Slang literal for an authored Constant of a given leaf type: as many of the
        // value's components as the leaf carries, in the matching constructor.
        Veng::string LeafLiteral(const Veng::vec4& v, MaterialLeafType leaf)
        {
            switch (leaf)
            {
            case MaterialLeafType::Float:
                return fmt::format("{}", v.x);
            case MaterialLeafType::Vec2:
                return fmt::format("float2({}, {})", v.x, v.y);
            case MaterialLeafType::Vec3:
                return fmt::format("float3({}, {}, {})", v.x, v.y, v.z);
            case MaterialLeafType::Vec4:
                return fmt::format("float4({}, {}, {}, {})", v.x, v.y, v.z, v.w);
            case MaterialLeafType::Uint:
                return fmt::format("uint({})", static_cast<Veng::u32>(v.x));
            }
            return fmt::format("float4({}, {}, {}, {})", v.x, v.y, v.z, v.w);
        }

        // Registers a value-producing node type with the given pins and a binary/unary/ternary
        // emit-fn, and wires its emit-fn into the table. The emit-fn body is built by the caller.
        NodeTypeId RegisterEmitter(NodeCatalog& catalog, MaterialEmitTable& emit, const char* name,
                                   Veng::vector<PinDesc> inputs, Veng::vector<PinDesc> outputs,
                                   NodeEmitFn fn)
        {
            NodeType type;
            type.Name = name;
            type.Inputs = std::move(inputs);
            type.Outputs = std::move(outputs);
            type.PropertySize = 0;
            const NodeTypeId id = catalog.Register(std::move(type));
            emit.Emitters[id.Value] = std::move(fn);
            return id;
        }

        // A unary intrinsic emit-fn: one input, one output of outType, emitting `intrinsic(x)`.
        NodeEmitFn UnaryIntrinsic(const char* intrinsic, PinType outType)
        {
            const Veng::string call = intrinsic;
            return [call, outType](std::span<const EmittedValue> inputs, std::span<const std::byte>,
                                   EmitContext&) -> Veng::vector<EmittedValue>
            {
                const Veng::string a = inputs.empty() ? Veng::string("0") : inputs[0].Expr;
                return {EmittedValue{
                    .Expr = fmt::format("{}({})", call, a), .Type = outType, .IsConst = false}};
            };
        }

        // A binary infix-operator emit-fn: two inputs, one output of outType, emitting `(a op b)`.
        NodeEmitFn BinaryOperator(const char* op, PinType outType)
        {
            const Veng::string oper = op;
            return [oper, outType](std::span<const EmittedValue> inputs, std::span<const std::byte>,
                                   EmitContext&) -> Veng::vector<EmittedValue>
            {
                const Veng::string a = inputs.size() > 0 ? inputs[0].Expr : Veng::string("0");
                const Veng::string b = inputs.size() > 1 ? inputs[1].Expr : Veng::string("0");
                return {EmittedValue{.Expr = fmt::format("({}) {} ({})", a, oper, b),
                                     .Type = outType,
                                     .IsConst = false}};
            };
        }

        // A binary intrinsic emit-fn: two inputs, one output of outType, emitting `intrinsic(a, b)`.
        NodeEmitFn BinaryIntrinsic(const char* intrinsic, PinType outType)
        {
            const Veng::string call = intrinsic;
            return [call, outType](std::span<const EmittedValue> inputs, std::span<const std::byte>,
                                   EmitContext&) -> Veng::vector<EmittedValue>
            {
                const Veng::string a = inputs.size() > 0 ? inputs[0].Expr : Veng::string("0");
                const Veng::string b = inputs.size() > 1 ? inputs[1].Expr : Veng::string("0");
                return {EmittedValue{.Expr = fmt::format("{}({}, {})", call, a, b),
                                     .Type = outType,
                                     .IsConst = false}};
            };
        }

        // A ternary intrinsic emit-fn: three inputs, one output of outType, emitting
        // `intrinsic(a, b, c)`.
        NodeEmitFn TernaryIntrinsic(const char* intrinsic, PinType outType)
        {
            const Veng::string call = intrinsic;
            return [call, outType](std::span<const EmittedValue> inputs, std::span<const std::byte>,
                                   EmitContext&) -> Veng::vector<EmittedValue>
            {
                const Veng::string a = inputs.size() > 0 ? inputs[0].Expr : Veng::string("0");
                const Veng::string b = inputs.size() > 1 ? inputs[1].Expr : Veng::string("0");
                const Veng::string c = inputs.size() > 2 ? inputs[2].Expr : Veng::string("0");
                return {EmittedValue{.Expr = fmt::format("{}({}, {}, {})", call, a, b, c),
                                     .Type = outType,
                                     .IsConst = false}};
            };
        }
    }

    void RegisterMathNodeTypes(NodeCatalog& catalog, MaterialEmitTable& emit)
    {
        const Veng::TypeId f32Type = TypeIdOf<Veng::f32>();
        const Veng::TypeId vec3Type = TypeIdOf<Veng::vec3>();
        const Veng::TypeId vec4Type = TypeIdOf<Veng::vec4>();
        const PinType f32Pin = ValuePin(f32Type);
        const PinType vec3Pin = ValuePin(vec3Type);
        const PinType vec4Pin = ValuePin(vec4Type);

        // --- Constant: an authored literal of a chosen leaf type, always inlined ---
        {
            NodeType type;
            type.Name = ConstantTypeName;
            type.Outputs = {PinDesc{.Name = "Value", .Type = vec4Pin}};
            type.Properties = {
                Veng::FieldDescriptor{
                    .Name = ConstantValueProperty,
                    .Type = vec4Type,
                    .Class = Veng::FieldClass::Vector,
                    .Offset = offsetof(ConstantProps, Value),
                },
                Veng::FieldDescriptor{
                    .Name = ConstantLeafTypeProperty,
                    .Type = TypeIdOf<MaterialLeafType>(),
                    .Class = Veng::FieldClass::Enum,
                    .Offset = offsetof(ConstantProps, LeafType),
                },
            };
            type.PropertySize = sizeof(ConstantProps);
            const NodeTypeId id = catalog.Register(std::move(type));
            emit.Emitters[id.Value] = [](std::span<const EmittedValue>,
                                         std::span<const std::byte> props,
                                         EmitContext&) -> Veng::vector<EmittedValue>
            {
                ConstantProps p;
                if (props.size() >= sizeof(ConstantProps))
                {
                    std::memcpy(&p, props.data(), sizeof(p));
                }
                return {EmittedValue{.Expr = LeafLiteral(p.Value, p.LeafType),
                                     .Type = ValuePin(LeafTypeId(p.LeafType)),
                                     .IsConst = true}};
            };
        }

        // --- ScalarParam: a Param specialized to float, carrying the same provenance ---
        {
            NodeType type;
            type.Name = ScalarParamTypeName;
            type.Outputs = {PinDesc{.Name = "Value", .Type = f32Pin}};
            type.Properties = {
                Veng::FieldDescriptor{
                    .Name = ScalarParamValueProperty,
                    .Type = f32Type,
                    .Class = Veng::FieldClass::Scalar,
                    .Offset = offsetof(ScalarParamProps, Value),
                },
                Veng::FieldDescriptor{
                    .Name = ScalarParamProvenanceProperty,
                    .Type = TypeIdOf<ParamProvenance>(),
                    .Class = Veng::FieldClass::Enum,
                    .Offset = offsetof(ScalarParamProps, Provenance),
                },
                Veng::FieldDescriptor{
                    .Name = NodeNameProperty,
                    .Type = TypeIdOf<Veng::string>(),
                    .Class = Veng::FieldClass::String,
                    .Offset = offsetof(ScalarParamProps, Name),
                    // The fixed inline name buffer is authored in the graph JSON, not the
                    // inspector — the String widget reinterprets the field as a Veng::string.
                    .Hidden = true,
                },
            };
            type.PropertySize = sizeof(ScalarParamProps);
            const NodeTypeId id = catalog.Register(std::move(type));
            emit.Emitters[id.Value] = [f32Type](std::span<const EmittedValue>,
                                                std::span<const std::byte> props,
                                                EmitContext& ctx) -> Veng::vector<EmittedValue>
            {
                ScalarParamProps p;
                if (props.size() >= sizeof(ScalarParamProps))
                {
                    std::memcpy(&p, props.data(), sizeof(p));
                }

                if (p.Provenance == ParamProvenance::Const)
                {
                    return {EmittedValue{.Expr = fmt::format("{}", p.Value),
                                         .Type = ValuePin(f32Type),
                                         .IsConst = true}};
                }

                const Veng::string authored = NameOf(p.Name);
                const Veng::string field = authored.empty() ? ctx.NodeKey : authored;
                EmittedParamField emitted{.Name = field,
                                          .SlangType = "float",
                                          .Kind = EmittedFieldKind::Param,
                                          .Alignment = 4,
                                          .ComponentCount = 1,
                                          .IsUint = false};
                if (p.Provenance == ParamProvenance::Exposed)
                {
                    emitted.Default = {p.Value};
                }
                ctx.ParamFields.push_back(std::move(emitted));
                return {EmittedValue{.Expr = fmt::format("p.{}", field),
                                     .Type = ValuePin(f32Type),
                                     .IsConst = false}};
            };
        }

        // --- Arithmetic: binary, component-wise; the scalar splats against a vector by the
        // link-recorded coercion, so a single vec4-pinned form covers every arity. ---
        RegisterEmitter(
            catalog, emit, MultiplyTypeName,
            {PinDesc{.Name = "A", .Type = vec4Pin}, PinDesc{.Name = "B", .Type = vec4Pin}},
            {PinDesc{.Name = "Out", .Type = vec4Pin}}, BinaryOperator("*", vec4Pin));
        RegisterEmitter(
            catalog, emit, AddTypeName,
            {PinDesc{.Name = "A", .Type = vec4Pin}, PinDesc{.Name = "B", .Type = vec4Pin}},
            {PinDesc{.Name = "Out", .Type = vec4Pin}}, BinaryOperator("+", vec4Pin));
        RegisterEmitter(
            catalog, emit, SubtractTypeName,
            {PinDesc{.Name = "A", .Type = vec4Pin}, PinDesc{.Name = "B", .Type = vec4Pin}},
            {PinDesc{.Name = "Out", .Type = vec4Pin}}, BinaryOperator("-", vec4Pin));
        RegisterEmitter(
            catalog, emit, DivideTypeName,
            {PinDesc{.Name = "A", .Type = vec4Pin}, PinDesc{.Name = "B", .Type = vec4Pin}},
            {PinDesc{.Name = "Out", .Type = vec4Pin}}, BinaryOperator("/", vec4Pin));

        // --- Component-wise min/max: the everyday clamps that ride a vec4 form, scalars
        // splatting by coercion. ---
        RegisterEmitter(
            catalog, emit, MinTypeName,
            {PinDesc{.Name = "A", .Type = vec4Pin}, PinDesc{.Name = "B", .Type = vec4Pin}},
            {PinDesc{.Name = "Out", .Type = vec4Pin}}, BinaryIntrinsic("min", vec4Pin));
        RegisterEmitter(
            catalog, emit, MaxTypeName,
            {PinDesc{.Name = "A", .Type = vec4Pin}, PinDesc{.Name = "B", .Type = vec4Pin}},
            {PinDesc{.Name = "Out", .Type = vec4Pin}}, BinaryIntrinsic("max", vec4Pin));

        // --- ScreenUV: the fullscreen fragment's interpolated UV (a vec2 source); the
        // PostProcess input the UV-space effects sample by. ---
        RegisterEmitter(catalog, emit, ScreenUVTypeName, {},
                        {PinDesc{.Name = "UV", .Type = ValuePin(TypeIdOf<Veng::vec2>())}},
                        [](std::span<const EmittedValue>, std::span<const std::byte>,
                           EmitContext&) -> Veng::vector<EmittedValue>
                        {
                            return {EmittedValue{.Expr = "input.v_UV",
                                                 .Type = ValuePin(TypeIdOf<Veng::vec2>()),
                                                 .IsConst = false}};
                        });

        // --- Interpolation / clamping: the everyday shaping operators ---
        RegisterEmitter(
            catalog, emit, LerpTypeName,
            {PinDesc{.Name = "A", .Type = vec4Pin}, PinDesc{.Name = "B", .Type = vec4Pin},
             PinDesc{.Name = "T", .Type = vec4Pin}},
            {PinDesc{.Name = "Out", .Type = vec4Pin}}, TernaryIntrinsic("lerp", vec4Pin));
        RegisterEmitter(catalog, emit, SaturateTypeName, {PinDesc{.Name = "X", .Type = vec4Pin}},
                        {PinDesc{.Name = "Out", .Type = vec4Pin}},
                        UnaryIntrinsic("saturate", vec4Pin));
        RegisterEmitter(
            catalog, emit, ClampTypeName,
            {PinDesc{.Name = "Value", .Type = vec4Pin}, PinDesc{.Name = "Min", .Type = vec4Pin},
             PinDesc{.Name = "Max", .Type = vec4Pin}},
            {PinDesc{.Name = "Out", .Type = vec4Pin}}, TernaryIntrinsic("clamp", vec4Pin));
        RegisterEmitter(
            catalog, emit, OneMinusTypeName, {PinDesc{.Name = "X", .Type = vec4Pin}},
            {PinDesc{.Name = "Out", .Type = vec4Pin}},
            [vec4Pin](std::span<const EmittedValue> inputs, std::span<const std::byte>,
                      EmitContext&) -> Veng::vector<EmittedValue>
            {
                const Veng::string x = inputs.empty() ? Veng::string("0") : inputs[0].Expr;
                return {EmittedValue{
                    .Expr = fmt::format("(1.0 - ({}))", x), .Type = vec4Pin, .IsConst = false}};
            });

        // --- Vector algebra: the output pin type follows the operation. Dot/Length reduce to
        // a float; Cross is vec3-only (its vec3 input pins make the topology validator reject a
        // non-vec3 connection at connect time); Normalize preserves arity. ---
        RegisterEmitter(
            catalog, emit, DotTypeName,
            {PinDesc{.Name = "A", .Type = vec4Pin}, PinDesc{.Name = "B", .Type = vec4Pin}},
            {PinDesc{.Name = "Out", .Type = f32Pin}}, BinaryIntrinsic("dot", f32Pin));
        RegisterEmitter(
            catalog, emit, CrossTypeName,
            {PinDesc{.Name = "A", .Type = vec3Pin}, PinDesc{.Name = "B", .Type = vec3Pin}},
            {PinDesc{.Name = "Out", .Type = vec3Pin}}, BinaryIntrinsic("cross", vec3Pin));
        RegisterEmitter(catalog, emit, NormalizeTypeName, {PinDesc{.Name = "X", .Type = vec4Pin}},
                        {PinDesc{.Name = "Out", .Type = vec4Pin}},
                        UnaryIntrinsic("normalize", vec4Pin));
        RegisterEmitter(catalog, emit, LengthTypeName, {PinDesc{.Name = "X", .Type = vec4Pin}},
                        {PinDesc{.Name = "Out", .Type = f32Pin}}, UnaryIntrinsic("length", f32Pin));

        // --- Channel plumbing: Split fans a vec4 out to four named scalar pins via swizzles;
        // Combine packs four scalar inputs into a vec4, an unconnected trailing input
        // defaulting to 0 (the f32 pin's zero default). ---
        RegisterEmitter(
            catalog, emit, SplitTypeName, {PinDesc{.Name = "In", .Type = vec4Pin}},
            {PinDesc{.Name = "X", .Type = f32Pin}, PinDesc{.Name = "Y", .Type = f32Pin},
             PinDesc{.Name = "Z", .Type = f32Pin}, PinDesc{.Name = "W", .Type = f32Pin}},
            [f32Pin](std::span<const EmittedValue> inputs, std::span<const std::byte>,
                     EmitContext&) -> Veng::vector<EmittedValue>
            {
                const Veng::string v =
                    inputs.empty() ? Veng::string("float4(0,0,0,0)") : inputs[0].Expr;
                return {
                    EmittedValue{
                        .Expr = fmt::format("({}).x", v), .Type = f32Pin, .IsConst = false},
                    EmittedValue{
                        .Expr = fmt::format("({}).y", v), .Type = f32Pin, .IsConst = false},
                    EmittedValue{
                        .Expr = fmt::format("({}).z", v), .Type = f32Pin, .IsConst = false},
                    EmittedValue{
                        .Expr = fmt::format("({}).w", v), .Type = f32Pin, .IsConst = false},
                };
            });
        RegisterEmitter(catalog, emit, CombineTypeName,
                        {PinDesc{.Name = "X", .Type = f32Pin}, PinDesc{.Name = "Y", .Type = f32Pin},
                         PinDesc{.Name = "Z", .Type = f32Pin},
                         PinDesc{.Name = "W", .Type = f32Pin}},
                        {PinDesc{.Name = "Out", .Type = vec4Pin}},
                        [vec4Pin](std::span<const EmittedValue> inputs, std::span<const std::byte>,
                                  EmitContext&) -> Veng::vector<EmittedValue>
                        {
                            const auto at = [&](Veng::usize i)
                            { return i < inputs.size() ? inputs[i].Expr : Veng::string("0"); };
                            return {EmittedValue{.Expr = fmt::format("float4({}, {}, {}, {})",
                                                                     at(0), at(1), at(2), at(3)),
                                                 .Type = vec4Pin,
                                                 .IsConst = false}};
                        });
    }
}
