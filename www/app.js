// app.js 가 text/javascript 로 전달되어 실행되면 상태 표시가 바뀐다.
// (nosniff 헤더 때문에 Content-Type 이 틀리면 브라우저가 실행을 거부한다)
document.getElementById("js-status").textContent = "(실행됨 ✔)";
document.getElementById("js-status").className = "ok";
