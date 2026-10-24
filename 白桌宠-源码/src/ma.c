/* ma.c —— 桌宠多智能体协作引擎（《多智能体协作系统完整设计方案》阶段1：主管+员工A 最小闭环）
 *
 * 编译: gcc -O2 -o ma.exe ma.c -lwinhttp
 *
 * 协议: 与 inference_backend.exe 完全一致 —— pet.c 的 local_pipe_thread 零改动复用
 *   stdin: "<max_tokens> <temp_x100>\n<prompt_bytes>\n<UTF-8 prompt>"
 *   stdout: 进度行 + 最终交付（UTF-8，分块即时 flush，桌宠气泡流式显示）
 *   stderr: 调试（桌宠重定向到 pet_child_err.log）
 *
 * 架构映射（文档 → 本实现）:
 *   星型拓扑      : 本进程内主管逻辑唯一路由；员工A = 一次 LLM 角色扮演调用
 *   任务卡 JSON    : 主管 LLM 产出，解析后落盘 blackboard\tasks\T-xxxx.json
 *   共享黑板      : exe 目录\blackboard\（tasks\ artifacts\ board.log）
 *   三级验收      : L1 文件存在+非空(纯代码,0 token) / L2 判卷LLM逐条验收标准 / L3 质检LLM
 *   打回上限2次    : attempts 计数，超限主管改写一次，再失败记入最终上报
 *   拓扑排序      : Kahn 分阶段（阶段1 串行执行，依赖检查照做）
 *   桌宠呈现      : 进度行【主管】【员工A】【质检】即文档 1.6 的"派发可视化"文字版
 */
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <io.h>
#include <fcntl.h>
#include "agent_config.h"
#include "compat.h"

#define DEF_MODEL     "qwen3.8-flash"

/* 构建号：只用于编译期改哈希（Smart App Control 会拦无签名新 exe，换哈希可放行），
   运行时打在 stderr 第一行，便于确认跑的是哪一版 */
#ifndef MA_TAG_NUM
#define MA_TAG_NUM 0
#endif

/* 端点运行时解析（PET_LLM_BASE / pet.exe --base），解析器在 compat.h。
   原来 API_HOST/API_PATH 是写死的宏，只有本地管道能换服务商。
   双端点：g_ep = 员工A（干活，可指向本机 Ollama）；g_mgr_ep = 主管（拆解/判卷/汇总，
   可单独指向强模型）。MA_MANAGER_* 不配时整体沿用员工A，行为和旧版一致。 */
static PetEndpoint g_ep;
static PetEndpoint g_mgr_ep;

/* 角色配置唯一真源 = agent_config.h 的 g_role_tbl；llm() 按它记调用与上下文，
 * B/C enabled=0 由 main 外层闸门挡下（阶段2 再接线线程） */
static AgentConfig g_cfg;
static const char *g_model = DEF_MODEL;      /* PET_LLM_MODEL：员工A 模型 */
static const char *g_mgr_model = DEF_MODEL;  /* MA_MANAGER_MODEL：主管模型 */
#define MAX_TASKS     12
#define MAX_CRIT      8
#define MAX_OUT       8
#define MAX_DEP       8

static void dbg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

/* ---------------- 动态字符串 ---------------- */
typedef struct { char *p; int len, cap; } SB;
static void sb_grow(SB *s, int need) {
    if (s->len + need <= s->cap)
        return;
    while (s->len + need > s->cap)
        s->cap = s->cap ? s->cap * 2 : 4096;
    s->p = (char *)realloc(s->p, (size_t)s->cap);
}
static void sb_put(SB *s, const char *p, int n) {
    sb_grow(s, n);
    memcpy(s->p + s->len, p, (size_t)n);
    s->len += n;
    s->p[s->len] = 0;
}
static void sb_puts(SB *s, const char *p) { sb_put(s, p, (int)strlen(p)); }
static void sb_printf(SB *s, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
        sb_put(s, buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}

/* ---------------- stdout 流式 ---------------- */
static void emit(const char *s, int n) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD wr;
    if (n < 0)
        n = (int)strlen(s);
    if (h != INVALID_HANDLE_VALUE && h != NULL)
        WriteFile(h, s, (DWORD)n, &wr, NULL);
}
static void emitf(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
        emit(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}

/* ---------------- 文件系统 ---------------- */
static char *read_file_a(const char *path, int *len_out) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 4 << 20) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    fclose(f);
    if (len_out)
        *len_out = (int)n;
    return buf;
}

static void wpath_join(WCHAR *full, int cap, const WCHAR *dir, const char *rel) {
    WCHAR wrel[1024];
    int i;
    MultiByteToWideChar(CP_UTF8, 0, rel, -1, wrel, 1024);
    for (i = 0; wrel[i]; i++)
        if (wrel[i] == L'/')
            wrel[i] = L'\\';
    lstrcpynW(full, dir, cap);
    lstrcatW(full, wrel);
}

static void mkdir_parents(WCHAR *full) {
    WCHAR tmp[2048];
    int i, n = (int)lstrlenW(full);
    if (n >= (int)(sizeof tmp / sizeof tmp[0]))
        return;
    lstrcpyW(tmp, full);
    for (i = 0; i < n; i++) {
        if (tmp[i] == L'\\' && i > 2) {
            tmp[i] = 0;
            CreateDirectoryW(tmp, NULL);
            tmp[i] = L'\\';
        }
    }
}

static int write_file_w(WCHAR *full, const char *data, int len) {
    FILE *f;
    WCHAR path[2048];
    lstrcpyW(path, full);
    mkdir_parents(path);
    f = _wfopen(path, L"wb");
    if (!f)
        return 0;
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    return 1;
}

/* ---------------- 最小 JSON 解析 ---------------- */
typedef struct JVal {
    int type;              /* 0 obj 1 arr 2 str 3 num/bool/null */
    char *key;
    char *s;
    struct JVal *child, *next;
} JVal;

