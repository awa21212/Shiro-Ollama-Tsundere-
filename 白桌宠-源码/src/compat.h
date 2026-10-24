/* compat.h —— 运行环境适配层（header-only，纯 C）
 *
 * 桌宠原来把"这台开发机恰好成立"的东西写死在代码里：Win10 才有的 DPI 导出、
 * C:\Users\<user>\ 这种绝对路径、D 盘、雅黑字体、只装一张网卡的网卡表。
 * 这里全部换成运行时探测 + 逐级回落，让同一个 exe 在别的电脑上也能跑。
 *
 * 约定：所有函数 static inline（多 TU 包含不重复定义）；
 *       网络相关的用 PET_COMPAT_NEED_NET 显式开启，避免给子进程带上 iphlpapi。
 */
#ifndef PET_COMPAT_H
#define PET_COMPAT_H

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#ifdef PET_COMPAT_NEED_NET
#include <iphlpapi.h>
#endif

#define PET_COMPAT_VER 1

/* ---------- 环境变量 ---------- */

static inline int env_w(const WCHAR *name, WCHAR *out, int cap) {
    DWORD n;
    if (cap <= 0 || !out) return 0;
    out[0] = 0;
    n = GetEnvironmentVariableW(name, out, (DWORD)cap);
    return (n > 0 && n < (DWORD)cap);
}

static inline int env_a(const char *name, char *out, int cap) {
    DWORD n;
    if (cap <= 0 || !out) return 0;
    out[0] = 0;
    n = GetEnvironmentVariableA(name, out, (DWORD)cap);
    return (n > 0 && n < (DWORD)cap);
}

/* ---------- DPI ----------
 * SetProcessDpiAwarenessContext / GetDpiForSystem 是 Win10 1703+ 的导出。
 * 原来在 pet.c 里用 extern 静态导入 —— 在 Win7/8/老 Win10 上 loader 找不到入口点，
 * exe 双击直接报错，连 main 都进不去。改成运行时解析，逐级回落：
 *   感知：PMv2(Win10 1703+) → SetProcessDPIAware(Vista+) → 不管
 *   取 DPI：GetDpiForSystem → shcore GetDpiForMonitor → GetDeviceCaps → 96
 */

typedef BOOL(WINAPI *PF_set_ctx_aware)(HANDLE);
typedef BOOL(WINAPI *PF_set_aware)(void);
typedef UINT(WINAPI *PF_get_dpi_sys)(void);
typedef HRESULT(WINAPI *PF_get_dpi_mon)(HMONITOR, int, UINT *, UINT *);

static inline UINT compat_dpi_init(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    HMODULE sh = LoadLibraryW(L"shcore.dll");
    UINT dpi = 0;
    if (u) {
        PF_set_ctx_aware f = (PF_set_ctx_aware)(void *)
            GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (f) {
            f((HANDLE)-4);                       /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        } else {
            PF_set_aware g = (PF_set_aware)(void *)
                GetProcAddress(u, "SetProcessDPIAware");
            if (g) g();
        }
        {
            PF_get_dpi_sys s = (PF_get_dpi_sys)(void *)
                GetProcAddress(u, "GetDpiForSystem");
            if (s) dpi = s();
        }
    }
    if (!dpi && sh) {
        PF_get_dpi_mon m = (PF_get_dpi_mon)(void *)
            GetProcAddress(sh, "GetDpiForMonitor");
        typedef HMONITOR(WINAPI *PF_mon_from_win)(HWND, DWORD);
        PF_mon_from_win mfw = u ? (PF_mon_from_win)(void *)
            GetProcAddress(u, "MonitorFromWindow") : NULL;
        if (m && mfw) {
            /* MONITORINFO 没有 hMonitor 字段，HMONITOR 由 MonitorFromWindow 直接给 */
            HMONITOR hm = mfw(NULL, 2 /*MONITOR_DEFAULTTONEAREST*/);
            UINT x = 0, y = 0;
            if (hm && SUCCEEDED(m(hm, 0 /*MDT_EFFECTIVE_DPI*/, &x, &y)) && x)
                dpi = x;
        }
    }
    if (!dpi) {
        HDC dc = GetDC(NULL);
        if (dc) {
            int v = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(NULL, dc);
            if (v > 0) dpi = (UINT)v;
        }
    }
    if (sh) FreeLibrary(sh);
    return dpi ? dpi : 96;
}

/* ---------- 可执行文件所在目录 ----------
 * 原来固定 MAX_PATH，装在深层目录（>260）会静默截断，之后所有素材路径都错。
 * 返回 1 成功；out 始终以 NUL 结尾且以反斜杠收尾。
 */
static inline int compat_exe_dir(WCHAR *out, int cap) {
    WCHAR full[2048];
    DWORD n, i;
    if (cap <= 1 || !out) return 0;
    out[0] = 0;
    n = GetModuleFileNameW(NULL, full, (DWORD)(sizeof full / sizeof full[0]));
    if (n == 0 || n >= (DWORD)(sizeof full / sizeof full[0]))
        return 0;                            /* 失败或被截断，宁可让上层报警 */
    for (i = n; i > 0; i--)
        if (full[i - 1] == L'\\') break;
    if (i == 0) return 0;
    if ((int)i + 1 > cap) return 0;
    memcpy(out, full, i * sizeof(WCHAR));
    out[i] = 0;                              /* 含末尾反斜杠 */
    return 1;
}

