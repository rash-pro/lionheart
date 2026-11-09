#!/usr/bin/env bash
# Keep private inputs and generated trees outside the checkout. Each diff log
# must be empty, so a reviewer can rerun the complete catalog gates.
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
: "${LIONHEART_DISCS:?set the read-only disc directory}"
: "${LIONHEART_DEFAULT_REFERENCE:?set the read-only default generated tree}"
: "${LIONHEART_PARITY_OUTPUT:?set a new output directory outside the checkout}"
output=$(realpath -m -- "$LIONHEART_PARITY_OUTPUT")
case "$output/" in
    "$root/"*) printf '%s\n' 'Private output must be outside the checkout' >&2; exit 1 ;;
esac
if [[ -e "$output" ]]; then
    printf '%s\n' 'Use a new output directory to exclude stale files' >&2
    exit 1
fi
mkdir -p "$output"
python=${PYTHON:-python3}
cmake=${CMAKE:-cmake}
native="$root/build/generator-parity/lionheart-gen"
sanitized="$root/build/generator-parity-sanitize/lionheart-gen"
mapfile -d '' cues < <(find -L "$LIONHEART_DISCS" -type f -iname '*.cue' -print0 | sort -z)
if [[ ${#cues[@]} != 4 ]]; then
    printf '%s\n' 'Expected exactly four cue files' >&2
    exit 1
fi
discs=()
for cue in "${cues[@]}"; do discs+=(--disc "$cue"); done

run() { nice -n 10 "$@"; }
record() { printf '%s\tPASS\t%s\n' "$1" "$2" | tee -a "$output/results.tsv"; }
run "$cmake" -S generator -B build/generator-parity -DCMAKE_BUILD_TYPE=Release > "$output/build.log" 2>&1
run "$cmake" --build build/generator-parity -j4 >> "$output/build.log" 2>&1
run "$cmake" -S generator -B build/generator-parity-sanitize -DCMAKE_BUILD_TYPE=Debug \
    -DLIONHEART_GENERATOR_SANITIZE=ON > "$output/build-sanitize.log" 2>&1
run "$cmake" --build build/generator-parity-sanitize -j4 >> "$output/build-sanitize.log" 2>&1

for timing in block loose; do
    for catalog in full legacy exe; do
        name="$catalog-$timing"
        args=("${discs[@]}")
        if [[ $catalog == legacy ]]; then args+=(--legacy-overlays); fi
        if [[ $catalog == exe ]]; then args=("$output/full-block-python/extracted/SLUS_008.92"); fi
        printf 'Generating %s references and native output\n' "$name"
        run "$python" -m recomp "${args[@]}" --timing "$timing" \
            -o "$output/$name-python" > "$output/$name-python.log" 2>&1
        run "$native" "${args[@]}" --timing "$timing" \
            -o "$output/$name-native" > "$output/$name-native.log" 2>&1
        diff -r "$output/$name-python" "$output/$name-native" > "$output/$name.diff"
        record "$name" "$(find "$output/$name-native" -type f | wc -l) files; empty diff -r"
    done
    printf 'Running sanitized legacy catalog with %s timing\n' "$timing"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        run "$sanitized" "${discs[@]}" --legacy-overlays --timing "$timing" \
        -o "$output/legacy-$timing-sanitize" > "$output/legacy-$timing-sanitize.log" 2>&1
    diff -r "$output/legacy-$timing-native" "$output/legacy-$timing-sanitize" > "$output/legacy-$timing-sanitize.diff"
    record "sanitize-$timing" 'no reports; empty diff -r'
done
printf '%s\n' 'Comparing default full catalog with the read-only reference'
run "$native" "${discs[@]}" -o "$output/full-default-native" \
    > "$output/full-default-native.log" 2>&1
diff -r "$LIONHEART_DEFAULT_REFERENCE" "$output/full-default-native" > "$output/full-default.diff"
record default 'empty diff -r against read-only reference'
git rev-parse HEAD > "$output/base-commit.txt"
git diff HEAD --binary -- generator tests/test_generator_parity.py > "$output/implementation.patch"
