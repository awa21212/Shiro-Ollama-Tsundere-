/* inference_backend.c —— 真模型版：OpenAI 兼容端点 + SSE 流式，token 逐块写 stdout
 *
 * 编译: gcc -O2 -o inference_backend.exe inference_backend.c -lwinhttp
 *
 * stdin 协议不变（pet.c 零改动）:
 *   "<max_tokens> <temperature_x100>\n<prompt_bytes>\n<UTF-8 prompt 原文>"
 * stdout: 只写 token 字节（UTF-8），每块立即 fflush
 * stderr: 调试/错误，绝不进 stdout
 *
 * 真模型通道 = 哥哥 settings.json 里那把 dashscope key + compatible-mode 端点。
 * key 读取顺序：settings.json 的 env → 环境变量兜底。
 * 为什么文件优先：本机环境变量里残留一把**过期**的 DASHSCOPE_API_KEY（115 位），
 * 从会话外启动的进程拿它会 401；settings.json 里那把（117 位）才是对的。
 * 没有 key 时回落 mock 生成器，示例链路不断。
 */
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include "compat.h"

#define DEFAULT_MODEL "qwen3.8-flash"

/* 多服务商：PET_LLM_BASE 可把端点切到任何 OpenAI 兼容服务（含本机 llama.cpp/Ollama）。
   解析器统一在 compat.h —— 原来这里和 ma.c 各写死一份 host，迟早漂移 */
static PetEndpoint g_ep;

static void dbg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static size_t read_exact(void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        size_t r = fread((char *)buf + got, 1, n - got, stdin);
        if (r == 0)
            break;
        got += r;
    }
    return got;
}

static void emit(const char *s, size_t n) {
    fwrite(s, 1, n, stdout);
    fflush(stdout);   /* 不 flush 就没有"流式"，只有"憋完再吐" */
}

/* ---------------- 配置读取 ---------------- */

static char *read_file_all(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1 << 20) {
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
    return buf;
}

/* 从 JSON 里抠 "key_lit": "value" 的 value（key_lit 含引号）。
 * 值里无转义（API key / 模型名都是 ASCII），扫到未转义的闭合引号即止。 */
static int json_get_string(const char *js, const char *key_lit, char *out, int cap) {
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

/* ---------------- 共享记忆 ---------------- */

/* exe_dir\memory\about_bro.txt：终端白负责初始维护，哥哥对桌宠说「记住：xxx」
 * 由 pet.c 追加进来。GetModuleFileNameA 拿到的是 ANSI(cp936) 字节，
 * 中文 exe 目录可直接给窄字符 fopen 用 */
static char g_mem[4096] = "";
static void load_memory(void) {
    char exe[MAX_PATH], *sl;
    char *p;
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH))
        return;
    sl = strrchr(exe, '\\');
    if (!sl)
        return;
    *(sl + 1) = 0;
    strncat(exe, "memory\\about_bro.txt", sizeof exe - strlen(exe) - 1);
    p = read_file_all(exe);
    if (p) {
        strncpy(g_mem, p, sizeof g_mem - 1);
        g_mem[sizeof g_mem - 1] = 0;
        free(p);
        dbg("memory loaded: %d bytes\n", (int)strlen(g_mem));
    } else {
        dbg("no memory file at %s\n", exe);
    }
}

/* ---------------- JSON 请求构造 ---------------- */

static void json_escape_append(char *dst, int cap, int *pos, const char *src) {
    for (; *src && *pos < cap - 8; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = (char)c;
        } else if (c == '\n') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 'n';
        } else if (c == '\r') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 'r';
        } else if (c == '\t') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 't';
        } else if (c < 0x20) {
            *pos += snprintf(dst + *pos, cap - *pos, "\\u%04x", c);
        } else {
            dst[(*pos)++] = (char)c;   /* UTF-8 原样进 JSON，合法 */
        }
    }
    dst[*pos] = 0;
}

/* ---------------- SSE 解析 ---------------- */

