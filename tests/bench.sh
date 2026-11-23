#!/usr/bin/env bash
#
# bench.sh: ab / wrk 로 동시 접속 수별 처리량을 재고, 마크다운 표를 만든다.
#
#   make bench                       # 결과: build/bench/results.md (+ 원본 출력 build/bench/raw/)
#   BENCH_SECS=2 BENCH_RUNS=1 bash tests/bench.sh   # 빠르게 대충
#
# 조합마다 BENCH_RUNS 번 재서 처리량의 "중간값" 회차를 표에 쓴다 (실행마다 흔들리므로).
# 서버 로그는 회차마다 지운다 (/tmp 가 메모리 위의 tmpfs 일 수 있으므로 쌓지 않는다).

set -u
cd "$(dirname "$0")/.." || exit 1

BIN=${BENCH_BIN:-build/tiny}
SECS=${BENCH_SECS:-4}
RUNS=${BENCH_RUNS:-3}
OUT=build/bench
RAW=$OUT/raw
HAVE_WRK=0
command -v wrk > /dev/null && HAVE_WRK=1

[ -x "$BIN" ] || { echo "서버가 없음: $BIN (먼저 make)" >&2; exit 1; }
command -v ab > /dev/null || { echo "ab 가 없음 (sudo apt install apache2-utils)" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$RAW"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/tiny-bench.XXXXXX") || exit 1
cp -r www "$WORK/www"
head -c 1048576 /dev/urandom > "$WORK/www/1mb.bin"