/* ---------- qwen CLI 定位 ----------
 * 原来写死 C:\Users\<user>\AppData\Roaming\npm\qwen.cmd，换用户名即失效。
 * 优先级：PET_QWEN_CMD > %APPDATA%\npm\qwen.cmd > PATH 搜索。
 * 返回 1 时 out 是完整 .cmd 路径；0 表示这台机器没有 qwen CLI。
 */
static inline int compat_find_qwen(WCHAR *out, int cap) {
    WCHAR dir[1024], probe[2200];
    DWORD n;
    if (cap <= 1 || !out) return 0;
    out[0] = 0;
    if (env_w(L"PET_QWEN_CMD", out, cap) && GetFileAttributesW(out) != (DWORD)-1)
        return 1;
    out[0] = 0;
    if (env_w(L"APPDATA", dir, (int)(sizeof dir / sizeof dir[0]))) {
        if (lstrlenW(dir) + 16 < (int)(sizeof probe / sizeof probe[0])) {
            lstrcpyW(probe, dir);
            lstrcatW(probe, L"\\npm\\qwen.cmd");
            if (GetFileAttributesW(probe) != (DWORD)-1) {
                lstrcpynW(out, probe, cap);
                return 1;
            }
            out[0] = 0;
        }
    }
    n = SearchPathW(NULL, L"qwen.cmd", NULL, (DWORD)cap, out, NULL);
    if (n > 0 && n < (DWORD)cap) return 1;
    out[0] = 0;
    n = SearchPathW(NULL, L"qwen", L".cmd", (DWORD)cap, out, NULL);
    if (n > 0 && n < (DWORD)cap) return 1;
    out[0] = 0;
    return 0;
}

/* ---------- Qwen Code settings.json 定位 ----------
 * 原来写死 C:\Users\<user>\.qwen\settings.json。
 * 优先级：PET_SETTINGS > %USERPROFILE%\.qwen\settings.json
 */
static inline int compat_settings_path_w(WCHAR *out, int cap) {
    WCHAR dir[1024];
    if (cap <= 1 || !out) return 0;
    out[0] = 0;
    if (env_w(L"PET_SETTINGS", out, cap)) return 1;
    out[0] = 0;
    if (!env_w(L"USERPROFILE", dir, (int)(sizeof dir / sizeof dir[0]))) return 0;
    if (lstrlenW(dir) + 24 >= cap) return 0;
    lstrcpyW(out, dir);
    lstrcatW(out, L"\\.qwen\\settings.json");
    return 1;
}

/* ---------- 存储面板用哪个盘 ----------
 * 原来写死 D:\，没有 D 盘的机器上面板永远显示 --。
 * 优先级：PET_DISK > 系统盘 > 第一个固定盘 > C:\
 */
static inline void compat_data_drive(WCHAR *out /* >=4 WCHAR */) {
    WCHAR env[64], sys[1024], root[4];
    DWORD mask, i;
    WCHAR c;
    out[0] = L'C'; out[1] = L':'; out[2] = L'\\'; out[3] = 0;
    if (env_w(L"PET_DISK", env, (int)(sizeof env / sizeof env[0]))) {
        c = env[0];
        if (c >= L'a' && c <= L'z') c = (WCHAR)(c - L'a' + L'A');
        if (c >= L'A' && c <= L'Z') {
            root[0] = c; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
            if (GetDriveTypeW(root) != DRIVE_NO_ROOT_DIR) { out[0] = c; return; }
        }
    }
    if (GetSystemWindowsDirectoryW(sys, (DWORD)(sizeof sys / sizeof sys[0])) >= 3 &&
        sys[1] == L':') {
        out[0] = sys[0];
        return;
    }
    mask = GetLogicalDrives();
    for (i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        root[0] = (WCHAR)(L'A' + i); root[1] = L':'; root[2] = L'\\'; root[3] = 0;
        if (GetDriveTypeW(root) == DRIVE_FIXED) { out[0] = root[0]; return; }
    }
}

/* ---------- 中文字体回落链 ----------
 * 原来 GdipCreateFontFamilyFromName("Microsoft YaHei") 不判返回值：
 * 非中文 Windows 没装雅黑时 *fam 不被写入，后面拿野指针建字体 → 崩。
 * 这里给出候选表，由调用方逐个试，全失败就走无文字降级。
 */
static const char *const PET_FONTS[] = {
    "Microsoft YaHei", "Microsoft JhengHei", "Segoe UI", "Tahoma",
    "MS Gothic", "Arial Unicode MS", "Noto Sans CJK SC", "SimSun", NULL
};

