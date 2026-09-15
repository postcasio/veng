#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>

namespace Veng::Localization
{
    /// @brief A localizable data value: a message key that presents as its active-locale string.
    ///
    /// A reflected field typed LocKey carries a translation *key* rather than a finished display
    /// string, so a data-driven name (a commodity, a ship class) localizes through the ordinary
    /// binding path: a `{obj.field}` binding onto a LocKey leaf resolves the key against the
    /// document's translator into the active-locale string, while the stored key stays the field's
    /// stable identity. Its identity *is* its type — the distinct leaf TypeId is what a formatter
    /// keys on, so no reflection flag and no module-ABI change is needed to mark a field localizable.
    ///
    /// The single `string Key` member is the whole representation, so a LocKey serializes exactly as
    /// its key string (it registers with FieldClass::String); the translation is a presentation
    /// concern resolved at read time, never stored.
    struct LocKey
    {
        /// @brief The message key — the field's stable identity; the presented string derives from it.
        string Key;
    };
}

/// @brief Registers LocKey as a String-representation leaf with its own reflection identity.
VE_LEAF(::Veng::Localization::LocKey, 0x4F7EAC36BA8006EAULL, ::Veng::FieldClass::String);
