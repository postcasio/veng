# Authoring a data table

Games are full of tabular tuning data: weapon stats, enemy archetypes, loot rules, damage
curves. veng answers that with a pair of cooked assets — a **`TableSchema`** declaring the
columns, and a **`DataTable`** holding rows validated against it. Both are CPU-only, both load by
`AssetId` through the ordinary `Load`/`LoadSync` path, and both are authored as JSON the cooker
checks before anything ships.

The guide's worked example is `examples/template`, whose pack carries
`assets/tables/tuning.tableschema.json` and `assets/tables/tuning.table.json` and reads a row in
`main.cpp`.

## A column is a reflected type

The one idea the rest follows from: **a column carries a reflection `TypeId`**, not a
table-specific kind. There is no column vocabulary to extend and no table-specific codec — a cell
is encoded, decoded, and validated by the same `WriteFieldValue` / `ReadFieldValue` /
`JsonReadFieldValue` walkers that encode a component's fields into a prefab blob.

So **any registered type is a legal column**: the scalars, `vec`/`quat`/`mat` types, `string`, a
reflected enum, an `AssetHandle<T>`, a nested struct, and — through a struct carrying a
`VE_ARRAY_FIELD` — an array. A cell authors in JSON exactly as that type authors as a struct
field, and a malformed one is a cook error located down to the offending inner field.

Two consequences worth knowing up front:

- **A table cook requires a type registry.** Column types resolve through
  `CookContext::Types`, so `vengc cook` without `--module` fails both table importers loudly
  rather than guessing a layout. This is true even for a table using only engine builtin types.
- **Rows are variable-size in general.** A `string` or array cell has no constant width, so rows
  are addressed through a `u32` row directory. When *every* column's type encodes to a constant
  width the importer omits the directory and records a stride instead. That is a property of the
  cooked blob, not of the format contract — see [the fast path](#the-fixed-stride-fast-path).

## 1. Declare the schema

A `*.tableschema.json` is a column list plus the key column:

```json
{
  "columns": [
    { "name": "id",        "type": "Veng::i64" },
    { "name": "spinSpeed", "type": "Veng::f32" },
    { "name": "tint",      "type": "Veng::vec4" },
    { "name": "enabled",   "type": "bool" },
    { "name": "icon",      "type": "Veng::AssetHandle<Texture>" },
    { "name": "label",     "type": "Veng::string" }
  ],
  "key": "id"
}
```

**`"type"` names a registered type by its fully-qualified name** — the same spelling a variant
alternative's `"type"` tag matches against. `"Veng::i64"`, `"Veng::vec4"`,
`"Veng::AssetHandle<Texture>"`, `"MyGame::Cadence"`. Get the spelling wrong and the cook says so
and lists nothing else; it does not fall back to a guess.

The rules a set of columns must satisfy live in exactly one function, **`LayOutTableSchema`**
(`Veng/Asset/DataTable.h`), which both the cooker's `ParseTableSchema` and the editor's schema
panel call. A schema the editor reports as valid is therefore precisely a schema the cook
accepts. It requires:

- names unique, non-empty, and within the cooked name capacity;
- every column's type registered, and not `FieldClass::Reference` (an intra-prefab entity
  reference means nothing in a table);
- a `"key"` naming a column that exists and whose type can be ordered.

It also does the layout: cells are packed in declaration order at their encoded widths, and a
column keeps a constant offset only while every *preceding* column is fixed-size.

### What can be a key

A key column needs a total order and a stable cooked encoding, which is `TableKeyKindForType`'s
job to decide. Legal keys are `string` and the integer scalars `u8`, `i32`, `u32`, `i64`.

Deliberately excluded: floats (an equality-keyed lookup over them is a trap), `bool` (it cannot
key more than two rows), and `u64` — the key index widens every integer key to `i64`, so a `u64`
above 2^63 would sort as negative and break the binary search.

Put the key column first if you like, but nothing requires it; the schema records its index.

