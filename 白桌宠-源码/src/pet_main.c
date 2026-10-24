/* pet_main.c —— 桌宠聊天管道示例：Unicode UI + 匿名管道 + 专用读取线程
 *
 * 编译（MinGW-w64，两个 exe 放同一目录）:
 *   gcc -O2 -mwindows -o pet_demo.exe pet_main.c
 *   gcc -O2 -o inference_backend.exe inference_backend.c
 *
 * 数据流（管道内统一 UTF-8）:
 *   UI 线程采集输入框（宽字符）
 *     -> BuildPrompt() 宽转 UTF-8 + 中文指令模板
 *     -> StartInferenceEngine() 建三根匿名管道 + CreateProcess(inference_backend.exe)
 *     -> WriteFile 把长度前缀请求写进子进程 stdin
 *     -> 子进程把 token 流到 stdout、调试流到 stderr
 *     -> 两个专用读取线程阻塞 ReadFile（绝不放 UI 主线程）
 *     -> UTF-8 半字对齐后 MultiByteToWideChar
 *     -> 堆复制宽字符串 PostMessage（跨线程只能传堆指针）
 *     -> UI 线程 EM_SETSEL/EM_REPLACESEL 追加进气泡 EDIT，用完 free
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 自定义消息。契约：wParam/lParam 携带的 wchar_t* 是读取线程 malloc 的堆副本，
 * UI 线程用完必须 free（同一 exe 同一 CRT，跨线程 malloc/free 安全） */
#define WM_TOKEN_TEXT  (WM_APP + 1) /* 一批 token（stdout） */
#define WM_STREAM_ERR  (WM_APP + 2) /* 一行调试（stderr） */
#define WM_STREAM_DONE (WM_APP + 3) /* wParam=退出码, lParam=wchar_t* 小结（堆） */

#define ID_EDIT_BUBBLE 101
#define ID_EDIT_INPUT  102
#define ID_BTN_SEND    103

typedef struct {
    char prompt[2048];     /* UTF-8，含指令模板 */
    int  max_tokens;       /* 规格硬限 <=512，示例取 256，防本地推理爆显存 */
    int  temperature_x100; /* 0.7 -> 70：整数过管道，避免浮点文本的区域设置歧义 */
} ModelRequest;

static HINSTANCE g_hinst;
static HWND g_hwnd, g_bubble, g_input, g_btn;
static HANDLE g_proc = NULL;        /* 子进程句柄，由 stdout 读取线程收尾关闭 */
static volatile LONG g_busy = 0;    /* 一次只允许一个在途请求 */
static WNDPROC g_inputProc = NULL;  /* 输入框原窗口过程（子类化用） */

typedef struct {
    HANDLE hRead;
    HWND   hwnd;
    UINT   msg;
    int    isStdout;
} ReaderCtx;

/* ------------------------------------------------------------------ */
/* UTF-8 工具                                                          */
/* ------------------------------------------------------------------ */

/* 返回 buf[0..len) 里"完整 UTF-8 字符"的前缀长度，结尾不完整的留到下一轮。
 * 为什么必须有：ReadFile 按管道缓冲切块，3 字节汉字会被劈成两次读；
 * 半截字符直接喂 MultiByteToWideChar 会变 U+FFFD —— 这是"中文乱码"的
 * 头号真凶，跟编码选错无关。 */
