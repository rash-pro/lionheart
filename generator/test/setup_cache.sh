#!/usr/bin/env bash
# Exercise the real developer catalog, keeping every private output in build/.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
: "${LIONHEART_DISCS:?set the read-only owned disc folder}"
output="$root/build/setup-cache-check"
if [[ -e "$output" ]]; then
    printf '%s\n' 'Use a fresh build/setup-cache-check directory' >&2
    exit 1
fi
mkdir -p "$output/catalog"
cp games/ff8-usa/*.json "$output/catalog/"
mapfile -d '' cues < <(find -L "$LIONHEART_DISCS/disc1" -iname '*.cue' -print0)
[[ ${#cues[@]} == 1 ]]
tool="$root/build/generator/lionheart-gen"
args=(--disc "${cues[0]}" --legacy-overlays --cache --catalog "$output/catalog" -o "$output/generated")
run() {
    local name=$1 expectation=$2; shift 2
    nice -n 10 "$tool" "${args[@]}" "$@" > "$output/$name.log" 2>&1
    grep -q "Catalog cache $expectation:" "$output/$name.log"
    printf '%s\tPASS\n' "$name" | tee -a "$output/results.tsv"
}
run cold miss
cp "$output/generated/setup-cache.native" "$output/initial-receipt"
run hit hit
printf '\n' >> "$output/catalog/overlays.json"
run catalog miss
run catalog-hit hit
run option miss --host-mode fast
run option-hit hit --host-mode fast
run option-reset miss
printf 'corrupt\n' >> "$output/generated/dispatch_table.c"
run corrupt miss
cp games/ff8-usa/overlays.json "$output/catalog/overlays.json"
run catalog-reset miss
cmp "$output/generated/setup-cache.native" "$output/initial-receipt"
# A distinct source fingerprint must invalidate the same output directory.
mkdir -p "$output/source"
cp -R generator/. "$output/source/"
printf '\n' >> "$output/source/src/decode.c"
nice -n 10 cmake -S "$output/source" -B "$output/fingerprint-build" > "$output/fingerprint-build.log" 2>&1
nice -n 10 cmake --build "$output/fingerprint-build" --target lionheart-gen -j "${CMAKE_BUILD_PARALLEL_LEVEL:-6}" >> "$output/fingerprint-build.log" 2>&1
original_tool=$tool
tool="$output/fingerprint-build/lionheart-gen"
[[ $("$tool" --fingerprint) != $("$original_tool" --fingerprint) ]]
run fingerprint miss
run fingerprint-hit hit
tool=$original_tool
run fingerprint-reset miss
# Wait for the library to retire its receipt, then kill discovery mid-call.
printf 'corrupt\n' >> "$output/generated/dispatch_table.c"
mkfifo "$output/progress"
nice -n 10 "$tool" "${args[@]}" > "$output/progress" 2> "$output/killed.err" &
pid=$!
IFS= read -r line < "$output/progress"
[[ $line == 'Catalog cache miss: generating native functions...' ]]
kill -KILL "$pid"
wait "$pid" 2>/dev/null && exit 1
[[ ! -e "$output/generated/setup-cache.native" ]]
run killed-retry miss
run killed-hit hit
# A legacy JSON receipt alone can never count as a native hit.
rm "$output/generated/setup-cache.native"
printf '{}\n' > "$output/generated/setup-cache.json"
run old-cache miss
printf '%s\n' 'All real-disc cache checks passed.'
