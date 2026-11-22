/*
 * mime: 파일 확장자 → Content-Type (MIME 타입)
 *   "index.html" → "text/html; charset=utf-8"
 *   "photo.JPG"  → "image/jpeg"   (확장자 대소문자 무시)
 *   모르는 확장자 / 확장자 없음 → "application/octet-stream" (= 그냥 바이트 덩어리)
 */
#ifndef MIME_H
#define MIME_H

#define MIME_DEFAULT "application/octet-stream"

/* path: 파일 경로 또는 이름. 반환값은 정적 문자열 (free 금지) */
const char *mime_type_for(const char *path);

#endif /* MIME_H */
