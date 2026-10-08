#!/usr/bin/env bash
# Check that the mutants the tool actually emits are consumable: each record
# must parse, verify, and survive an -O2 pipeline.
#
# Records are NUL-separated, so the stream is split with `csplit`-free awk on
# the separator rather than by guessing at IR boundaries.
set -uo pipefail

MUTATOR=${MUTATOR:-./build/llvm-ir-mutator}
SEED_FILE=${SEED_FILE:-tests/seeds/all.ll}
COUNT=${COUNT:-300}
SEED=${SEED:-1}
TIMEOUT=${TIMEOUT:-300}
OPT=${OPT:-opt}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

timeout "$TIMEOUT" "$MUTATOR" "$SEED_FILE" --count="$COUNT" --seed="$SEED" \
  2>/dev/null > "$work/stream" || { echo "FAIL: mutator did not finish" >&2; exit 1; }

# Split the NUL-separated stream into one file per mutant.
( cd "$work" && csplit -z -s -f m -b '%04d.ll' \
    <(tr '\0' '\n' < stream) '/^; mutant=/' '{*}' ) || true

n=0 bad=0
for f in "$work"/m*.ll; do
  [ -e "$f" ] || continue
  n=$((n + 1))
  if ! "$OPT" -passes=verify -disable-output "$f" 2>"$work/err"; then
    bad=$((bad + 1)); echo "verify failed: $f"; head -3 "$work/err"; continue
  fi
  if ! "$OPT" -passes='default<O2>' -disable-output "$f" 2>"$work/err"; then
    bad=$((bad + 1)); echo "O2 failed: $f"; head -3 "$work/err"
  fi
done

echo "checked $n mutants, $bad bad"
[ "$n" -gt 0 ] || { echo "FAIL: no mutants produced" >&2; exit 1; }
[ "$bad" -eq 0 ] || exit 1
echo "PASS: every mutant parses, verifies and survives -O2."

# --alive2-safe must emit none of the constructs Alive2 cannot reason about.
# Alive2 itself is not required to check this, which keeps the test cheap.
timeout "$TIMEOUT" "$MUTATOR" "$SEED_FILE" --count="$COUNT" --seed="$SEED" \
  --alive2-safe 2>/dev/null | tr '\0' '\n' > "$work/safe"

safe_bad=0
for pat in 'volatile' 'noalias' '\bafn\b' '\barcp\b' '\bcontract\b' '\breassoc\b' '\bfast\b' 'vscale' 'range\(' 'itofp (nnan|ninf|nsz)'; do
  hits=$(grep -cP "$pat" "$work/safe")
  if [ "$hits" -ne 0 ]; then
    echo "FAIL: --alive2-safe emitted $hits occurrences of /$pat/" >&2
    safe_bad=1
  fi
done

# No function may call itself: Alive2 reports a recursive callee as "function
# did not return". Only direct self-calls are checked here; the scrubber breaks
# longer cycles too, but spotting those needs a real call graph.
rec=$(awk '
  /^define/ { cur = ""; if (match($0, /@[-A-Za-z0-9_.$]+\(/)) cur = substr($0, RSTART+1, RLENGTH-2); next }
  /call/    { if (cur != "" && index($0, "@" cur "(")) n++ }
  END { print n+0 }' "$work/safe")
if [ "$rec" -ne 0 ]; then
  echo "FAIL: --alive2-safe emitted $rec recursive call(s)" >&2
  safe_bad=1
fi

# The flags Alive2 models exactly must still get through, or the scrubber is
# simply deleting everything.
kept=0
for pat in '\bnnan\b' '\bninf\b' '\bnsz\b' '\bnsw\b' '\bnuw\b'; do
  [ "$(grep -cP "$pat" "$work/safe")" -gt 0 ] && kept=$((kept + 1))
done
if [ "$kept" -lt 3 ]; then
  echo "FAIL: --alive2-safe scrubbed the soundly-modelled flags too ($kept/5 kept)" >&2
  safe_bad=1
fi

[ "$safe_bad" -eq 0 ] || exit 1
echo "PASS: --alive2-safe emits nothing Alive2 rejects, and keeps what it models."
