/*
 * nfcwatcher — macOS 한글 파일명 NFD→NFC 자동 변환 데몬
 *
 * 목적: macOS(Finder/앱)가 만드는 분해형(NFD) 한글 파일명을 Windows 호환의
 *       완성형(NFC)으로 자동 변환한다. APFS는 우리가 rename으로 준 바이트를
 *       보존하므로 NFC 이름이 그대로 유지된다.
 *
 * 특징: FSEvents 이벤트 기반 → 유휴 시 CPU 0%. 단일 디바운스 타이머는
 *       처리할 항목이 없으면 suspend 되어 깨어나지 않는다. 메모리 사용 최소.
 *
 * 빌드: cc -O2 -o nfcwatcher src/nfcwatcher.c -framework CoreServices
 */

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>
#include <stdbool.h>
#include <signal.h>
#include <ftw.h>
#include <stdarg.h>
#include <errno.h>

#define VERSION "1.0.0"

/* ----------------------------- 설정 ----------------------------- */

typedef enum { CONFLICT_SKIP, CONFLICT_RENAME, CONFLICT_OVERWRITE } conflict_t;

typedef struct {
    char   **watch;            /* 감시 폴더 절대경로 배열 */
    int      watch_count;
    double   debounce_seconds; /* 이벤트 후 안정화 대기 (쓰기 중 파일 보호) */
    double   latency_seconds;  /* FSEvents 배치 지연 */
    char   **ignore_ext;       /* 무시 확장자(소문자) */
    int      ignore_ext_count;
    bool     ignore_hidden;
    conflict_t conflict;
    char    *log_path;
    bool     undo_log;
} config_t;

static config_t   g_cfg;
static FILE      *g_log = NULL;
static bool       g_dry_run = false;    /* --dry-run: 세기만 하고 rename 안 함 */
static char      *g_pause_flag = NULL;  /* 이 파일이 있으면 데몬은 변환을 멈춤 */
static char     **g_scan_roots = NULL;  /* --scan 에 넘긴 특정 폴더(들) */
static int        g_scan_roots_count = 0;

/* ----------------------------- 유틸 ----------------------------- */

