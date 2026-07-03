#!/usr/bin/env python3
"""Migrate minted 64-bit ids in checked-in JSON assets to the canonical hex-string form.

Every minted ``AssetId`` / ``SystemId`` / ``ActionId`` stored in a ``*.vengpack.json``,
``project.veng``, or per-asset JSON source is converted from a bare JSON number to the
canonical ``"0x{:016X}"`` string (``0x`` + exactly 16 uppercase hex digits) — the same
spelling the C++ literal convention uses. A string is a string, so the value round-trips
through any JSON tool losslessly; a bare number past ``2^53`` silently truncates through
any IEEE-754-double JSON pipeline, which is the bug this migration closes.

Reliability rules (do not relax):

* Each id is parsed with Python's arbitrary-precision ``int`` and re-emitted with
  ``"0x{:016X}"``. Never ``float`` (it truncates past ``2^53``), and never a
  digit-rewriting regex (which cannot know a value's width or type).
* The document is walked with ``json.load`` / ``json.dump`` and an ``object_pairs_hook``
  that preserves key order, so a converted file differs only in the id values.
* Only the id-bearing keys are edited, and the convert / keep-numeric decision is scoped
  **per file schema** as ``(file glob, key path)`` rules — the same key name (``parent``,
  ``id``) means different things in different files, so a rule can never convert the
  wrong file's key. Every other integer is left untouched.

The **one hard rule**: these integers are *not* minted ids and stay numeric —
material-field std140 ``value`` scalars, mesh material-slot map *keys* (decimal-string
keys), inputmap ``control`` (a raw scancode), pack ``version``, texture/environment
``max_size``, animation ``clip`` / ``trimStart`` / ``trimEnd``. The ``.prefab.json`` /
``.level.json`` files (game-defined ``AssetHandle`` field names) and ``.graph.json`` asset
properties are *not* swept by this script — they are hand-migrated — so this script never
walks them.

Run from the repo root:

    python3 scripts/migrate_ids.py [--check] [PATH ...]

With no PATH arguments it sweeps the default in-repo asset roots. ``--check`` reports what
would change and exits nonzero if anything would, without writing.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import sys
from collections import OrderedDict
from pathlib import Path

# --------------------------------------------------------------------------------------
# The canonical codec: arbitrary-precision int in, "0x" + 16 uppercase hex digits out.
# --------------------------------------------------------------------------------------


def format_hex_id(value: int) -> str:
    """Format a non-negative 64-bit int as the canonical "0x{:016X}" string."""
    if not isinstance(value, int) or isinstance(value, bool):
        raise TypeError(f"expected an int id, got {value!r}")
    if value < 0 or value > 0xFFFFFFFFFFFFFFFF:
        raise ValueError(f"id {value} out of 64-bit range")
    return f"0x{value:016X}"


def is_bare_id_number(value) -> bool:
    """True when value is a bare JSON integer eligible for conversion (not already hex)."""
    return isinstance(value, int) and not isinstance(value, bool)


# --------------------------------------------------------------------------------------
# Per-file-schema rules. Each rule names, for a glob, the id-bearing key *paths* to
# convert. A path is a tuple walked from the document root; "[]" descends into every
# element of an array, "*" descends into every value of an object.
# --------------------------------------------------------------------------------------

# The pack-manifest rule: each asset entry's `id` (`version` stays numeric). A manifest
# is recognized by shape, not filename — a fixture pack is often named `mesh_pack.json`
# or `gbuffer_pack.json`, not `*.vengpack.json` — so this rule is applied to any file
# that structurally *is* a manifest (see `is_pack_manifest`), covering both.
PACK_MANIFEST_KEYPATHS = [("assets", "[]", "id")]

# (glob, list of key-path tuples that carry a minted id and must convert)
CONVERT_RULES = [
    # Pack manifest by name. Structural detection also catches unsuffixed fixture packs.
    ("*.vengpack.json", PACK_MANIFEST_KEYPATHS),
    # Project entrypoint: the startup level id.
    ("project.veng", [("startupLevel",)]),
    # Shader source: the vertex-layout id.
    ("*.shader.json", [("vertex_layout",)]),
    # Material parent: default-instance id, shader ids, and each texture field's `id`.
    ("*.vmat.json", [("defaultInstance",), ("shaders", "vertex"), ("shaders", "fragment"),
                     ("fields", "[]", "id")]),
    # Material instance: the parent id. (`overrides` values are param scalars or texture
    # ids; no in-tree instance carries a texture override, so none is swept here.)
    ("*.vmatinst.json", [("parent",)]),
    # Mesh: the skeleton id and each material-slot *value* (the map keys stay numeric).
    ("*.mesh.json", [("skeleton",), ("materials", "*")]),
    # Input map: each action's id and each binding's action id.
    ("*.inputmap.json", [("actions", "[]", "id"), ("bindings", "[]", "action")]),
]

# Globs the script must never sweep — hand-migrated (game-defined key names defeat any
# static rule) — asserted clean by the post-sweep guard, not converted here.
SKIP_GLOBS = ["*.prefab.json", "*.level.json", "*.graph.json"]

# Default sweep roots (relative to the repo root), matching the plan's file set.
DEFAULT_ROOTS = [
    "engine/assets",
    "editor/assets",
    "examples/hello-triangle",
    "examples/template",
    "tests",
]

# Directories never swept (build outputs, worktrees, CMake API replies).
EXCLUDE_DIR_PARTS = {"build", "build-debug", "cmake-build-debug", ".cmake", ".claude", ".git"}


def is_pack_manifest(doc) -> bool:
    """True when a loaded document has a pack manifest's shape (`version` + `assets[]`)."""
    return (isinstance(doc, dict) and "assets" in doc and isinstance(doc["assets"], list)
            and "version" in doc)


