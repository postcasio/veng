#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/InputRouter.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;
    class InputMappingContext;
    struct InputContextStack;
}

namespace Veng::Renderer
{
    class Viewport;
}

namespace Veng
{
    /// @brief A locally-owned seat resolved from a scene: its Viewer entity and mutable components.
    ///
    /// The result of ResolveInputSeat — the first locally-owned seat with the full
    /// (Viewer, InputContextStack, PlayerInput) trio. The two mutable component pointers are
    /// borrowed from the scene's storage, valid until a structural change to the scene; a UI
    /// takeover reads and swaps them through this handle without re-querying.
    struct InputSeat
    {
        /// @brief The seat's Viewer entity, the key its focus stack and pointer association use.
        Entity Viewer = Entity::Null;
        /// @brief The seat's active input contexts, swapped while a UI surface holds it.
        InputContextStack* Contexts = nullptr;
    };

    /// @brief Resolves the first locally-owned input seat in a scene, null-safe before the world exists.
    ///
    /// Walks the scene for the first entity holding (Viewer, InputContextStack, PlayerInput) that is
    /// locally owned — the seat a single-seat consumer drives — and returns it with a borrowed
    /// pointer to its InputContextStack. A null scene, a scene with no such seat, or a seat that is
    /// not locally owned yields an empty seat (Viewer == Entity::Null, Contexts == nullptr), so a
    /// consumer resolving before its world spawns gets a safe empty result rather than a crash.
    /// @param scene  The scene to resolve a seat from, or nullptr before the world exists.
    /// @return The first locally-owned seat, or an empty InputSeat when none resolves.
    [[nodiscard]] VE_API InputSeat ResolveInputSeat(Scene* scene);

    /// @brief RAII takeover of a seat's input by a UI surface — a scoped state flip, no input flow.
    ///
    /// The engine-owned form of "a UI surface holds this seat's focus". Constructed over a seat, it
    /// (a) pushes a token UI entry on that seat's router focus stack, (b) swaps the seat's
    /// InputContextStack to the given UI context (suspending the gameplay contexts) when a context is
    /// supplied, and (c) associates the given viewport with the seat for pointer routing when a
    /// viewport is supplied; destruction restores all three in inverse order. Because the pop is by
    /// token, an interleaved second scope over the same seat pops its own entry safely even when it
    /// is not on top.
    ///
    /// It handles no input — nothing flows through it. It only flips the scoped state a UI screen
    /// (a fullscreen map, a pause menu, an interactive document) otherwise hand-rolls. An empty seat
    /// (ResolveInputSeat returned Entity::Null) makes the scope inert: it flips nothing and restores
    /// nothing, so a consumer that opens one before its world spawns is safe.
    class SeatFocusScope
    {
    public:
        /// @brief Opens the takeover: pushes UI focus, swaps the context, associates the viewport.
        /// @param router    The router whose focus stack and pointer associations are flipped.
        /// @param seat      The seat taken over; an empty seat makes the scope inert.
        /// @param viewport  The viewport associated with the seat for pointer routing, or nullptr for none.
        /// @param context   The UI mapping context swapped in (suspending gameplay), or empty for none.
        SeatFocusScope(InputRouter& router, const InputSeat& seat,
                       const Renderer::Viewport* viewport,
                       AssetHandle<InputMappingContext> context = {});

        /// @brief Closes the takeover: restores the context, drops the association, pops the token.
        ~SeatFocusScope();

        SeatFocusScope(const SeatFocusScope&) = delete;
        SeatFocusScope& operator=(const SeatFocusScope&) = delete;
        SeatFocusScope(SeatFocusScope&&) = delete;
        SeatFocusScope& operator=(SeatFocusScope&&) = delete;

    private:
        /// @brief The router whose state this scope flipped; restored in the destructor.
        InputRouter& m_Router;
        /// @brief The seat taken over; Entity::Null makes the scope inert.
        InputSeat m_Seat;
        /// @brief The viewport associated for pointer routing, or nullptr when none was.
        const Renderer::Viewport* m_Viewport;
        /// @brief The token of the pushed UI focus entry, popped on destruction.
        FocusToken m_Token;
        /// @brief The seat's contexts saved before the UI swap, restored on destruction.
        vector<AssetHandle<InputMappingContext>> m_SavedContexts;
        /// @brief Whether the constructor swapped the context (so the destructor restores it).
        bool m_SwappedContext = false;
    };
}
