#!/bin/sh
# Upload qa/dual-platform/baseline/* (6 files) via Alpine/Linux native CLI.
# Usage: from repo root — sh qa/dual-platform/restore-baseline.sh <tag>
# Log: qa/dual-platform/restore-<tag>.log
# Credentials: config/ftp.ini only (never on argv).

set -eu
TAG="${1:-manual}"
ROOT="$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2

EXE="$ROOT/build-alpine/bin/StelliferumAuditor"
if [ ! -x "$EXE" ]; then
  EXE="$ROOT/build/bin/StelliferumAuditor"
fi
if [ ! -x "$EXE" ]; then
  echo "ERROR: no StelliferumAuditor binary" >&2
  exit 2
fi

REMOTE_ROOT="/104.192.226.196_2322"
LOG="$ROOT/qa/dual-platform/restore-${TAG}.log"
FAIL=0

: >"$LOG"
{
  echo "=== restore-baseline tag=${TAG} when=$(date -Iseconds 2>/dev/null || date) ==="
  echo "exe=$EXE"
  echo "cwd=$ROOT"
  echo "note=credentials from config/ftp.ini only; no secrets logged"
} >>"$LOG"

upload_one() {
  local_rel="$1"
  remote="$2"
  if [ ! -f "$local_rel" ]; then
    echo "ec=2 size=0 local=$local_rel remote=$remote ERROR=missing_local" >>"$LOG"
    FAIL=$((FAIL + 1))
    return
  fi
  size=$(wc -c <"$local_rel" | tr -d ' ')
  if [ "$size" -le 0 ]; then
    echo "ec=2 size=0 local=$local_rel remote=$remote ERROR=zero_byte_local" >>"$LOG"
    FAIL=$((FAIL + 1))
    return
  fi
  set +e
  "$EXE" --ftp-upload --local "$local_rel" --remote "$remote"
  ec=$?
  set -e
  echo "ec=$ec size=$size local=$local_rel remote=$remote" >>"$LOG"
  if [ "$ec" -ne 0 ]; then
    FAIL=$((FAIL + 1))
  fi
}

upload_one "qa/dual-platform/baseline/types.xml" \
  "$REMOTE_ROOT/mpmissions/dayzOffline.chernarusplus/db/types.xml"
upload_one "qa/dual-platform/baseline/cfgspawnabletypes.xml" \
  "$REMOTE_ROOT/mpmissions/dayzOffline.chernarusplus/cfgspawnabletypes.xml"
upload_one "qa/dual-platform/baseline/cfgeconomycore.xml" \
  "$REMOTE_ROOT/mpmissions/dayzOffline.chernarusplus/cfgeconomycore.xml"
upload_one "qa/dual-platform/baseline/cfglimitsdefinitionuser.xml" \
  "$REMOTE_ROOT/mpmissions/dayzOffline.chernarusplus/cfglimitsdefinitionuser.xml"
upload_one "qa/dual-platform/baseline/cfgrandompresets.xml" \
  "$REMOTE_ROOT/mpmissions/dayzOffline.chernarusplus/cfgrandompresets.xml"
upload_one "qa/dual-platform/baseline/TraderConfig.txt" \
  "$REMOTE_ROOT/profiles/Trader/TraderConfig.txt"

# Optional types size check
VERIFY="qa/dual-platform/_restore-verify-types.xml"
REMOTE_TYPES="$REMOTE_ROOT/mpmissions/dayzOffline.chernarusplus/db/types.xml"
BASE_SZ=$(wc -c <"qa/dual-platform/baseline/types.xml" | tr -d ' ')
rm -f "$VERIFY"
set +e
"$EXE" --ftp-download --remote "$REMOTE_TYPES" --local "$VERIFY"
dl_ec=$?
set -e
GOT_SZ=0
if [ -f "$VERIFY" ]; then
  GOT_SZ=$(wc -c <"$VERIFY" | tr -d ' ')
  rm -f "$VERIFY"
fi
MATCH=0
if [ "$BASE_SZ" -gt 0 ] && [ "$BASE_SZ" -eq "$GOT_SZ" ]; then
  MATCH=1
fi
echo "verify_types ec=$dl_ec baseline_size=$BASE_SZ remote_size=$GOT_SZ size_match=$MATCH" >>"$LOG"
if [ "$dl_ec" -ne 0 ] || [ "$MATCH" -ne 1 ]; then
  FAIL=$((FAIL + 1))
fi

if [ "$FAIL" -eq 0 ]; then
  echo "ALL_OK=True fail_count=0" >>"$LOG"
  exit 0
fi
echo "ALL_OK=False fail_count=$FAIL" >>"$LOG"
exit 1