static char *expand_tilde(const char *p) {
    if (p && p[0] == '~' && (p[1] == '/' || p[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home) home = "";
        size_t n = strlen(home) + strlen(p);
        char *out = malloc(n + 1);
        snprintf(out, n + 1, "%s%s", home, p + 1);
        return out;
    }
    return strdup(p);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}

static void logmsg(const char *level, const char *fmt, ...) {
    if (!g_log) return;
    char ts[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(g_log, "%s [%s] ", ts, level);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* 파일명(basename)을 NFC UTF-8 문자열로 변환. 반환은 malloc, 호출자 free.
 * 이미 NFC거나 변환 불가면 NULL 반환(= 변경 불필요). */
static char *to_nfc(const char *name) {
    CFStringRef in = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    if (!in) return NULL;
    CFMutableStringRef m = CFStringCreateMutableCopy(NULL, 0, in);
    CFRelease(in);
    if (!m) return NULL;
    CFStringNormalize(m, kCFStringNormalizationFormC);
    CFIndex maxlen = CFStringGetMaximumSizeForEncoding(
                         CFStringGetLength(m), kCFStringEncodingUTF8) + 1;
    char *buf = malloc(maxlen);
    if (!buf) { CFRelease(m); return NULL; }
    if (!CFStringGetCString(m, buf, maxlen, kCFStringEncodingUTF8)) {
        free(buf); CFRelease(m); return NULL;
    }
    CFRelease(m);
    if (strcmp(buf, name) == 0) { free(buf); return NULL; } /* 이미 NFC */
    return buf;
}

/* ------------------------- 핵심 변환 로직 ------------------------- */

/* 경로 하나를 필요 시 NFC로 rename. 변환했으면 true. */
static bool normalize_path(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return false;   /* 이미 사라짐 */

    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1 : path;
    size_t dirlen = slash ? (size_t)(slash - path) : 0;

    if (g_cfg.ignore_hidden && name[0] == '.') return false;

    /* 무시 확장자 검사 */
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        char ext[64]; size_t k = 0;
        for (const char *c = dot + 1; *c && k < sizeof(ext) - 1; c++)
            ext[k++] = (char)tolower((unsigned char)*c);
        ext[k] = 0;
        for (int i = 0; i < g_cfg.ignore_ext_count; i++)
            if (strcmp(ext, g_cfg.ignore_ext[i]) == 0) return false;
    }

    char *nfc = to_nfc(name);
    if (!nfc) return false;   /* 이미 NFC */

    /* 대상 경로 조립 */
    size_t tlen = dirlen + 1 + strlen(nfc) + 1;
    char *target = malloc(tlen);
    if (dirlen) snprintf(target, tlen, "%.*s/%s", (int)dirlen, path, nfc);
    else        snprintf(target, tlen, "%s", nfc);
    free(nfc);

    /* 충돌 처리.
     * 주의: APFS는 정규화 비민감(normalization-insensitive)이라 NFC 이름으로
     * stat 하면 원본 NFD 파일이 잡힌다. inode(dev+ino)가 원본과 같으면 같은
     * 파일이므로 충돌이 아니다 — rename으로 저장 바이트만 NFC로 바꾸면 된다. */
    struct stat tst;
    bool real_conflict = false;
    if (lstat(target, &tst) == 0) {
        if (!(tst.st_dev == st.st_dev && tst.st_ino == st.st_ino))
            real_conflict = true;   /* 다른 파일 → 진짜 충돌 */
    }
    if (real_conflict) {
        switch (g_cfg.conflict) {
        case CONFLICT_SKIP:
            logmsg("WARN", "SKIP 충돌: %s (대상 존재)", path);
            free(target); return false;
        case CONFLICT_OVERWRITE:
            break; /* rename이 덮어씀 */
        case CONFLICT_RENAME: {
            char *base = strdup(target);
            char *bdot = strrchr(base, '.');
            char *bslash = strrchr(base, '/');
            if (bdot && (!bslash || bdot > bslash)) *bdot = 0;
            const char *ext = (bdot && (!bslash || bdot > bslash)) ? bdot + 1 : "";
            char *cand = malloc(tlen + 16);
            for (int i = 1; ; i++) {
                if (ext[0]) snprintf(cand, tlen + 16, "%s (%d).%s", base, i, ext);
                else        snprintf(cand, tlen + 16, "%s (%d)", base, i);
                if (access(cand, F_OK) != 0) break;
            }
            free(base);
            free(target);
            target = cand;
            break;
        }
        }
    }

    if (g_dry_run) {   /* 미리보기: 실제로는 바꾸지 않고 '변환 대상'으로만 집계 */
        free(target);
        return true;
    }

    if (rename(path, target) == 0) {
        logmsg("INFO", "변환: %s -> %s", name, strrchr(target, '/') ? strrchr(target, '/') + 1 : target);
        if (g_cfg.undo_log) logmsg("UNDO", "%s\t%s", target, path);
        free(target);
        return true;
    } else {
        logmsg("ERROR", "실패 rename %s -> %s: %s", path, target, strerror(errno));
        free(target);
        return false;
    }
}

/* --------------------- 디바운스 (단일 타이머) --------------------- */

typedef struct { char *path; double due; } pend_t;

static dispatch_queue_t  g_q;
static dispatch_source_t g_timer;
static bool              g_timer_running = false;
static pend_t           *g_pend = NULL;
static int               g_pend_len = 0, g_pend_cap = 0;

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* q 컨텍스트에서만 호출됨 */
static void pend_add(const char *path) {
    double due = now_sec() + g_cfg.debounce_seconds;
    for (int i = 0; i < g_pend_len; i++)          /* 중복이면 갱신 */
        if (strcmp(g_pend[i].path, path) == 0) { g_pend[i].due = due; return; }
    if (g_pend_len == g_pend_cap) {
        g_pend_cap = g_pend_cap ? g_pend_cap * 2 : 16;
        g_pend = realloc(g_pend, sizeof(pend_t) * g_pend_cap);
    }
    g_pend[g_pend_len].path = strdup(path);
    g_pend[g_pend_len].due  = due;
    g_pend_len++;
    if (!g_timer_running) {           /* 대기열 생기면 타이머 가동 */
        dispatch_resume(g_timer);
        g_timer_running = true;
    }
}

static void timer_fire(void) {
    double now = now_sec();
    /* 일시정지 상태면 변환하지 않고 대기 항목을 비운다 (자동 변환 Off). */
    bool paused = g_pause_flag && access(g_pause_flag, F_OK) == 0;
    for (int i = 0; i < g_pend_len; ) {
        if (g_pend[i].due <= now) {
            if (!paused) normalize_path(g_pend[i].path);
            free(g_pend[i].path);
            g_pend[i] = g_pend[--g_pend_len];   /* 마지막을 당겨 제거 */
        } else i++;
    }
    if (g_pend_len == 0 && g_timer_running) {   /* 비면 타이머 정지 → CPU 0 */
        dispatch_suspend(g_timer);
        g_timer_running = false;
    }
}

/* ------------------------- FSEvents 콜백 ------------------------- */

static void fsevent_cb(ConstFSEventStreamRef stream, void *info,
                       size_t n, void *paths,
                       const FSEventStreamEventFlags flags[],
                       const FSEventStreamEventId ids[]) {
    (void)stream; (void)info; (void)flags; (void)ids;
    char **pp = (char **)paths;
    for (size_t i = 0; i < n; i++) pend_add(pp[i]);
}

/* --------------------------- 설정 로드 --------------------------- */

static void add_watch(config_t *c, const char *p) {
    c->watch = realloc(c->watch, sizeof(char *) * (c->watch_count + 1));
    c->watch[c->watch_count++] = expand_tilde(p);
}
static void add_ext(config_t *c, const char *e) {
    c->ignore_ext = realloc(c->ignore_ext, sizeof(char *) * (c->ignore_ext_count + 1));
    char *low = strdup(e);
    for (char *q = low; *q; q++) *q = (char)tolower((unsigned char)*q);
    c->ignore_ext[c->ignore_ext_count++] = low;
}

static void set_defaults(config_t *c) {
    memset(c, 0, sizeof *c);
    add_watch(c, "~/Downloads");
    add_watch(c, "~/Desktop");
    c->debounce_seconds = 2.0;
    c->latency_seconds  = 1.0;
    const char *ex[] = {"tmp","part","crdownload","download","partial"};
    for (int i = 0; i < 5; i++) add_ext(c, ex[i]);
    c->ignore_hidden = true;
    c->conflict = CONFLICT_SKIP;
    c->log_path = expand_tilde("~/Library/Logs/nfcwatcher.log");
    c->undo_log = true;
}

static void write_default_config(const char *path) {
    char *p = expand_tilde(path);
    /* 상위 디렉토리 생성 */
    char *dir = strdup(p);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdir(dir, 0755); }
    free(dir);
    FILE *f = fopen(p, "w");
    if (f) {
        fprintf(f,
          "# nfcwatcher 설정 파일\n"
          "# 감시 폴더 (여러 줄 가능). ~ 는 홈으로 확장됨.\n"
          "watch = ~/Downloads\n"
          "watch = ~/Desktop\n\n"
          "# 이벤트 후 안정화 대기(초). 복사 중 파일을 건드리지 않도록.\n"
          "debounce_seconds = 2.0\n"
          "# FSEvents 배치 지연(초). 클수록 깨어남이 줄어 CPU 절약.\n"
          "latency_seconds = 1.0\n\n"
          "# 무시할 확장자 (공백 구분, 점 제외)\n"
          "ignore_ext = tmp part crdownload download partial\n"
          "# 숨김 파일(.으로 시작) 무시\n"
          "ignore_hidden = true\n\n"
          "# 이름 충돌 시: skip | rename | overwrite\n"
          "conflict = skip\n\n"
          "# 로그 파일\n"
          "log = ~/Library/Logs/nfcwatcher.log\n"
          "# 변환 내역을 UNDO 매핑으로 기록\n"
          "undo_log = true\n");
        fclose(f);
    }
    free(p);
}

static void load_config(const char *path) {
    set_defaults(&g_cfg);
    char *p = expand_tilde(path);

    /* 일시정지 플래그 경로 = 설정 파일과 같은 디렉토리의 "paused".
     * 메뉴바 앱이 이 파일을 만들거나 지워 자동 변환을 On/Off 한다. */
    {
        char *dir = strdup(p);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0; else strcpy(dir, ".");
        size_t n = strlen(dir) + strlen("/paused") + 1;
        g_pause_flag = malloc(n);
        snprintf(g_pause_flag, n, "%s/paused", dir);
        free(dir);
    }

    FILE *f = fopen(p, "r");
    if (!f) {                       /* 없으면 기본 설정 파일 생성 후 기본값 사용 */
        write_default_config(path);
        free(p);
        return;
    }
    /* 파일이 있으니 설정 초기화 후 파싱 */
    /* watch/ext는 파일 내용으로 대체하기 위해 리스트를 비운다 */
    for (int i = 0; i < g_cfg.watch_count; i++) free(g_cfg.watch[i]);
    free(g_cfg.watch); g_cfg.watch = NULL; g_cfg.watch_count = 0;
    for (int i = 0; i < g_cfg.ignore_ext_count; i++) free(g_cfg.ignore_ext[i]);
    free(g_cfg.ignore_ext); g_cfg.ignore_ext = NULL; g_cfg.ignore_ext_count = 0;

    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (*s == 0 || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq + 1);
        if      (strcmp(key, "watch") == 0)            add_watch(&g_cfg, val);
        else if (strcmp(key, "debounce_seconds") == 0) g_cfg.debounce_seconds = atof(val);
        else if (strcmp(key, "latency_seconds") == 0)  g_cfg.latency_seconds = atof(val);
        else if (strcmp(key, "ignore_hidden") == 0)    g_cfg.ignore_hidden = (strcmp(val,"true")==0);
        else if (strcmp(key, "undo_log") == 0)         g_cfg.undo_log = (strcmp(val,"true")==0);
        else if (strcmp(key, "log") == 0)            { free(g_cfg.log_path); g_cfg.log_path = expand_tilde(val); }
        else if (strcmp(key, "conflict") == 0) {
            if      (strcmp(val,"rename")==0)    g_cfg.conflict = CONFLICT_RENAME;
            else if (strcmp(val,"overwrite")==0) g_cfg.conflict = CONFLICT_OVERWRITE;
            else                                 g_cfg.conflict = CONFLICT_SKIP;
        }
        else if (strcmp(key, "ignore_ext") == 0) {
            char *tok = strtok(val, " \t,");
            while (tok) { add_ext(&g_cfg, tok); tok = strtok(NULL, " \t,"); }
        }
    }
    fclose(f);
    if (g_cfg.watch_count == 0) { add_watch(&g_cfg, "~/Downloads"); add_watch(&g_cfg, "~/Desktop"); }
    free(p);
}

