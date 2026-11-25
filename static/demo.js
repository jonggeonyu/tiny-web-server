/*
 * demo.js - Reverse Proxy 데모 화면 (docs/SPEC.md 8장)
 *
 * 구조: [이벤트 공급기] → handleEvent() → 화면 상태(V) → 캔버스·패널 그리기
 *
 *   - 실제 모드(기본): EventSource('/api/events')가 공급기. 버튼은 POST /api/control/... 호출
 *   - 모의 모드(?mock=1): 화면 안의 "가짜 이벤트 공급기"가 SPEC 7장과 같은 형식의
 *     req·status·sys 이벤트를 만들어 냄. 버튼은 이 공급기를 직접 바꿈.
 *   두 모드 모두 같은 handleEvent()·그리기 코드를 씁니다.
 *
 * 화면 모양·색·크기·애니메이션은 docs/prototype.html을 그대로 따릅니다.
 */
'use strict';

// ===================================================================
// 1. 화면 배치: 1920x1080 무대를 창 크기에 맞춰 비율 유지 (prototype과 같음)
// ===================================================================
const stage = document.getElementById('stage');
function fit() {
  const k = Math.min(innerWidth / 1920, innerHeight / 1080);
  stage.style.transform = 'scale(' + k + ')';
  stage.style.left = Math.round((innerWidth - 1920 * k) / 2) + 'px';
  stage.style.top = Math.round((innerHeight - 1080 * k) / 2) + 'px';
}
fit();
addEventListener('resize', fit);

// ===================================================================
// 2. 상수 (위치·색은 prototype과 같음)
// ===================================================================
const cv = document.getElementById('cv'), g = cv.getContext('2d');
const CW = 1856, CH = 600, CAP = 8;      // 캔버스 크기, 서버당 동시 처리 한계 (SPEC 4.2)
const SEG_SEC = 0.6;                     // 애니메이션 한 구간 0.6초 (SPEC 8.2)
const HOLD_SEC = 3;                      // 타임아웃 원형 타이머 3초
const MAX_DOTS = 300;                    // 한 번에 움직이는 아이콘 상한 (화면이 버벅이지 않게)
const K = { bx: '#f4f4f2', bd: '#d3d1c7', t: '#2c2c2a', s: '#5f5e5a', get: '#1D9E75', post: '#D85A30',
            hit: '#534AB7', red: '#E24B4A', ok: '#1D9E75', amb: '#EF9F27', ambD: '#854F0B' };
const UC = [230, 320], P = [930, 320], SX = 1470, SW = 340, SH = 84;   // 사용자들, Proxy, 서버 상자 위치

// 서버 5대 (SPEC 2장). 화면 배치 순서 = 이 배열 순서
const SERVER_IDS = ['products-1', 'products-2', 'products-3', 'orders-1', 'orders-2'];
const SVY = [124, 216, 308, 456, 548], GRP = [0, 0, 0, 1, 1];
const GR = [
  { key: 'products', name: '상품 그룹', path: '/products/*', ids: [0, 1, 2], y: 44, h: 316, fill: '#F2FAF6', st: '#5DCAA5', tx: '#085041' },
  { key: 'orders', name: '주문 그룹', path: '/orders/*', ids: [3, 4], y: 376, h: 220, fill: '#FDF4EF', st: '#F0997B', tx: '#712B13' },
];
const SE = SVY.map(y => [SX, y]);        // 서버 상자 입구
const SIN = SVY.map(y => [SX + 44, y]);  // 서버 상자 안 (원형 타이머 자리)
const NS = SVY.map(y => [SX - 60, y]);   // 서버 상자 앞 (연결 실패 ✕ 자리)
const IDX = {};
SERVER_IDS.forEach((id, i) => { IDX[id] = i; });

// 화면용 한국어 이름 (SPEC 7.3): products-2 → "상품 서버 2"
function displayName(id) {
  let m = /^products-(\d+)$/.exec(id || '');
  if (m) return '상품 서버 ' + m[1];
  m = /^orders-(\d+)$/.exec(id || '');
  if (m) return '주문 서버 ' + m[1];
  return id || '-';
}
const NAME = SERVER_IDS.map(displayName);

// tries 결과 → 화면 문구
const RETRY_TAG = { timeout: '재시도 · 타임아웃', connect_fail: '재시도 · 미전달', no_response: '재시도 · 응답 없음' };
const RESULT_WORD = { timeout: '타임아웃', connect_fail: '연결 실패', no_response: '응답 없음', bad_response: '이상 응답' };
const MODES = { normal: '정상', hang: '응답 멈춤', bad: '이상 응답' };   // 서버 모드 이름 (메뉴·sys 문구)

// 상태 알림 (캔버스 위쪽 가운데): 3초 보였다가 1초 동안 서서히 사라짐, 최대 3개
const NOTE_SHOW = 3, NOTE_FADE = 1, NOTE_MAX = 3;

const isErr = s => s === 'ERR' || s >= 500;

// ===================================================================
// 3. 화면 상태 V: 받은 이벤트로만 바뀜 (버튼을 누른 쪽이 직접 바꾸지 않음)
// ===================================================================
function freshServers() {
  return SERVER_IDS.map(() => ({ mode: 'normal', excluded: false, fails: 0, active: 0, queue: 0,
                                 overloaded: false, done: 0, sm: 0, rec: 0 }));
}
function freshStats() { return { win: [], tot: 0, gets: 0, hits: 0, errs: 0, lat: [] }; }

const V = {
  proxyOn: true, cacheOn: false, sale: 'pre', countdown: 0, rate: 50, next: {},
  srv: freshServers(),
  dots: [], fx: [],          // 움직이는 아이콘, 잠깐 떴다 사라지는 배지
  notes: [],                 // 상태 알림 (sys) · 새 것이 앞
  log: [], sel: null, logDirty: true,
  st: freshStats(),          // 통계 (req 이벤트로 브라우저에서 계산)
};

// 지금 트래픽 양: 세일 중이면 슬라이더 값, 아니면 1건/초 (그릴 확률 계산에 씀)
function effectiveRate() { return V.sale === 'on' ? V.rate : 1; }

