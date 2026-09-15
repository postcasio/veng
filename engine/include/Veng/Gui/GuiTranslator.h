#pragma once

#include <string_view>

#include <Veng/Veng.h>

namespace Veng::Gui
{
    /// @brief A document's decoupled view onto a localization service: a key to its active-locale string.
    ///
    /// A Gui::Document resolves loc-keyed markup text and LocKey-typed bound leaves through a
    /// borrowed translator rather than naming the concrete localization service, exactly as it
    /// resolves asset declarations through a borrowed AssetManager. A host (Application) installs an
    /// adapter over its own service; a headless, editor, or test host may install none, in which case
    /// a loc-key resolves to itself — visible, never blank.
    ///
    /// The Generation counter is what lets a document re-resolve on a language change without any
    /// per-document wiring: it moves whenever the active locale changes, and a document compares it
    /// against the value it last resolved at.
    class GuiTranslator
    {
    public:
        /// @brief Destroys the translator.
        virtual ~GuiTranslator() = default;

        /// @brief Resolves a message key to its active-locale string.
        ///
        /// The returned view must stay valid until the next language change (the document copies it
        /// into an element's presented text, so it need not outlive that copy). A key the service
        /// does not define resolves to the key itself, never a blank.
        /// @param key  The message key.
        /// @return The active-locale string, or the key when it is undefined.
        [[nodiscard]] virtual std::string_view Translate(std::string_view key) const = 0;

        /// @brief Returns a counter bumped on every language change, so a document knows to re-resolve.
        [[nodiscard]] virtual u32 Generation() const = 0;
    };
}
