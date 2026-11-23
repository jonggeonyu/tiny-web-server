/* 정적 파일 (F6~F8). static은 C 키워드라 모듈 이름을 files로 했다.
 * 경로 해석과 MIME은 파일 시스템을 건드리지 않고 문자열만 다룬다. */
#ifndef FILES_H
#define FILES_H

#include <stddef.h>
#include <sys/types.h>

/* files_open이 연 정적 파일 */
typedef struct {
    int fd;                  /* 부른 쪽이 닫는다 */
    off_t size;              /* Content-Length로 보낼 바이트 수 */
    const char *mime_type;   /* files_find_mime_type 결과 */
} StaticFile;

/* 경로의 확장자로 Content-Type 값을 고른다 (대소문자 무시).
 * 모르는 확장자이거나 확장자가 없으면 "application/octet-stream". */
const char *files_find_mime_type(const char *path);

/* 요청 URI를 root 아래 파일 경로로 바꿔 out에 쓴다.
 * 쿼리(?…)와 조각(#…)은 떼고, %XX는 풀고, 푼 뒤에 ".." 조각이 있으면
 * 루트 밖으로 나갈 수 있으므로 거부한다.
 * 반환: 200, 잘못된 % 인코딩 또는 %00은 400, ".." 조각은 403,
 *       out이 모자라면 414 */
int files_resolve_path(const char *root, const char *uri,
                       char *out, size_t out_size);

/* files_resolve_path가 만든 path를 열어 file을 채운다.
 * 디렉터리면 path 뒤에 "/index.html"을 붙인다 (path 버퍼를 고친다).
 * root는 realpath로 정규화한 절대 경로여야 한다. 심볼릭 링크를 따라간
 * 실제 위치가 root 밖이면 403.
 * 반환: 200, 권한 없음·일반 파일 아님·루트 밖 403, 없음 404,
 *       경로가 너무 김 414, 그 밖의 실패 500 (errno에 원인이 남아 있다).
 * 200이면 file->fd는 부른 쪽이 닫는다. */
int files_open(const char *root, char *path, size_t path_size,
               StaticFile *file);

/* CGI(F10) 프로그램 경로를 확인한다. path는 files_resolve_path 결과.
 * 심볼릭 링크를 따라간 실제 위치가 root 안이고, 일반 파일이고, 실행 권한이
 * 있어야 한다. 통과하면 path를 그 실제 경로로 바꾼다 (path 버퍼를 고친다).
 * 반환: 200, 루트 밖·일반 파일 아님·실행 권한 없음 403, 없음 404,
 *       경로가 너무 김 414, 그 밖의 실패 500 */
int files_resolve_program(const char *root, char *path, size_t path_size);

#endif
