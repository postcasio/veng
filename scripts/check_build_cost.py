#!/usr/bin/env python3
"""Compare a ``VENG_TIME_TRACE=ON`` build tree's compile cost against the checked-in baseline.

The baseline is ``docs/build-cost-baseline.md``; this script is what produces it and what
checks a tree against it. It reads clang's ``-ftime-trace`` JSON **directly** — one Chrome
Trace document per translation unit, written beside that TU's object file — aggregates the
whole tree, and fails when a tracked figure has regressed past the threshold below.

Run it after any change to the reflection headers, the include graph, or the PCH set::

    cmake -B build-trace -S . -DVE_DEBUG=ON -DVENG_TIME_TRACE=ON
    cmake --build build-trace -j 6
    python3 scripts/check_build_cost.py --tree build-trace

To refresh the baseline (a deliberate act, with a commit whose body says why the number
moved — a refresh with no stated reason turns the tripwire into a rubber stamp)::

    python3 scripts/check_build_cost.py --tree build-trace --write-baseline

Exit codes: ``0`` pass, ``1`` regression, ``2`` skipped (no tracing tree, or the tree's
provenance does not match the baseline's), ``3`` inconclusive (the tree carries a stall
artifact — see ``--help`` and the artifact note below).

**The thresholds.** Total compile CPU may exceed the baseline by 5 %. A single origin's
instantiation total may exceed its baseline by 10 % *and* by 5 s absolute — both conjuncts,
because 10 % of a small origin is thermal drift while 10 % of a large one is a real header
change.

**Provenance mismatch skips, it does not fail.** Compile CPU varies by a factor of ~2 across
CPU models, so a baseline taken on another host, compiler, build type or option set is not
comparable. A check that fires permanently on everyone else's machine is a check that gets
ignored.

**A single tracing build is not a measurement.** ``-ftime-trace`` durations are wall time
*inside* the compile, so a machine-level stall lands in them as compile cost with nothing to
distinguish it — a measured instance inflated four adjacent TUs ~170x and the whole tree by
57 %. The tree is therefore scanned for a small cluster of order-of-magnitude outliers before
anything is compared, and a detected cluster makes the run **inconclusive** rather than a
failure: an artifact and a regression are indistinguishable to a threshold, and reporting one
as the other is worse than reporting neither. Recompile the named TUs serially (touch the
source, rebuild that target) and re-run.

**This is not a ``ctest`` test.** It needs a tracing tree no ordinary build produces, so as a
test it would be permanently skipped or would force a cold tracing build into every run. It is
a script an operator or a CI job invokes.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

# --- thresholds --------------------------------------------------------------

TOTAL_TOLERANCE = 0.05  # total compile CPU: +5 %
ORIGIN_TOLERANCE = 0.10  # one origin's instantiation: +10 % ...
ORIGIN_ABSOLUTE_S = 5.0  # ... and +5 s, both conjuncts

# A cluster of TUs each an order of magnitude above the next TU down is a machine
# stall recorded as compile cost, not a compile cost. Bounded in size because a
# real regression is broad: a genuinely slow tree has no such gap.
STALL_RATIO = 10.0
STALL_MAX_CLUSTER = 16

EXIT_PASS, EXIT_FAIL, EXIT_SKIP, EXIT_INCONCLUSIVE = 0, 1, 2, 3

# --- the trace format --------------------------------------------------------

# clang names a TU's trace after its object file, so the source extension survives.
# Everything else in a build tree ending in .json — compile_commands.json, cache
# entries, fixtures — is excluded by the same test.
TU_EXTENSIONS = {".cpp", ".cc", ".cxx", ".c", ".m", ".mm", ".hxx", ".hpp", ".h", ".hh"}

# clang's own pre-aggregated summary events arrive on their own tid at ts 0 carrying a
# whole category's total as `dur`. Under any root rule they read as additional roots and
# silently double every total, so they are excluded on the way in — and summed separately
# as the independent cross-check for the rule that replaces them.
SUMMARY_PREFIX = "Total "

FRONTEND = "Frontend"
BACKEND = "Backend"
EXECUTE = "ExecuteCompiler"
SOURCE = "Source"
INSTANTIATE = frozenset({"InstantiateFunction", "InstantiateClass"})

# The origin buckets, in match order — **first match wins**, which is why `std::format`
# precedes `libc++` (every one of its symbols is also a `std::` one). This is a data table,
# not code structure: a new heavy dependency is one more entry.
#
# Only the symbol forms are carried. This script buckets the *instantiation* branch alone,
# whose event details are template-ids in clang's pretty form; the path forms that bucket
# the parse branch, and the mangled forms the backend branch needs, are not reached here.
ORIGINS = (
    ("std::format", ("std::__format", "std::format", "std::formatter", "std::vformat", "std::basic_format")),
    ("libc++", ("std::", "__gnu_cxx::")),
    ("nlohmann", ("nlohmann::",)),
    ("fmt", ("fmt::",)),
    ("veng reflection", ("Veng::VengReflect", "VengReflect", "Veng::TypeRegistry", "TypeRegistry", "Veng::Reflection")),
    ("first-party", ("Veng::", "VengEditor::", "VengGraph::", "VengCook::")),
)
RESIDUAL_ORIGIN = "other"


def origin_of(detail: str) -> str:
    """Bucket one instantiation event's ``args.detail`` by first-match-wins prefix."""
    for name, symbols in ORIGINS:
        for prefix in symbols:
            if detail.startswith(prefix):
                return name
    return RESIDUAL_ORIGIN