/* ---------------------------- 스캔 모드 ---------------------------- */

static char **g_scan = NULL;
static int    g_scan_len = 0, g_scan_cap = 0;

static int scan_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftw) {
    (void)sb; (void)typeflag; (void)ftw;
    if (g_scan_len == g_scan_cap) {
        g_scan_cap = g_scan_cap ? g_scan_cap * 2 : 256;
        g_scan = realloc(g_scan, sizeof(char *) * g_scan_cap);
    }
    g_scan[g_scan_len++] = strdup(fpath);
    return 0;
}
static int by_len_desc(const void *a, const void *b) {
    size_t la = strlen(*(char *const *)a), lb = strlen(*(char *const *)b);
    return (la < lb) - (la > lb);   /* 긴 경로(깊은 곳) 먼저 */
}

static int run_scan(void) {
    int changed = 0;
    /* --scan 에 폴더를 직접 지정했으면 그걸, 아니면 설정의 감시 폴더를 스캔 */
    char **roots = g_scan_roots_count ? g_scan_roots : g_cfg.watch;
    int    nroots = g_scan_roots_count ? g_scan_roots_count : g_cfg.watch_count;
    for (int i = 0; i < nroots; i++)
        nftw(roots[i], scan_cb, 32, FTW_PHYS);
    qsort(g_scan, g_scan_len, sizeof(char *), by_len_desc);
    if (!g_dry_run) logmsg("INFO", "일괄 스캔 시작: %d개 항목", g_scan_len);
    for (int i = 0; i < g_scan_len; i++) {
        if (normalize_path(g_scan[i])) changed++;
        free(g_scan[i]);
    }
    free(g_scan);
    if (g_dry_run) {
        /* 메뉴바 앱이 파싱하는 기계 판독용 한 줄 + 사람이 읽는 줄 */
        printf("count=%d\n", changed);
        printf("변환 대상: %d개\n", changed);
    } else {
        logmsg("INFO", "일괄 스캔 완료: %d개 변환", changed);
        printf("count=%d\n", changed);
        printf("완료: %d개 파일을 NFC로 변환했습니다. 로그: %s\n", changed, g_cfg.log_path);
    }
    return changed;
}

