/* deck sharing: WinHTTP client + proof of work, run on a worker thread */
#include "app.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

#define POW_BITS 20

NetJob net_job;
static HWND notify_hwnd;
static UINT notify_msg;

static void sha256(BCRYPT_ALG_HANDLE alg, const void *data, ULONG len, unsigned char out[32]) {
  BCryptHash(alg, NULL, 0, (PUCHAR)data, len, out, 32);
}

static int zero_bits(const unsigned char *h) {
  int n = 0, i;
  for (i = 0; i < 32; i++) {
    unsigned char x = h[i];
    if (x == 0) { n += 8; continue; }
    while (!(x & 0x80)) { n++; x <<= 1; }
    break;
  }
  return n;
}

/* find nonce so sha256(hex(sha256(body)) + ":" + nonce) has POW_BITS leading zeros */
static unsigned long long proof_of_work(const char *body) {
  BCRYPT_ALG_HANDLE alg;
  unsigned char h[32];
  char buf[96];
  int i, prefix;
  unsigned long long nonce;
  BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
  sha256(alg, body, (ULONG)strlen(body), h);
  for (i = 0; i < 32; i++) { sprintf(buf + i * 2, "%02x", h[i]); }
  buf[64] = ':';
  prefix = 65;
  for (nonce = 0; ; nonce++) {
    int n = prefix + sprintf(buf + prefix, "%llu", nonce);
    sha256(alg, buf, n, h);
    if (zero_bits(h) >= POW_BITS) { break; }
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return nonce;
}

/* returns http status, or 0 on connection failure; *resp is malloc'd */
static int http(const char *method, const char *url, const char *body, const wchar_t *headers, char **resp) {
  wchar_t wurl[1024], host[256], path[768];
  URL_COMPONENTS uc;
  HINTERNET ses = NULL, con = NULL, req = NULL;
  DWORD status = 0, sz = sizeof(status), avail;
  wchar_t wmethod[8];
  size_t len = 0;
  *resp = NULL;

  utf8_to_wide(url, -1, wurl, 1024);
  utf8_to_wide(method, -1, wmethod, 8);
  memset(&uc, 0, sizeof(uc));
  uc.dwStructSize = sizeof(uc);
  uc.lpszHostName = host; uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path; uc.dwUrlPathLength = 768;
  if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) { return 0; }

  ses = WinHttpOpen(L"WaitWhat/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
  if (ses) { con = WinHttpConnect(ses, host, uc.nPort, 0); }
  if (con) {
    req = WinHttpOpenRequest(con, wmethod, path, NULL, NULL, NULL,
      uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  }
  if (req && WinHttpSendRequest(req, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, (DWORD)-1L,
        (void*)body, body ? (DWORD)strlen(body) : 0, body ? (DWORD)strlen(body) : 0, 0)
      && WinHttpReceiveResponse(req, NULL)) {
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      NULL, &status, &sz, NULL);
    *resp = malloc(1);
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0 && len < 1024 * 1024) {
      DWORD got = 0;
      *resp = realloc(*resp, len + avail + 1);
      WinHttpReadData(req, *resp + len, avail, &got);
      len += got;
    }
    (*resp)[len] = 0;
  }
  if (req) { WinHttpCloseHandle(req); }
  if (con) { WinHttpCloseHandle(con); }
  if (ses) { WinHttpCloseHandle(ses); }
  return (int)status;
}

static void server_url(char *out, int cap, const char *path) {
  int n = snprintf(out, cap, "%s", settings.server);
  while (n > 0 && out[n - 1] == '/') { out[--n] = 0; }
  snprintf(out + n, cap - n, "%s", path);
}

static void error_from(const char *resp, int status) {
  cJSON *j = resp ? cJSON_Parse(resp) : NULL;
  cJSON *e = cJSON_GetObjectItem(j, "error");
  if (cJSON_IsString(e)) { snprintf(net_job.msg, sizeof(net_job.msg), "%s", e->valuestring); }
  else if (status == 0) { snprintf(net_job.msg, sizeof(net_job.msg), "Couldn't reach the server."); }
  else { snprintf(net_job.msg, sizeof(net_job.msg), "Server error (%d).", status); }
  cJSON_Delete(j);
}

static DWORD WINAPI worker(void *arg) {
  char url[512], *resp = NULL;
  int status;
  (void)arg;
  if (net_job.kind == NET_SHARE) {
    wchar_t hdr[128];
    swprintf(hdr, 128, L"Content-Type: application/json\r\nX-Nonce: %llu\r\n", proof_of_work(net_job.body));
    server_url(url, sizeof(url), "/d");
    status = http("POST", url, net_job.body, hdr, &resp);
    if (status == 200) {
      cJSON *j = cJSON_Parse(resp), *c = cJSON_GetObjectItem(j, "code");
      if (cJSON_IsString(c)) {
        snprintf(net_job.code, sizeof(net_job.code), "%s", c->valuestring);
        net_job.ok = 1;
      } else {
        error_from(resp, status);
      }
      cJSON_Delete(j);
    } else {
      error_from(resp, status);
    }
    free(resp);
  } else {
    char path[32];
    snprintf(path, sizeof(path), "/d/%s", net_job.code);
    server_url(url, sizeof(url), path);
    status = http("GET", url, NULL, NULL, &resp);
    if (status == 200 && resp) {
      net_job.result = resp; /* handed to the main thread */
      net_job.ok = 1;
    } else {
      error_from(resp, status);
      free(resp);
    }
  }
  net_job.running = 0;
  PostMessageW(notify_hwnd, notify_msg, 0, 0);
  return 0;
}

void net_set_notify(HWND hwnd, UINT msg) {
  notify_hwnd = hwnd;
  notify_msg = msg;
}

int net_start(int kind, const char *arg) {
  HANDLE t;
  if (net_job.running) { return 0; }
  free(net_job.body);
  free(net_job.result);
  memset(&net_job, 0, sizeof(net_job));
  net_job.kind = kind;
  if (!settings.server[0]) {
    snprintf(net_job.msg, sizeof(net_job.msg), "Set a sharing server in Settings first.");
    return 0;
  }
  if (kind == NET_SHARE) {
    net_job.body = _strdup(arg);
  } else {
    /* codes are lowercase base32; be forgiving about spaces and case */
    int j = 0;
    for (; *arg && j < (int)sizeof(net_job.code) - 1; arg++) {
      char c = *arg;
      if (c >= 'A' && c <= 'Z') { c = c - 'A' + 'a'; }
      if ((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7')) { net_job.code[j++] = c; }
    }
    if (j != 8) {
      snprintf(net_job.msg, sizeof(net_job.msg), "Codes are 8 letters/digits, like wk7f2qxa.");
      return 0;
    }
  }
  net_job.running = 1;
  t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
  if (!t) { net_job.running = 0; return 0; }
  CloseHandle(t);
  return 1;
}