def rules_for(path: Path, doc=None):
    """Return the convert key-paths for this file: by filename glob, else by manifest shape.

    `doc` is the already-loaded document when available; passing it lets an unsuffixed
    fixture pack (e.g. `mesh_pack.json`) be recognized structurally.
    """
    name = path.name
    for glob, keypaths in CONVERT_RULES:
        if fnmatch.fnmatch(name, glob):
            return keypaths
    if doc is not None and is_pack_manifest(doc):
        return PACK_MANIFEST_KEYPATHS
    return None


def should_skip(path: Path) -> bool:
    name = path.name
    return any(fnmatch.fnmatch(name, glob) for glob in SKIP_GLOBS)


# --------------------------------------------------------------------------------------
# Applying a key-path to a loaded document.
# --------------------------------------------------------------------------------------


def _convert_slot(container, key, label, converted):
    """Convert one container[key] terminal in place if it is a bare-int id."""
    value = container[key]
    if isinstance(value, str):
        return  # already migrated (idempotent re-run)
    if is_bare_id_number(value):
        container[key] = format_hex_id(value)
        converted.append((label, value, container[key]))


def convert_at_path(node, keypath, converted):
    """Walk keypath into node, converting each terminal bare-int id to a hex string.

    A step is ``"[]"`` (descend every list element), ``"*"`` (descend every object
    value), or a named key. When a step is the *last* in the path it names the slot(s)
    to convert; earlier steps only descend. Records each conversion as (label, old, new).
    """
    if not keypath:
        return

    head, rest = keypath[0], keypath[1:]
    terminal = not rest

    if head == "[]":
        if not isinstance(node, list):
            return
        for i, elem in enumerate(node):
            if terminal:
                _convert_slot(node, i, "[]", converted)
            else:
                convert_at_path(elem, rest, converted)
        return

    if head == "*":
        if not isinstance(node, dict):
            return
        for key in list(node.keys()):
            if terminal:
                _convert_slot(node, key, key, converted)
            else:
                convert_at_path(node[key], rest, converted)
        return

    # A named key.
    if not isinstance(node, dict) or head not in node:
        return
    if terminal:
        _convert_slot(node, head, head, converted)
    else:
        convert_at_path(node[head], rest, converted)


# --------------------------------------------------------------------------------------
# The kept-numeric audit: report every integer this script deliberately left numeric in
# an in-scope file, so a genuine miss can't hide among the expected positives.
# --------------------------------------------------------------------------------------


def audit_kept_numeric(node, path_prefix, kept):
    """Collect every remaining bare-int leaf as (dotted-path, value) into `kept`."""
    if isinstance(node, dict):
        for key, value in node.items():
            audit_kept_numeric(value, f"{path_prefix}.{key}" if path_prefix else key, kept)
    elif isinstance(node, list):
        for i, value in enumerate(node):
            audit_kept_numeric(value, f"{path_prefix}[{i}]", kept)
    elif is_bare_id_number(node):
        kept.append((path_prefix, node))


# --------------------------------------------------------------------------------------
# Driver.
# --------------------------------------------------------------------------------------


def load_ordered(text):
    return json.loads(text, object_pairs_hook=OrderedDict)


def dump_ordered(doc):
    # Match the tree's 2-space indentation; keep non-ASCII (none in ids) intact.
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def iter_files(roots):
    for root in roots:
        root_path = Path(root)
        if root_path.is_file():
            yield root_path
            continue
        for path in sorted(root_path.rglob("*")):
            if not path.is_file():
                continue
            if any(part in EXCLUDE_DIR_PARTS for part in path.parts):
                continue
            if path.suffix == ".json" or path.name == "project.veng":
                yield path


def migrate_file(path: Path, check: bool):
    """Return (changed, converted_list) for one file; write unless check."""
    original = path.read_text(encoding="utf-8")
    doc = load_ordered(original)

    keypaths = rules_for(path, doc)
    if keypaths is None:
        return False, []

    converted = []
    for keypath in keypaths:
        convert_at_path(doc, keypath, converted)

    if not converted:
        return False, []

    if not check:
        path.write_text(dump_ordered(doc), encoding="utf-8")
    return True, converted


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*", help="files or roots to sweep (default: asset roots)")
    parser.add_argument("--check", action="store_true",
                        help="report changes and exit nonzero if any, without writing")
    parser.add_argument("--audit-kept", action="store_true",
                        help="also list every integer left numeric in swept files")
    args = parser.parse_args(argv)

    roots = args.paths if args.paths else DEFAULT_ROOTS

    total_files = 0
    total_ids = 0
    kept_total = 0
    for path in iter_files(roots):
        if should_skip(path):
            continue
        changed, converted = migrate_file(path, args.check)
        if changed:
            total_files += 1
            total_ids += len(converted)
            verb = "would convert" if args.check else "converted"
            print(f"{path}: {verb} {len(converted)} id(s)")
            for key, old, new in converted:
                print(f"    {key}: {old} -> {new}")
        if args.audit_kept:
            doc = load_ordered(path.read_text(encoding="utf-8"))
            if rules_for(path, doc) is not None:
                kept = []
                audit_kept_numeric(doc, "", kept)
                for dotted, value in kept:
                    kept_total += 1
                    print(f"    [kept numeric] {path}: {dotted} = {value}")

    print(f"\n{'Would migrate' if args.check else 'Migrated'} {total_ids} id(s) "
          f"across {total_files} file(s).")
    if args.audit_kept:
        print(f"Left {kept_total} integer(s) numeric across swept files.")

    if args.check and total_ids > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
