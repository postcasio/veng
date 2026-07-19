#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <span>
#include <string_view>

#include <nlohmann/json.hpp>

/// @brief The in-memory documents the table editor panels edit, free of any UI dependency.
///
/// Both panels accumulate their edits here and write nothing until an explicit save, so the
/// authoring rules — column uniqueness, key-column eligibility, cell binding, key uniqueness —
/// are all testable without a frame. Validation runs through the engine's own
/// `Veng::LayOutTableSchema` and `Veng::JsonReadFieldValue`, the same calls the importer makes,
/// so a document the panel reports as valid is one the cook accepts.

namespace VengEditor
{
    /// @brief One authored column of a `*.tableschema.json`.
    struct TableSchemaColumn
    {
        /// @brief The authored column name.
        Veng::string Name;
        /// @brief The authored fully-qualified reflected type name.
        ///
        /// Kept alongside the resolved id so a column naming a type this editor's registry does
        /// not carry stays visible and repairable instead of silently becoming untyped.
        Veng::string TypeName;
        /// @brief The resolved reflected type, or InvalidTypeId when TypeName resolves to nothing.
        Veng::TypeId Type = Veng::InvalidTypeId;
        /// @brief The JSON object this column parsed from, so unknown keys survive a save.
        nlohmann::json Source = nlohmann::json::object();
    };

    /// @brief The editable contents of a `*.tableschema.json`: its columns and its key column.
    struct TableSchemaDocument
    {
        /// @brief The columns in declaration order.
        Veng::vector<TableSchemaColumn> Columns;
        /// @brief Name of the column rows are keyed by; may name no column while being authored.
        Veng::string Key;

        /// @brief Parses a schema document, tolerating anything an editor should let a user repair.
        ///
        /// A malformed column is skipped, an unresolvable type name is kept unresolved, and a key
        /// naming no column is kept as authored — none of which the cook accepts, but all of which
        /// the editor must be able to open and fix.
        /// @param doc    The parsed `*.tableschema.json` document.
        /// @param types  Registry each column's authored type name resolves through.
        /// @return The parsed document.
        [[nodiscard]] static TableSchemaDocument Read(const nlohmann::json& doc,
                                                      const Veng::TypeRegistry& types);

        /// @brief Assigns the columns and key into an existing document, preserving unknown keys.
        ///
        /// Every key `into` carries that this document does not own is untouched, and each column
        /// is written back into the JSON object it parsed from — so a hand-authored comment key
        /// survives a rename or a reorder.
        /// @param into  Destination JSON object, patched in place.
        void Write(nlohmann::json& into) const;

        /// @brief Appends a column, naming it uniquely.
        /// @param name  Desired name; suffixed when the schema already declares it.
        /// @param type  The column's reflected type.
        /// @return The new column's index.
        Veng::usize AddColumn(std::string_view name, const Veng::TypeInfo& type);

        /// @brief Removes a column; the key column reverts to unset when it was the one removed.
        /// @param index  Index of the column to remove.
        void RemoveColumn(Veng::usize index);

        /// @brief Renames a column, carrying the key reference with it when it named that column.
        /// @param index  Index of the column to rename.
        /// @param name   The new name, taken as authored (validation reports a collision).
        void RenameColumn(Veng::usize index, std::string_view name);

        /// @brief Repoints a column at a different reflected type.
        /// @param index  Index of the column to retype.
        /// @param type   The column's new reflected type.
        void SetColumnType(Veng::usize index, const Veng::TypeInfo& type);

        /// @brief Moves a column to a new position, shifting the rest.
        /// @param from  Index of the column to move.
        /// @param to    Destination index.
        void MoveColumn(Veng::usize from, Veng::usize to);

        /// @brief Returns the index of a column by name, or nullopt when none carries it.
        [[nodiscard]] Veng::optional<Veng::usize> FindColumn(std::string_view name) const;

        /// @brief Resolves the document into laid-out column descriptors.
        ///
        /// The single validation entry point: it runs `Veng::LayOutTableSchema`, so the rules and
        /// the diagnostics are the importer's, not a second copy. An unresolved authored type name
        /// is reported before the layout runs, naming the spelling that failed.
        /// @param types    Registry the columns' types resolve through.
        /// @param columns  Receives the laid-out descriptors; valid only when this succeeds.
        /// @return The resolved row layout, or the reason the document is not yet a legal schema.
        [[nodiscard]] Veng::Result<Veng::TableSchemaLayout>
        Resolve(const Veng::TypeRegistry& types,
                Veng::vector<Veng::TableColumnDescriptor>& columns) const;
    };

