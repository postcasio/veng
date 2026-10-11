#!/usr/bin/env python3
"""Generates the case-mapping tables in engine/src/Text/CaseMapping.cpp from the Unicode
Character Database.

    python3 -I scripts/generate_case_tables.py <UnicodeData.txt> <SpecialCasing.txt>

prints the C++ between the "BEGIN GENERATED" and "END GENERATED" markers of CaseMapping.cpp;
paste it over what is there and clang-format the file. The UCD files are not vendored: fetch them
for the Unicode version named in that file's comment from
https://www.unicode.org/Public/<version>/ucd/.

The coverage is every simple mapping (UnicodeData.txt fields 12 and 13) whose source or target
lies below U+0500 -- Basic Latin through Cyrillic -- plus, for the upper-case direction only, the
unconditional multi-codepoint expansions of SpecialCasing.txt for sources in that range whose
simple upper case is none (U+00DF and its kin), so an upper-cased run leaves no lower-case letter
behind.
"""

import sys

COVERED_END = 0x500


def read_unicode_data(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            fields = line.rstrip("\n").split(";")
            if len(fields) < 15:
                continue
            rows.append(
                (
                    int(fields[0], 16),
                    fields[2],
                    int(fields[12], 16) if fields[12] else None,
                    int(fields[13], 16) if fields[13] else None,
                )
            )
    return rows


def read_special_upper(path, simple_upper):
    """Unconditional full upper-case expansions of covered codepoints lacking a simple one."""
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            body = line.split("#", 1)[0].strip()
            if not body:
                continue
            fields = [x.strip() for x in body.split(";")]
            # code; lower; title; upper; (condition_list;)?
            if len(fields) >= 5 and fields[4]:
                continue
            code = int(fields[0], 16)
            upper = [int(x, 16) for x in fields[3].split()]
            if code >= COVERED_END or len(upper) < 2 or code in simple_upper:
                continue
            out[code] = upper
    return out


def runs(pairs):
    """Compresses sorted (source, target) pairs into (first, last, stride, delta) runs."""
    result = []
    i = 0
    while i < len(pairs):
        first, target = pairs[i]
        delta = target - first
        last = first
        stride = 1
        j = i + 1
        if j < len(pairs) and pairs[j][1] - pairs[j][0] == delta and pairs[j][0] - first in (1, 2):
            stride = pairs[j][0] - first
            while (
                j < len(pairs)
                and pairs[j][0] == last + stride
                and pairs[j][1] - pairs[j][0] == delta
            ):
                last = pairs[j][0]
                j += 1
        else:
            j = i + 1
        result.append((first, last, stride, delta))
        i = j
    return result


def ranges(codes):
    result = []
    for c in sorted(codes):
        if result and result[-1][1] == c - 1:
            result[-1][1] = c
        else:
            result.append([c, c])
    return result


def emit_runs(name, table):
    print(f"        constexpr std::array<CaseRun, {len(table)}> {name}{{{{")
    for first, last, stride, delta in table:
        print(f"            {{0x{first:04X}, 0x{last:04X}, {stride}, {delta}}},")
    print("        }};")


def emit_ranges(name, table):
    print(f"        constexpr std::array<CodepointRange, {len(table)}> {name}{{{{")
    for first, last in table:
        print(f"            {{0x{first:04X}, 0x{last:04X}}},")
    print("        }};")


def main():
    rows = read_unicode_data(sys.argv[1])
    covered = lambda a, b: a < COVERED_END or b < COVERED_END
    upper = sorted((c, u) for c, _, u, _ in rows if u is not None and covered(c, u))
    lower = sorted((c, l) for c, _, _, l in rows if l is not None and covered(c, l))
    special = read_special_upper(sys.argv[2], {c for c, _ in upper})

    # Cased: a letter of category Lu/Ll/Lt in the covered range, or anything with a mapping.
    cased = {c for c, cat, _, _ in rows if c < COVERED_END and cat in ("Lu", "Ll", "Lt")}
    cased |= {c for c, _ in upper} | {c for c, _ in lower}
    # Case-ignorable within the covered range: marks, format controls, modifier letters and
    # symbols, and the word-internal punctuation (Word_Break MidLetter/MidNumLet/Single_Quote).
    ignorable = {
        c for c, cat, _, _ in rows if c < COVERED_END and cat in ("Mn", "Me", "Cf", "Lm", "Sk")
    }
    ignorable |= {0x0027, 0x002E, 0x003A, 0x00B7, 0x0387, 0x2018, 0x2019, 0x2024, 0x2027}
    ignorable |= set(range(0x200B, 0x2010))

    emit_runs("UpperRuns", runs(upper))
    print()
    emit_runs("LowerRuns", runs(lower))
    print()
    print(f"        constexpr std::array<UpperExpansion, {len(special)}> UpperExpansions{{{{")
    for code in sorted(special):
        seq = ", ".join(f"0x{x:04X}" for x in special[code])
        print(f"            {{0x{code:04X}, {{{seq}}}, {len(special[code])}}},")
    print("        }};")
    print()
    emit_ranges("CasedRanges", ranges(cased))
    print()
    emit_ranges("CaseIgnorableRanges", ranges(ignorable))


if __name__ == "__main__":
    main()