// ===================================================================
// 4. 그리기 도구 (prototype과 같음)
// ===================================================================
function rr(x, y, w, h, r) {
  g.beginPath(); g.moveTo(x + r, y);
  g.arcTo(x + w, y, x + w, y + h, r); g.arcTo(x + w, y + h, x, y + h, r);
  g.arcTo(x, y + h, x, y, r); g.arcTo(x, y, x + w, y, r); g.closePath();
}
// 아이콘: 카트(GET 조회) 또는 상자(POST 주문)
function icon(x, y, box, c) {
  g.save(); g.translate(x, y); g.scale(2.1, 2.1); g.strokeStyle = c; g.fillStyle = c; g.lineWidth = 1.7;
  if (box) { g.strokeRect(-7, -6, 14, 11); g.beginPath(); g.moveTo(-7, -1); g.lineTo(7, -1); g.stroke(); }
  else {
    g.beginPath(); g.moveTo(-9, -7); g.lineTo(-6, -7); g.lineTo(-3, 3); g.lineTo(7, 3); g.lineTo(8, -4); g.lineTo(-5, -4); g.stroke();
    g.beginPath(); g.arc(-2, 7, 1.8, 0, 7); g.arc(6, 7, 1.8, 0, 7); g.fill();
  }
  g.restore();
}
// 배지: 둥근 사각형 안의 흰 글씨
function tag(x, y, t, c) {
  g.font = '500 19px sans-serif'; const w = g.measureText(t).width + 20;
  rr(x - w / 2, y - 15, w, 30, 7); g.fillStyle = c; g.fill();
  g.fillStyle = '#fff'; g.textAlign = 'center'; g.fillText(t, x, y + 7);
}
const lerp = (a, b, u) => [a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u];
function fx(x, y, t, c, ttl) { V.fx.push({ x, y, t, c, ttl: ttl || 1, a: 0 }); }
function startPoint() { return [UC[0] + 150, UC[1] + (Math.random() - 0.5) * 220]; }

// ===================================================================
// 5. 이벤트 받기: 공급기(실제·모의)는 모두 이 함수로 넘김
// ===================================================================
function handleEvent(ev) {
  if (!ev || !ev.type) return;
  if (ev.type === 'status') onStatus(ev);
  else if (ev.type === 'req') onReq(ev);
  else if (ev.type === 'sys') onSys(ev);
}

// status (0.5초마다): 서버 상자·연결선·스위치·버튼을 이 값으로 맞춤
function onStatus(s) {
  if (s.cache_on !== V.cacheOn) { V.st.hits = 0; V.st.gets = 0; } // 캐시를 켜고 끌 때 Hit 비율을 새로 셈
  V.proxyOn = !!s.proxy_on;
  V.cacheOn = !!s.cache_on;
  V.sale = s.sale || 'pre';
  V.countdown = s.countdown || 0;
  V.rate = typeof s.rate === 'number' ? s.rate : V.rate;
  V.next = s.next || {};
  (s.servers || []).forEach(x => {
    const i = IDX[x.id];
    if (i === undefined) return;
    const v = V.srv[i];
    if (v.excluded && !x.excluded) v.rec = 1.6;   // 제외 → 복귀: 초록 연결선과 "복귀" 배지 1.6초
    v.mode = x.mode; v.excluded = !!x.excluded; v.fails = x.fails || 0;
    v.active = x.active || 0; v.queue = x.queue || 0; v.overloaded = !!x.overloaded; v.done = x.done || 0;
  });
  syncControls();
}

// sys: 로그에 회색 줄. "초기화"면 화면의 통계·로그도 처음으로 (탭 여러 개가 함께 초기화됨)
function onSys(e) {
  if (typeof e.msg === 'string' && e.msg.indexOf('초기화') === 0) resetView();
  notify(e.msg, e.level);
}

/*
 * 상태 변화 알림: 로그에 회색 줄 + 흐름 화면 위쪽 가운데에 크게.
 * level(테두리 색)은 서버가 정해서 보냄: 'bad' 빨강, 'good' 초록, 그 밖 회색.
 * 서버가 보내지 않는 알림(연결 끊김 등)은 부르는 쪽이 정함.
 */
function notify(text, level) {
  text = String(text || '');
  addLog({ sys: true, text });
  const kind = level === 'bad' || level === 'good' ? level : 'info';
  V.notes.unshift({ text, kind, age: 0 });   // 새 알림이 위
  if (V.notes.length > NOTE_MAX) V.notes.length = NOTE_MAX;
}

// req: 통계 → 로그 → 애니메이션
function onReq(e) {
  e.tries = Array.isArray(e.tries) ? e.tries : [];
  updateStats(e);
  addLog(makeLogEntry(e));
  maybeAnimate(e);
}

function resetView() {
  V.st = freshStats();
  V.log = []; V.sel = null; V.dots = []; V.fx = [];
  V.srv.forEach(v => { v.sm = 0; v.rec = 0; });
  document.getElementById('det').innerHTML = EMPTY_DET;
  V.logDirty = true;
}

// ===================================================================
// 6. req 이벤트 → 애니메이션 경로 (SPEC 8.2)
//    이벤트는 요청이 "끝난 뒤" 오므로, 받은 순간부터 결과를 느리게 다시 그립니다.
// ===================================================================

// 그릴지 정함: 일반 min(1, 12/rate), 중요한 요청(HIT·재시도·에러) min(1, 30/rate)
function maybeAnimate(e) {
  // 탭이 안 보이는 동안은 화면이 멈춰 있으므로 쌓지 않음 (돌아왔을 때 한꺼번에 쏟아지지 않게)
  if (document.hidden || V.dots.length >= MAX_DOTS) return;
  const important = e.cache === 'HIT' || e.tries.length > 1 || isErr(e.status);
  const p = Math.min(1, (important ? 30 : 12) / Math.max(1, effectiveRate()));
  if (Math.random() > p) return;
  const d = buildPath(e);
  if (d) V.dots.push(d);
}

/*
 * 경로 하나 = 점(pts)들을 차례로 지나가는 아이콘.
 *   cols[k]  : 점 k → k+1 구간의 아이콘 색
 *   tags[k]  : 그 구간에서 아이콘 위에 붙는 배지
 *   dash[k]  : 그 구간을 점선 자취로 그림(재시도 이동)
 *   holds[k] : 점 k에 도착하면 그 자리에서 원형 타이머(초)
 *   arrive[k]: 점 k에 도착할 때 한 번 실행 (HIT·✕ 배지 등)
 */
