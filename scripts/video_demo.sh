#!/usr/bin/env bash
# Reproducible, loopback-only terminal demo for task-scheduler.
# Save this file as <repository>/scripts/video_demo.sh and run with bash.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD="${BUILD_DIR:-$HOME/build/task-scheduler-video}"
AUTO=0
WORKERS=2

usage() {
  cat <<'HELP'
Usage: bash scripts/video_demo.sh [--build-dir DIR] [--auto]

  --build-dir DIR   Release build directory containing controller/worker/taskctl
                    (default: $HOME/build/task-scheduler-video)
  --auto            Do not pause between scenes (useful for a rehearsal)
  -h, --help        Print help

The script only starts services on 127.0.0.1 through scripts/local_cluster.py.
It retains its unique /tmp task/log directory and safely stops owned services.
HELP
}
while (($#)); do
  case "$1" in
    --build-dir)
      (($# >= 2)) || { echo 'Missing --build-dir value' >&2; exit 2; }
      BUILD="$2"; shift 2 ;;
    --auto) AUTO=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -f "$SRC/scripts/local_cluster.py" ]] || {
  echo 'Place this script at <repository>/scripts/video_demo.sh.' >&2
  exit 1
}
for exe in controller worker taskctl; do
  [[ -x "$BUILD/$exe" ]] || {
    printf 'Missing executable: %s/%s\n' "$BUILD" "$exe" >&2
    printf 'Build first:\n  cmake -S "%s" -B "%s" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF\n  cmake --build "%s" -j4\n' \
      "$SRC" "$BUILD" "$BUILD" >&2
    exit 1
  }
done

ROOT=""
STARTED=0
cleanup() {
  local code=$?
  trap - EXIT
  if (( STARTED )); then
    printf '\n[Cleanup] Stopping only the processes owned by this demo...\n'
    if ! python3 "$SRC/scripts/local_cluster.py" stop --root "$ROOT"; then
      echo '[Cleanup] Automatic stop failed. Inspect the retained demo directory; do not kill processes by name.' >&2
      code=1
    fi
  fi
  if [[ -n "$ROOT" ]]; then
    printf '[Artifacts] Logs, SQLite data and diagnostics retained in: %s\n' "$ROOT"
  fi
  exit "$code"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

section() {
  printf '\n============================================================\n%s\n============================================================\n' "$1"
}
pause() {
  if (( AUTO )); then
    sleep 1
  elif [[ -t 0 ]]; then
    read -r -p '[Press Enter to continue] ' _ || true
  fi
}
show_cmd() {
  printf '$ taskctl --host 127.0.0.1 --port %s ' "$PORT"
  printf '%q ' "$@"
  printf '\n'
}
cli() { "$BUILD/taskctl" --host 127.0.0.1 --port "$PORT" "$@"; }
run_cli() { show_cmd "$@"; cli "$@"; }
submit_task() {
  # Command information goes to stderr so stdout contains only the Task UUID.
  show_cmd "$@" >&2
  cli "$@"
}
get_state() {
  cli --json get "$1" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["task"]["state"])'
}
wait_state() {
  local id="$1" wanted="$2" timeout="${3:-15}"
  local started=$SECONDS state
  while (( SECONDS - started < timeout )); do
    state="$(get_state "$id")"
    if [[ "$state" == "$wanted" ]]; then
      printf '[Observed] %s -> %s\n' "$id" "$state"
      return 0
    fi
    case "$state" in
      SUCCEEDED|FAILED|CANCELLED)
        printf 'Unexpected terminal state for %s: got %s, expected %s\n' "$id" "$state" "$wanted" >&2
        return 1 ;;
    esac
    sleep 0.2
  done
  printf 'Timed out waiting for %s -> %s (last: %s)\n' "$id" "$wanted" "$state" >&2
  return 1
}

section 'Scene 1 / 5 — Start Controller + two Workers'
ROOT="$(mktemp -d /tmp/task-scheduler-video.XXXXXX)"
printf 'Demo artifact directory: %s\n' "$ROOT"
python3 "$SRC/scripts/local_cluster.py" start \
  --bin-dir "$BUILD" --root "$ROOT" --workers "$WORKERS"
STARTED=1
PORT="$(python3 - "$ROOT/cluster.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as f:
    print(json.load(f)['client_port'])
PY
)"
run_cli workers
pause

section 'Scene 2 / 5 — Submit, durable completion, and stdout'
SUCCESS_ID="$(submit_task submit --cpu 1 --memory 16 /bin/echo 'Hello Task Scheduler')"
printf 'Task ID: %s\n' "$SUCCESS_ID"
wait_state "$SUCCESS_ID" SUCCEEDED
run_cli get "$SUCCESS_ID"
run_cli logs "$SUCCESS_ID" --attempt 1 --stdout
printf '\n'
pause

section 'Scene 3 / 5 — Cancel an ACTIVE task'
CANCEL_ID="$(submit_task submit /bin/sleep 30)"
printf 'Task ID: %s\n' "$CANCEL_ID"
wait_state "$CANCEL_ID" ACTIVE
run_cli get "$CANCEL_ID"
run_cli cancel "$CANCEL_ID"
wait_state "$CANCEL_ID" CANCELLED
run_cli get "$CANCEL_ID"
pause

section 'Scene 4 / 5 — Timeout a running task'
TIMEOUT_ID="$(submit_task submit --timeout 1 /bin/sleep 30)"
printf 'Task ID: %s\n' "$TIMEOUT_ID"
wait_state "$TIMEOUT_ID" FAILED
run_cli get "$TIMEOUT_ID"
printf '[Expected] Task=FAILED, Attempt=TIMED_OUT.\n'
pause

section 'Scene 5 / 5 — Manual retry preserves Attempt history'
RETRY_PATH="$ROOT/retry-precondition"
RETRY_ID="$(submit_task submit /usr/bin/ls "$RETRY_PATH")"
printf 'Task ID: %s\n' "$RETRY_ID"
wait_state "$RETRY_ID" FAILED
run_cli get "$RETRY_ID"
printf '\n[Attempt 1 stderr]\n'
run_cli logs "$RETRY_ID" --attempt 1 --stderr
pause

printf '\n$ mkdir %q\n' "$RETRY_PATH"
mkdir -- "$RETRY_PATH"
run_cli retry "$RETRY_ID"
wait_state "$RETRY_ID" SUCCEEDED
run_cli get "$RETRY_ID"
printf '\n[Attempt 1 stderr remains available]\n'
run_cli logs "$RETRY_ID" --attempt 1 --stderr
printf '\n[Attempt 2 stdout]\n'
run_cli logs "$RETRY_ID" --attempt 2 --stdout
printf '\n'

section 'Final — Verify both Workers idle, then stop cleanly'
run_cli workers
printf '\nAll scenes completed. The EXIT handler will stop the demo-owned processes.\n'
pause