SRV=
cleanup() { [ -n "$SRV" ] && kill -KILL "$SRV" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

PORT=$(python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')

start() {
    "$BIN" -p "$PORT" -r "$WORK/www" -c cgi-bin > "$WORK/srv.log" 2>&1 &
    SRV=$!
    local i
    for ((i = 0; i < 50; i++)); do
        curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$PORT/" && return 0
        sleep 0.1
    done
    echo "서버 시작 실패" >&2; cat "$WORK/srv.log" >&2; exit 1
}
stop() {
    kill -INT "$SRV" 2>/dev/null
    local i
    for ((i = 0; i < 100; i++)); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.1; done
    kill -KILL "$SRV" 2>/dev/null
    wait "$SRV" 2>/dev/null
    SRV=
}

# ------------------------------------------------------------------ 한 번 재기
# 결과 한 줄: "rps 평균ms p50ms p99ms 실패 non2xx 소켓에러 전체요청수"
run_ab() { # run_ab 이름 동시수 keepalive(0/1) 경로
    local name=$1 c=$2 k=$3 path=$4 f
    f=$RAW/$name.txt
    local kopt=""; [ "$k" = 1 ] && kopt="-k"
    # -t: 정해진 시간 동안, -n: 그 안에 끝나지 않을 만큼 크게, -s: 요청 하나 최대 대기
    # -e: 응답 시간 분포를 소수점까지 CSV 로 (화면 출력의 분포는 1ms 단위 정수라 빠른 요청이 모두 0 으로 보임)
    timeout $((SECS + 30)) ab -q $kopt -s 10 -t "$SECS" -n 50000000 -c "$c" -e "$f.csv" \
        "http://127.0.0.1:$PORT$path" > "$f" 2>&1
    local p50 p99
    p50=$(awk -F, '$1 == 50 {print $2}' "$f.csv" 2>/dev/null)
    p99=$(awk -F, '$1 == 99 {print $2}' "$f.csv" 2>/dev/null)
    # "Failed requests" 에는 "Length" 실패가 섞인다: 응답 길이가 첫 응답과 다르면 실패로 센다
    # (503 에러 페이지가 여기에 해당) → 진짜 실패(연결/수신/예외)만 따로 센다
    awk -v p50="${p50:-0}" -v p99="${p99:-0}" '
        /^Requests per second/          { rps = $4 }
        /^Time per request:.*\(mean\)$/ { mean = $4 }
        /\(Connect: /                   { gsub(/[(),]/, " "); real = $2 + $4 + $8 }
        /^Non-2xx responses/            { non2 = $3 }
        /^Complete requests/            { total = $3 }
        END { printf "%s %s %s %s %s %s 0 %s\n", rps+0, mean+0, p50, p99, real+0, non2+0, total+0 }' "$f"
}

# wrk 의 시간 표기(예: 1.23ms, 45.6us, 2.1s) → ms
to_ms() { awk -v v="$1" 'BEGIN{ n=v+0; if (v ~ /us$/) n/=1000; else if (v ~ /ms$/) ; else if (v ~ /s$/) n*=1000; printf "%.3f", n }'; }

run_wrk() { # run_wrk 이름 동시수 경로
    local name=$1 c=$2 path=$3 f t
    f=$RAW/$name.txt
    t=4; [ "$c" -lt 4 ] && t=$c # 스레드 수는 연결 수보다 많을 수 없다
    timeout $((SECS + 30)) wrk -t"$t" -c"$c" -d"${SECS}s" --timeout 10s --latency \
        "http://127.0.0.1:$PORT$path" > "$f" 2>&1
    local rps avg p50 p99 non2 serr
    rps=$(awk '/^Requests\/sec/ {print $2}' "$f")
    avg=$(to_ms "$(awk '$1=="Latency" {print $2; exit}' "$f")")
    p50=$(to_ms "$(awk '$1=="50%" {print $2}' "$f")")
    p99=$(to_ms "$(awk '$1=="99%" {print $2}' "$f")")
    non2=$(awk '/Non-2xx or 3xx responses/ {print $5}' "$f")
    serr=$(awk '/Socket errors/ {gsub(",",""); print $4+$6+$8+$10}' "$f")
    local total; total=$(awk '/ requests in / {print $1}' "$f")
    echo "${rps:-0} $avg $p50 $p99 0 ${non2:-0} ${serr:-0} ${total:-0}"
}

# 같은 조합을 RUNS 번 → 처리량 중간값 회차의 줄
median_of() { # median_of 명령...
    local i lines=""
    for ((i = 1; i <= RUNS; i++)); do
        lines+="$("$@")"$'\n'
    done
    printf '%s' "$lines" | sort -n -k1 | awk -v n="$RUNS" 'NR == int((n + 1) / 2)'
}

# 표 한 줄: | 동시 | 처리량 | 성공 처리량 | 평균 | p50 | p99 | 실패 | 503 등 |
# 성공 처리량 = 처리량 × (2xx 응답 비율). 503 은 일을 거의 안 하고 빨리 끝나서 처리량을 부풀리기 때문
row() { # row 동시수 결과줄
    echo "$2" | awk -v c="$1" '{
        ok = ($8 > 0) ? $1 * ($8 - $6) / $8 : $1
        printf "| %s | %s | %s | %.2f | %.2f | %.2f | %s | %s |\n", c, int($1), int(ok), $2, $3, $4, $5 + $7, $6 }'
}
HDR='| 동시 접속 | 처리량 (요청/초) | 성공 처리량 (2xx/초) | 평균 응답 (ms) | 50% (ms) | 99% (ms) | 진짜 실패 (연결/수신/소켓) | 2xx 아닌 응답 (503 등) |
|---:|---:|---:|---:|---:|---:|---:|---:|'

R=$OUT/results.md
{
    echo "# 측정 결과 (tests/bench.sh 가 자동 생성)"
    echo
    echo "- 날짜: $(date '+%Y-%m-%d %H:%M')"
    echo "- CPU: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //') ($(nproc) 스레드), 메모리 $(free -h | awk '/^Mem/{print $2}')"
    echo "- OS: $(uname -sr)"
    echo "- 서버: $BIN (gcc -O2), 일꾼/대기열은 기본값 (아래 서버 시작 로그 참고), 접근 로그는 파일로 기록"
    echo "- 도구: $(ab -V | head -1 | sed 's/This is //')$( [ $HAVE_WRK = 1 ] && echo ", wrk $(wrk --version 2>&1 | head -1 | awk '{print $2}')")"
    echo "- 조합마다 ${SECS}초씩 ${RUNS}번 측정, 처리량 중간값 회차를 기록"
} > "$R"

start
SRVINFO=$(grep -m1 '일꾼' "$WORK/srv.log" | sed 's/^ *//')
echo "- 서버 설정: $SRVINFO" >> "$R"
stop

measure_table() { # measure_table 제목 동시수목록 명령접두... (마지막 인자로 동시수가 들어감)
    local title=$1 cs=$2; shift 2
    echo; echo "## $title"; echo; echo "$HDR"
    local c line
    for c in $cs; do
        printf '  %-40s 동시 %-4s ' "$title" "$c" >&2
        start
        line=$(median_of "$@" "$c")
        stop
        row "$c" "$line"
        echo "→ $(echo "$line" | awk '{print int($1)}') 요청/초" >&2
    done
}

# 이름이 겹치지 않게 회차마다 raw 파일 이름을 바꾸는 작은 감싸기
seq_id=0
ab_static()   { seq_id=$((seq_id+1)); run_ab  "ab-static-$1-c$3-$seq_id" "$3" "$2" /index.html; }
ab_big()      { seq_id=$((seq_id+1)); run_ab  "ab-1mb-c$1-$seq_id"       "$1" 1 /1mb.bin; }
ab_cgi()      { seq_id=$((seq_id+1)); run_ab  "ab-cgi-c$1-$seq_id"       "$1" 1 /cgi-bin/hello.sh; }
wrk_static()  { seq_id=$((seq_id+1)); run_wrk "wrk-static-c$1-$seq_id"   "$1" /index.html; }
wrk_cgi()     { seq_id=$((seq_id+1)); run_wrk "wrk-cgi-c$1-$seq_id"      "$1" /cgi-bin/hello.sh; }
ab_static_nk() { ab_static nk 0 "$1"; }
ab_static_k()  { ab_static k 1 "$1"; }

{
    measure_table "정적 1.4KB (index.html) · ab · keep-alive 없음" "1 10 50 100 200 500" ab_static_nk
    measure_table "정적 1.4KB (index.html) · ab -k · keep-alive" "1 10 50 100 200 500" ab_static_k
    if [ $HAVE_WRK = 1 ]; then
        measure_table "정적 1.4KB (index.html) · wrk · keep-alive" "1 10 50 100 200 500" wrk_static
    fi
    measure_table "정적 1MB 파일 · ab -k" "1 10 50 100" ab_big
    measure_table "CGI (hello.sh, 요청마다 프로세스 생성) · ab -k" "1 10 32 64 128" ab_cgi
    if [ $HAVE_WRK = 1 ]; then
        measure_table "CGI (hello.sh) · wrk" "1 10 32 64 128" wrk_cgi
    fi
} >> "$R"

[ $HAVE_WRK = 1 ] || printf '\n> wrk 가 설치되어 있지 않아 wrk 측정은 건너뜀 (sudo apt install wrk)\n' >> "$R"
echo "완료: $R (원본 출력: $RAW/)" >&2