function buildPath(e) {
  const post = e.method === 'POST', col = post ? K.post : K.get, a = startPoint();
  const d = { pts: [a], cols: [], tags: {}, dash: {}, holds: {}, arrive: {}, box: post,
              k: 0, u: 0, hold: 0, holdMax: 0, held: {} };
  const go = (pt, c) => { d.pts.push(pt); d.cols.push(c); return d.pts.length - 1; };
  const label = String(e.status);
  // 실패: 빨갛게 사용자에게 돌아옴 (Proxy를 거쳤으면 Proxy를 지나서)
  const back = (via) => {
    if (via === 'proxy') { const k = go(P, K.red); d.tags[k - 1] = [label, K.red]; }
    const k = go(a, K.red); d.tags[k - 1] = [label, K.red];
  };

  // --- Proxy 끔: 사용자들 → 상품 서버 1 직결 ---
  if (e.via === 'direct') {
    const t = e.tries[0] || { result: 'ok' };
    if (t.result === 'ok' && !isErr(e.status)) { go(SE[0], col); return d; }
    if (t.result === 'timeout') { const k = go(SIN[0], col); d.holds[k] = HOLD_SEC; }
    else if (t.result === 'connect_fail') { const k = go(NS[0], col); d.arrive[k] = () => fx(NS[0][0], NS[0][1] - 40, '✕', K.red); }
    else go(SE[0], col);
    back('direct');   // 503(과부하)·ERR: 빨갛게 돌아옴
    return d;
  }

  // --- Proxy 거침 ---
  go(P, col);
  if (e.cache === 'HIT') {
    // Proxy에서 바로 되돌아오고 보라색 HIT 배지
    d.arrive[1] = () => fx(P[0], P[1] - 115, 'HIT', K.hit);
    go(a, K.hit);
    return d;
  }
  if (e.tries.length === 0) {
    // 보낼 서버가 없음(503) 등 Proxy가 바로 답함 → Proxy에서 바로 돌아옴
    const k = go(a, K.red); d.tags[k - 1] = [label, K.red];
    return d;
  }

  for (let j = 0; j < e.tries.length; j++) {
    const t = e.tries[j], i = IDX[t.server];
    if (i === undefined) continue;
    const last = j === e.tries.length - 1;

    // 이번 시도의 목적지: 연결 실패는 서버 앞, 타임아웃은 서버 안, 나머지는 서버 입구
    const dest = t.result === 'connect_fail' ? NS[i] : t.result === 'timeout' ? SIN[i] : SE[i];
    const k = go(dest, col);
    if (j > 0) {
      // 재시도 이동: 점선으로 다른 서버에 가며 앞 시도의 실패 이유 배지
      d.dash[k - 1] = true;
      d.tags[k - 1] = [RETRY_TAG[e.tries[j - 1].result] || '재시도', K.ambD];
    }
    if (t.result === 'timeout') d.holds[k] = HOLD_SEC;
    if (t.result === 'connect_fail') d.arrive[k] = () => fx(NS[i][0], NS[i][1] - 40, '✕', K.red);

    if (t.result === 'ok') {
      // 응답 받음: 200이면 서버에서 끝. 백엔드가 준 503(과부하) 등은 서버에서 빨갛게 돌아옴
      if (isErr(e.status) || e.status >= 400) back('proxy');
      return d;
    }
    if (t.result === 'bad_response' || last) {
      // 이상 응답(502), 또는 마지막 시도도 실패(502·504): 빨갛게 사용자에게
      back('proxy');
      return d;
    }
  }
  return d;
}

// 경로를 한 프레임만큼 움직이고 그립니다. 끝났으면 false
function stepDot(d, dt) {
  if (d.hold > 0) {
    d.hold -= dt;
    if (d.hold > 0) { const p = d.pts[d.k]; ring(p[0], p[1], d.hold / d.holdMax); icon(p[0], p[1], d.box, d.cols[d.k - 1] || d.cols[0]); return true; }
  }
  d.u += dt / SEG_SEC;
  while (d.u >= 1) {
    d.u -= 1; d.k++;                          // 점 d.k에 도착
    if (d.arrive[d.k]) d.arrive[d.k]();
    if (d.k >= d.pts.length - 1) return false;
    if (d.holds[d.k] && !d.held[d.k]) {       // 여기서 원형 타이머
      d.held[d.k] = true; d.hold = d.holdMax = d.holds[d.k]; d.u = 0;
      const p = d.pts[d.k]; ring(p[0], p[1], 1); icon(p[0], p[1], d.box, d.cols[d.k - 1]);
      return true;
    }
  }
  const p = lerp(d.pts[d.k], d.pts[d.k + 1], d.u);
  if (d.dash[d.k]) {
    g.strokeStyle = K.ambD; g.lineWidth = 2; g.setLineDash([5, 7]);
    g.beginPath(); g.moveTo(d.pts[d.k][0], d.pts[d.k][1]); g.lineTo(p[0], p[1]); g.stroke(); g.setLineDash([]);
  }
  icon(p[0], p[1], d.box, d.cols[d.k]);
  if (d.tags[d.k]) tag(p[0], p[1] - 38, d.tags[d.k][0], d.tags[d.k][1]);
  return true;
}

// ===================================================================
// 7. 통계 (SPEC 8.4) · req 이벤트로 브라우저에서 계산
// ===================================================================
function updateStats(e) {
  const st = V.st;
  st.win.push(performance.now());            // 최근 1초 요청 수
  st.tot++;
  if (e.method === 'GET') st.gets++;
  if (e.cache === 'HIT') st.hits++;
  if (isErr(e.status)) st.errs++;
  if (e.status === 200) { st.lat.push(e.latency_ms); if (st.lat.length > 300) st.lat.shift(); }
}
function renderStats() {
  const st = V.st, now = performance.now();
  st.win = st.win.filter(t => now - t < 1000);
  document.getElementById('m1').textContent = st.win.length;
  document.getElementById('m2').textContent = st.tot.toLocaleString();
  document.getElementById('m3').textContent = V.cacheOn && st.gets ? Math.round(100 * st.hits / st.gets) + '%' : '-';
  document.getElementById('m4').textContent = st.errs.toLocaleString();
  const rl = st.lat.slice(-150);              // 최근 성공 150건 평균
  document.getElementById('m5').textContent = rl.length ? Math.round(rl.reduce((a, b) => a + b, 0) / rl.length) + 'ms' : '-';
}

// ===================================================================
// 8. 로그·상세 패널 (SPEC 8.4)
// ===================================================================
const logEl = document.getElementById('log');
const EMPTY_DET = '<div style="color:#888780;font-size:17px;margin-top:20px">로그 줄을 누르면 요청 정보가 여기 표시됩니다</div>';
document.getElementById('det').innerHTML = EMPTY_DET;
let hover = false;
logEl.addEventListener('mouseenter', () => { hover = true; });
logEl.addEventListener('mouseleave', () => { hover = false; V.logDirty = true; renderLog(); });