static int utf8_complete_len(const char *buf, int len) {
    int lead = len - 1, back = 0;
    unsigned char c;
    int need, have;
    while (lead >= 0 && back < 3 && ((unsigned char)buf[lead] & 0xC0) == 0x80) {
        lead--;              /* 0x80..0xBF 都是后续字节，往回找首字节 */
        back++;
    }
    if (lead < 0)
        return len;          /* 找不到首字节：脏数据整体放行，转出来顶多是替换符 */
    c = (unsigned char)buf[lead];
    need = (c < 0x80) ? 1 : (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    have = len - lead;
    return (have < need) ? lead : len;
}

/* 把一批 UTF-8 字节转宽字符并 PostMessage 给 UI。
 * 必须堆分配复制：PostMessage 异步入队，UI 处理时本线程栈早已翻篇，
 * 传栈指针 = 野指针（规格点名要修的坑）。 */
static void post_text(HWND hwnd, UINT msg, const char *u8, int len) {
    int wlen;
    wchar_t *w;
    if (len <= 0)
        return;
    wlen = MultiByteToWideChar(CP_UTF8, 0, u8, len, NULL, 0);
    if (wlen <= 0)
        return;
    w = (wchar_t *)malloc((size_t)(wlen + 1) * sizeof(wchar_t));
    if (!w)
        return;
    MultiByteToWideChar(CP_UTF8, 0, u8, len, w, wlen);
    w[wlen] = 0;
    if (!PostMessageW(hwnd, msg, (WPARAM)w, 0))
        free(w);             /* 窗口已销毁投递失败：线程侧自己释放，否则泄漏 */
}

/* ------------------------------------------------------------------ */
/* 读取线程：阻塞 ReadFile 只能在这里，绝不在 UI 线程（否则窗口卡死）   */
/* ------------------------------------------------------------------ */
static DWORD WINAPI reader_thread(LPVOID p) {
    ReaderCtx *ctx = (ReaderCtx *)p;
    char buf[1024];
    char acc[2048];
    int accLen = 0;
    DWORD got;
    for (;;) {
        /* ReadFile 失败(ERROR_BROKEN_PIPE=子进程关了写端)或读到 0 字节都是流结束 */
        if (!ReadFile(ctx->hRead, buf, sizeof buf, &got, NULL) || got == 0)
            break;
        if (ctx->isStdout) {
            if (accLen + (int)got > (int)sizeof acc)
                accLen = 0;  /* 溢出保护：token 块很小，正常到不了这里 */
            memcpy(acc + accLen, buf, got);
            accLen += (int)got;
            {
                int use = utf8_complete_len(acc, accLen);
                if (use > 0) {
                    post_text(ctx->hwnd, ctx->msg, acc, use);
                    memmove(acc, acc + use, (size_t)(accLen - use));
                    accLen -= use;
                }
            }
        } else {
            post_text(ctx->hwnd, ctx->msg, buf, (int)got); /* stderr 直通，按块转发 */
        }
    }
    if (accLen > 0)
        post_text(ctx->hwnd, ctx->msg, acc, accLen); /* 收尾残字节（可能是被截断的字符）也交给 UI */
    if (ctx->isStdout) {
        DWORD code = 0;
        if (g_proc) {
            /* stdout 已 EOF 说明子进程关了全部输出，短等本体退出拿退出码 */
            WaitForSingleObject(g_proc, 5000);
            GetExitCodeProcess(g_proc, &code);
            CloseHandle(g_proc); /* 进程句柄父进程必须关，否则内核对象泄漏 */
            g_proc = NULL;
        }
        {
            wchar_t *done = (wchar_t *)malloc(64 * sizeof(wchar_t));
            if (done) {
                _snwprintf(done, 63, L"\n[子进程退出 code=%lu]\n", code);
                done[63] = 0;
                if (!PostMessageW(ctx->hwnd, WM_STREAM_DONE, code, (LPARAM)done))
                    free(done);
            }
        }
    }
    CloseHandle(ctx->hRead); /* 读完关读端，与 StartInferenceEngine 关写端对称 */
    free(ctx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 请求封装与子进程启动                                                 */
/* ------------------------------------------------------------------ */

/* 宽字符输入 -> UTF-8 请求。模板按规格写死：简洁中文纯文本、禁代码块/JSON */
static void BuildPrompt(const wchar_t *user_input, ModelRequest *req) {
    char u8[1200];
    int n = WideCharToMultiByte(CP_UTF8, 0, user_input, -1, u8, (int)sizeof u8, NULL, NULL);
    if (n <= 0)
        u8[0] = 0;
    req->max_tokens = 256;       /* 规格：256 建议值，硬上限 512 */
    req->temperature_x100 = 70;  /* 0.7 */
    snprintf(req->prompt, sizeof req->prompt,
             "你是桌宠「白」。用简洁中文回答，不要代码块，不要 JSON，"
             "只输出纯文本，不超过三句话。\n用户说：%s", u8);
}

static BOOL StartInferenceEngine(const ModelRequest *req) {
    SECURITY_ATTRIBUTES sa;
    HANDLE in_r, in_w, out_r, out_w, err_r, err_w;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe[MAX_PATH + 32];
    wchar_t *slash;
    ReaderCtx *c;
    HANDLE th;
    char head[64];
    DWORD wr;
    int hlen, plen;

    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;    /* 管道的"子端"必须可继承，CreateProcess 才带得走 */
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 0))
        return FALSE;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r); CloseHandle(in_w);
        return FALSE;
    }
    /* 第三根管道专走 stderr：调试信息不进 token 流（规格红线） */
    if (!CreatePipe(&err_r, &err_w, &sa, 0)) {
        CloseHandle(in_r); CloseHandle(in_w); CloseHandle(out_r); CloseHandle(out_w);
        return FALSE;
    }
    /* 不能省的一步：父端句柄全部摘掉继承位。本机实测过的死法——
     * 子进程继承了父端的 in_w 副本，父写完关闭后写端引用不归零，
     * 子进程 stdin 永远等不到 EOF，双方一起挂死。out_r/err_r 同理。 */
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    /* 写请求：两行 ASCII 头 + 长度前缀 prompt 原始字节。
     * 长度前缀的理由：prompt 里有中文/空格/引号/换行，塞命令行或单行文本
     * 都得转义，错一处就崩；"字节数+原始字节"零歧义。
     * 总量 <2KB，落得进管道缓冲，先写完再 CreateProcess 不会阻塞。 */
    plen = (int)strlen(req->prompt);
    hlen = snprintf(head, sizeof head, "%d %d\n%d\n",
                    req->max_tokens, req->temperature_x100, plen);
    WriteFile(in_w, head, (DWORD)hlen, &wr, NULL);
    WriteFile(in_w, req->prompt, (DWORD)plen, &wr, NULL);
    CloseHandle(in_w);           /* 写完立刻关父端写端，子进程读完 prompt 即见 EOF */

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = in_r;        /* 三个标准句柄必须全是有效且可继承的句柄； */
    si.hStdOutput = out_w;       /* 任何一个给 NULL，子进程 printf/fprintf(stderr) */
    si.hStdError  = err_w;       /* 就落在无效句柄上，轻则丢日志重则写失败 */

    GetModuleFileNameW(NULL, exe, MAX_PATH);  /* 约定后端与本 exe 同目录 */
    slash = wcsrchr(exe, L'\\');
    if (slash)
        *(slash + 1) = 0;
    wcscat(exe, L"inference_backend.exe");

    memset(&pi, 0, sizeof pi);
    /* bInheritHandles=TRUE：让子进程拿到 si 里的三个句柄。
     * CREATE_NO_WINDOW：不弹黑窗。本机实测修正：只要句柄继承修对，
     * 无控制台不会挂死，不需要给子进程开隐藏控制台。 */
    if (!CreateProcessW(exe, NULL, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(in_r); CloseHandle(out_r); CloseHandle(out_w);
        CloseHandle(err_r); CloseHandle(err_w);
        return FALSE;
    }
    /* 无论成败，父进程立刻关三个"子端"：子进程已持有自己的副本；
     * 父留着 out_w/err_w，子退出后写端引用不归零，
     * 读取线程永远等不到 EOF —— 线程挂死、句柄泄漏双杀。 */
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    CloseHandle(pi.hThread);     /* 线程句柄用不上，早关不影响线程运行 */
    g_proc = pi.hProcess;        /* 进程句柄留给 stdout 线程收退出码后关闭 */

    c = (ReaderCtx *)malloc(sizeof *c);
    if (!c) { CloseHandle(out_r); CloseHandle(err_r); return FALSE; }
    c->hRead = out_r; c->hwnd = g_hwnd; c->msg = WM_TOKEN_TEXT; c->isStdout = 1;
    th = CreateThread(NULL, 0, reader_thread, c, 0, NULL);
    if (th) CloseHandle(th);     /* 线程句柄即关，线程照跑 */
    else { CloseHandle(out_r); free(c); }

    c = (ReaderCtx *)malloc(sizeof *c);
    if (!c) { CloseHandle(err_r); return FALSE; }
    c->hRead = err_r; c->hwnd = g_hwnd; c->msg = WM_STREAM_ERR; c->isStdout = 0;
    th = CreateThread(NULL, 0, reader_thread, c, 0, NULL);
    if (th) CloseHandle(th);
    else { CloseHandle(err_r); free(c); }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static void append_bubble(const wchar_t *text) {
    int len = lstrlenW(text), i, j = 0;
    wchar_t *buf;
    int end;
    if (len <= 0)
        return;
    buf = (wchar_t *)malloc((size_t)(len * 2 + 4) * sizeof(wchar_t));
    if (!buf)
        return;
    /* 多行 EDIT 内部换行是 \r\n；模型输出常是裸 \n，不规范化就不换行 */
    for (i = 0; i < len; i++) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r'))
            buf[j++] = L'\r';
        buf[j++] = text[i];
    }
    buf[j] = 0;
    end = GetWindowTextLengthW(g_bubble);
    SendMessageW(g_bubble, EM_SETSEL, (WPARAM)end, (LPARAM)end); /* 光标移到末尾 */
    SendMessageW(g_bubble, EM_REPLACESEL, FALSE, (LPARAM)buf);   /* 在光标处追加 */
    SendMessageW(g_bubble, EM_SCROLLCARET, 0, 0);                /* 滚到底 */
    free(buf);
}

