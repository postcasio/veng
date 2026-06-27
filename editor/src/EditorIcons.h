#pragma once

#include <Veng/Veng.h>
#include <Veng/Vendor/FontAwesome7.h>

namespace VengEditor::Icons
{
    using namespace Veng;

    /// @brief Reinterprets a FontAwesome `u8` glyph literal as a `char` string for an ImGui label.
    ///
    /// `FontAwesome7.h` spells each glyph as a `u8"…"` literal (a `char8_t` array under C++26),
    /// while `Veng::UI` labels are `char`-based `string_view`s. The byte sequence is identical;
    /// only the element type differs, so the cast is a representation change, not a conversion.
    /// @param glyph  A `FontAwesome7.h` `ICON_FA_*` literal.
    /// @return The same bytes typed as a `const char*`, usable as a button/menu label.
    inline const char* Glyph(const char8_t* glyph)
    {
        return reinterpret_cast<const char*>(glyph);
    }

    /// @brief Play-transport start glyph.
    inline const char* const Play = Glyph(ICON_FA_PLAY);
    /// @brief Play-transport stop glyph.
    inline const char* const Stop = Glyph(ICON_FA_STOP);
    /// @brief Play-transport pause glyph.
    inline const char* const Pause = Glyph(ICON_FA_PAUSE);

    /// @brief Translate-gizmo glyph (four-way arrows).
    inline const char* const Translate = Glyph(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT);
    /// @brief Rotate-gizmo glyph.
    inline const char* const Rotate = Glyph(ICON_FA_ROTATE);
    /// @brief Scale-gizmo glyph (diagonal resize arrows).
    inline const char* const Scale = Glyph(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER);

    /// @brief Frame-the-selection glyph.
    inline const char* const Frame = Glyph(ICON_FA_EXPAND);

    /// @brief Save glyph (floppy disk).
    inline const char* const Save = Glyph(ICON_FA_FLOPPY_DISK);
    /// @brief Revert/discard-changes glyph (counter-clockwise arrow).
    inline const char* const Revert = Glyph(ICON_FA_ARROW_ROTATE_LEFT);
    /// @brief Clear-list glyph (trash can).
    inline const char* const Clear = Glyph(ICON_FA_TRASH_CAN);

    /// @brief Add/create glyph (plus).
    inline const char* const Add = Glyph(ICON_FA_PLUS);
    /// @brief Remove/close glyph (cross).
    inline const char* const Remove = Glyph(ICON_FA_XMARK);

    /// @brief Detail-list view-mode glyph.
    inline const char* const ViewList = Glyph(ICON_FA_LIST);
    /// @brief Columns view-mode glyph.
    inline const char* const ViewColumns = Glyph(ICON_FA_TABLE_COLUMNS);
    /// @brief Icon-grid view-mode glyph.
    inline const char* const ViewGrid = Glyph(ICON_FA_TABLE_CELLS_LARGE);
}