const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const GROUP_NAME = { products: '상품 그룹', orders: '주문 그룹' };

// 로그 문장의 "→ 이후" 부분. 예: 상품 서버 2 타임아웃 → 상품 서버 3 재시도
function describe(e) {
  if (e.via === 'direct') {
    const t = e.tries[0] || {};
    let s = displayName(t.server || 'products-1') + ' 직결';
    if (t.result && t.result !== 'ok') s += ' · ' + RESULT_WORD[t.result];
    else if (e.status === 503) s += ' · 과부하';
    return s;
  }
  if (e.cache === 'HIT') return 'Cache HIT';
  if (e.tries.length === 0) {
    if (e.status === 503) return (GROUP_NAME[e.route] || '그룹') + ' 보낼 서버 없음';
    return 'Proxy 응답 없음';
  }
  const parts = e.tries.map((t, j) => {
    let s = displayName(t.server) + (j > 0 ? ' 재시도' : '');
    if (t.result !== 'ok') s += ' ' + (RESULT_WORD[t.result] || t.result);
    return s;
  });
  let s = parts.join(' → ');
  const lastOk = e.tries[e.tries.length - 1].result === 'ok';
  if (lastOk && e.status === 503) s += ' 과부하';
  if (e.cache === 'MISS') s += ' · MISS';
  return s;
}

function fmtTime(ms) {
  const d = new Date(ms), z = (n, l) => String(n).padStart(l || 2, '0');
  return z(d.getHours()) + ':' + z(d.getMinutes()) + ':' + z(d.getSeconds()) + '.' + z(d.getMilliseconds(), 3);
}

function makeLogEntry(e) {
  return { m: e.method, p: e.path, d: describe(e), st: e.status, lat: e.latency_ms, time: fmtTime(e.t || Date.now()),
           hit: e.cache === 'HIT', retry: e.tries.length > 1 };
}

// 로그에 한 줄 추가 (최근 100줄). 실제 다시 그리기는 아래 타이머가 1초에 4번으로 묶어서 함
function addLog(entry) {
  V.log.push(entry);
  if (V.log.length > 100) V.log.shift();
  V.logDirty = true;
}

function renderLog() {
  if (hover || !V.logDirty) return;   // 마우스를 올리면 멈춤
  V.logDirty = false;
  logEl.innerHTML = V.log.map((e, i) => {
    if (e.sys) return '<div class="sys">— ' + esc(e.text) + '</div>';
    const cls = isErr(e.st) ? 'er' : e.hit ? 'hit' : e.retry ? 'rt' : '';
    return '<div data-i="' + i + '" class="' + cls + (V.sel === e ? ' sel' : '') + '">' +
           esc(e.m + ' ' + e.p + ' → ' + e.d + ' · ' + e.st + ' · ' + e.lat + 'ms') + '</div>';
  }).join('');
  logEl.scrollTop = logEl.scrollHeight;
}
setInterval(renderLog, 250);   // 초당 100건이 와도 버벅이지 않게 묶어서 그림

logEl.addEventListener('click', ev => {
  const div = ev.target.closest('div[data-i]');
  if (!div) return;
  const e = V.log[+div.dataset.i];
  if (!e || e.sys) return;
  V.sel = e;
  logEl.querySelectorAll('.sel').forEach(x => x.classList.remove('sel'));
  div.classList.add('sel');
  const c = isErr(e.st) ? '#A32D2D' : '#085041';
  document.getElementById('det').innerHTML =
    '<div class="r"><span class="k">메서드</span><span class="v">' + esc(e.m) + '</span></div>' +
    '<div class="r"><span class="k">경로</span><span class="v">' + esc(e.p) + '</span></div>' +
    '<div class="r"><span class="k">상태 코드</span><span class="v" style="color:' + c + '">' + esc(e.st) + '</span></div>' +
    '<div class="r"><span class="k">요청 시각</span><span class="v">' + esc(e.time) + '</span></div>';
});

// ===================================================================
// 9. 캔버스 그리기 (prototype의 draw를 화면 상태 V로)
// ===================================================================
function group(G) {
  rr(SX - 30, G.y, SW + 60, G.h, 18); g.fillStyle = G.fill; g.fill(); g.strokeStyle = G.st; g.lineWidth = 2; g.stroke();
  g.textAlign = 'left'; g.fillStyle = G.tx; g.font = '500 20px sans-serif'; g.fillText(G.name, SX - 10, G.y + 28);
  g.font = '18px sans-serif'; g.fillText(G.path, SX + 90, G.y + 28);
  // "다음: 서버 N" (status.next = Round-Robin이 다음에 고를 서버)
  let t = '';
  if (V.proxyOn) {
    const nx = V.next[G.key];
    t = nx ? '다음: ' + displayName(nx).slice(3) : '보낼 서버 없음';
  }
  g.textAlign = 'right'; g.font = '17px sans-serif'; g.fillStyle = G.tx; g.fillText(t, SX + SW + 10, G.y + 28);
}

function server(i) {
  const x = SX, y = SVY[i] - SH / 2, s = V.srv[i], md = s.mode, ovl = s.overloaded, G = GR[GRP[i]];
  rr(x, y, SW, SH, 12); g.fillStyle = '#fff'; g.fill();
  g.strokeStyle = md === 'hang' ? K.amb : (md === 'bad' || ovl) ? K.red : G.st;
  g.lineWidth = (md !== 'normal' || ovl) ? 3 : 1.5; g.stroke();
  g.textAlign = 'left'; g.fillStyle = K.t; g.font = '500 23px sans-serif'; g.fillText(NAME[i], x + 18, y + 33);
  g.textAlign = 'right'; g.font = '16px sans-serif'; g.fillStyle = K.s; g.fillText('처리 ' + s.done.toLocaleString() + '건', x + SW - 18, y + 32);
  g.textAlign = 'left';
  if (md === 'hang') { g.fillStyle = K.ambD; g.font = '500 19px sans-serif'; g.fillText('응답 멈춤', x + 90, y + 66); }
  else if (md === 'bad') { g.fillStyle = K.red; g.font = '500 19px sans-serif'; g.fillText('이상 응답', x + 18, y + 66); }
  else if (ovl) { g.fillStyle = K.red; g.font = '500 19px sans-serif'; g.fillText('과부하', x + 18, y + 66); }
  else {
    // 부하 게이지 (active+queue)/8, 부드럽게 변화. 0.55 이상 주황, 0.85 이상 빨강 (SPEC 8.3)
    s.sm += ((s.active + s.queue) - s.sm) * 0.12;
    const lv = Math.min(1, s.sm / CAP), bw = 150;
    g.fillStyle = '#ebe9e2'; rr(x + 18, y + 52, bw, 12, 6); g.fill();
    if (lv > 0.02) { g.fillStyle = lv > 0.85 ? K.red : lv > 0.55 ? K.amb : K.get; rr(x + 18, y + 52, Math.max(12, bw * lv), 12, 6); g.fill(); }
    g.textAlign = 'right'; g.fillStyle = K.s; g.font = '15px sans-serif';
    g.fillText('동시 ' + s.active + '/' + CAP + ' · 대기 ' + s.queue, x + SW - 18, y + 64);
  }
  g.lineWidth = 1;
}