static void utf8_encode(unsigned cp, char *out, int *n) {
    if (cp < 0x80) {
        out[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*n)++] = (char)(0xC0 | (cp >> 6));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[(*n)++] = (char)(0xE0 | (cp >> 12));
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*n)++] = (char)(0xF0 | (cp >> 18));
        out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static int hex4(const char *s, unsigned *out) {
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
    *out = v;
    return 1;
}

/* 从 "content":" 后的首字符解码 JSON 字符串值并 emit；转义全处理，
 * \uD800-\uDBFF 配 \uDC00-\uDFFF 合成码点。原始 UTF-8 字节直接透传。 */
static void decode_and_emit(const char *p) {
    char acc[4096];
    int n = 0;
    while (*p && n < (int)sizeof acc - 4) {
        char c = *p++;
        if (c == '"')
            break;
        if (c != '\\') {
            acc[n++] = c;
            continue;
        }
        c = *p++;
        if (!c)
            break;
        if (c == 'u') {
            unsigned cp;
            if (!hex4(p, &cp))
                break;
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                unsigned lo;
                if (hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            utf8_encode(cp, acc, &n);
        } else {
            switch (c) {
            case 'n': acc[n++] = '\n'; break;
            case 'r': acc[n++] = '\r'; break;
            case 't': acc[n++] = '\t'; break;
            case 'b': acc[n++] = '\b'; break;
            case 'f': acc[n++] = '\f'; break;
            default:  acc[n++] = c;    break;   /* \" \\ \/ */
            }
        }
    }
    if (n > 0)
        emit(acc, (size_t)n);
}

typedef struct {
    char line[8192];
    int lineLen;
    int done;
    int lines;
} SseCtx;

static void sse_line(SseCtx *ctx) {
    char *s = ctx->line;
    int i;
    ctx->line[ctx->lineLen] = 0;
    for (i = (int)strlen(s) - 1; i >= 0 && (s[i] == '\r'); i--)
        s[i] = 0;
    if (ctx->lines++ < 3)
        dbg("sse raw: %.180s\n", s);   /* 格式探针：只走 stderr，不碰 token 流 */
    if (strncmp(s, "data:", 5) == 0) {
        s += 5;
        while (*s == ' ')
            s++;
        if (strcmp(s, "[DONE]") == 0) {
            ctx->done = 1;
        } else {
            /* 注意 "content":" 前带引号，不会误配 "reasoning_content":" */
            char *v = strstr(s, "\"content\":\"");
            if (v)
                decode_and_emit(v + 11);   /* "content":" 共 11 字符，+10 会从闭合引号起步 */
        }
    }
    ctx->lineLen = 0;
}

static void sse_feed(SseCtx *ctx, const char *data, int len) {
    int i;
    for (i = 0; i < len && !ctx->done; i++) {
        if (data[i] == '\n') {
            sse_line(ctx);
        } else if (ctx->lineLen < (int)sizeof ctx->line - 1) {
            ctx->line[ctx->lineLen++] = data[i];
        } else {
            ctx->lineLen = 0;   /* 病态长行：丢弃重来，不撑爆 */
        }
    }
}

/* ---------------- 真模型调用 ---------------- */

static int api_stream(const char *key, const char *model, const char *prompt,
                      int max_tokens, int temp_x100) {
    static const char *SYS =
        "你是桌宠「白」，《游戏人生》里的白，用户是你哥哥「空」。"
        "用简洁中文口语回答，不用敬语，可偶尔傲娇，不超过三句话，不要代码块和JSON。";
    char sysbuf[5120], body[16384], hdr[512];
    wchar_t whdr[1024];
    int bp = 0, blen;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0, avail, got;
    char buf[4096];
    SseCtx ctx;
    int rc = 3;

    snprintf(body, sizeof body,
             "{\"model\":\"%s\",\"messages\":[{\"role\":\"system\",\"content\":\"", model);
    if (g_mem[0])
        snprintf(sysbuf, sizeof sysbuf,
                 "%s\n你记得关于哥哥的事（这是你和哥哥的回忆，自然用在回答里，不要逐条念出来）：\n%s",
                 SYS, g_mem);
    else
        snprintf(sysbuf, sizeof sysbuf, "%s", SYS);
    bp = (int)strlen(body);
    json_escape_append(body, sizeof body, &bp, sysbuf);
    {
        static const char *MID = "\"},{\"role\":\"user\",\"content\":\"";
        memcpy(body + bp, MID, strlen(MID));
        bp += (int)strlen(MID);
    }
    json_escape_append(body, sizeof body, &bp, prompt);
    {
        static const char *TAIL = "\"}],\"max_tokens\":";
        memcpy(body + bp, TAIL, strlen(TAIL));
        bp += (int)strlen(TAIL);
    }
    bp += snprintf(body + bp, sizeof body - (size_t)bp,
                   "%d,\"temperature\":%.2f,\"stream\":true%s}",
                   max_tokens, temp_x100 / 100.0,
                   compat_ep_is_dashscope(&g_ep) ? ",\"enable_thinking\":false" : "");

    dbg("api request body=%d bytes model=%s\n", bp, model);

    ses = WinHttpOpen(L"pet-inference/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!ses)
        return 4;
    WinHttpSetTimeouts(ses, 5000, 10000, 15000, 60000);
    con = WinHttpConnect(ses, g_ep.host, g_ep.port, 0);
    req = con ? WinHttpOpenRequest(con, L"POST", g_ep.path, NULL, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   g_ep.secure ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (!req)
        goto out;
    blen = snprintf(hdr, sizeof hdr,
                    "Authorization: Bearer %s\r\nContent-Type: application/json\r\n", key);
    hdr[blen] = 0;
    MultiByteToWideChar(CP_UTF8, 0, hdr, -1, whdr, (int)(sizeof whdr / sizeof whdr[0]));
    WinHttpAddRequestHeaders(req, whdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    /* lpOptional 要的是原始字节：body 直接发 UTF-8。
       上一版把 body 转成 wchar_t 再发，字节流里全是 NUL，
       服务端 JSON 解析失败回 400 "Required body invalid" */
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            body, (DWORD)strlen(body), (DWORD)strlen(body), 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        dbg("send/recv fail %lu\n", GetLastError());
        goto out;
    }
    {
        /* 第5参是 LPDWORD（指向"缓冲区长度"的指针），不是长度值本身；
           直接传 sizeof 会让 API 解引用地址 4 → 0xC0000005 */
        DWORD cb = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &status, &cb, NULL);
    }
    dbg("http status=%lu\n", status);
    if (status != 200) {
        /* 400 光看状态码没法定位，把服务端返回体挖出来 */
        char ebuf[1024] = "";
        int ep = 0;
        while (ep < (int)sizeof ebuf - 1) {
            DWORD na = 0, ng = 0;
            if (!WinHttpQueryDataAvailable(req, &na) || na == 0)
                break;
            if (!WinHttpReadData(req, buf, na < sizeof buf ? na : sizeof buf, &ng) || ng == 0)
                break;
            {
                DWORD i;
                for (i = 0; i < ng && ep < (int)sizeof ebuf - 1; i++) {
                    unsigned char ch = (unsigned char)buf[i];
                    ebuf[ep++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : ' ';
                }
            }
        }
        ebuf[ep] = 0;
        dbg("error body: %s\n", ebuf);
        goto out;
    }

    memset(&ctx, 0, sizeof ctx);
    while (!ctx.done) {
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0)
            break;
        if (!WinHttpReadData(req, buf, avail < sizeof buf ? avail : sizeof buf, &got) || got == 0)
            break;
        sse_feed(&ctx, buf, (int)got);
    }
    if (ctx.lineLen > 0)
        sse_line(&ctx);   /* 收尾无 \n 的残行 */
    rc = 0;
out:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return rc;
}

/* 没 key / 断网时的兜底，保证示例链路可跑 */
static void mock_stream(void) {
    static const char *reply = "……没连上模型，先 mock 一句。哥哥，管道是通的。";
    size_t i = 0, len = strlen(reply);
    while (i < len) {
        size_t j = i;
        unsigned char c = (unsigned char)reply[j];
        j += (c < 0x80) ? 1 : (c >= 0xE0) ? 3 : 2;
        emit(reply + i, j - i);
        i = j;
        Sleep(60);
    }
}

int main(void) {
    char head[64], key[256] = "", model[64] = DEFAULT_MODEL;
    char *prompt, *js;
    const char *envk;
    long plen;
    int max_tokens = 256, temp_x100 = 70;

    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    if (!fgets(head, sizeof head, stdin) || sscanf(head, "%d %d", &max_tokens, &temp_x100) < 1) {
        dbg("FATAL: no param line\n");
        return 2;
    }
    if (max_tokens <= 0 || max_tokens > 512)
        max_tokens = 256;
    if (!fgets(head, sizeof head, stdin)) {
        dbg("FATAL: no length line\n");
        return 2;
    }
    plen = atol(head);
    if (plen <= 0 || plen > 8192) {
        dbg("FATAL: bad prompt length %ld\n", plen);
        return 2;
    }
    prompt = (char *)malloc((size_t)plen + 1);
    if (!prompt || read_exact(prompt, (size_t)plen) != (size_t)plen) {
        dbg("FATAL: prompt read fail\n");
        free(prompt);
        return 2;
    }
    prompt[plen] = 0;
    dbg("prompt %ld bytes received\n", plen);

    load_memory();

    /* base：PET_LLM_BASE 指任何 OpenAI 兼容服务；解析失败保持 dashscope */
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
       环境变量那把 dashscope 是过期的（见坑位记忆），排最后 */
    {
        const char *k = getenv("PET_LLM_KEY");
        if (k && k[0])
            strncpy(key, k, sizeof key - 1);
    }
    if (!key[0]) {
        const char *kf = getenv("PET_LLM_KEYFILE");
        if (kf && kf[0]) {
            char *c = NULL;
            WCHAR wp[1024];
            /* 原来只走窄字符 fopen：中文/非当前 ANSI 代码页的密钥路径读不到 */
            if (MultiByteToWideChar(CP_ACP, 0, kf, -1, wp,
                                    (int)(sizeof wp / sizeof wp[0])) > 0)
                c = compat_read_file_w(wp, NULL);
            if (!c)
                c = read_file_all(kf);
            if (c) {
                int L = (int)strlen(c);
                while (L > 0 && (c[L - 1] == '\n' || c[L - 1] == '\r' ||
                                 c[L - 1] == ' ' || c[L - 1] == '\t'))
                    c[--L] = 0;
                strncpy(key, c, sizeof key - 1);
                free(c);
                dbg("key from keyfile, %d chars\n", (int)strlen(key));
            } else
                dbg("keyfile unreadable: %s\n", kf);
        }
    }
    if (!key[0]) {
        WCHAR sp[1024];
        /* js 在函数头声明且无初值，必须先置 NULL：
           compat_settings_path_w 返回 false 时它不会被赋值，下面 free 会拿栈垃圾 */
        js = NULL;
        /* 原来写死 C:\Users\<user>\.qwen\settings.json，换用户名就读不到 key */
        if (compat_settings_path_w(sp, (int)(sizeof sp / sizeof sp[0])))
            js = compat_read_file_w(sp, NULL);
        if (js) {
            json_get_string(js, "\"DASHSCOPE_API_KEY\"", key, sizeof key);
            free(js);
        }
    }
    if (!key[0]) {
        envk = getenv("DASHSCOPE_API_KEY");
        if (envk)
            strncpy(key, envk, sizeof key - 1);
    }
    {
        const char *m = getenv("PET_LLM_MODEL");
        if (m && m[0])
            strncpy(model, m, sizeof model - 1);
    }
    if (!key[0]) {
        dbg("no api key -> mock fallback\n");
        mock_stream();
        return 0;
    }

    if (api_stream(key, model, prompt, max_tokens, temp_x100) != 0) {
        dbg("api fail -> mock fallback\n");
        mock_stream();
        return 0;
    }
    dbg("api stream ok\n");
    return 0;
}