@dataclass
class Span:
    name: str
    detail: str | None
    ts: float
    dur: float
    parent: int = -1


def to_spans(events: list) -> dict[int, list[Span]]:
    """Chrome Trace events to spans with a resolved duration, one list per tid.

    ``X`` events carry their own duration. ``Source`` arrives instead as async ``b``/``e``
    pairs sharing one id, paired LIFO per (pid, tid, id) after ordering by timestamp —
    which is exactly their nesting. An unclosed ``b`` has no duration to attribute and is
    dropped rather than guessed at.
    """
    complete: list[tuple[int, Span]] = []
    pending: list[dict] = []
    for e in events:
        if not isinstance(e, dict):
            continue
        name = e.get("name")
        if not isinstance(name, str) or e.get("ph") == "M" or name.startswith(SUMMARY_PREFIX):
            continue
        try:
            ts = float(e["ts"])
        except (KeyError, TypeError, ValueError):
            continue
        args = e.get("args")
        detail = args.get("detail") if isinstance(args, dict) else None
        if not isinstance(detail, str):
            detail = None
        ph = e.get("ph")
        if ph == "X":
            try:
                dur = float(e.get("dur", 0.0))
            except (TypeError, ValueError):
                dur = 0.0
            complete.append((e.get("tid", 0), Span(name, detail, ts, dur)))
        elif ph in ("b", "e"):
            pending.append({"e": e, "ts": ts, "detail": detail, "name": name})

    pending.sort(key=lambda p: (p["ts"], 0 if p["e"].get("ph") == "b" else 1))
    open_stacks: dict[tuple, list[dict]] = {}
    for p in pending:
        e = p["e"]
        key = (e.get("pid"), e.get("tid"), e.get("id"))
        stack = open_stacks.setdefault(key, [])
        if e.get("ph") == "b":
            stack.append(p)
            continue
        if not stack:
            continue  # an `e` with no `b`: a truncated or foreign stream
        begin = stack.pop()
        complete.append((e.get("tid", 0), Span(begin["name"], begin["detail"], begin["ts"], p["ts"] - begin["ts"])))

    by_tid: dict[int, list[Span]] = {}
    for tid, span in complete:
        by_tid.setdefault(tid, []).append(span)
    return by_tid


def link(spans: list[Span]) -> list[Span]:
    """Link each span to the innermost span containing it.

    Nesting on one tid is containment, so ties break longest-first: an enclosing span opens
    before what it contains.
    """
    spans.sort(key=lambda s: (s.ts, -s.dur))
    open_idx: list[int] = []
    for i, s in enumerate(spans):
        while open_idx:
            top = spans[open_idx[-1]]
            if top.ts + top.dur > s.ts:
                break
            open_idx.pop()
        s.parent = open_idx[-1] if open_idx else -1
        open_idx.append(i)
    return spans