static void do_send(void) {
    wchar_t in[1024];
    ModelRequest req;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        append_bubble(L"[上一条还在回复中，等一下……]\n");
        return;
    }
    GetWindowTextW(g_input, in, 1023);
    in[1023] = 0;
    if (!in[0]) {
        InterlockedExchange(&g_busy, 0);
        return;
    }
    SetWindowTextW(g_input, L"");
    EnableWindow(g_btn, FALSE);
    append_bubble(L"我：");
    append_bubble(in);
    append_bubble(L"\n白：");
    BuildPrompt(in, &req);
    if (!StartInferenceEngine(&req)) {
        append_bubble(L"[启动 inference_backend.exe 失败——它和本 exe 在同一目录吗？]\n");
        InterlockedExchange(&g_busy, 0);
        EnableWindow(g_btn, TRUE);
    }
}

/* 子类化输入框：回车即发送（单行 EDIT 默认把 VK_RETURN 吃掉只"叮"一声） */
static LRESULT CALLBACK InputProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        do_send();
        return 0;
    }
    if (m == WM_NCDESTROY && g_inputProc) {
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)g_inputProc);
        g_inputProc = NULL;
    }
    if (g_inputProc)
        return CallWindowProcW(g_inputProc, h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT); /* 默认位图字体没中文，必须换 GUI 字体 */
        g_bubble = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            10, 10, 520, 300, h, (HMENU)(INT_PTR)ID_EDIT_BUBBLE, g_hinst, NULL);
        g_input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOVSCROLL,
            10, 320, 410, 30, h, (HMENU)(INT_PTR)ID_EDIT_INPUT, g_hinst, NULL);
        g_btn = CreateWindowExW(0, L"BUTTON", L"发送",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            430, 320, 100, 30, h, (HMENU)(INT_PTR)ID_BTN_SEND, g_hinst, NULL);
        SendMessageW(g_bubble, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(g_input, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(g_btn, WM_SETFONT, (WPARAM)f, TRUE);
        g_inputProc = (WNDPROC)SetWindowLongPtrW(g_input, GWLP_WNDPROC, (LONG_PTR)InputProc);
        append_bubble(L"[管道示例就绪。输入中文，回车或点「发送」。]\n");
        return 0;
    }
    case WM_SIZE: {
        int cw = LOWORD(l), ch = HIWORD(l);
        if (g_bubble) MoveWindow(g_bubble, 10, 10, cw - 20, ch - 95, TRUE);
        if (g_input)  MoveWindow(g_input, 10, ch - 72, cw - 130, 30, TRUE);
        if (g_btn)    MoveWindow(g_btn, cw - 110, ch - 72, 100, 30, TRUE);
        return 0;
    }
    case WM_TOKEN_TEXT:
        if (w) { append_bubble((wchar_t *)w); free((void *)w); } /* 契约：UI 用完释放 */
        return 0;
    case WM_STREAM_ERR:
        if (w) { append_bubble(L"[err] "); append_bubble((wchar_t *)w); free((void *)w); }
        return 0;
    case WM_STREAM_DONE:
        if (l) { append_bubble((wchar_t *)l); free((void *)l); }
        InterlockedExchange(&g_busy, 0);
        EnableWindow(g_btn, TRUE);
        SetFocus(g_input);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == ID_BTN_SEND && HIWORD(w) == BN_CLICKED) {
            do_send();
            return 0;
        }
        break;
    case WM_DESTROY:
        /* 关窗时子进程若还在跑必须先终止：否则读取线程的 ReadFile 永不返回，
         * 线程、句柄全泄漏。TerminateProcess 后管道断开，线程自然走 EOF 收尾。 */
        if (g_proc) {
            TerminateProcess(g_proc, 9);
            CloseHandle(g_proc);
            g_proc = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE u1, LPSTR u2, int nShow) {
    WNDCLASSW wc;
    MSG msg;
    (void)u1; (void)u2;
    g_hinst = hInst;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"PetDemoChat";   /* Unicode 窗口：类名/标题/控件全走 W 系 API，
                                            中文不经过 ANSI 代码页，从源头杜绝乱码 */
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, L"PetDemoChat", L"桌宠聊天管道示例（白）",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             560, 480, NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);  /* 把按键消息翻成 WM_CHAR，少了它输入框收不到字符 */
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