## 2. Author the rows

A `*.table.json` names its schema by hex id and lists rows as objects keyed by column name:

```json
{
  "schema": "0x611C84D6F60F2E72",
  "rows": [
    { "id": 20, "spinSpeed": 2.5,  "tint": [0.9, 0.4, 0.2, 1.0], "enabled": true,
      "icon": "0x502E61AE5D720E64", "label": "brisk" },
    { "id": 10, "spinSpeed": 0.75, "tint": [0.2, 0.5, 0.9, 1.0], "enabled": true,
      "icon": "0x502E61AE5D720E64", "label": "calm" },
    { "id": 30, "spinSpeed": 0.0,  "tint": [0.6, 0.6, 0.6, 1.0], "enabled": false,
      "icon": "0x502E61AE5D720E64", "label": "still" }
  ]
}
```

Rows are authored in whatever order suits the file — the importer sorts the key index, so the
example's `20, 10, 30` is fine and `FindRow` still binary-searches.

`DataTableImporter` resolves the schema through `CookContext::Resolve`, re-parses it, then binds
every cell with `JsonReadFieldValue` and encodes it with `WriteFieldValue`. Each of these is a
**located cook error**, not a silent skip:

- a column the schema does not declare, or a schema column the row omits;
- a value that does not bind to the column's type;
- two rows carrying the same key;
- an asset-handle cell naming an id that does not resolve, **or one whose target is the wrong
  asset type**.

Every asset-handle cell resolves through `Resolve` as well, which is what records the schema and
each referenced asset in the cooked dependency graph — so editing a referenced source recooks the
table.

The importer also fails the cook if the row region would cross the 4 GiB a `u32` row offset can
address. Tables are sized for full residency — 10 MB is a normal large table, 100 MB the working
extreme — so the ceiling is a guard, not a design limit.

## 3. Add both to the pack

Two ordinary manifest entries; the schema is just another asset:

```json
{ "id": "0x611C84D6F60F2E72", "type": "TableSchema", "source": "tables/tuning.tableschema.json" },
{ "id": "0x8C1F2A47B0D3E915", "type": "DataTable",   "source": "tables/tuning.table.json" }
```

Mint both ids with `vengc generate-id --reference <pack.json>`. Because a table cook needs a type
registry, the pack's cook must pass `--module` — `veng_add_project(... MODULE <lib>)` is what
wires that.

## 4. Read it at runtime

A table loads like anything else, and its schema comes along as an ordinary streamed dependency —
you never load the schema yourself:

```cpp
const AssetResult<AssetHandle<DataTable>> tuning =
    GetAssetManager().LoadSync<DataTable>(TuningTableId);
if (!tuning)
{
    return;
}

const optional<u32> row = (*tuning)->FindRow(TuningRowKey);
if (!row)
{
    return;
}

const TableColumn<f32> spinSpeed = (*tuning)->GetColumn<f32>("spinSpeed");
const TableColumn<AssetId> icon   = (*tuning)->GetAssetIdColumn("icon");
const Result<std::string_view> label = (*tuning)->GetStringCell(*row, "label");
```

`FindRow(key)` is an allocation-free binary search over the sorted key index — a **separate**
structure from the row directory, because the two answer different questions (key → row index,
row index → bytes). It takes an `i64` or a `std::string_view` depending on the table's
`GetKeyKind()`; calling the wrong overload is API misuse and asserts.

### Choosing an accessor

| | Use | Cost |
|---|---|---|
| `GetColumn<T>(name)` | a fixed-size column at a constant offset | resolve once, then `column[row]` is a flat bounds-checked read — the hot-loop path |
| `GetAssetIdColumn(name)` | an `AssetHandle<T>` column | same, yielding a bare `AssetId` |
| `GetStringCell(row, name)` | a `string` column | a view into the row's encoded bytes, no copy |
| `ReadCell<T>(row, name, out)` | anything, including variable-size columns | the general reflected read; reaching a cell past the first variable-size column walks that row's preceding cells |
| `ReadRow<T>(row, out)` | a whole row into a struct | binds cells to fields **by name** |