def roots(spans: list[Span], member) -> list[int]:
    """The non-nested members of one family, by the root rule run within that family.

    A span counts toward the family only when no ancestor already did, which is what stops
    nesting from counting the same microseconds twice. Root-ness is per family, never global:
    every TU's outermost span is ``ExecuteCompiler``, so one global pass would attribute
    everything to it. An ``InstantiateFunction`` inside another one is counted once, while
    one inside a ``Source`` still counts toward instantiation.
    """
    counted = bytearray(len(spans))
    out: list[int] = []
    for i, s in enumerate(spans):
        if not member(s):
            continue
        p = s.parent
        nested = False
        while p >= 0:
            if counted[p]:
                nested = True
                break
            p = spans[p].parent
        counted[i] = 1
        if not nested:
            out.append(i)
    return out


def carrier_map(spans: list[Span], carriers: set[int]) -> list[int]:
    """For each span, the nearest self-or-ancestor carrier, or -1 under none.

    Linked spans are ordered parent-before-child, so one forward pass resolves the whole
    forest — which matters: walking ancestors per (carrier, span) pair is quadratic on the
    TUs carrying tens of thousands of instantiations.
    """
    out = [-1] * len(spans)
    for i, s in enumerate(spans):
        out[i] = i if i in carriers else (out[s.parent] if s.parent >= 0 else -1)
    return out


@dataclass
class TuCost:
    """One translation unit's aggregated cost, in seconds."""

    name: str = ""
    execute: float = 0.0
    frontend: float = 0.0
    backend: float = 0.0
    instantiation: float = 0.0
    parse: float = 0.0
    # When the compiler wrote this TU's trace, as an epoch timestamp. clang's own `ts`
    # values restart at zero in every document, so they say nothing about where in the
    # build a TU compiled; the file's mtime is the only ordering the tree carries.
    finished: float = 0.0
    origins: dict[str, float] = field(default_factory=dict)
    summary: dict[str, float] = field(default_factory=dict)


US = 1e6


def aggregate_tu(path: Path, tu_name: str) -> TuCost | None:
    """Fold one TU's ``-ftime-trace`` document into the figures the baseline tracks.

    The hierarchy is TU -> category -> origin, each level attributed by the root rule run
    within its own family, so no microsecond is counted twice on any path down.
    Instantiation is carved out of the ``Source`` spans containing it, so parse cost is
    parse cost net of the instantiation already counted beneath it.
    """
    try:
        with path.open("rb") as fh:
            doc = json.load(fh)
    except (OSError, ValueError):
        return None
    events = doc.get("traceEvents") if isinstance(doc, dict) else None
    if not isinstance(events, list):
        return None

    cost = TuCost(name=tu_name)
    try:
        cost.finished = path.stat().st_mtime
    except OSError:
        cost.finished = 0.0
    for e in events:
        if isinstance(e, dict) and isinstance(e.get("name"), str) and e["name"].startswith(SUMMARY_PREFIX):
            try:
                cost.summary[e["name"][len(SUMMARY_PREFIX) :]] = cost.summary.get(e["name"][len(SUMMARY_PREFIX) :], 0.0) + float(e.get("dur", 0.0))
            except (TypeError, ValueError):
                pass

    for spans in to_spans(events).values():
        link(spans)

        for i in roots(spans, lambda s: s.name == EXECUTE):
            cost.execute += spans[i].dur
        for i in roots(spans, lambda s: s.name == FRONTEND):
            cost.frontend += spans[i].dur
        for i in roots(spans, lambda s: s.name == BACKEND):
            cost.backend += spans[i].dur

        inst_root_list = roots(spans, lambda s: s.name in INSTANTIATE)
        inst_roots = set(inst_root_list)
        for i in inst_root_list:
            cost.instantiation += spans[i].dur

        # Origin rows beneath the instantiation category: bucket every detail-carrying
        # instantiation span in the category's subtree, counting a span unless an ancestor
        # was already attributed to some bucket — the same root rule over the origin family.
        cmap = carrier_map(spans, inst_roots)
        claimed = bytearray(len(spans))
        under_claimed = bytearray(len(spans))
        for i, s in enumerate(spans):
            if s.parent >= 0:
                under_claimed[i] = under_claimed[s.parent] or claimed[s.parent]
            if not s.detail or cmap[i] < 0 or s.name not in INSTANTIATE:
                continue
            claimed[i] = 1
            if under_claimed[i]:
                continue
            name = origin_of(s.detail)
            cost.origins[name] = cost.origins.get(name, 0.0) + s.dur

        # Parse cost is the root Source spans less whatever instantiation was already
        # counted inside them, which keeps the two categories disjoint.
        parse_roots = set(roots(spans, lambda s: s.name == SOURCE))
        parse_us = {i: spans[i].dur for i in parse_roots}
        under_source = carrier_map(spans, parse_roots)
        for j in inst_root_list:
            owner = under_source[j]
            if owner >= 0:
                parse_us[owner] -= spans[j].dur
        for i in parse_roots:
            cost.parse += max(0.0, parse_us[i])

    if cost.execute == 0.0:
        return None
    cost.execute /= US
    cost.frontend /= US
    cost.backend /= US
    cost.instantiation /= US
    cost.parse /= US
    cost.origins = {k: v / US for k, v in cost.origins.items()}
    cost.summary = {k: v / US for k, v in cost.summary.items()}
    return cost