/* ---------- 网卡累计字节数 ----------
 * 原来用 static MIB_IFTABLE tb —— 该结构只含 1 个 MIB_IFROW，
 * 多网卡/多虚拟网卡的机器上 GetIfTable 返回 ERROR_INSUFFICIENT_BUFFER，
 * 网速永远显示 --。改成两段式：先探大小再 malloc。
 * 返回 0 成功（bytes 有效），-1 失败。
 */
#ifdef PET_COMPAT_NEED_NET
static inline int compat_net_bytes(ULONGLONG *bytes) {
    MIB_IFTABLE probe;
    DWORD sz = sizeof(probe), r;
    MIB_IFTABLE *tb;
    int i;
    *bytes = 0;
    r = GetIfTable(&probe, &sz, FALSE);
    if (r == NO_ERROR) {
        sz = sizeof(MIB_IFTABLE) + 8 * sizeof(MIB_IFROW);   /* 单网卡机器 */
    } else if (r != ERROR_INSUFFICIENT_BUFFER || sz == 0) {
        return -1;
    } else {
        sz += sizeof(MIB_IFROW) * 8;                        /* 余量：枚举间可能新增 */
    }
    tb = (MIB_IFTABLE *)malloc(sz);
    if (!tb) return -1;
    r = GetIfTable(tb, &sz, FALSE);
    if (r == NO_ERROR) {
        for (i = 0; i < (int)tb->dwNumEntries; i++) {
            if (tb->table[i].dwType == IF_TYPE_SOFTWARE_LOOPBACK ||
                tb->table[i].dwType == IF_TYPE_TUNNEL)
                continue;
            *bytes += (ULONGLONG)tb->table[i].dwInOctets + tb->table[i].dwOutOctets;
        }
    }
    free(tb);
    return r == NO_ERROR ? 0 : -1;
}
#endif /* PET_COMPAT_NEED_NET */

/* ---------- 宽字符 UTF-8 读文件 ----------
 * 原来一律 fopen(窄字符 ANSI)，中文用户名/中文安装路径下读不到 settings.json。
 * 统一走 _wfopen + UTF-8 字节流。返回 malloc 的 NUL 结尾缓冲，失败 NULL。
 */
static inline char *compat_read_file_w(const WCHAR *path, int *len_out) {
    FILE *f;
    long len;
    char *buf;
    size_t got;
    if (len_out) *len_out = 0;
    if (!path || !path[0]) return NULL;
    f = _wfopen(path, L"rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = 0;
    if (len_out) *len_out = (int)got;
    return buf;
}

/* ---------- OpenAI 兼容端点解析 ----------
 * 原来只有 inference_backend.c 里一份 parse_base()，ma.c 把 host/path 写死成宏。
 * 两份实现迟早漂移，统一收口到这里。
 * 用 WORD 存端口，避免让只 include windows.h 的 pet.c 被迫依赖 winhttp.h。
 */
typedef struct {
    WCHAR host[128];
    WCHAR path[256];
    WORD  port;
    int   secure;
} PetEndpoint;

static inline void compat_ep_default(PetEndpoint *ep) {
    lstrcpynW(ep->host, L"dashscope.aliyuncs.com", 128);
    lstrcpynW(ep->path, L"/compatible-mode/v1/chat/completions", 256);
    ep->port = 443;
    ep->secure = 1;
}

/* "[scheme://]host[:port][/prefix]" → host + prefix/chat/completions
 * 无 scheme 按 https 处理。返回 0 表示解析不出主机名（调用方保持默认值）。 */
static inline int compat_parse_base(const char *url, PetEndpoint *ep) {
    char h[128], p[200], full[260];
    const char *s = url;
    int i = 0, n, j = 0, port;
    if (!url || !url[0] || !ep)
        return 0;
    if (strnicmp(s, "https://", 8) == 0) { s += 8; ep->secure = 1; }
    else if (strnicmp(s, "http://", 7) == 0) { s += 7; ep->secure = 0; }
    else ep->secure = 1;
    while (s[i] && s[i] != '/' && s[i] != ':' && i < 127) { h[i] = s[i]; i++; }
    h[i] = 0;
    if (!h[0])
        return 0;
    n = i;
    ep->port = ep->secure ? 443 : 80;
    if (s[n] == ':') {
        port = 0; n++;
        while (s[n] >= '0' && s[n] <= '9') { port = port * 10 + (s[n] - '0'); n++; }
        if (port > 0 && port < 65536)
            ep->port = (WORD)port;
    }
    if (s[n] == '/') {
        while (s[n] && j < 199) p[j++] = s[n++];
        p[j] = 0;
        while (j > 0 && p[j - 1] == '/') p[--j] = 0;
    } else p[0] = 0;
    snprintf(full, sizeof full, "%s/chat/completions", p);
    if (MultiByteToWideChar(CP_UTF8, 0, h, -1, ep->host, 128) == 0)
        return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, full, -1, ep->path, 256) == 0)
        return 0;
    return 1;
}

/* dashscope 专属参数，换服务商时不能盲发（严格端点对未知字段会 400） */
static inline int compat_ep_is_dashscope(const PetEndpoint *ep) {
    return wcsstr(ep->host, L"dashscope") != NULL;
}

#endif /* PET_COMPAT_H */
