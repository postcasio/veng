#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>

namespace Veng::Gui
{
    struct Element;

    /// @brief A named C++ event handler fired when its element event triggers.
    ///
    /// The signature every `onClick`/`onChange`/… handler resolves to: it receives the element
    /// whose event fired, so one handler shared across elements can read the element that raised
    /// it. Handlers are registered by name on a BindingContext and named from markup.
    using EventHandler = function<void(Element& element)>;

    /// @brief The game-supplied view-model and handler table a document's bindings resolve against.
    ///
    /// A document binds one context (typically at instantiate/attach, rebindable): a reflected
    /// **data object** — an arbitrary game struct given by its base pointer and its registered
    /// TypeId — plus a table of named **handlers**. A `{obj.field}` binding resolves its field path
    /// against the data object through the TypeRegistry; an `onClick="Name"` handler names an entry
    /// in the handler table. The context borrows the data object; the caller keeps it alive for as
    /// long as the context is bound and bumps Version whenever a bound field changes so the next
    /// Update re-reads the dirtied bindings.
    class BindingContext
    {
    public:
        /// @brief Constructs an empty context — no data object, no handlers.
        BindingContext() = default;

        /// @brief Binds a reflected data object by its base pointer and registered type.
        ///
        /// The object a `{path}` binding resolves against: reflection walks its type's field path
        /// to the leaf value. The context borrows the pointer; it must outlive the binding. Passing
        /// a null pointer clears the bound object. Bumps Version so bindings re-read on the next
        /// Update.
        /// @param object  The base pointer of the data object, or nullptr to clear.
        /// @param type    The object's registered TypeId (ignored when object is null).
        void SetData(void* object, TypeId type);

        /// @brief Typed convenience over SetData: binds an object by its reflected type.
        /// @tparam T  The object's reflected type; its TypeId is read off its trait.
        /// @param object  The object to bind; the context borrows it and it must outlive the binding.
        template <class T>
        void SetData(T& object)
        {
            SetData(&object, TypeIdOf<T>());
        }

        /// @brief Registers (or replaces) a named event handler.
        ///
        /// A markup `onClick="Name"` fires the handler registered here under "Name". Registering an
        /// existing name replaces it; an empty handler removes the name.
        /// @param name     The handler name markup references.
        /// @param handler  The callback fired with the element that raised the event.
        void SetHandler(string name, EventHandler handler);

        /// @brief Looks up a named handler, or nullptr when none is registered under the name.
        /// @param name  The handler name to resolve.
        /// @return The registered handler, or nullptr.
        [[nodiscard]] const EventHandler* FindHandler(string_view name) const;

        /// @brief Returns the bound data object's base pointer, or nullptr when none is bound.
        [[nodiscard]] void* GetData() const { return m_Data; }

        /// @brief Returns the bound data object's TypeId (InvalidTypeId when none is bound).
        [[nodiscard]] TypeId GetDataType() const { return m_DataType; }

        /// @brief Marks the bound data changed so the next binding pass re-reads every binding.
        ///
        /// The explicit signal a game raises after mutating a bound field: it bumps Version, which
        /// the document's binding pass compares against the version it last read to decide whether
        /// to re-resolve. SetData/SetHandler bump it too.
        void Invalidate() { ++m_Version; }

        /// @brief Returns the monotonic version, bumped on every data/handler change.
        ///
        /// The document caches the version it last resolved bindings against and re-reads only when
        /// this moved — so a static context costs no per-frame reflection walk.
        [[nodiscard]] u64 GetVersion() const { return m_Version; }

    private:
        /// @brief The bound data object's base pointer, or nullptr when none is bound.
        void* m_Data = nullptr;
        /// @brief The bound data object's registered TypeId, or InvalidTypeId when none is bound.
        TypeId m_DataType = InvalidTypeId;
        /// @brief The name→handler table markup event names resolve against.
        map<string, EventHandler> m_Handlers;
        /// @brief Monotonic change counter bumped on every data/handler mutation and Invalidate.
        u64 m_Version = 1;
    };
}