def _worker(args: tuple[str, str]) -> TuCost | None:
    return aggregate_tu(Path(args[0]), args[1])


def find_traces(tree: Path) -> list[tuple[str, str]]:
    """Every per-TU trace under a build tree, as (path, repo-relative TU name)."""
    out: list[tuple[str, str]] = []
    for root, _dirs, files in os.walk(tree):
        for name in files:
            if not name.endswith(".json"):
                continue
            if Path(name[: -len(".json")]).suffix.lower() not in TU_EXTENSIONS:
                continue
            out.append((str(Path(root) / name), name[: -len(".json")]))
    out.sort()
    return out


@dataclass
class TreeCost:
    """The whole-tree roll-up the baseline records."""

    tus: list[TuCost]

    @property
    def execute(self) -> float:
        return sum(t.execute for t in self.tus)

    @property
    def frontend(self) -> float:
        return sum(t.frontend for t in self.tus)

    @property
    def backend(self) -> float:
        return sum(t.backend for t in self.tus)

    @property
    def instantiation(self) -> float:
        return sum(t.instantiation for t in self.tus)

    @property
    def parse(self) -> float:
        return sum(t.parse for t in self.tus)

    def origins(self) -> dict[str, tuple[float, int]]:
        """Instantiation seconds and the number of TUs paying, per origin."""
        totals: dict[str, float] = {}
        counts: dict[str, int] = {}
        for tu in self.tus:
            for name, seconds in tu.origins.items():
                totals[name] = totals.get(name, 0.0) + seconds
                counts[name] = counts.get(name, 0) + 1
        return {k: (totals[k], counts[k]) for k in sorted(totals, key=lambda n: -totals[n])}

    def summary(self) -> dict[str, float]:
        """clang's own ``Total *`` roll-up — the independent cross-check."""
        out: dict[str, float] = {}
        for tu in self.tus:
            for k, v in tu.summary.items():
                out[k] = out.get(k, 0.0) + v
        return out


def stall_cluster(tus: list[TuCost]) -> list[TuCost]:
    """The suspected stall cluster: a bounded prefix of TUs an order above the next.

    A machine stall inflates a handful of concurrently-compiling TUs together and reads as
    compile cost. A real regression is broad and leaves no such gap, so a gap this large at
    the top of a bounded prefix is the artifact and not the thing being measured.
    """
    ranked = sorted(tus, key=lambda t: -t.execute)
    for size in range(min(STALL_MAX_CLUSTER, len(ranked) - 1), 0, -1):
        if ranked[size].execute > 0 and ranked[size - 1].execute >= STALL_RATIO * ranked[size].execute:
            return ranked[:size]
    return []


# --- provenance --------------------------------------------------------------

CACHE_OPTIONS = (
    "CMAKE_BUILD_TYPE",
    "VE_DEBUG",
    "VE_PROFILE",
    "VENG_TIME_TRACE",
    "VENG_BUILD_TESTS",
    "VENG_BUILD_EXAMPLES",
    "VENG_INSTALL_SDK",
    "VENG_EDITOR_WITH_MCP",
    "VENG_ENABLE_CLANG_TIDY",
    "VENG_ENABLE_COVERAGE",
    "VENG_USE_EMBED",
    "VENG_BUILD_CONFIG",
)


