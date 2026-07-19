#pragma once

#include <Veng/Result.h>
#include <Veng/Veng.h>

namespace VengEditor
{
    /// @brief Sets the `src` attribute of the @p n-th `<Image>` start tag (pre-order) to @p idHex.
    ///
    /// Inserts the attribute when the tag carries none. A live element tree mirrors the markup's
    /// pre-order element order, so an ordinal picks out the matching start tag with no XML parser.
    /// @param text   The `*.vui.xml` markup.
    /// @param n      Zero-based ordinal of the target tag among the markup's `<Image>` start tags.
    /// @param idHex  Texture asset id, in the hex-string form the markup carries.
    /// @return The edited markup; nullopt when the tag or its start-tag terminator cannot be
    ///         located, so the mutation is abandoned rather than guessed.
    [[nodiscard]] Veng::optional<Veng::string> SetNthImageSrc(Veng::string text, Veng::usize n,
                                                              const Veng::string& idHex);

    /// @brief Appends a new `<Image>` as the document root's last child.
    ///
    /// The element is inserted before the final closing tag, since the outermost element closes
    /// last. The authored image starts un-sourced — a styled box until a texture is assigned.
    /// @param text  The `*.vui.xml` markup.
    /// @return The edited markup; nullopt when the markup carries no closing tag.
    [[nodiscard]] Veng::optional<Veng::string> AppendImage(Veng::string text);

    /// @brief The in-memory `*.vui.xml` markup an editor edits, written only on an explicit save.
    ///
    /// The markup text is the document: an authoring action rewrites it in memory and marks the
    /// editor dirty, and nothing reaches the file until Write. Holding it free of any UI dependency
    /// is what makes the editor's edit-then-save contract checkable without a frame or a device.
    class UIDocumentSource
    {
    public:
        /// @brief Reads @p file into the in-memory markup, replacing whatever it held.
        /// @param file  The `*.vui.xml` source to read.
        /// @return Empty on success; an error naming the file when it cannot be read.
        [[nodiscard]] Veng::VoidResult Load(const Veng::path& file);

        /// @brief Writes the in-memory markup to @p file, reporting a failure at open or at close.
        /// @param file  The `*.vui.xml` source to overwrite.
        /// @return Empty on success; an error naming the file otherwise.
        [[nodiscard]] Veng::VoidResult Write(const Veng::path& file) const;

        /// @brief Applies @p edit to the in-memory markup; touches no file.
        /// @param edit  Transforms the markup; returns nullopt to abandon the edit unchanged.
        /// @return True when the markup changed, so the caller marks the document dirty.
        bool Edit(const Veng::function<Veng::optional<Veng::string>(const Veng::string&)>& edit);

        /// @brief Returns the in-memory markup.
        [[nodiscard]] const Veng::string& GetText() const { return m_Text; }

    private:
        /// @brief The markup as edited, which matches the file's content only after a Write.
        Veng::string m_Text;
    };
}