static void jskip(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
        (*p)++;
}

static int hex4(const char *s, unsigned *o) {
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *o = v;
    return 1;
}

static void utf8_put(unsigned cp, char *out, int *n) {
    if (cp < 0x80) out[(*n)++] = (char)cp;
    else if (cp < 0x800) { out[(*n)++] = (char)(0xC0 | cp >> 6); out[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out[(*n)++] = (char)(0xE0 | cp >> 12); out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
    else { out[(*n)++] = (char)(0xF0 | cp >> 18); out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
}

static char *parse_string(const char **p) {
    const char *s = *p + 1;
    char *out = (char *)malloc(strlen(s) + 1);
    int n = 0;
    while (*s && *s != '"') {
        if (*s == '\\') {
            s++;
            switch (*s) {
            case 'n': out[n++] = '\n'; s++; break;
            case 'r': out[n++] = '\r'; s++; break;
            case 't': out[n++] = '\t'; s++; break;
            case 'b': out[n++] = '\b'; s++; break;
            case 'f': out[n++] = '\f'; s++; break;
            case 'u': {
                unsigned cp;
                if (hex4(s + 1, &cp)) {
                    s += 5;
                    if (cp >= 0xD800 && cp <= 0xDBFF && s[0] == '\\' && s[1] == 'u') {
                        unsigned lo;
                        if (hex4(s + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            s += 6;
                        }
                    }
                    utf8_put(cp, out, &n);
                } else s++;
                break;
            }
            default: out[n++] = *s; s++; break;   /* \" \\ \/ 与其它 */
            }
        } else out[n++] = *s++;
    }
    out[n] = 0;
    if (*s == '"')
        s++;
    *p = s;
    return out;
}

static JVal *jnew(int t) {
    JVal *v = (JVal *)calloc(1, sizeof(JVal));
    v->type = t;
    return v;
}

static JVal *parse_val(const char **p) {
    JVal *v;
    const char *s;
    jskip(p);
    if (**p == '{') {
        v = jnew(0);
        (*p)++;
        for (;;) {
            JVal *m, **tail;
            jskip(p);
            if (**p == '}') { (*p)++; break; }
            if (**p != '"') break;
            m = jnew(2);
            m->key = parse_string(p);
            jskip(p);
            if (**p == ':') (*p)++;
            m->s = NULL;
            {
                JVal *val = parse_val(p);
                if (val) { val->key = m->key; free(m); m = val; }
            }
            if (!m) break;
            tail = &v->child;
            while (*tail)
                tail = &(*tail)->next;
            *tail = m;
            jskip(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == '}') { (*p)++; break; }
            break;
        }
        return v;
    }
    if (**p == '[') {
        v = jnew(1);
        (*p)++;
        for (;;) {
            JVal *el, **tail;
            jskip(p);
            if (**p == ']') { (*p)++; break; }
            el = parse_val(p);
            if (!el) break;
            tail = &v->child;
            while (*tail)
                tail = &(*tail)->next;
            *tail = el;
            jskip(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == ']') { (*p)++; break; }
            break;
        }
        return v;
    }
    if (**p == '"') {
        v = jnew(2);
        v->s = parse_string(p);
        return v;
    }
    s = *p;
    while (**p && **p != ',' && **p != '}' && **p != ']' && **p != ' ' && **p != '\n' && **p != '\r' && **p != '\t')
        (*p)++;
    if (*p > s) {
        v = jnew(3);
        v->s = (char *)malloc((size_t)(*p - s) + 1);
        memcpy(v->s, s, (size_t)(*p - s));
        v->s[*p - s] = 0;
        return v;
    }
    *p = s;
    return NULL;
}

static void jfree(JVal *v) {
    JVal *c;
    if (!v)
        return;
    c = v->child;
    while (c) {
        JVal *nx = c->next;
        jfree(c);
        c = nx;
    }
    free(v->key);
    free(v->s);
    free(v);
}

static JVal *jfind(JVal *obj, const char *key) {
    JVal *c;
    if (!obj || obj->type != 0)
        return NULL;
    for (c = obj->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0)
            return c;
    return NULL;
}

static const char *jstr(JVal *obj, const char *key, const char *dflt) {
    JVal *v = jfind(obj, key);
    return (v && v->s) ? v->s : dflt;
}

/* ---------------- LLM 客户端（非流式） ---------------- */
static char g_key[256] = "";       /* 员工A 的 key */
static char g_mgr_key[256] = "";   /* 主管的 key；MA_MANAGER_KEY 不配时复制员工A 的 */

static int json_get_string_flat(const char *js, const char *key_lit, char *out, int cap) {
    const char *p = strstr(js, key_lit);
    int j = 0;
    if (!p)
        return 0;
    p = strchr(p + strlen(key_lit), ':');
    if (!p)
        return 0;
    p = strchr(p, '"');
    if (!p)
        return 0;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\')
            p++;
        if (j < cap - 1)
            out[j++] = *p;
        p++;
    }
    out[j] = 0;
    return j > 0;
}

static void json_escape(SB *dst, const char *src) {
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { sb_put(dst, "\\", 1); sb_put(dst, (const char *)&c, 1); }
        else if (c == '\n') sb_puts(dst, "\\n");
        else if (c == '\r') sb_puts(dst, "\\r");
        else if (c == '\t') sb_puts(dst, "\\t");
        else if (c < 0x20) sb_printf(dst, "\\u%04x", c);
        else sb_put(dst, (const char *)&c, 1);
    }
}

/* 调一次 chat，返回 malloc 的 content（调用方 free）；失败返回 NULL。
 * 端点/密钥/模型都显式传入：主管与员工A 可以挂不同服务商。 */
static char *llm(const PetEndpoint *ep, const char *key, const char *model,
                 const char *system, const char *user, int max_tokens) {
    SB body, resp;
    wchar_t whdr[1024];
    char hdr[512];
    HINTERNET ses, con, req;
    DWORD status = 0, cb, avail, got;
    char *content = NULL;
    JVal *root, *choices, *c0, *msg;

    memset(&body, 0, sizeof body);
    memset(&resp, 0, sizeof resp);
    /* 本轮模型调用计数（MAX_CALLS_PER_ROUND=8，含拆解/工人/判卷/质检/汇总）；
     * 上下文粗估 tokens≈bytes/3 只记请求侧，过红线后 llm 直接失败 */
    if (g_cfg.calls_left <= 0) {
        dbg("calls quota exhausted\n");
        return NULL;
    }
    g_cfg.calls_left--;
    ctx_add(&g_cfg, (long)((strlen(system) + strlen(user)) / 3));
    sb_puts(&body, "{\"model\":\"");
    sb_puts(&body, model);
    sb_puts(&body, "\",\"messages\":[{\"role\":\"system\",\"content\":\"");
    json_escape(&body, system);
    sb_puts(&body, "\"},{\"role\":\"user\",\"content\":\"");
    json_escape(&body, user);
    /* enable_thinking 是 dashscope 专属参数，别的 OpenAI 兼容端点收到未知字段可能 400 */
    if (compat_ep_is_dashscope(ep))
        sb_printf(&body, "\"}],\"max_tokens\":%d,\"temperature\":0.70,\"enable_thinking\":false}", max_tokens);
    else
        sb_printf(&body, "\"}],\"max_tokens\":%d,\"temperature\":0.70}", max_tokens);

    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\nContent-Type: application/json\r\n", key);
    MultiByteToWideChar(CP_UTF8, 0, hdr, -1, whdr, 1024);
    if (body.len > 60000) {
        dbg("llm: body huge (%d)\n", body.len);
    }

    ses = WinHttpOpen(L"ma/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    WinHttpSetTimeouts(ses, 5000, 15000, 30000, 120000);
    con = WinHttpConnect(ses, ep->host, ep->port, 0);
    req = WinHttpOpenRequest(con, L"POST", ep->path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             ep->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req)
        goto out;
    WinHttpAddRequestHeaders(req, whdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    /* lpOptional 要原始 UTF-8 字节（wchar 版坑见记忆） */
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body.p, (DWORD)body.len, (DWORD)body.len, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        dbg("llm send fail %lu\n", GetLastError());
        goto out;
    }
    cb = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &cb, NULL);
    if (status != 200) {
        dbg("llm http %lu\n", status);
        goto out;
    }
    for (;;) {
        avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0)
            break;
        {
            char buf[4096];
            DWORD want = avail < sizeof buf ? avail : sizeof buf;
            if (!WinHttpReadData(req, buf, want, &got) || got == 0)
                break;
            sb_put(&resp, buf, (int)got);
        }
    }
    if (!resp.p)
        goto out;
    {
        const char *pp = resp.p;
        root = parse_val(&pp);
        choices = jfind(root, "choices");
        c0 = choices ? choices->child : NULL;
        msg = jfind(c0, "message");
        content = (char *)jstr(msg, "content", "");
        content = _strdup(content);
        jfree(root);
    }
    if (!content || !content[0]) {
        dbg("llm no content, resp head: %.200s\n", resp.p ? resp.p : "(empty)");
        free(content);
        content = NULL;
    }
out:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(body.p);
    free(resp.p);
    return content;
}

/* 剥 ```json 围栏：返回第一个 '[' 或 '{' */
static const char *strip_fence(char *s) {
    char *a = strchr(s, '['), *b = strchr(s, '{');
    if (a && b) return a < b ? a : b;
    return a ? a : b ? b : s;
}

/* ---------------- 任务卡 ---------------- */
typedef struct {
    char id[16], llm_id[16], type[16], title[96], goal[512], assignee[24];
    char *crit[MAX_CRIT]; int ncrit;
    char *out[MAX_OUT];  int nout;
    char dep[MAX_DEP][16]; int ndep;
    int attempts, passed, done, wrote;
    int dynout;              /* 1=任务未声明输出，落盘路径动态登记进 out[] 供 L1-L3 查验 */
    char *notes[6]; int nnotes;
} Task;

static Task tasks[MAX_TASKS];
static int ntasks = 0;
static WCHAR g_bb[MAX_PATH], g_tasks_dir[MAX_PATH], g_art_dir[MAX_PATH];
static int task_counter = 0;

static void board_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    FILE *f;
    WCHAR path[2048];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    lstrcpyW(path, g_bb);
    lstrcatW(path, L"board.log");
    f = _wfopen(path, L"a");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

static void save_task(Task *t) {
    SB j;
    WCHAR full[2048];
    int i;
    memset(&j, 0, sizeof j);
    sb_puts(&j, "{\n  \"task_id\": \"");
    sb_puts(&j, t->id);
    sb_printf(&j, "\",\n  \"type\": \"%s\",\n  \"title\": \"", t->type);
    json_escape(&j, t->title);
    sb_printf(&j, "\",\n  \"goal\": \"");
    json_escape(&j, t->goal);
    sb_printf(&j, "\",\n  \"assignee\": \"%s\",\n  \"status\": \"%s\",\n  \"attempts\": %d,\n  \"acceptance_criteria\": [",
              t->assignee, t->passed ? "passed" : (t->done ? "failed" : "pending"), t->attempts);
    for (i = 0; i < t->ncrit; i++) {
        if (i) sb_puts(&j, ", ");
        sb_puts(&j, "\"");
        json_escape(&j, t->crit[i]);
        sb_puts(&j, "\"");
    }
    sb_puts(&j, "],\n  \"review_notes\": [");
    for (i = 0; i < t->nnotes; i++) {
        if (i) sb_puts(&j, ", ");
        sb_puts(&j, "\"");
        json_escape(&j, t->notes[i]);
        sb_puts(&j, "\"");
    }
    sb_puts(&j, "]\n}");
    lstrcpyW(full, g_tasks_dir);
    {
        WCHAR wid[32];
        MultiByteToWideChar(CP_UTF8, 0, t->id, -1, wid, 32);
        lstrcatW(full, wid);
        lstrcatW(full, L".json");
    }
    write_file_w(full, j.p, j.len);
    free(j.p);
}

/* ---------------- 主管：拆解 ---------------- */
static const char *SUP_SYS =
    "你是多智能体项目主管。把用户需求拆解为任务卡 JSON 数组，只输出 JSON，不要任何解释。\n"
    "字段：task_id(T-0001起)、type(research|design|code|test|review|doc)、title(一句话)、goal、"
    "assignee_role(固定\"员工A\")、outputs([{\"file\":\"相对路径\"}])、"
    "acceptance_criteria(至少3条可验证陈述句)、depends_on([task_id]或[])、priority(数字越小越先)。\n"
    "规则：一张卡一个产出物；验收标准禁止\"优雅\"\"合理\"等模糊词；任务总数不超过5个；"
    "简单需求直接1-2张 code 卡，不要过度拆解。全部用中文。";

static int decompose(const char *request) {
    char *out;
    JVal *arr, *el;
    int i = 0, n;
    /* crit/out 字符串活在 arr 里，jfree 前必须拷进静态池 */
    static char critbuf[MAX_TASKS][MAX_CRIT][256];
    static char outbuf[MAX_TASKS][MAX_OUT][128];
    emitf("【主管】收到需求，拆解任务…\n");
    out = llm(&g_mgr_ep, g_mgr_key, g_mgr_model, SUP_SYS, request, 1200);
    if (!out)
        return -1;
    {
        const char *p = strip_fence(out);
        arr = parse_val(&p);
    }
    if (!arr || arr->type != 1) {
        dbg("decompose: bad json: %.200s\n", out);
        free(out);
        jfree(arr);
        return -1;
    }
    for (el = arr->child; el && i < MAX_TASKS; el = el->next, i++) {
        Task *t = &tasks[i];
        JVal *outs, *o, *deps, *cr;
        int m;
        memset(t, 0, sizeof *t);
        task_counter++;
        /* id 一律引擎自己编号：LLM 给的 T-001/T-0001 位数不稳，直接当文件名会撞车互相覆盖。
           LLM 原 id 留着做映射，depends_on 才能对上 */
        {
            char llm_id[16];
            lstrcpynA(llm_id, jstr(el, "task_id", ""), 16);
            snprintf(t->id, sizeof t->id, "T-%04d", task_counter);
            lstrcpynA(t->llm_id, llm_id, 16);
        }
        lstrcpynA(t->type, jstr(el, "type", "code"), 16);
        lstrcpynA(t->title, jstr(el, "title", "未命名任务"), 96);
        lstrcpynA(t->goal, jstr(el, "goal", ""), 512);
        lstrcpynA(t->assignee, "员工A", 24);
        t->ncrit = 0;
        cr = jfind(el, "acceptance_criteria");
        if (cr && cr->type == 1) {
            for (o = cr->child; o && t->ncrit < MAX_CRIT; o = o->next)
                if (o->s) {
                    lstrcpynA(critbuf[i][t->ncrit], o->s, 256);
                    t->ncrit++;
                }
        }
        for (m = 0; m < t->ncrit; m++)
            t->crit[m] = critbuf[i][m];
        t->nout = 0;
        outs = jfind(el, "outputs");
        if (outs && outs->type == 1) {
            for (o = outs->child; o && t->nout < MAX_OUT; o = o->next) {
                const char *f = jstr(o, "file", NULL);
                if (f) {
                    lstrcpynA(outbuf[i][t->nout], f, 128);
                    t->nout++;
                }
            }
        }
        for (m = 0; m < t->nout; m++)
            t->out[m] = outbuf[i][m];
        deps = jfind(el, "depends_on");
        if (deps && deps->type == 1) {
            JVal *d;
            for (d = deps->child; d && t->ndep < MAX_DEP; d = d->next)
                if (d->s) {
                    lstrcpynA(t->dep[t->ndep], d->s, 16);
                    t->ndep++;
                }
        }
        board_log("create %s [%s] %s crit=%d", t->id, t->type, t->title, t->ncrit);
    }
    n = i;
    jfree(arr);
    free(out);
    ntasks = n;
    if (ntasks <= 0)
        return -1;
    emitf("【主管】拆成 %d 个任务：", ntasks);
    for (i = 0; i < ntasks; i++) {
        save_task(&tasks[i]);
        emitf("%s ", tasks[i].id);
    }
    emitf("\n");
    return ntasks;
}

/* ---------------- 员工A：执行 ---------------- */
static const char *WRK_SYS =
    "你是项目员工A（主执行者）。根据任务卡产出文件。只输出 JSON，格式：\n"
    "{\"files\":[{\"path\":\"相对路径\",\"content\":\"文件完整内容\"}],\"summary\":\"一句话\"}\n"
    "要求：path 必须与任务卡 outputs 一致；content 是完整可运行的文件内容；"
    "逐条满足 acceptance_criteria；不修改任务卡之外的文件。";

/* 模型常把 ```python 围栏当文件内容一起吐出来。落盘前剥掉首尾围栏，
   否则产物直接运行会 SyntaxError。只动首尾，正文里的缩进/空行原样保留。 */
static char *strip_code_fences(const char *src) {
    const char *a = src, *b;
    size_t n;
    char *out;
    while (*a == '\n' || *a == '\r' || *a == ' ' || *a == '\t')
        a++;
    if (strncmp(a, "```", 3) == 0) {
        a = strchr(a, '\n');
        if (!a)
            return _strdup(src);
        a++;
    }
    b = a + strlen(a);
    while (b > a && (b[-1] == '\n' || b[-1] == '\r' || b[-1] == ' ' || b[-1] == '\t'))
        b--;
    if (b - a >= 3 && strncmp(b - 3, "```", 3) == 0)
        b -= 3;
    while (b > a && (b[-1] == '\n' || b[-1] == '\r' || b[-1] == ' ' || b[-1] == '\t'))
        b--;
    n = (size_t)(b - a);
    out = (char *)malloc(n + 2);
    if (!out)
        return NULL;
    memcpy(out, a, n);
    out[n] = '\n';
    out[n + 1] = 0;
    return out;
}

static int run_worker(Task *t) {
    SB u;
    char *out;
    JVal *root, *files, *f;
    int wrote = 0;
    if (t->dynout)
        t->nout = 0;   /* 每次尝试重新登记实际落盘路径，防重派后新旧混杂 */
    memset(&u, 0, sizeof u);
    sb_puts(&u, "任务卡：\n");
    {
        SB j;
        memset(&j, 0, sizeof j);
        /* 复用 save_task 的序列化：直接内联拼 */
        sb_printf(&j, "{\"task_id\":\"%s\",\"type\":\"%s\",\"goal\":\"", t->id, t->type);
        json_escape(&j, t->goal);
        sb_puts(&j, "\",\"outputs\":[");
        {
            int i;
            for (i = 0; i < t->nout; i++) {
                if (i) sb_puts(&j, ",");
                sb_printf(&j, "{\"file\":\"%s\"}", t->out[i]);
            }
        }
        sb_puts(&j, "],\"acceptance_criteria\":[");
        {
            int i;
            for (i = 0; i < t->ncrit; i++) {
                if (i) sb_puts(&j, ",");
                sb_puts(&j, "\"");
                json_escape(&j, t->crit[i]);
                sb_puts(&j, "\"");
            }
        }
        sb_puts(&j, "]}");
        sb_puts(&u, j.p ? j.p : "");
        free(j.p);
    }
    if (t->nnotes) {
        int i;
        sb_puts(&u, "\n\n上次驳回意见，必须逐条解决：\n");
        for (i = 0; i < t->nnotes; i++) {
            sb_puts(&u, "- ");
            sb_puts(&u, t->notes[i]);
            sb_puts(&u, "\n");
        }
    }
    out = llm(&g_ep, g_key, g_model, WRK_SYS, u.p ? u.p : "", 2000);
    free(u.p);
    if (!out)
        return 0;
    {
        const char *p = strip_fence(out);
        root = parse_val(&p);
    }
    files = jfind(root, "files");
    if (files && files->type == 1) {
        for (f = files->child; f; f = f->next) {
            const char *path = jstr(f, "path", NULL);
            const char *content = jstr(f, "content", NULL);
            char *clean;
            WCHAR full[2048];
            if (!path || !content)
                continue;
            if (strchr(path, ':') || path[0] == '/' || path[0] == '\\' || strstr(path, "..")) {
                dbg("worker: unsafe path %s skipped\n", path);
                continue;
            }
            lstrcpyW(full, g_art_dir);
            wpath_join(full + lstrlenW(full), (int)(sizeof full / sizeof full[0]) - (int)lstrlenW(full), L"", path);
            clean = strip_code_fences(content);
            if (!clean)
                continue;
            if (write_file_w(full, clean, (int)strlen(clean))) {
                wrote++;
                board_log("artifact %s %s", t->id, path);
                /* 单卡直通：把实际落盘路径登记进 out[]，L1 查存在、L2/L3 才能读到内容 */
                if (t->dynout && t->nout < MAX_OUT)
                    t->out[t->nout++] = _strdup(path);
            }
            free(clean);
        }
    } else {
        dbg("worker: no files array: %.200s\n", out);
    }
    jfree(root);
    free(out);
    return wrote;
}

/* ---------------- 三级验收 ---------------- */
static int level1(Task *t, char *why, int cap) {
    int i;
    if (t->nout == 0) {
        /* 单卡直通（员工A角色/拆解失败降级）：路径由工人自选，落过盘即过 L1 */
        if (t->wrote > 0)
            return 1;
        snprintf(why, cap, "工人未落任何文件");
        return 0;
    }
    for (i = 0; i < t->nout; i++) {
        WCHAR wfull[2048];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        lstrcpyW(wfull, g_art_dir);
        wpath_join(wfull + lstrlenW(wfull), (int)(sizeof wfull / sizeof wfull[0]) - (int)lstrlenW(wfull), L"", t->out[i]);
        if (!GetFileAttributesExW(wfull, GetFileExInfoStandard, &fa)) {
            snprintf(why, cap, "缺少输出文件: %s", t->out[i]);
            return 0;
        }
        if (fa.nFileSizeLow == 0) {
            snprintf(why, cap, "输出文件为空: %s", t->out[i]);
            return 0;
        }
    }
    return 1;
}

/* 把产出文件内容（每个截 4000 字节）拼进 prompt，L2/L3 共用 */
static void append_files(SB *u, Task *t) {
    int i;
    for (i = 0; i < t->nout; i++) {
        WCHAR wfull[2048];
        char *data;
        int len;
        FILE *f;
        lstrcpyW(wfull, g_art_dir);
        wpath_join(wfull + lstrlenW(wfull), (int)(sizeof wfull / sizeof wfull[0]) - (int)lstrlenW(wfull), L"", t->out[i]);
        f = _wfopen(wfull, L"rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        len = (int)ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 4000)
            len = 4000;
        data = (char *)malloc((size_t)len + 1);
        len = (int)fread(data, 1, (size_t)len, f);
        data[len < 0 ? 0 : len] = 0;
        fclose(f);
        sb_printf(u, "--- %s ---\n", t->out[i]);
        sb_puts(u, data);
        sb_puts(u, "\n");
        free(data);
    }
}

static int level2(Task *t, SB *failnotes) {
    SB u;
    char *out;
    JVal *root;
    int pass = 1;
    memset(&u, 0, sizeof u);
    sb_puts(&u, "验收标准：\n");
    {
        int i;
        for (i = 0; i < t->ncrit; i++) {
            sb_printf(&u, "%d. ", i + 1);
            sb_puts(&u, t->crit[i]);
            sb_puts(&u, "\n");
        }
    }
    sb_puts(&u, "\n产出文件内容：\n");
    append_files(&u, t);
    out = llm(&g_mgr_ep, g_mgr_key, g_mgr_model,
              "你是验收判卷员。对每条验收标准基于文件内容判定是否满足。"
              "只输出 JSON：{\"pass\":true或false,\"failed\":[\"未通过的标准原文及原因\"]}",
              u.p ? u.p : "", 400);
    free(u.p);
    if (!out)
        return 1;   /* 判卷挂了不卡流程，交给 L3 */
    {
        const char *p = strip_fence(out);
        root = parse_val(&p);
    }
    {
        JVal *v = jfind(root, "pass");
        if (v && v->s && strcmp(v->s, "false") == 0) {
            JVal *fl = jfind(root, "failed");
            pass = 0;
            if (fl) {
                JVal *c;
                for (c = fl->child; c; c = c->next)
                    if (c->s) {
                        sb_puts(failnotes, "验收未过: ");
                        sb_puts(failnotes, c->s);
                        sb_puts(failnotes, "\n");
                    }
            }
        }
    }
    jfree(root);
    free(out);
    return pass;
}

static int level3(Task *t, SB *failnotes) {
    SB u;
    char *out;
    JVal *root;
    int ok = 1;
    int i;
    memset(&u, 0, sizeof u);
    sb_printf(&u, "任务目标：%s\n验收标准：\n", t->goal);
    for (i = 0; i < t->ncrit; i++) {
        sb_printf(&u, "%d. ", i + 1);
        sb_puts(&u, t->crit[i]);
        sb_puts(&u, "\n");
    }
    sb_puts(&u, "\n产出文件内容：\n");
    append_files(&u, t);
    sb_puts(&u, "\n只依据上面的验收标准和任务目标判定。风格、类型注解、任务卡没写的边界处理"
                "一律不是驳回理由。标准都满足就 APPROVE；确有违反才 REQUEST_CHANGES 并逐条指出违反了哪条。"
                "只输出 JSON：{\"result\":\"APPROVE\"或\"REQUEST_CHANGES\",\"issues\":[\"...\"]}");
    out = llm(&g_mgr_ep, g_mgr_key, g_mgr_model,
              "你是质检员C。铁律：acceptance_criteria 是唯一评审依据，不得发明新要求。",
              u.p ? u.p : "", 400);
    free(u.p);
    if (!out)
        return 1;
    {
        const char *p = strip_fence(out);
        root = parse_val(&p);
    }
    if (strcmp(jstr(root, "result", "APPROVE"), "APPROVE") != 0) {
        JVal *iss = jfind(root, "issues");
        ok = 0;
        if (iss) {
            JVal *c;
            for (c = iss->child; c; c = c->next)
                if (c->s) {
                    sb_puts(failnotes, "质检: ");
                    sb_puts(failnotes, c->s);
                    sb_puts(failnotes, "\n");
                }
        }
    }
    jfree(root);
    free(out);
    return ok;
}

/* ---------------- 调度主循环 ---------------- */
static int dep_satisfied(Task *t) {
    int i, j;
    for (i = 0; i < t->ndep; i++) {
        for (j = 0; j < ntasks; j++)
            /* 依赖是 LLM 写的 id，任务被引擎重新编号过，两个名字都要认 */
            if (strcmp(tasks[j].llm_id, t->dep[i]) == 0 ||
                strcmp(tasks[j].id, t->dep[i]) == 0) {
                if (!tasks[j].passed)
                    return 0;
                break;
            }
        /* 依赖指向不存在的 task：视为满足，不卡死 */
    }
    return 1;
}

static void run_task(Task *t) {
    SB fail;
    int round;
    for (round = 0; round < 3; round++) {
        emitf("【主管】%s→员工A：%s\n", t->attempts ? "重派" : "派发", t->title);
        board_log("dispatch %s attempt=%d", t->id, t->attempts);
        emitf("【员工A】%s 执行中…\n", t->id);
        t->wrote = run_worker(t);

        memset(&fail, 0, sizeof fail);
        {
            char why[256] = "";
            if (!level1(t, why, sizeof why)) {
                sb_puts(&fail, why);
                sb_puts(&fail, "\n");
            } else if (!level2(t, &fail)) {
                /* fail 已填 */
            } else if (!level3(t, &fail)) {
                /* fail 已填 */
            } else {
                t->passed = 1;
                t->done = 1;
                save_task(t);
                board_log("passed %s", t->id);
                emitf("【主管】%s 验收通过 ✓\n", t->id);
                free(fail.p);
                return;
            }
        }
        t->attempts++;
        if (t->nnotes < 6 && fail.p) {
            t->notes[t->nnotes++] = _strdup(fail.p);
            save_task(t);
        }
        board_log("reject %s attempt=%d why=%.120s", t->id, t->attempts, fail.p ? fail.p : "");
        if (fail.p) {
            char *nl = strchr(fail.p, '\n');
            if (nl)
                *nl = 0;   /* 首行原因发给用户，全文留在 review_notes */
        }
        emitf("【主管】%s 打回(%d/2)：%s\n", t->id, t->attempts, fail.p ? fail.p : "原因未知");
        free(fail.p);
        if (t->attempts >= 2)
            break;
    }
    t->done = 1;
    save_task(t);
    board_log("failed %s", t->id);
    emitf("【主管】%s 两次未过，记录上报。\n", t->id);
}

static void orchestrate(void) {
    int guard = 0;
    while (guard++ < ntasks + 2) {
        int progressed = 0, i;
        for (i = 0; i < ntasks; i++) {
            Task *t = &tasks[i];
            if (t->done)
                continue;
            if (!dep_satisfied(t))
                continue;
            run_task(t);
            progressed = 1;
            break;   /* 阶段1 串行 */
        }
        if (!progressed)
            break;
    }
    /* 依赖死锁/未跑的收尾：强制处理剩余 */
    {
        int i;
        for (i = 0; i < ntasks; i++)
            if (!tasks[i].done) {
                tasks[i].done = 1;
                board_log("skipped %s", tasks[i].id);
                emitf("【主管】%s 因依赖未满足跳过。\n", tasks[i].id);
            }
    }
}

static void deliver(const char *request) {
    SB u;
    char *out;
    int i;
    memset(&u, 0, sizeof u);
    sb_puts(&u, "用户需求：");
    sb_puts(&u, request);
    sb_puts(&u, "\n\n任务结果：\n");
    for (i = 0; i < ntasks; i++)
        sb_printf(&u, "%s %s [%s] %s\n", tasks[i].passed ? "✓" : "✗", tasks[i].id,
                  tasks[i].title, tasks[i].passed ? "通过" : "未通过/跳过");
    sb_puts(&u, "\n请用简洁中文向用户汇总交付：完成了什么、产出文件在哪(blackboard\\artifacts\\)、未解决事项。不要 JSON。");
    emitf("【主管】汇总交付…\n");
    out = llm(&g_mgr_ep, g_mgr_key, g_mgr_model, "你是项目主管，对用户的最终答复者。", u.p ? u.p : "", 800);
    free(u.p);
    if (out) {
        emit(out, -1);
        emitf("\n");
        free(out);
    } else {
        emitf("交付汇总失败，产出文件在 blackboard\\artifacts\\ 里自查。\n");
    }
}

/* ---------------- stdin 帧读取 ---------------- */
typedef struct { char buf[4096]; int pos, len; } SBin;
static int sbc_get(SBin *s) {
    if (s->pos >= s->len) {
        DWORD g = 0;
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || !ReadFile(h, s->buf, sizeof s->buf, &g, NULL) || g == 0)
            return -1;
        s->len = (int)g;
        s->pos = 0;
    }
    return (unsigned char)s->buf[s->pos++];
}
static int sbc_line(SBin *s, char *out, int cap) {
    int n = 0, c;
    for (;;) {
        c = sbc_get(s);
        if (c == -1)
            return n ? n : -1;
        if (c == '\n')
            break;
        if (n < cap - 1)
            out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}
static int sbc_read(SBin *s, char *out, int n) {
    int got = 0;
    while (got < n) {
        int c = sbc_get(s);
        if (c == -1)
            break;
        out[got++] = (char)c;
    }
    return got;
}

int main(void) {
    SBin in;
    char head[64], *prompt;
    long plen;
    WCHAR base[MAX_PATH], *sl;
    _setmode(_fileno(stdin), _O_BINARY);
    memset(&in, 0, sizeof in);
    dbg("ma build tag=%d\n", MA_TAG_NUM);

    /* 路径：exe 目录\blackboard\ */
    GetModuleFileNameW(NULL, base, MAX_PATH);
    sl = wcsrchr(base, L'\\');
    if (sl)
        *(sl + 1) = 0;
    lstrcpyW(g_bb, base);    lstrcatW(g_bb, L"blackboard\\");
    lstrcpyW(g_tasks_dir, g_bb); lstrcatW(g_tasks_dir, L"tasks\\");
    lstrcpyW(g_art_dir, g_bb);   lstrcatW(g_art_dir, L"artifacts\\");
    CreateDirectoryW(g_bb, NULL);
    CreateDirectoryW(g_tasks_dir, NULL);
    CreateDirectoryW(g_art_dir, NULL);

    /* 端点运行时解析：pet.exe 的 --base 会写进 PET_LLM_BASE 环境变量 */
    compat_ep_default(&g_ep);
    {
        const char *b = getenv("PET_LLM_BASE");
        if (b && b[0]) {
            if (compat_parse_base(b, &g_ep))
                dbg("base switched: %s\n", b);   /* 只记 URL，不记 key */
            else
                dbg("bad PET_LLM_BASE, keep dashscope\n");
        }
    }

    /* key 优先级：PET_LLM_KEY > PET_LLM_KEYFILE > settings.json > 环境变量。
       换服务商时前两条是唯一通路——settings.json 里只有 DASHSCOPE_API_KEY 这个字段名 */
    {
        const char *k = getenv("PET_LLM_KEY");
        if (k && k[0])
            lstrcpynA(g_key, k, sizeof g_key);
    }
    if (!g_key[0]) {
        const char *kf = getenv("PET_LLM_KEYFILE");
        if (kf && kf[0]) {
            WCHAR wp[1024];
            char *c = NULL;
            if (MultiByteToWideChar(CP_ACP, 0, kf, -1, wp,
                                    (int)(sizeof wp / sizeof wp[0])) > 0)
                c = compat_read_file_w(wp, NULL);
            if (!c)
                c = read_file_a(kf, NULL);
            if (c) {
                int L = (int)strlen(c);
                while (L > 0 && (c[L - 1] == '\n' || c[L - 1] == '\r' ||
                                 c[L - 1] == ' ' || c[L - 1] == '\t'))
                    c[--L] = 0;
                lstrcpynA(g_key, c, sizeof g_key);
                dbg("key from keyfile, %d chars\n", (int)strlen(g_key));
                free(c);
            } else
                dbg("keyfile unreadable: %s\n", kf);
        }
    }
    if (!g_key[0]) {
        WCHAR sp[1024];
        char *js = NULL;
        if (compat_settings_path_w(sp, (int)(sizeof sp / sizeof sp[0])))
            js = compat_read_file_w(sp, NULL);
        if (js) {
            json_get_string_flat(js, "\"DASHSCOPE_API_KEY\"", g_key, sizeof g_key);
            free(js);
        }
    }
    if (!g_key[0]) {
        const char *e = getenv("DASHSCOPE_API_KEY");
        if (e)
            lstrcpynA(g_key, e, sizeof g_key);
    }
    if (!g_key[0]) {
        emitf("【主管】没找到 API key，跑不了多智能体。\n");
        return 3;
    }
    {
        const char *m = getenv("PET_LLM_MODEL");
        if (m && m[0])
            g_model = m;   /* key 仍共用 settings.json 那把；环境变量只换模型 */
    }

    /* 主管端点：MA_MANAGER_* 不配就整体沿用员工A，旧行为不变。
       典型用法：员工A 指本机 Ollama，主管指云端强模型（拆解/判卷/汇总更稳）。 */
    g_mgr_ep = g_ep;
    lstrcpynA(g_mgr_key, g_key, sizeof g_mgr_key);
    g_mgr_model = g_model;
    {
        const char *b = getenv("MA_MANAGER_BASE");
        if (b && b[0]) {
            PetEndpoint e;
            compat_ep_default(&e);
            if (compat_parse_base(b, &e)) {
                g_mgr_ep = e;
                dbg("mgr base switched: %s\n", b);
            } else {
                dbg("bad MA_MANAGER_BASE, keep worker endpoint\n");
            }
        }
    }
    {
        const char *k = getenv("MA_MANAGER_KEY");
        if (k && k[0])
            lstrcpynA(g_mgr_key, k, sizeof g_mgr_key);
    }
    if (g_mgr_key[0] == 0 || getenv("MA_MANAGER_KEY") == NULL) {
        const char *kf = getenv("MA_MANAGER_KEYFILE");
        if (kf && kf[0]) {
            WCHAR wp[1024];
            char *c = NULL;
            if (MultiByteToWideChar(CP_ACP, 0, kf, -1, wp,
                                    (int)(sizeof wp / sizeof wp[0])) > 0)
                c = compat_read_file_w(wp, NULL);
            if (!c)
                c = read_file_a(kf, NULL);
            if (c) {
                int L = (int)strlen(c);
                while (L > 0 && (c[L - 1] == '\n' || c[L - 1] == '\r' ||
                                 c[L - 1] == ' ' || c[L - 1] == '\t'))
                    c[--L] = 0;
                lstrcpynA(g_mgr_key, c, sizeof g_mgr_key);
                free(c);
                dbg("mgr key from keyfile, %d chars\n", (int)strlen(g_mgr_key));
            }
        }
    }
    {
        const char *m = getenv("MA_MANAGER_MODEL");
        if (m && m[0])
            g_mgr_model = m;
    }
    dbg("mgr model=%s key=%d\n", g_mgr_model, (int)strlen(g_mgr_key));

    /* 帧："<max> <temp> <role>\n<len>\n<prompt>"；role 即 AgentRole 枚举值 */
    if (sbc_line(&in, head, sizeof head) < 0)
        return 2;
    {
        int a = 0, b = 0, role_in = 0;
        sscanf(head, "%d %d %d", &a, &b, &role_in);
        if (!init_agent_config((AgentRole)role_in, &g_cfg))
            init_agent_config(ROLE_MANAGER, &g_cfg);   /* 坏值回落主管，不猜 */
    }
    board_log("role=%d mode=%s", (int)g_cfg.role,
              g_cfg.mode == EXEC_SERIAL ? "serial" : "parallel");
    if (!g_cfg.enabled) {
        /* 双保险：菜单灰显挡手，这道闸挡程序化点法 */
        emitf("【主管】员工%s 在阶段2 等坑位，调度闸已挡下（SCHED_BUSY）。\n",
              g_cfg.role == ROLE_EMP_B ? "B" : "C");
        return 0;
    }
    if (sbc_line(&in, head, sizeof head) < 0)
        return 2;
    plen = atol(head);
    if (plen <= 0 || plen > 8192)
        return 2;
    prompt = (char *)malloc((size_t)plen + 1);
    if (sbc_read(&in, prompt, (int)plen) != (int)plen) {
        free(prompt);
        return 2;
    }
    prompt[plen] = 0;
    board_log("==== request: %.200s", prompt);

    if (g_cfg.role == ROLE_EMP_A || decompose(prompt) <= 0) {
        /* 员工A 角色=跳过拆解单卡直通（做事更快）；主管拆解失败也落到这条路 */
        Task *t = &tasks[0];
        memset(t, 0, sizeof *t);
        lstrcpynA(t->id, "T-0001", 16);
        lstrcpynA(t->type, "code", 16);
        lstrcpynA(t->title, "直接执行", 96);
        lstrcpynA(t->goal, prompt, 512);
        lstrcpynA(t->assignee, "员工A", 24);
        t->crit[0] = "artifacts 下至少生成一个文件，内容与需求相关";
        t->crit[1] = "代码完整可运行，不是伪代码或占位注释";
        t->crit[2] = "summary 用一句话说清做了什么";
        t->ncrit = 3;
        t->dynout = 1;
        ntasks = 1;
        board_log("single task direct");
    }
    orchestrate();
    deliver(prompt);
    board_log("==== done");
    free(prompt);
    return 0;
}