def read_cache(tree: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    cache = tree / "CMakeCache.txt"
    if not cache.is_file():
        return values
    for line in cache.read_text(errors="replace").splitlines():
        m = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
        if m:
            values[m.group(1)] = m.group(2)
    return values


def compiler_of(tree: Path) -> str:
    for path in sorted(tree.glob("CMakeFiles/*/CMakeCXXCompiler.cmake")):
        text = path.read_text(errors="replace")
        cid = re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]*)"\)', text)
        ver = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([^"]*)"\)', text)
        if cid and ver:
            return f"{cid.group(1)} {ver.group(1)}"
    return "unknown"


def host_of() -> str:
    cpu = ""
    if platform.system() == "Darwin":
        try:
            cpu = subprocess.run(
                ["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True, check=False
            ).stdout.strip()
        except OSError:
            cpu = ""
    return f"{platform.system()} {platform.machine()}" + (f" / {cpu}" if cpu else "")


def git_sha(repo: Path) -> str:
    try:
        r = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "--short", "HEAD"], capture_output=True, text=True, check=False
        )
        return r.stdout.strip() or "unknown"
    except OSError:
        return "unknown"


def build_span(tus: list[TuCost]) -> float:
    """Wall seconds from the first trace written to the last — the build's own span."""
    stamps = [t.finished for t in tus if t.finished]
    return max(stamps) - min(stamps) if len(stamps) > 1 else 0.0


def provenance(tree: Path, repo: Path, tus: list[TuCost]) -> dict[str, str]:
    cache = read_cache(tree)
    options = ";".join(f"{k}={cache[k]}" for k in CACHE_OPTIONS if k in cache and k != "CMAKE_BUILD_TYPE")
    return {
        "compiler": compiler_of(tree),
        "build type": cache.get("CMAKE_BUILD_TYPE", "unknown"),
        "options": options,
        "host": host_of(),
        "TUs": str(len(tus)),
        "build span": f"{build_span(tus):.0f} s",
        "git SHA": git_sha(repo),
    }


# The provenance fields a comparison requires to agree. The rest are recorded but not
# compared: the tree legitimately grows TUs, the SHA is what a comparison is *for*, and the
# build span is a measurement-conditions signal rather than a configuration.
#
# The span is worth reading before trusting a delta. `-ftime-trace` durations are wall time
# inside the compile, so a tree built in one saturated parallel pass and a tree built with
# idle stretches are not measuring the same machine: contention inflates the TUs compiled
# late in a saturated build by a fifth or more, with no way to tell that from a regression.
COMPARED_PROVENANCE = ("compiler", "build type", "options", "host")


# --- the baseline file -------------------------------------------------------

BASELINE_PATH = Path("docs/build-cost-baseline.md")

PREAMBLE = """# Build-cost baseline

The whole-tree compile cost of this repository, as of the commit that carries this file. It is
the comparison substrate for `scripts/check_build_cost.py`, which parses a `VENG_TIME_TRACE=ON`
tree's `-ftime-trace` JSON and fails when a tracked figure has regressed past its threshold.

**This file is generated, and refreshing it is a deliberate act.** Take a fresh tracing tree and
run the script with `--write-baseline`:

```sh
cmake -B build-trace -S . -DVE_DEBUG=ON -DVENG_TIME_TRACE=ON
cmake --build build-trace -j 6
python3 scripts/check_build_cost.py --tree build-trace --write-baseline
```

Whoever changes the reflection headers, the include graph or the precompiled-header set owns
this number: either the check passes, or the same commit refreshes this file **and its body says
why the number moved**. A refresh with no stated reason turns the tripwire into a rubber stamp,
which is why the rationale lives in the commit message rather than here — the history of this
file *is* the build-cost history.

Every figure is compile **CPU**, summed over translation units, from a cold uncached tracing
build. It is not wall clock, and a warm or cached build sees none of it. The provenance below is
part of the measurement: compile CPU varies by a factor of ~2 across CPU models, and a figure
without its configuration is not a measurement at all. The check **skips** rather than fails when
the tree it is given does not match this provenance.

**A single tracing build is not a measurement, and the recorded build span is how to tell.**
`-ftime-trace` durations are wall time *inside* the compile, so anything the machine does to a
compile — a stall, contention from the rest of the build, thermal throttling — lands in them as
compile cost. A tree built in one saturated parallel pass and a tree built with idle stretches are
not measuring the same machine. Compare spans before trusting a delta, and re-take rather than
reason about a figure taken under conditions that differ.
"""


