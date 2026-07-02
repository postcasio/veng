# Writes the #embed shim translation unit for veng_embed_binary, run at build time
# via `cmake -P` so it sees the embedded file's current bytes.
#
# The shim carries a SHA-256 of the embedded file in a comment. #embed splices the
# file's bytes only at compile time, and ccache does not capture an #embed-ed file
# as a dependency (depend mode hashes the depfile, which never lists it; an #embed
# change is otherwise a false cache hit returning a stale object). Baking the hash
# into the source makes the TU's bytes change whenever the embedded file changes, so
# the recompile is a real cache miss.
#
# Inputs (passed with -D): VENG_EMBED_INPUT, VENG_EMBED_SYMBOL, VENG_EMBED_OUTPUT.

file(SHA256 "${VENG_EMBED_INPUT}" embedHash)

# Written to a sibling temporary and renamed into place: the rename is atomic, so
# a killed or concurrent run never leaves a torn shim with a fresh mtime that the
# build would treat as up to date.
string(RANDOM LENGTH 8 tempSuffix)
set(tempOutput "${VENG_EMBED_OUTPUT}.${tempSuffix}.tmp")

file(WRITE "${tempOutput}" "\
// Auto-generated #embed shim — do not edit.
// embed-content-hash: ${embedHash}
namespace Veng {
extern const unsigned char ${VENG_EMBED_SYMBOL}[] = {
#embed \"${VENG_EMBED_INPUT}\"
};
extern const unsigned long ${VENG_EMBED_SYMBOL}Size = sizeof(${VENG_EMBED_SYMBOL});
}
")

file(RENAME "${tempOutput}" "${VENG_EMBED_OUTPUT}")