`ReadRow<T>` is the typed row bridge: each column whose name and reflected type match a field of
`T` decodes straight into that field, a column `T` does not declare is decoded and discarded, and
a field no column declares keeps its caller-constructed value — the same drift tolerance the
record encoding gives any struct.

**An asset-handle cell yields a bare `AssetId`, never a handle.** The table holds no reference to
what it names and loads nothing; the consumer decides what to load and when. That is what keeps a
100 MB table's residency its own bytes.

### Fatal versus recoverable

The split follows the engine's rule exactly, and it is worth internalizing because the two look
similar:

- **Fatal (`VE_ASSERT`) — API misuse in your code.** Naming a column the schema does not declare,
  reading a column as the wrong type, or calling `GetColumn<T>` on a variable-size column or one
  with no constant offset. Read that column with `ReadCell` instead.
- **Recoverable (`AssetError::Corrupt`) — a malformed blob**, rejected by the loader before you
  ever hold a table. That includes a blob whose key index, row directory, and row region disagree
  on how many rows exist.

`ReadCell` / `ReadRow` / `GetStringCell` additionally return a `Result` for a malformed *encoded
row*, which is the recoverable residue.

### The fixed-stride fast path

When every column's type is fixed-size, the importer omits the row directory and records a
stride; addressing becomes arithmetic. `IsFixedStride()` and `GetRowStride()` expose it.

**Nothing should branch on it.** The accessor API is identical under both layouts, so neither the
runtime nor the editor needs to know which one a given blob took — treat it as a cooked-blob
property you can observe, not a mode you handle. Put your fixed-size columns first if you want
more of them to keep a constant offset (and so stay eligible for `GetColumn<T>`); that ordering is
the only authoring decision the layout rewards.

## 5. Edit it in the editor

Two panels, both on the editor-wide explicit-save contract — edits accumulate in memory and reach
disk only through File▸Save / Ctrl+S:

- **`TableSchemaEditorPanel`** edits the column list (add / remove / rename / retype / reorder)
  and the key column, whose combo is restricted to the types a key index can order. It shows the
  resolved row layout and every validation failure live, and warns — without blocking — that a
  removed or retyped column invalidates tables already cooked against the schema. That is the
  *table's* cook error to surface, not the schema's.
- **`DataTableEditorPanel`** is the grid: one column per schema column, each cell drawn by the
  inspector widget for the column's reflected type, so an enum column gets a named combo and an
  asset-handle column gets the asset picker with no per-column widget written anywhere. A
  composite column (struct / variant / array) cannot fit one cell and opens a popup holding a real
  property table. Rows add / remove / duplicate / reorder; duplicate keys are marked live; the row
  body virtualizes, so a large table stays responsive.

Both panels validate through the importer's own rules rather than a copy — the schema panel calls
the same `LayOutTableSchema`, and a cell binds through the same `JsonReadFieldValue` — so the
diagnostics they show are the cook's, character for character.

## Referencing a table from a component

`AssetHandle<DataTable>` and `AssetHandle<TableSchema>` are both reflected leaves, so a component
can hold one and a prefab can author it as a plain hex id:

```cpp
struct Tuning
{
    AssetHandle<DataTable> Table;
};
```

```json
"Tuning": { "Table": "0x8C1F2A47B0D3E915" }
```

It resolves as an ordinary load-time prefab dependency and is resident before your system runs —
usually nicer than the `LoadSync` above.

## Boundaries

- **No per-column preload.** An asset-reference column hands back ids; nothing eagerly loads them.
- **No CSV import/export.** Tables are authored as JSON or through the editor grid.
- **No incremental recook.** Saving a table recooks the whole table.
- **A schema change does not migrate cooked tables.** Recook them; the table's own cook is what
  reports a row that no longer matches.
