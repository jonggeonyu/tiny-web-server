#!/usr/bin/env bash
# tests/e2e/e2e_*.sh 를 전부 돌린다. 하나라도 실패하면 종료 코드 1.
status=0
for script in "$(dirname "$0")"/e2e_*.sh; do
    echo "== $(basename "$script")"
    bash "$script" || status=1
done
exit $status