function legend() {
  const items = [['cart', 'GET 조회', K.get], ['box', 'POST 주문', K.post], ['sq', 'Cache Hit', K.hit], ['sq', '재시도', K.ambD], ['sq', '에러', K.red]];
  let x = CW - 10; g.font = '17px sans-serif'; g.textAlign = 'right';
  for (let k = items.length - 1; k >= 0; k--) {
    const it = items[k]; g.fillStyle = K.s; g.fillText(it[1], x, 28);
    if (it[0] === 'sq') { x -= g.measureText(it[1]).width + 32; g.fillStyle = it[2]; rr(x + 4, 12, 17, 17, 4); g.fill(); }
    else { x -= g.measureText(it[1]).width + 56; icon(x + 24, 21, it[0] === 'box', it[2]); }
    x -= 24;
  }
}

// 상태 알림: 왼쪽 위 타이머와 오른쪽 위 범례 사이 가운데에 최대 3개를 위아래로 쌓음
const NOTE_X = 700, NOTE_Y = 34, NOTE_H = 46, NOTE_GAP = 8;
const NOTE_BORDER = { good: K.ok, bad: K.red, info: '#b4b2a9' };
function drawNotes(dt) {
  V.notes = V.notes.filter(n => (n.age += dt) < NOTE_SHOW + NOTE_FADE);
  V.notes.forEach((n, k) => {
    const y = NOTE_Y + k * (NOTE_H + NOTE_GAP);
    g.globalAlpha = n.age < NOTE_SHOW ? 1 : Math.max(0, 1 - (n.age - NOTE_SHOW) / NOTE_FADE);
    g.font = '500 26px sans-serif';
    const w = g.measureText(n.text).width + 44;
    rr(NOTE_X - w / 2, y, w, NOTE_H, 12);
    g.fillStyle = '#fff'; g.fill();
    g.strokeStyle = NOTE_BORDER[n.kind]; g.lineWidth = 3; g.stroke();
    g.fillStyle = K.t; g.textAlign = 'center'; g.fillText(n.text, NOTE_X, y + 32);
    g.globalAlpha = 1; g.lineWidth = 1;
  });
}

function ring(x, y, f) {
  g.strokeStyle = '#ebe9e2'; g.lineWidth = 5; g.beginPath(); g.arc(x, y, 30, 0, 7); g.stroke();
  g.strokeStyle = K.amb; g.beginPath(); g.arc(x, y, 30, -Math.PI / 2, -Math.PI / 2 + Math.PI * 2 * f); g.stroke(); g.lineWidth = 1;
}

function draw(dt) {
  g.clearRect(0, 0, CW, CH);
  // 왼쪽 위 타이머: "세일 전" / "세일까지 00:0N" / "세일 중"
  g.textAlign = 'left'; g.font = '500 26px sans-serif';
  if (V.sale === 'pre') { g.fillStyle = K.s; g.fillText('세일 전', 8, 30); }
  else if (V.sale === 'count') { g.fillStyle = K.t; g.fillText('세일까지 00:0' + Math.max(1, V.countdown), 8, 30); }
  else { g.fillStyle = K.post; g.fillText('세일 중', 8, 30); }
  legend();
  GR.forEach(group);

  // 연결선: 실패 중이면 빨간 점선, 제외되면 사라짐, 복귀하면 초록선
  g.lineWidth = 2;
  if (V.proxyOn) {
    g.strokeStyle = K.bd; g.setLineDash([8, 10]); g.beginPath(); g.moveTo(UC[0] + 150, UC[1]); g.lineTo(P[0] - 140, P[1]); g.stroke();
    SVY.forEach((y, i) => {
      const s = V.srv[i];
      if (s.excluded) return;
      let c = K.bd, dsh = [8, 10];
      if (s.fails > 0) { c = K.red; dsh = [5, 7]; }
      if (s.rec > 0) { c = K.ok; dsh = []; }
      g.strokeStyle = c; g.setLineDash(dsh); g.beginPath(); g.moveTo(P[0] + 140, P[1]); g.lineTo(SX - 30, y); g.stroke();
    });
  } else {
    // Proxy 끔: 사용자들 → 상품 서버 1 빨간 점선 직결
    g.strokeStyle = K.red; g.setLineDash([8, 10]); g.beginPath(); g.moveTo(UC[0] + 150, UC[1]); g.lineTo(SX - 30, SVY[0]); g.stroke();
  }
  g.setLineDash([]);

  rr(UC[0] - 150, UC[1] - 170, 300, 340, 18); g.fillStyle = K.bx; g.fill(); g.strokeStyle = K.bd; g.lineWidth = 2; g.stroke();
  g.fillStyle = K.t; g.font = '500 28px sans-serif'; g.textAlign = 'center'; g.fillText('사용자들', UC[0], UC[1] - 125);

  // Proxy 상자: 끄면 흐려지고 "OFF"
  g.globalAlpha = V.proxyOn ? 1 : 0.35;
  rr(P[0] - 140, P[1] - 80, 280, 160, 18); g.fillStyle = '#fff'; g.fill(); g.strokeStyle = K.hit; g.lineWidth = 4; g.stroke();
  g.fillStyle = K.t; g.font = '500 34px sans-serif'; g.fillText('Proxy', P[0], P[1] - 4);
  g.fillStyle = K.s; g.font = '18px sans-serif'; g.fillText(V.proxyOn ? '경로 라우팅 · Round-Robin' : 'OFF', P[0], P[1] + 30);
  if (V.proxyOn && V.cacheOn) g.fillText('캐시: 상품 상세만', P[0], P[1] + 56);
  g.globalAlpha = 1; g.lineWidth = 1;

  [0, 1, 2, 3, 4].forEach(server);
  SVY.forEach((y, i) => {
    if (V.srv[i].excluded) tag(SX - 130, y - 26, '제외됨', K.red);
    else if (V.srv[i].rec > 0) tag(SX - 130, y - 26, '복귀', K.ok);
  });

  V.dots = V.dots.filter(d => stepDot(d, dt));
  V.fx = V.fx.filter(f => {
    f.a += dt; if (f.a > f.ttl) return false;
    g.globalAlpha = 1 - f.a / f.ttl; tag(f.x, f.y - f.a * 20, f.t, f.c); g.globalAlpha = 1; return true;
  });
  drawNotes(dt);   // 알림은 아이콘보다 위에 그림
}

