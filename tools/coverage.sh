#!/usr/bin/env bash
# Prove every mutation pathway is observable: run each one in isolation and
# require that it actually changed a module at least once.
#
# Every invocation is bounded by both --count and timeout, so this script
# cannot hang even though the mutator's default mode is an endless stream.
set -uo pipefail

MUTATOR=${MUTATOR:-./build/llvm-ir-mutator}
SEED_FILE=${SEED_FILE:-tests/seeds/all.ll}
COUNT=${COUNT:-150}
SEEDS=${SEEDS:-"1 2 3"}
TIMEOUT=${TIMEOUT:-60}

pathways=$("$MUTATOR" --list | awk '{print $1}')
if [ -z "$pathways" ]; then
  echo "FAIL: --list produced no pathways" >&2
  exit 1
fi
printf '%-12s %10s %10s %10s %8s\n' pathway applied no-op invalid result
printf -- '--------------------------------------------------------\n'

failed=0
for p in $pathways; do
  applied=0 noop=0 invalid=0
  for s in $SEEDS; do
    row=$(timeout "$TIMEOUT" "$MUTATOR" "$SEED_FILE" \
            --only="$p" --count="$COUNT" --seed="$s" --report --emit=bc \
            2>&1 >/dev/null | awk -v p="$p" '$1 == p {print $3, $4, $5}')
    if [ -z "$row" ]; then
      echo "  $p: no report for seed $s (crash or timeout)" >&2
      failed=1
      continue
    fi
    set -- $row
    applied=$((applied + $1)) noop=$((noop + $2)) invalid=$((invalid + $3))
  done

  if [ "$applied" -gt 0 ]; then
    result=PASS
  else
    result=FAIL
    failed=1
  fi
  printf '%-12s %10d %10d %10d %8s\n' "$p" "$applied" "$noop" "$invalid" "$result"
done

if [ "$failed" -ne 0 ]; then
  echo
  echo "FAIL: at least one pathway never changed a module." >&2
  exit 1
fi
echo
echo "PASS: every pathway is observable."
