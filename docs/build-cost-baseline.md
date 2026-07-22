# Build-cost baseline

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

**Read the figures in this order of trust: the TU count paying an origin first** — a structural
count, immune to timing — **then the median per-TU ratio against the previous tree, then the
per-origin totals, and the whole-tree total last, for completeness only.** Two takes of one tree
on the reference host have differed by ~11 % on the whole-tree total while their median per-TU
ratio stayed within 5 % of parity, the excess concentrated in the last build deciles: thermal
behaviour, not compile cost. So the check's **5 % total threshold sits below this host's
run-to-run spread**, and a total-only movement is a prompt to re-take the tree rather than
evidence of anything. The **per-origin rule is the load-bearing half** — its +10 % *and* +5 s
conjunct is broad enough to survive that spread and narrow enough to name the origin that moved.
The limitation is documented rather than tuned away: a threshold retuned to the noise would stop
catching the failure the check exists for.

## Provenance

| field | value |
|---|---|
| compiler | AppleClang 21.0.0.21000099 |
| build type | Debug |
| options | VE_DEBUG=ON;VE_PROFILE=ON;VENG_TIME_TRACE=ON;VENG_BUILD_TESTS=ON;VENG_BUILD_EXAMPLES=ON;VENG_INSTALL_SDK=ON;VENG_EDITOR_WITH_MCP=ON;VENG_ENABLE_CLANG_TIDY=OFF;VENG_ENABLE_COVERAGE=OFF;VENG_USE_EMBED=OFF;VENG_BUILD_CONFIG=macos |
| host | Darwin arm64 / Apple M2 |
| TUs | 627 |
| build span | 211 s |
| git SHA | 23c98d09 |

## Whole-tree totals

| figure | seconds |
|---|---|
| compile CPU (ExecuteCompiler) | 930.1 |
| Frontend | 811.1 |
| Backend | 104.5 |
| Template instantiation | 363.1 |
| Parse (Source) | 299.8 |

## Template instantiation by origin

The origin table is first-match-wins by the instantiated template's qualified name, so
`std::vector<OurType>` counts as `libc++`. Some share of that bucket is therefore driven by
first-party usage rather than inherent to the standard library.

| origin | seconds | TUs paying |
|---|---|---|
| libc++ | 247.2 | 624 |
| nlohmann | 46.9 | 140 |
| first-party | 25.4 | 359 |
| other | 15.9 | 610 |
| fmt | 15.0 | 597 |
| std::format | 10.4 | 386 |
| veng reflection | 2.3 | 57 |
