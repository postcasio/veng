#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Input/Actions.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    /// @brief The reflected on-disk payload of an input map: its actions and bindings.
    ///
    /// The single reflected record the cook writes and the loader reads (through the shared
    /// WriteFields/ReadFields encoder), so an input map needs no bespoke binary format. Both
    /// fields are FieldClass::Array of a reflected element type, each element self-describing;
    /// a new action or binding field evolves tolerantly within the fixed CookedInputMapVersion.
    struct InputMapData
    {
        /// @brief The actions this context declares, in declaration order.
        vector<InputAction> Actions;

        /// @brief The raw-source → action bindings this context contributes.
        vector<Binding> Bindings;

        /// @brief Whether this context resolves only while its seat holds gameplay focus.
        ///
        /// Authored `"requiresGameplayFocus"` in the *.inputmap.json source; carried into the
        /// ResolvedContext the InputMappingSystem gates on. A tolerant field within the fixed
        /// CookedInputMapVersion — absent in a pre-change blob, so existing cooked maps load
        /// unchanged with the false default. False leaves the context always active.
        bool RequiresGameplayFocus = false;
    };

    /// @brief A named, remappable set of input-action bindings — the authored input scheme.
    ///
    /// Declares the actions it defines (id + name + kind) and the raw-source → action bindings.
    /// A seat's InputContextStack references one or more of these; InputMappingSystem resolves
    /// the active set against the raw snapshot. Cooked from a *.inputmap.json source, loaded by
    /// AssetId like any asset.
    ///
    /// A CPU-only asset (no GPU resource): the declared actions plus the bindings, kept in the
    /// resolver-ready ResolvedContext form built once at load. The GetResolved() form is what
    /// ResolveActions reads; GetActions()/GetBindings() expose the authored data (the editor's
    /// binding-table source).
    class InputMappingContext
    {
    public:
        /// @brief Creates a context from its decoded actions and bindings.
        ///
        /// Builds the resolver-ready ResolvedContext once, so GetResolved() is a plain read.
        /// @param actions               The declared actions, in declaration order.
        /// @param bindings              The raw-source → action bindings.
        /// @param requiresGameplayFocus Whether the context resolves only under gameplay focus.
        /// @return The constructed context.
        static Ref<InputMappingContext> Create(vector<InputAction> actions,
                                               vector<Binding> bindings,
                                               bool requiresGameplayFocus = false);

        /// @brief Returns the actions this context declares, in declaration order.
        [[nodiscard]] std::span<const InputAction> GetActions() const { return m_Actions; }

        /// @brief Returns the raw-source → action bindings this context contributes.
        [[nodiscard]] std::span<const Binding> GetBindings() const { return m_Bindings; }

        /// @brief Returns the resolver-ready form ResolveActions reads, built once at load.
        [[nodiscard]] const ResolvedContext& GetResolved() const { return m_Resolved; }

    private:
        InputMappingContext(vector<InputAction> actions, vector<Binding> bindings,
                            bool requiresGameplayFocus);

        /// @brief The declared actions, in declaration order.
        vector<InputAction> m_Actions;
        /// @brief The raw-source → action bindings.
        vector<Binding> m_Bindings;
        /// @brief The resolver-ready form, built from the actions + bindings at construction.
        ResolvedContext m_Resolved;
    };

    /// @brief AssetTypeTrait specialization mapping InputMappingContext to AssetTypes::InputMap.
    template <>
    struct AssetTypeTrait<InputMappingContext>
    {
        /// @brief The asset type tag for InputMappingContext.
        static constexpr AssetTypeId Type = AssetTypes::InputMap;
    };
}

VE_REFLECT(::Veng::InputMapData, 0x02571406767C54BCULL)
VE_ARRAY_FIELD(Actions, .DisplayName = "Actions")
VE_ARRAY_FIELD(Bindings, .DisplayName = "Bindings")
VE_FIELD(RequiresGameplayFocus, .DisplayName = "Requires Gameplay Focus")
VE_REFLECT_END();