    /// @brief One row of a data table: a decoded reflected value per schema column, in column order.
    ///
    /// Cells are held decoded rather than as JSON so an in-progress edit is the live value the
    /// widget writes into, and a save is one encode pass rather than a decode/encode per keystroke.
    class TableRow
    {
    public:
        /// @brief Constructs a row of default-constructed cells for the given columns.
        /// @param columns  The schema's columns, in declaration order.
        /// @param types    Registry the column types resolve through.
        TableRow(std::span<const Veng::TableColumnDescriptor> columns,
                 const Veng::TypeRegistry& types);

        /// @brief Returns the storage of one cell.
        /// @param column  Column index, below GetCellCount().
        [[nodiscard]] void* Cell(Veng::usize column) const { return m_Cells[column]->Get(); }

        /// @brief Returns the number of cells, which is the column count the row was built for.
        [[nodiscard]] Veng::usize GetCellCount() const { return m_Cells.size(); }

        /// @brief The JSON object this row parsed from, so unknown keys survive a save.
        nlohmann::json Source = nlohmann::json::object();

    private:
        /// @brief One decoded value per column; stable addresses, so held indirectly.
        Veng::vector<Veng::Unique<Veng::ReflectedStorage>> m_Cells;
    };

    /// @brief The editable contents of a `*.table.json`: its schema reference and its rows.
    struct TableDataDocument
    {
        /// @brief The schema the rows are authored against.
        Veng::AssetId Schema;
        /// @brief The rows in authored order.
        Veng::vector<TableRow> Rows;

        /// @brief Parses a table document's rows against a resolved schema.
        ///
        /// Tolerant in the same way the schema read is: a cell that fails to bind is left
        /// default-constructed and reported, so a table with one bad value still opens for repair.
        /// A cell's diagnostics are `JsonReadFieldValue`'s, so they read exactly as the cook's do.
        /// @param doc          The parsed `*.table.json` document.
        /// @param columns      The resolved schema columns.
        /// @param types        Registry the column types resolve through.
        /// @param diagnostics  Receives one message per cell that failed to bind.
        /// @return The parsed document.
        [[nodiscard]] static TableDataDocument
        Read(const nlohmann::json& doc, std::span<const Veng::TableColumnDescriptor> columns,
             const Veng::TypeRegistry& types, Veng::vector<Veng::string>& diagnostics);

        /// @brief Assigns the schema reference and rows into an existing document.
        ///
        /// Preserves unknown top-level keys and, per row, every key the schema does not name.
        /// @param into     Destination JSON object, patched in place.
        /// @param columns  The resolved schema columns, naming what each row writes.
        /// @param types    Registry the column types resolve through.
        void Write(nlohmann::json& into, std::span<const Veng::TableColumnDescriptor> columns,
                   const Veng::TypeRegistry& types) const;

        /// @brief Appends a row of default-constructed cells.
        /// @param columns  The resolved schema columns.
        /// @param types    Registry the column types resolve through.
        /// @return The new row's index.
        Veng::usize AddRow(std::span<const Veng::TableColumnDescriptor> columns,
                           const Veng::TypeRegistry& types);

        /// @brief Inserts a value-copy of a row directly after it.
        ///
        /// The copy round-trips each cell through the JSON walkers — a reflected type carries no
        /// copy thunk, and the walkers are the encoding both sides of a save already agree on.
        /// @param row      Index of the row to duplicate.
        /// @param columns  The resolved schema columns.
        /// @param types    Registry the column types resolve through.
        /// @return The new row's index.
        Veng::usize DuplicateRow(Veng::usize row,
                                 std::span<const Veng::TableColumnDescriptor> columns,
                                 const Veng::TypeRegistry& types);

        /// @brief Removes a row.
        /// @param row  Index of the row to remove.
        void RemoveRow(Veng::usize row);

        /// @brief Moves a row to a new position, shifting the rest.
        /// @param from  Index of the row to move.
        /// @param to    Destination index.
        void MoveRow(Veng::usize from, Veng::usize to);

        /// @brief Returns, per row, whether its key cell duplicates an earlier row's.
        ///
        /// The uniqueness rule the importer enforces, evaluated live so the offending cells can be
        /// marked while they are being authored. Keys are compared by their authored JSON value,
        /// which is the same identity the cook's decoded comparison resolves to.
        /// @param columns    The resolved schema columns.
        /// @param keyColumn  Index of the key column within @p columns.
        /// @param types      Registry the column types resolve through.
        /// @return One flag per row, true when the row's key appeared in an earlier row.
        [[nodiscard]] Veng::vector<bool>
        DuplicateKeys(std::span<const Veng::TableColumnDescriptor> columns, Veng::u32 keyColumn,
                      const Veng::TypeRegistry& types) const;
    };
}