// ===================================================================
// 10. 메인 루프
// ===================================================================
let prev = performance.now(), statT = 0;
function loop(now) {
  const dt = Math.min(0.05, (now - prev) / 1000); prev = now;
  V.srv.forEach(s => { if (s.rec > 0) s.rec -= dt; });
  statT += dt;
  if (statT >= 0.4) { statT = 0; renderStats(); }
  draw(dt);
  requestAnimationFrame(loop);
}

// ===================================================================
// 11. 조작 바: 버튼은 공급기의 control()만 부르고, 화면은 status 이벤트로 맞춤
// ===================================================================
const trEl = document.getElementById('tr'), trv = document.getElementById('trv');
const tp = document.getElementById('tp'), tc = document.getElementById('tc');
const srvBtns = document.querySelectorAll('.pb.srv');
let source = null;   // 아래 12·13에서 정함
const ctl = (name, params) => source.control(name, params || {});

document.getElementById('sale').addEventListener('click', () => ctl('sale'));
tp.addEventListener('click', () => ctl('proxy', { on: V.proxyOn ? 0 : 1 }));
tc.addEventListener('click', () => ctl('cache', { on: V.cacheOn ? 0 : 1 }));
// 서버 버튼: 누르면 버튼 바로 위에 메뉴 [정상 | 멈춤 | 이상 응답]이 뜸.
// 항목을 누르면 그 모드로 한 번만 호출하고 닫음. 바깥을 누르거나 Esc면 그냥 닫음.
const menuEl = document.getElementById('srvmenu');
let menuFor = -1;   // 메뉴가 열린 서버 번호 (-1 = 닫힘)

function openMenu(i, btn) {
  menuFor = i;
  menuEl.classList.add('open');
  // .bar 기준 위치: 버튼의 왼쪽에 맞추되 바 밖으로 나가지 않게
  const bar = btn.offsetParent;
  const left = Math.min(btn.offsetLeft, bar.clientWidth - menuEl.offsetWidth - 8);
  menuEl.style.left = Math.max(8, left) + 'px';
  markMenu();
}
function closeMenu() { menuFor = -1; menuEl.classList.remove('open'); }
// 지금 상태인 항목 표시 (status가 올 때마다 다시 맞춤)
function markMenu() {
  if (menuFor < 0) return;
  const cur = V.srv[menuFor].mode;
  menuEl.querySelectorAll('.mi').forEach(m => m.classList.toggle('cur', m.dataset.mode === cur));
}

srvBtns.forEach(b => b.addEventListener('click', ev => {
  ev.stopPropagation();
  const i = +b.dataset.i;
  if (menuFor === i) closeMenu(); else openMenu(i, b);
}));
menuEl.addEventListener('click', ev => {
  ev.stopPropagation();
  const item = ev.target.closest('.mi');
  if (!item || menuFor < 0) return;
  ctl('server', { id: SERVER_IDS[menuFor], mode: item.dataset.mode });
  closeMenu();
});
document.addEventListener('click', () => { if (menuFor >= 0) closeMenu(); });   // 바깥 누르기
document.addEventListener('keydown', ev => { if (ev.key === 'Escape') closeMenu(); });
document.getElementById('rst').addEventListener('click', () => ctl('reset'));

// 트래픽 슬라이더: 끄는 동안과 놓은 직후 1초는 status가 값을 되돌리지 않음. 보내기는 150ms에 한 번
let dragging = false, userRateUntil = 0, rateTimer = null, rateSentAt = 0;
function sendRate() { rateTimer = null; rateSentAt = performance.now(); ctl('rate', { value: trEl.value }); }
trEl.addEventListener('pointerdown', () => { dragging = true; });
addEventListener('pointerup', () => { if (dragging) { dragging = false; userRateUntil = performance.now() + 1000; } });
trEl.addEventListener('input', () => {
  trv.textContent = trEl.value + '건/초';
  userRateUntil = performance.now() + 1000;
  const wait = 150 - (performance.now() - rateSentAt);
  if (wait <= 0) sendRate();
  else if (!rateTimer) rateTimer = setTimeout(sendRate, wait);
});

// status 값으로 스위치·버튼·슬라이더를 맞춤 (탭을 두 개 열어도 같은 모습)
function syncControls() {
  tp.classList.toggle('act', V.proxyOn);
  tc.classList.toggle('act', V.cacheOn);
  srvBtns.forEach(b => {
    const m = V.srv[+b.dataset.i].mode;
    b.classList.toggle('hang', m === 'hang');
    b.classList.toggle('bad', m === 'bad');
  });
  markMenu();
  if (!dragging && performance.now() > userRateUntil && !rateTimer && +trEl.value !== V.rate) {
    trEl.value = V.rate;
    trv.textContent = V.rate + '건/초';
  }
}

const bannerEl = document.getElementById('banner');
function setBanner(text, cls) {
  bannerEl.textContent = text || '';
  bannerEl.className = text ? cls : '';
}