def render_baseline(cost: TreeCost, prov: dict[str, str]) -> str:
    lines = [PREAMBLE.rstrip(), "", "## Provenance", "", "| field | value |", "|---|---|"]
    for k, v in prov.items():
        lines.append(f"| {k} | {v} |")
    lines += [
        "",
        "## Whole-tree totals",
        "",
        "| figure | seconds |",
        "|---|---|",
        f"| compile CPU (ExecuteCompiler) | {cost.execute:.1f} |",
        f"| Frontend | {cost.frontend:.1f} |",
        f"| Backend | {cost.backend:.1f} |",
        f"| Template instantiation | {cost.instantiation:.1f} |",
        f"| Parse (Source) | {cost.parse:.1f} |",
        "",
        "## Template instantiation by origin",
        "",
        "The origin table is first-match-wins by the instantiated template's qualified name, so",
        "`std::vector<OurType>` counts as `libc++`. Some share of that bucket is therefore driven by",
        "first-party usage rather than inherent to the standard library.",
        "",
        "| origin | seconds | TUs paying |",
        "|---|---|---|",
    ]
    for name, (seconds, tus) in cost.origins().items():
        lines.append(f"| {name} | {seconds:.1f} | {tus} |")
    lines.append("")
    return "\n".join(lines)


@dataclass
class Baseline:
    provenance: dict[str, str]
    totals: dict[str, float]
    origins: dict[str, tuple[float, int]]


def parse_baseline(text: str) -> Baseline:
    """Read the generated markdown back: the three tables, by section heading."""
    section = ""
    prov: dict[str, str] = {}
    totals: dict[str, float] = {}
    origins: dict[str, tuple[float, int]] = {}
    for line in text.splitlines():
        if line.startswith("## "):
            section = line[3:].strip()
            continue
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not cells or set(cells[0]) <= {"-", ":"} or not cells[0]:
            continue
        if section == "Provenance":
            if cells[0] != "field":
                prov[cells[0]] = cells[1] if len(cells) > 1 else ""
        elif section == "Whole-tree totals" and len(cells) > 1:
            try:
                totals[cells[0]] = float(cells[1])
            except ValueError:
                pass
        elif section.startswith("Template instantiation by origin") and len(cells) > 2:
            try:
                origins[cells[0]] = (float(cells[1]), int(cells[2]))
            except ValueError:
                pass
    return Baseline(prov, totals, origins)


# --- the check ---------------------------------------------------------------

TOTAL_KEY = "compile CPU (ExecuteCompiler)"


def report_tree(cost: TreeCost, prov: dict[str, str]) -> None:
    print(f"  provenance: {prov['compiler']}, {prov['build type']}, {prov['host']}, {prov['TUs']} TUs, "
          f"built over {prov['build span']}, {prov['git SHA']}")
    print(f"  compile CPU {cost.execute:.1f} s  (Frontend {cost.frontend:.1f}, Backend {cost.backend:.1f})")
    print(f"  instantiation {cost.instantiation:.1f} s, parse {cost.parse:.1f} s")
    summary = cost.summary()
    if "ExecuteCompiler" in summary:
        print(f"  clang's own Total ExecuteCompiler roll-up: {summary['ExecuteCompiler']:.1f} s (cross-check)")


