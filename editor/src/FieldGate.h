#pragma once

#include <Veng/Reflection/FieldDescriptor.h>

namespace VengEditor
{
    /// @brief Whether a field's VisibleIf condition admits it for the given owning instance.
    ///
    /// Pure and device-free — no ImGui, no GPU — so the conditional-display decision is
    /// unit-testable. An empty VisibleIf is unconditionally visible; otherwise the predicate
    /// is evaluated against the owning struct's base. A false result means the inspector skips
    /// the row entirely.
    /// @param field      The field descriptor carrying the optional VisibleIf predicate.
    /// @param ownerBase  Base pointer of the immediately-enclosing reflected struct instance.
    /// @return True when the field should be shown.
    [[nodiscard]] bool IsFieldVisible(const Veng::FieldDescriptor& field, const void* ownerBase);

    /// @brief Whether a field's EnabledIf condition admits editing for the given owning instance.
    ///
    /// Pure and device-free, the editable twin of IsFieldVisible. An empty EnabledIf is
    /// unconditionally enabled; otherwise the predicate is evaluated against the owning struct's
    /// base. It does not fold in FieldDescriptor::ReadOnly — the caller composes the two so a
    /// field is editable only when both allow it.
    /// @param field      The field descriptor carrying the optional EnabledIf predicate.
    /// @param ownerBase  Base pointer of the immediately-enclosing reflected struct instance.
    /// @return True when the field should be editable (before composing ReadOnly).
    [[nodiscard]] bool IsFieldEnabled(const Veng::FieldDescriptor& field, const void* ownerBase);
}
