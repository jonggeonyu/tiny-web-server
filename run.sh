#!/usr/bin/env bash
#
# run.sh - 백엔드 5개 + proxy를 한 번에 띄우고, Ctrl+C로 모두 끕니다.
#
#   사용법: ./run.sh        (또는 make run)
#           ./run.sh -v     proxy가 모든 요청을 로그에 남김
#           ./run.sh -C     캐시를 켠 채 시작 (시험용, 둘을 함께 써도 됨: ./run.sh -v -C)
#           ASAN=1 ./run.sh 메모리 점검용 빌드(make asan)로 실행
#
# 각 프로그램의 출력 앞에 [products-1] 같은 이름을 붙여 한 터미널에 모아 보여 줍니다.
#
set -u
cd "$(dirname "$0")" || exit 1

# 이름  포트  그룹   (SPEC 2장)
BACKENDS=(
    "products-1 8001 products"
    "products-2 8002 products"
    "products-3 8003 products"
    "orders-1   8011 orders"
    "orders-2   8012 orders"
)
PROXY_PORT=8080
PROXY_ARGS=(-c proxy.conf)

# ASAN=1이면 make asan으로 만든 메모리 점검용 실행 파일을 씀
BACKEND_BIN=./backend
PROXY_BIN=./proxy
if [ "${ASAN:-0}" = "1" ]; then
    BACKEND_BIN=./backend-asan
    PROXY_BIN=./proxy-asan
fi
for arg in "$@"; do
    case "$arg" in
        -v|-C) PROXY_ARGS+=("$arg") ;; # proxy에 그대로 넘김
        *) echo "[run] 알 수 없는 옵션: $arg (쓸 수 있는 것: -v, -C)"; exit 1 ;;
    esac
done

PIDS=()   # 띄운 프로그램의 PID
NAMES=()  # 같은 순서의 이름

# 포트를 누가 이미 쓰고 있는지 확인 (LISTEN 상태인 소켓이 있으면 사용 중)
port_in_use() {
    ss -Hltn "sport = :$1" | grep -q .
}

# 프로그램 하나를 백그라운드로 띄움. 출력은 sed를 거쳐 줄 앞에 [이름]을 붙임.
#   - 이미 "[이름]"으로 시작하는 줄(backend·proxy가 직접 붙인 것)에는 다시 붙이지 않음, 빈 줄은 지움
#   - sed는 setsid로 따로 띄워 Ctrl+C(SIGINT)를 받지 않게 함: 프로그램이 종료하면서 남기는
#     마지막 메시지까지 보여 주기 위해. sed는 프로그램이 끝나 출력이 닫히면 스스로 끝남
#   - 참고: 스크립트에서 &로 띄운 프로그램은 SIGINT를 무시한 채 시작됨(bash 규칙).
#     그래서 Ctrl+C 뒤에는 cleanup()이 SIGTERM을 보내 끔
start() {
    local name=$1
    shift
    "$@" > >(exec setsid sed -u -e '/^$/d' -e "/^\[$name\]/! s/^/[$name] /") 2>&1 &
    PIDS+=($!)
    NAMES+=("$name")
}

# 모두 종료하고, 남은 프로세스가 없는지 확인한 뒤 끝냄
cleanup() {
    trap '' INT TERM # 정리 중에 Ctrl+C를 또 눌러도 정리가 끊기지 않게
    echo "[run] 종료합니다..."
    for pid in "${PIDS[@]}"; do
        kill -TERM "$pid" 2>/dev/null
    done

    # 최대 7초 기다림 (proxy는 처리 중인 연결을 최대 5초 기다렸다 종료)
    for _ in $(seq 70); do
        local alive=0
        for pid in "${PIDS[@]}"; do
            kill -0 "$pid" 2>/dev/null && alive=1
        done
        [ "$alive" -eq 0 ] && break
        sleep 0.1
    done

    # 그래도 남아 있으면 강제 종료
    for i in "${!PIDS[@]}"; do
        if kill -0 "${PIDS[$i]}" 2>/dev/null; then
            echo "[run] ${NAMES[$i]}가 끝나지 않아 강제 종료합니다"
            kill -KILL "${PIDS[$i]}" 2>/dev/null
        fi
    done
    wait "${PIDS[@]}" 2>/dev/null
    sleep 0.2 # sed가 마지막 줄을 출력할 시간

    # 확인: 프로세스와 포트가 모두 정리됐는지
    local left=()
    for i in "${!PIDS[@]}"; do
        kill -0 "${PIDS[$i]}" 2>/dev/null && left+=("${NAMES[$i]}")
    done
    for p in 8001 8002 8003 8011 8012 $PROXY_PORT; do
        port_in_use "$p" && left+=("포트 $p")
    done
    if [ ${#left[@]} -gt 0 ]; then
        echo "[run] 아직 남아 있음: ${left[*]}"
        exit 1
    fi
    echo "[run] ${#PIDS[@]}개 모두 종료됨 (남은 프로세스·포트 없음)"
    exit "${1:-0}"
}

# 1) 빌드 결과물 확인
for f in "$BACKEND_BIN" "$PROXY_BIN"; do
    if [ ! -x "$f" ]; then
        echo "[run] $f 가 없습니다. 먼저 make(ASAN=1이면 make asan)를 실행하세요."
        exit 1
    fi
done

# 2) 포트가 비어 있는지 확인. 하나라도 사용 중이면 시작하지 않음
busy=()
for b in "${BACKENDS[@]}"; do
    read -r _ port _ <<< "$b"
    port_in_use "$port" && busy+=("$port")
done
port_in_use "$PROXY_PORT" && busy+=("$PROXY_PORT")
if [ ${#busy[@]} -gt 0 ]; then
    echo "[run] 이미 사용 중인 포트가 있어 시작하지 않습니다: ${busy[*]}"
    echo "[run] 사용 중인 프로그램 확인: ss -ltnp | grep -E ':($(IFS='|'; echo "${busy[*]}"))\\b'"
    exit 1
fi

# 3) 시작
trap cleanup INT TERM
for b in "${BACKENDS[@]}"; do
    read -r name port group <<< "$b"
    start "$name" "$BACKEND_BIN" -p "$port" -n "$name" -g "$group"
done
sleep 0.3 # 백엔드가 먼저 준비되도록
start proxy "$PROXY_BIN" "${PROXY_ARGS[@]}"
sleep 0.5

# 4) 모두 살아 있는지 확인
for i in "${!PIDS[@]}"; do
    if ! kill -0 "${PIDS[$i]}" 2>/dev/null; then
        echo "[run] ${NAMES[$i]}가 시작하지 못했습니다. 모두 종료합니다."
        cleanup 1
    fi
done
echo "[run] ${#PIDS[@]}개 실행 중. 브라우저: http://localhost:$PROXY_PORT  (Ctrl+C로 모두 종료)"

# 5) Ctrl+C를 기다림. 그 사이 하나가 혼자 죽으면 알려 줌
reported=()
while true; do
    sleep 1
    for i in "${!PIDS[@]}"; do
        if ! kill -0 "${PIDS[$i]}" 2>/dev/null && [ -z "${reported[$i]:-}" ]; then
            echo "[run] 경고: ${NAMES[$i]}가 종료됐습니다"
            reported[$i]=1
        fi
    done
done