def main(argv: list[str]) -> int:
    repo = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tree", default="build-trace", help="the VENG_TIME_TRACE=ON build tree (default build-trace)")
    ap.add_argument("--baseline", default=str(repo / BASELINE_PATH), help="the baseline file to compare against")
    ap.add_argument("--write-baseline", action="store_true", help="rewrite the baseline from this tree instead of checking")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="parse workers")
    args = ap.parse_args(argv)

    tree = Path(args.tree)
    if not tree.is_absolute():
        tree = (repo / tree).resolve()
    if not tree.is_dir():
        print(f"skipped: no tracing build tree at {tree}")
        print("  configure one with: cmake -B build-trace -S . -DVE_DEBUG=ON -DVENG_TIME_TRACE=ON")
        return EXIT_SKIP

    traces = find_traces(tree)
    if not traces:
        print(f"skipped: {tree} carries no per-TU -ftime-trace JSON")
        print("  a tracing tree is built with -DVENG_TIME_TRACE=ON, and the option disables the compiler cache")
        return EXIT_SKIP

    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        tus = [t for t in pool.map(_worker, traces, chunksize=4) if t is not None]
    if not tus:
        print(f"skipped: {len(traces)} trace files under {tree} carried no usable ExecuteCompiler span")
        return EXIT_SKIP

    cost = TreeCost(tus)
    prov = provenance(tree, repo, tus)

    print(f"tree: {tree}")
    report_tree(cost, prov)

    cluster = stall_cluster(tus)
    if cluster:
        print()
        print(f"INCONCLUSIVE: {len(cluster)} translation unit(s) cost an order of magnitude more than the next.")
        print("  -ftime-trace records wall time inside the compile, so a machine-level stall reads as")
        print("  compile cost. This is the shape of that artifact, not of a regression.")
        first = min((t.finished for t in tus if t.finished), default=0.0)
        for tu in cluster:
            print(f"    {tu.execute:8.1f} s  finishing at {tu.finished - first:7.1f} s into the build  {tu.name}")
        print("  Recompile these serially (touch the source, rebuild that target) and re-run.")
        return EXIT_INCONCLUSIVE

    if args.write_baseline:
        out = Path(args.baseline)
        out.write_text(render_baseline(cost, prov))
        print(f"\nbaseline written: {out}")
        print("  commit it, and say in the commit body why the number moved.")
        return EXIT_PASS

    baseline_path = Path(args.baseline)
    if not baseline_path.is_file():
        print(f"skipped: no baseline at {baseline_path}; take one with --write-baseline")
        return EXIT_SKIP
    base = parse_baseline(baseline_path.read_text())

    mismatched = [(k, base.provenance.get(k, "(absent)"), prov[k]) for k in COMPARED_PROVENANCE if base.provenance.get(k) != prov[k]]
    if mismatched:
        print(f"\nskipped: this tree's provenance differs from the baseline ({baseline_path}).")
        for k, was, now in mismatched:
            print(f"  {k}:\n    baseline: {was}\n    tree:     {now}")
        print("  Compile CPU varies by a factor of ~2 across CPU models, so the figures are not comparable.")
        return EXIT_SKIP

    base_total = base.totals.get(TOTAL_KEY)
    if base_total is None:
        print(f"skipped: baseline {baseline_path} carries no '{TOTAL_KEY}' row")
        return EXIT_SKIP

    failures: list[str] = []
    limit = base_total * (1.0 + TOTAL_TOLERANCE)
    print(f"\ntotal compile CPU: {cost.execute:.1f} s against baseline {base_total:.1f} s "
          f"({(cost.execute - base_total) / base_total * 100:+.1f} %, limit +{TOTAL_TOLERANCE * 100:.0f} %)")
    if cost.execute > limit:
        failures.append(f"total compile CPU {cost.execute:.1f} s exceeds the baseline {base_total:.1f} s by more than {TOTAL_TOLERANCE * 100:.0f} % (limit {limit:.1f} s)")

    now_origins = cost.origins()
    names = sorted(set(now_origins) | set(base.origins), key=lambda n: -now_origins.get(n, (0.0, 0))[0])
    print("\ntemplate instantiation by origin (seconds / TUs paying):")
    print(f"  {'origin':<18} {'baseline':>18} {'now':>18} {'delta':>10}")
    for name in names:
        was_s, was_tus = base.origins.get(name, (0.0, 0))
        now_s, now_tus = now_origins.get(name, (0.0, 0))
        print(f"  {name:<18} {was_s:11.1f} /{was_tus:5d} {now_s:11.1f} /{now_tus:5d} {now_s - was_s:+9.1f}")
        if now_s > was_s * (1.0 + ORIGIN_TOLERANCE) and now_s - was_s > ORIGIN_ABSOLUTE_S:
            failures.append(
                f"origin '{name}' instantiation {was_s:.1f} s across {was_tus} TUs -> {now_s:.1f} s across {now_tus} TUs "
                f"({now_s - was_s:+.1f} s, {(now_s - was_s) / was_s * 100:+.1f} %)" if was_s else
                f"origin '{name}' instantiation appears at {now_s:.1f} s across {now_tus} TUs, absent from the baseline"
            )

    if failures:
        print("\nFAIL: build cost regressed past the threshold.")
        for f in failures:
            print(f"  - {f}")
        print(f"\n  Either fix the regression, or refresh {BASELINE_PATH} with --write-baseline")
        print("  and say in the commit body why the number moved.")
        return EXIT_FAIL

    print("\nPASS: build cost is within the baseline's thresholds.")
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