// ===================================================================
// 12. 실제 서버 공급기: SSE로 받고, 제어는 POST
// ===================================================================
function startRealSource() {
  const es = new EventSource('/api/events');
  let lost = false;   // 끊김·재연결 알림을 바뀔 때 한 번만 띄우기 위함
  es.onopen = () => {
    setBanner(null);
    if (lost) { lost = false; notify('서버 다시 연결됨', 'good'); }
  };
  es.onerror = () => {   // 브라우저가 알아서 다시 연결을 시도함
    setBanner('서버 연결 끊김', 'err');
    if (!lost) { lost = true; notify('서버 연결 끊김', 'bad'); }
  };
  es.onmessage = m => {
    try { handleEvent(JSON.parse(m.data)); }
    catch (err) { console.warn('이벤트를 읽을 수 없음', err); }
  };
  return {
    control(name, params) {
      const q = new URLSearchParams(params).toString();
      fetch('/api/control/' + name + (q ? '?' + q : ''), { method: 'POST' })
        .then(r => { if (!r.ok) r.json().then(j => notify('제어 실패: ' + (j.error || r.status), 'bad')).catch(() => {}); })
        .catch(() => notify('제어 실패: 서버 연결 끊김', 'bad'));
    },
  };
}

// ===================================================================
// 13. 모의 모드 공급기 (SPEC 8.6, 발표 백업)
//     prototype의 "가짜 서버 흉내"를 옮겨, 실제 서버와 같은 형식의 이벤트를 만들어 냅니다.
//     숫자와 규칙은 SPEC 4~7장과 같습니다.
// ===================================================================
function startMockSource() {
  setBanner('모의 모드', 'mock');
  const SVC = 300, QMAX = 50, READ_TO = 3000, CLIENT_TO = 10000, HANG_MAX = 30000, TTL = 10000;
  const GROUP_IDS = { products: [0, 1, 2], orders: [3, 4] };
  let S;
  function fresh() {
    S = { proxy: true, cache: false, sale: 'pre', countStart: 0, rate: 50,
          mode: SERVER_IDS.map(() => 'normal'), excl: SERVER_IDS.map(() => false), fails: SERVER_IDS.map(() => 0),
          srv: SERVER_IDS.map(() => ({ act: [], q: [], hang: [], ovl: false, done: 0 })),
          rr: { products: 0, orders: 0 }, cT: {}, nextId: 1, gap: 0 };
  }
  fresh();
  const now = () => performance.now();
  const emit = e => handleEvent(e);
  const sys = (msg, level) => emit({ type: 'sys', t: Date.now(), level: level || 'info', msg });

  // --- 가짜 백엔드 (SPEC 4장): 동시 8건, 대기열, 과부하, 장애 모드 ---
  // cb(결과, 상태): 'ok'+200/503, 'bad'(HTTP 아님), 'closed'(응답 없이 닫힘)
  function backend(i, cb, abandoned) {
    const m = S.mode[i];
    if (m === 'bad') { cb('bad'); return; }
    if (m === 'hang') { S.srv[i].hang.push({ cb, abandoned, start: now() }); return; }
    process(i, cb);
  }
  function process(i, cb) {
    const s = S.srv[i];
    if (s.ovl) { cb('ok', 503); return; }
    if (s.act.length < 8) s.act.push({ end: now() + SVC, cb });
    else {
      s.q.push({ cb });
      if (s.q.length > QMAX && !s.ovl) { s.ovl = true; sys('과부하: ' + NAME[i] + ' 다운 (새 요청에 503)', 'bad'); }
    }
  }
  // 모드가 hang이 아니게 되면 기다리던 요청을 바뀐 모드대로 처리 (이미 포기된 요청은 버림)
  function setMode(i, m) {
    const old = S.mode[i];
    S.mode[i] = m;
    if (old === 'hang' && m !== 'hang') {
      const list = S.srv[i].hang; S.srv[i].hang = [];
      list.forEach(h => { if (h.abandoned()) return; if (m === 'bad') h.cb('bad'); else process(i, h.cb); });
    }
  }

  // --- 가짜 Proxy (SPEC 5장): 시도 한 번 = 제한 시간이 있는 백엔드 호출 ---
  function attempt(i, timeoutMs, onResult) {
    let finished = false;
    const timer = setTimeout(() => { if (!finished) { finished = true; onResult('timeout'); } }, timeoutMs);
    backend(i, (res, status) => {
      if (finished) return;
      finished = true; clearTimeout(timer);
      if (res === 'bad') onResult('bad_response');
      else if (res === 'closed') onResult('no_response');
      else onResult('ok', status);
    }, () => finished);
  }
  function pick(group, skip) {
    const ids = GROUP_IDS[group];
    for (let k = 0; k < ids.length; k++) {
      const n = (S.rr[group] + k) % ids.length, i = ids[n];
      if (!S.excl[i] && i !== skip) { S.rr[group] = (n + 1) % ids.length; return i; }
    }
    return null;
  }
  function peek(group) {
    const ids = GROUP_IDS[group];
    for (let k = 0; k < ids.length; k++) { const i = ids[(S.rr[group] + k) % ids.length]; if (!S.excl[i]) return SERVER_IDS[i]; }
    return null;
  }

  // 요청 하나가 끝나면 SPEC 7.1 형식의 req 이벤트
  function finish(r, status) {
    emit({ type: 'req', id: r.id, t: r.t, method: r.method, path: r.path, kind: r.kind, via: r.via,
           route: r.route, upstream: r.upstream || null, tries: r.tries, cache: r.cache, status,
           latency_ms: Math.max(1, Math.round(now() - r.t0)) });
  }

  // --- 트래픽 생성기 (SPEC 3장, 6.1): 상세 45%, 재고 30%, 주문 25% ---
  function newRequest() {
    const pick100 = Math.random() * 100, pid = 1 + Math.floor(Math.random() * 3);
    const kind = pick100 < 45 ? 'detail' : pick100 < 75 ? 'stock' : 'order';
    const r = { id: S.nextId++, t: Date.now(), t0: now(), kind, method: kind === 'order' ? 'POST' : 'GET',
                path: kind === 'order' ? '/orders' : '/products/' + pid + (kind === 'stock' ? '/stock' : ''),
                via: S.proxy ? 'proxy' : 'direct', route: '-', tries: [], cache: 'BYPASS', upstream: null };
    if (r.via === 'direct') {
      // Proxy 끔: 모든 요청이 상품 서버 1로 직결 (클라이언트 제한 시간 10초)
      attempt(0, CLIENT_TO, (res, status) => {
        r.tries = [{ server: 'products-1', result: res }];
        if (res === 'ok') { r.upstream = 'products-1'; finish(r, status); } else finish(r, 'ERR');
      });
      return;
    }
    const group = kind === 'order' ? 'orders' : 'products';
    r.route = group;
    if (kind === 'detail' && S.cache) {
      if (S.cT[r.path] > now()) { r.cache = 'HIT'; setTimeout(() => finish(r, 200), 1); return; }
      r.cache = 'MISS';
    }
    const first = pick(group, -1);
    if (first === null) { finish(r, 503); return; }   // 그룹 전체 제외
    const tryServer = (i, isRetry) => attempt(i, READ_TO, (res, status) => {
      r.tries.push({ server: SERVER_IDS[i], result: res });
      if (res === 'ok') {
        r.upstream = SERVER_IDS[i];
        if (status === 200 && r.cache === 'MISS' && S.cache) S.cT[r.path] = now() + TTL;
        finish(r, status); return;
      }
      // 재시도 규칙 (SPEC 5.6): 응답 멈춤·응답 없음은 GET만 1회, 이상 응답은 안 함
      const retryable = (res === 'timeout' || res === 'no_response' || res === 'connect_fail') &&
                        (r.method === 'GET' || res === 'connect_fail');
      if (retryable && !isRetry) {
        const alt = pick(group, i);
        if (alt !== null) { tryServer(alt, true); return; }
      }
      finish(r, res === 'timeout' ? 504 : 502);
    });
    tryServer(first, false);
  }

  // --- 헬스 체크 (SPEC 5.7): 2초마다, 연속 2회 실패 → 제외, 1회 성공 → 복귀 ---
  function health() {
    S.mode.forEach((m, i) => {
      if (m !== 'normal') {
        S.fails[i]++;
        if (S.fails[i] >= 2 && !S.excl[i]) { S.excl[i] = true; sys('헬스 체크: ' + NAME[i] + ' 제외 (' + (m === 'hang' ? '응답 없음' : '이상 응답') + ')', 'bad'); }
      } else {
        if (S.excl[i]) { S.excl[i] = false; sys('헬스 체크: ' + NAME[i] + ' 복귀', 'good'); }
        S.fails[i] = 0;
      }
    });
  }

  // --- status 이벤트 (SPEC 7.2) ---
  function emitStatus() {
    let countdown = 0;
    if (S.sale === 'count') countdown = Math.max(1, 5 - Math.floor((now() - S.countStart) / 1000));
    emit({ type: 'status', proxy_on: S.proxy, cache_on: S.cache, sale: S.sale, countdown, rate: S.rate,
           next: { products: peek('products'), orders: peek('orders') },
           servers: SERVER_IDS.map((id, i) => ({ id, group: GRP[i] ? 'orders' : 'products', mode: S.mode[i],
             excluded: S.excl[i], fails: S.fails[i], active: S.srv[i].act.length, queue: S.srv[i].q.length,
             overloaded: S.srv[i].ovl, done: S.srv[i].done })) });
  }

  // --- 시간 흐름: 처리 완료, 대기열, 과부하 회복, hang 30초, 세일, 요청 생성 ---
  let last = now(), healthT = 0, statusT = 0;
  setInterval(() => {
    const t = now(), dt = (t - last) / 1000; last = t;
    S.srv.forEach((s, i) => {
      s.act = s.act.filter(a => { if (a.end <= t) { s.done++; a.cb('ok', 200); return false; } return true; });
      while (s.act.length < 8 && s.q.length) { const q = s.q.shift(); s.act.push({ end: t + SVC, cb: q.cb }); }
      if (s.ovl && s.q.length === 0 && s.act.length < 3) { s.ovl = false; sys('과부하 회복: ' + NAME[i], 'good'); }
      s.hang = s.hang.filter(h => { if (t - h.start >= HANG_MAX) { h.cb('closed'); return false; } return true; });
    });
    if (S.sale === 'count' && t - S.countStart >= 5000) { S.sale = 'on'; S.gap = 0; sys('세일 시작!'); }
    const rate = S.sale === 'on' ? S.rate : 1;
    if (rate > 0) {
      S.gap -= dt;
      while (S.gap <= 0) { newRequest(); S.gap += -Math.log(1 - Math.random()) / rate; }   // 포아송 도착
    }
    healthT += dt; if (healthT >= 2) { healthT = 0; health(); }
    statusT += dt; if (statusT >= 0.5) { statusT = 0; emitStatus(); }
  }, 10);

  // --- 조작: 실제 제어 API(SPEC 6.3)와 같은 동작과 sys 문구 ---
  return {
    control(name, p) {
      if (name === 'sale') {
        if (S.sale === 'pre') { S.sale = 'count'; S.countStart = now(); sys('세일 카운트다운 시작 (5초)'); }
      } else if (name === 'rate') {
        const v = +p.value;
        if (Number.isInteger(v) && v >= 0 && v <= 100) { S.rate = v; S.gap = Math.min(S.gap, v > 0 ? -Math.log(1 - Math.random()) / v : 1); }
      } else if (name === 'proxy') {
        const on = +p.on === 1;
        if (on !== S.proxy) { S.proxy = on; sys(on ? 'Proxy 켜짐' : 'Proxy 꺼짐: 모든 요청을 상품 서버 1로 직결'); }
      } else if (name === 'cache') {
        const on = +p.on === 1;
        if (on !== S.cache) { S.cache = on; S.cT = {}; sys(on ? '캐시 켜짐 (TTL 10초)' : '캐시 꺼짐 (저장된 내용 비움)'); }
      } else if (name === 'server') {
        const i = IDX[p.id];
        if (i !== undefined && MODES[p.mode]) {
          setMode(i, p.mode);
          sys('서버 모드: ' + NAME[i] + ' → ' + MODES[p.mode], p.mode === 'normal' ? 'good' : 'bad');
        }
      } else if (name === 'reset') {
        // 실제 reset과 같게: 처리·대기 중인 요청은 그대로 두고 값만 시작 상태로
        SERVER_IDS.forEach((id, i) => { setMode(i, 'normal'); S.srv[i].done = 0; S.srv[i].ovl = false; S.excl[i] = false; S.fails[i] = 0; });
        S.sale = 'pre'; S.rate = 50; S.proxy = true; S.cache = false; S.cT = {}; S.rr = { products: 0, orders: 0 };
        sys('초기화');
      }
      emitStatus();   // 누른 결과가 바로 보이도록
    },
  };
}

// ===================================================================
// 14. 시작
// ===================================================================
const MOCK = new URLSearchParams(location.search).get('mock') === '1';
source = MOCK ? startMockSource() : startRealSource();
addLog({ sys: true, text: MOCK ? '모의 모드 · 서버 없이 화면 안에서 흉내 냅니다' : '대기 중 · 세일 시작을 눌러 보세요' });
requestAnimationFrame(loop);