/* ------------------------------ 종료 ------------------------------ */

static FSEventStreamRef g_stream = NULL;
static void on_term(void) {
    logmsg("INFO", "종료 신호 수신, 정리");
    if (g_stream) { FSEventStreamStop(g_stream); FSEventStreamInvalidate(g_stream); FSEventStreamRelease(g_stream); }
    if (g_log) fflush(g_log);
    _exit(0);
}

/* ------------------------------ main ------------------------------ */

static void usage(void) {
    printf(
      "nfcwatcher %s — macOS 한글 파일명 NFD→NFC 자동 변환 데몬\n\n"
      "사용법:\n"
      "  nfcwatcher [--config <path>]              데몬 실행 (폴더 상시 감시)\n"
      "  nfcwatcher --scan [--dry-run] [폴더...]   일괄 변환. 폴더 미지정 시 설정의 감시 폴더\n"
      "                                            --dry-run 은 세기만 하고 바꾸지 않음\n"
      "  nfcwatcher --version | --help\n\n"
      "기본 설정: ~/.config/nfcwatcher/nfcwatcher.conf (없으면 자동 생성)\n",
      VERSION);
}

int main(int argc, char **argv) {
    const char *cfg_path = "~/.config/nfcwatcher/nfcwatcher.conf";
    bool scan = false;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) { printf("nfcwatcher %s\n", VERSION); return 0; }
        else if (!strcmp(argv[i], "--scan")) scan = true;
        else if (!strcmp(argv[i], "--dry-run")) g_dry_run = true;
        else if (!strcmp(argv[i], "--config") || !strcmp(argv[i], "-c")) {
            if (++i >= argc) { fprintf(stderr, "--config 뒤에 경로 필요\n"); return 2; }
            cfg_path = argv[i];
        }
        else if (argv[i][0] == '-') { fprintf(stderr, "알 수 없는 인자: %s\n", argv[i]); usage(); return 2; }
        else {  /* 위치 인자 = 스캔할 특정 폴더 */
            g_scan_roots = realloc(g_scan_roots, sizeof(char *) * (g_scan_roots_count + 1));
            g_scan_roots[g_scan_roots_count++] = expand_tilde(argv[i]);
        }
    }
    if (g_scan_roots_count > 0) scan = true;   /* 폴더를 주면 스캔 모드로 간주 */

    load_config(cfg_path);
    g_log = fopen(g_cfg.log_path, "a");
    if (!g_log) g_log = stderr;
    if (g_dry_run) { fclose(g_log); g_log = fopen("/dev/null", "w"); }  /* 미리보기는 로그 안 남김 */

    if (scan) return run_scan() >= 0 ? 0 : 1;

    /* 감시 대상 유효성 검사 */
    CFMutableArrayRef paths = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (int i = 0; i < g_cfg.watch_count; i++) {
        struct stat st;
        if (stat(g_cfg.watch[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            CFStringRef s = CFStringCreateWithCString(NULL, g_cfg.watch[i], kCFStringEncodingUTF8);
            CFArrayAppendValue(paths, s);
            CFRelease(s);
        } else {
            logmsg("WARN", "감시 대상 없음/폴더 아님, 건너뜀: %s", g_cfg.watch[i]);
        }
    }
    if (CFArrayGetCount(paths) == 0) {
        logmsg("ERROR", "감시할 유효한 폴더가 없습니다. 설정 확인 요망.");
        return 1;
    }

    g_q = dispatch_queue_create("nfcwatcher.q", DISPATCH_QUEUE_SERIAL);

    /* 디바운스 타이머: 0.5초 간격, 초기 suspend 상태 */
    g_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_q);
    dispatch_source_set_timer(g_timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                              (uint64_t)(0.5 * NSEC_PER_SEC), (uint64_t)(0.2 * NSEC_PER_SEC));
    dispatch_source_set_event_handler_f(g_timer, (dispatch_function_t)timer_fire);
    /* 생성 직후 suspend 상태로 두기 위해 아직 resume 하지 않음 */

    FSEventStreamContext ctx = {0, NULL, NULL, NULL, NULL};
    FSEventStreamCreateFlags flags =
        kFSEventStreamCreateFlagFileEvents |
        kFSEventStreamCreateFlagNoDefer    |
        kFSEventStreamCreateFlagIgnoreSelf |
        kFSEventStreamCreateFlagWatchRoot;

    g_stream = FSEventStreamCreate(NULL, fsevent_cb, &ctx, paths,
                                   kFSEventStreamEventIdSinceNow,
                                   g_cfg.latency_seconds, flags);
    CFRelease(paths);
    if (!g_stream) { logmsg("ERROR", "FSEventStream 생성 실패"); return 1; }
    FSEventStreamSetDispatchQueue(g_stream, g_q);
    if (!FSEventStreamStart(g_stream)) { logmsg("ERROR", "FSEventStream 시작 실패"); return 1; }

    /* 시그널 처리 (launchd는 SIGTERM 전송) */
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT,  SIG_IGN);
    dispatch_source_t st1 = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_t st2 = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT,  0, dispatch_get_main_queue());
    dispatch_source_set_event_handler_f(st1, (dispatch_function_t)on_term);
    dispatch_source_set_event_handler_f(st2, (dispatch_function_t)on_term);
    dispatch_resume(st1); dispatch_resume(st2);

    logmsg("INFO", "nfcwatcher %s 데몬 실행 중", VERSION);
    dispatch_main();
    return 0;
}
