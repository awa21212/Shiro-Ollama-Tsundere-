// 桌宠 C 版 v2：互动核心 + 输入对话框（回车接通 qwen-code 无头调用）
// 编译: gcc -O2 -mwindows -o pet.exe pet.c -lgdiplus
#include <windows.h>
#ifndef __cplusplus
typedef float REAL;
#endif
#include <gdiplus/gdiplusenums.h>
#include <gdiplus/gdipluspixelformats.h>
#include <gdiplus/gdiplustypes.h>
#include <gdiplus/gdiplusgpstubs.h>
#include <gdiplus/gdiplusinit.h>

/* MinGW 的 gdiplusflat.h 在 C 下类型循环依赖，无法整体包含；只声明用到的函数 */
extern GpStatus WINGDIPAPI GdipCreateBitmapFromFile(const WCHAR *, GpBitmap **);
extern GpStatus WINGDIPAPI GdipCreateFromHDC(HDC, GpGraphics **);
extern GpStatus WINGDIPAPI GdipGraphicsClear(GpGraphics *, ARGB);
extern GpStatus WINGDIPAPI GdipDrawImageRectI(GpGraphics *, GpImage *, INT, INT, INT, INT);
extern GpStatus WINGDIPAPI GdipCloneBitmapAreaI(INT, INT, INT, INT, PixelFormat, GpBitmap *, GpBitmap **);
extern GpStatus WINGDIPAPI GdipCreateFontFamilyFromName(const WCHAR *, const GpFontCollection *, GpFontFamily **);
extern GpStatus WINGDIPAPI GdipCreateFont(const GpFontFamily *, REAL, INT, INT, GpFont **);
extern GpStatus WINGDIPAPI GdipCreateStringFormat(INT, LANGID, GpStringFormat **);
extern GpStatus WINGDIPAPI GdipSetStringFormatAlign(GpStringFormat *, INT);
extern GpStatus WINGDIPAPI GdipSetStringFormatLineAlign(GpStringFormat *, INT);
extern GpStatus WINGDIPAPI GdipCreateSolidFill(ARGB, GpSolidFill **);
extern GpStatus WINGDIPAPI GdipDrawString(GpGraphics *, const WCHAR *, INT, const GpFont *, const RectF *, const GpStringFormat *, const GpBrush *);
extern GpStatus WINGDIPAPI GdipDeleteBrush(GpBrush *);
extern GpStatus WINGDIPAPI GdipDeleteStringFormat(GpStringFormat *);
extern GpStatus WINGDIPAPI GdipDeleteFont(GpFont *);
extern GpStatus WINGDIPAPI GdipDeleteFontFamily(GpFontFamily *);
extern GpStatus WINGDIPAPI GdipDeleteGraphics(GpGraphics *);
extern GpStatus WINGDIPAPI GdipMeasureString(GpGraphics *, const WCHAR *, INT, const GpFont *, const RectF *, const GpStringFormat *, RectF *, INT *, INT *);
extern GpStatus WINGDIPAPI GdipCreatePath(INT, GpPath **);
extern GpStatus WINGDIPAPI GdipAddPathArc(GpPath *, REAL, REAL, REAL, REAL, REAL, REAL);
extern GpStatus WINGDIPAPI GdipClosePathFigure(GpPath *);
extern GpStatus WINGDIPAPI GdipFillPath(GpGraphics *, GpBrush *, GpPath *);
extern GpStatus WINGDIPAPI GdipDrawPath(GpGraphics *, GpPen *, GpPath *);
extern GpStatus WINGDIPAPI GdipCreatePen1(ARGB, REAL, INT, GpPen **);
extern GpStatus WINGDIPAPI GdipDeletePen(GpPen *);
extern GpStatus WINGDIPAPI GdipDeletePath(GpPath *);
extern GpStatus WINGDIPAPI GdipDrawLineI(GpGraphics *, GpPen *, INT, INT, INT, INT);
extern GpStatus WINGDIPAPI GdipGetImageGraphicsContext(GpImage *, GpGraphics **);
extern GpStatus WINGDIPAPI GdipCreateBitmapFromScan0(INT, INT, INT, PixelFormat, BYTE *, GpBitmap **);
extern GpStatus WINGDIPAPI GdipBitmapLockBits(GpBitmap *, const GpRect *, UINT, PixelFormat, void *);
extern GpStatus WINGDIPAPI GdipBitmapUnlockBits(GpBitmap *, void *);
extern GpStatus WINGDIPAPI GdipDisposeImage(GpImage *);
extern GpStatus WINGDIPAPI GdipTranslateWorldTransform(GpGraphics *, REAL, REAL, INT);
extern GpStatus WINGDIPAPI GdipScaleWorldTransform(GpGraphics *, REAL, REAL, INT);
extern GpStatus WINGDIPAPI GdipResetWorldTransform(GpGraphics *);
extern GpStatus WINGDIPAPI GdipFillEllipseI(GpGraphics *, GpBrush *, INT, INT, INT, INT);
extern GpStatus WINGDIPAPI GdipFillRectangleI(GpGraphics *, GpBrush *, INT, INT, INT, INT);
extern GpStatus WINGDIPAPI GdipAddPathEllipse(GpPath *, REAL, REAL, REAL, REAL);
extern GpStatus WINGDIPAPI GdipAddPathRectangleI(GpPath *, INT, INT, INT, INT);
/* DPI API 一律运行时解析（见 compat.h）：这两个是 Win10 1703+ 导出，
   静态 extern 会让老系统在 loader 阶段就报错，exe 双击根本进不到 main */

/* gdiplusimaging.h 无法在 C 下包含，按 MS 布局自定义 */
typedef struct {
    UINT Width;
    UINT Height;
    INT Stride;
    INT PixelFormat;
    BYTE *Scan0;
    UINT_PTR Reserved;
} BitmapDataX;
#define PF_32BPP_ARGB  0x0026200A
#define PF_32BPP_PARGB 0x000E200B
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>
#include <iphlpapi.h>
#define PET_COMPAT_NEED_NET
#include "compat.h"
#include "bubble.h"

#define IDT_BLINK     1
#define IDT_BLINKOFF  2
#define IDT_TALK      3
#define IDT_BOW       4
#define IDT_CARET     5
#define IDT_QWEN      6
#define IDT_WALK      7
#define IDT_STEP      8
#define IDT_HOVER     9
#define IDT_SQUASH    10
#define IDT_SYSINFO   11
#define IDT_INTRO     12
#define IDT_EXITANIM  13
#define IDT_RANDOM    14
#define IDT_ACT       15
#define IDT_ALARM     16
#define IDT_RING      17
#define IDT_TURN      18
#define IDM_EXIT      1001
#define IDM_WALK      1002
#define IDM_CPU       1010
#define IDM_MEM       1011
#define IDM_DISK      1012
#define IDM_NET       1013
#define IDM_ALARM     1020
#define IDM_SIZE_BASE 1100
#define IDM_BACKEND_QWEN  1030
#define IDM_BACKEND_LOCAL 1031
#define IDM_BACKEND_MA    1032
#define IDM_ROLE_BASE     1040   /* +0 主管 +1 员工A +2 B +3 C；B/C 灰显（enabled=0） */
#define WM_QWEN_DONE  (WM_APP + 1)
#define WM_STREAM_TOKEN (WM_APP + 2)  /* wParam=请求id, lParam=堆 WCHAR*（UI free） */
#define WM_STREAM_END   (WM_APP + 3)  /* wParam=请求id, lParam=退出码 */

#define BASE_H 400
#define WALK_MAX 24              /* 散步帧数组上限；哥哥以后给更多帧不用改代码 */
#define WALK_CYCLE_MS 1430   /* 一个完整步态周期的时长；帧间隔 = 周期/帧数，换素材不改速度 */
#define HEAD_RATIO 0.38f
#define VIS_CHARS 12
#define PIN_CHARS 4

/* 聊天气泡.png 九宫格切片边界（源像素，alpha 实测）：本体 x[316..2080] y[296..1000]，
   左卷轴/右折角/上下边不拉伸，只拉中间素纸；羽毛笔是本体下独立装饰；
   右下 alpha 岛是没抠掉的"豆包 AI 生成"水印，不画 */
#define BUB_SRC_X 316
#define BUB_SRC_Y 296
#define BUB_SRC_W 1764
#define BUB_SRC_H 704
#define BUB_SL_L  329
#define BUB_SL_R  274
#define BUB_SL_T  74
#define BUB_SL_B  110
#define QUIL_SRC_X 1364
#define QUIL_SRC_Y 1087
#define QUIL_SRC_W 147
#define QUIL_SRC_H 129

/* 全部路径相对 exe 目录：整个文件夹拷走也能跑。
   缓冲统一 1024，别用 MAX_PATH —— 深层安装目录会静默截断 */
#define PET_PATH_LEN 1024
static WCHAR g_asset_dir[PET_PATH_LEN], g_log_path[PET_PATH_LEN], g_errlog_path[PET_PATH_LEN],
             g_backend_path[PET_PATH_LEN], g_ma_path[PET_PATH_LEN], g_memfile[PET_PATH_LEN];
/* qwen CLI 路径运行时探测（原来写死 C:\Users\<user>\AppData\Roaming\npm\qwen.cmd） */
static WCHAR g_qwen_path[PET_PATH_LEN * 2];
static BOOL g_path_probe_fail;   /* exe 目录探测失败 → 退化成 .\ ，日志里报出来 */
static const char *QWEN_ARGS =
    "--channel desktop --output-format json --approval-mode plan "
    "--max-tool-calls 0 --chat-recording false --safe-mode --input-format text";
static const char *LINES[] = {
    "……哥哥，摸摸头。",
    "哼，才不是喜欢你。",
    "白，最强。",
    "……陪白玩一会。",
    "哥哥，笨蛋。",
    "……这里，痒。",
    "♪ 哥哥回来了。",
};
#define LINE_COUNT (sizeof(LINES) / sizeof(LINES[0]))

static GpBitmap *img_stand1, *img_stand2, *img_kneel1, *img_drag, *img_bubble;
static GpBitmap *img_inbar;
static GpBitmap *img_walk[WALK_MAX];   /* 散步关键帧，帧数运行时探测（遇缺即停） */
static int walk_n;
static GpBitmap *img_front, *img_back, *img_blinkf;   /* 正面/背面/正面闭眼帧 */
static GpBitmap *img_kneelb;   /* 跪坐闭眼帧：与 img_kneel1 同机位同画布，跪坐中才敢眨 */
static GpBitmap *bub_slice[10];
static GpBitmap *cur;
static HWND hwnd;
static HDC memDC;
static HBITMAP dib, dib_old;
static void *dib_bits;
static GpBitmap *canvas;
static int winW, winH, sprW, sprH, bubW, bubH, sprX, sprY;
static int inX, inY, inW, inH, fontIn;
static int pct = 60;
static UINT sys_dpi = 96;
static BOOL talking, bowing, dragging, captured;
static BOOL dialog_open, caret_on, waiting, reply_mode;
static BOOL walk_on, squash, sitting;
static int walk_dir = 1, walk_pause, step_phase, hover_region = -1;
static int turn_stage;   /* 撞墙转向：0 无 1 背面 2 正面 */
static RECT workarea;
static BOOL show_cpu, show_mem, show_disk, show_net;
static BOOL simple_mode, q_mode, exiting, intro, acting;
static BOOL alarm_set, alarm_input_mode, ringing;
static int alarm_h, alarm_m, ring_count, panH, bubY, info_n, bubTailH;
static WCHAR info_lines[4][64];
static ULONGLONG cpu_prev_idle, cpu_prev_total, net_prev_bytes;
static DWORD net_prev_ms;
static GpBitmap *img_q;
static POINT downPt;
static POINT grabOff;
static WCHAR inp[512];
static int inp_len;
static WCHAR talkText[1024];
/* 本地推理管道（v2 方案接线）：backend_local 选路；id 只动 UI 线程，无锁 */
static int backend_local, stream_started, ma_role;   /* ma_role: 0 主管 1 员工A */
/* 多实例：一个 exe 三只白 —— --slot N --name X --model M --backend qwen|local|ma
   三只共用同一个 dashscope 端点 = 同一把 key（settings.json），只有模型不同 */
static WCHAR selfName[24] = L"白";
static WCHAR selfModel[64], skinArg[64];
static WCHAR g_skin_dir[PET_PATH_LEN * 2];   /* assets\<名字>\：同名文件优先覆盖公共形象 */
static int selfSlot;
static unsigned long long req_counter, cur_req;

static void render(void);

static void dbg(const char *fmt, ...) {
    FILE *lf;
    va_list ap;
    lf = _wfopen(g_log_path, L"a");
    if (!lf)
        return;
    fprintf(lf, "[%lu] ", GetCurrentProcessId());   /* 多实例共写一份日志，pid 前缀分辨 */
    va_start(ap, fmt);
    vfprintf(lf, fmt, ap);
    va_end(ap);
    fputc('\n', lf);
    fclose(lf);
}

static void utf8w(const char *s, WCHAR *out, int cap) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
}

static void init_paths(void) {
    WCHAR base[PET_PATH_LEN];
    if (!compat_exe_dir(base, PET_PATH_LEN)) {
        lstrcpyW(base, L".\\");     /* 拿不到 exe 目录也要能跑，绝不 ExitProcess */
        g_path_probe_fail = TRUE;
    }
    lstrcpyW(g_asset_dir, base);    lstrcatW(g_asset_dir, L"assets\\");
    lstrcpyW(g_log_path, base);     lstrcatW(g_log_path, L"pet_debug.log");
    lstrcpyW(g_errlog_path, base);  lstrcatW(g_errlog_path, L"pet_child_err.log");
    lstrcpyW(g_backend_path, base); lstrcatW(g_backend_path, L"inference_backend.exe");
    lstrcpyW(g_ma_path, base);      lstrcatW(g_ma_path, L"ma.exe");
    {
        WCHAR md[PET_PATH_LEN];
        lstrcpyW(md, base);
        lstrcatW(md, L"memory");
        CreateDirectoryW(md, NULL);
    }
    lstrcpyW(g_memfile, base);      lstrcatW(g_memfile, L"memory\\about_bro.txt");
}

static GpBitmap *try_load_png(const WCHAR *dir, const WCHAR *wname) {
    WCHAR wpath[PET_PATH_LEN * 2];
    GpBitmap *bmp = NULL;
    if (lstrlenW(dir) + lstrlenW(wname) + 1 >= (int)(sizeof wpath / sizeof wpath[0]))
        return NULL;                       /* 拼得下才拼，别信 lstrcat 不越界 */
    lstrcpyW(wpath, dir);
    lstrcatW(wpath, wname);
    if (GdipCreateBitmapFromFile(wpath, &bmp) != Ok)
        return NULL;
    return bmp;
}

static GpBitmap *placeholder_bitmap(int w, int h) {
    /* 必需素材缺失时给一张半透明占位图：桌宠照样跑，不再闪退 */
    GpBitmap *bmp = NULL;
    ARGB *px;
    int i;
    px = (ARGB *)malloc((size_t)w * h * sizeof(ARGB));
    if (!px)
        return NULL;
    for (i = 0; i < w * h; i++)
        px[i] = 0x66C9B6E4;
    if (GdipCreateBitmapFromScan0(w, h, w * 4, PF_32BPP_ARGB, (BYTE *)px, &bmp) != Ok) {
        free(px);
        return NULL;
    }
    return bmp;   /* px 归 bitmap 持有，进程生命周期内不释放 */
}

static GpBitmap *load_png(const char *name) {
    WCHAR wname[256];
    GpBitmap *bmp = NULL;
    utf8w(name, wname, 256);
    /* 皮肤优先：assets\<名字>\<name> 命中即用，否则回落公共 assets */
    if (g_skin_dir[0])
        bmp = try_load_png(g_skin_dir, wname);
    if (!bmp)
        bmp = try_load_png(g_asset_dir, wname);
    if (!bmp) {
        /* 原来是 MessageBoxW + ExitProcess(1)：换台机器素材没拷全就直接闪退。
           改成占位图继续跑，缺哪张记进日志由人查 */
        dbg("asset missing, placeholder used: %s", name);
        bmp = placeholder_bitmap(433, 617);
    }
    return bmp;
}

static GpBitmap *load_png_opt(const char *name) {
    WCHAR wname[256];
    GpBitmap *bmp = NULL;
    utf8w(name, wname, 256);
    if (g_skin_dir[0])
        bmp = try_load_png(g_skin_dir, wname);
    if (!bmp)
        bmp = try_load_png(g_asset_dir, wname);
    return bmp;
}

static void rebuild_dib(void) {
    BITMAPINFO bmi;
    void *bits;
    if (dib) {
        SelectObject(memDC, dib_old);
        DeleteObject(dib);
    }
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = winW;
    bmi.bmiHeader.biHeight = -winH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    dib_bits = bits;
    dib_old = SelectObject(memDC, dib);
    if (canvas)
        GdipDisposeImage((GpImage *)canvas);
    GdipCreateBitmapFromScan0(winW, winH, winW * 4, PF_32BPP_ARGB, NULL, &canvas);
}

static void layout(void) {
    sprH = BASE_H * pct / 100 * (int)sys_dpi / 96;
    sprW = sprH * 455 / 648;
    {
        /* 气泡尺寸走 bubble.h 引擎：字数定宽高，再按 pct/DPI 缩到设计单位之外 */
        ChatBubble_t bb;
        double sc = (double)pct / 100 * (double)sys_dpi / 96;
        RenderBubbleW(&bb, talking ? talkText : L"", workarea.right - workarea.left);
        bubW = (int)(bb.bubble_width * sc);
        bubH = (int)(bb.bubble_height * sc);
        if (bubW < sprW)
            bubW = sprW;
        bubTailH = (int)(bubH * (double)QUIL_SRC_H / BUB_SRC_H);
    }
    fontIn = sprH / 12;
    if (fontIn < 14)
        fontIn = 14;
    inH = fontIn * 2;
    inW = fontIn * (VIS_CHARS + 1);
    /* 输入条素材 1185x329（羽化重抠版，输入框白底完整）；等比缩放 */
    if (img_inbar)
        inH = inW * 329 / 1185;
    info_n = (show_cpu ? 1 : 0) + (show_mem ? 1 : 0) + (show_disk ? 1 : 0) + (show_net ? 1 : 0);
    panH = info_n ? info_n * (fontIn + 6) + 12 : 0;
    bubY = panH;
    winW = bubW > inW ? bubW : inW;
    if (info_n && fontIn * 9 > winW)
        winW = fontIn * 9;
    inX = (winW - inW) / 2;
    inY = bubY + bubH + bubTailH + 4;
    sprX = (winW - sprW) / 2;
    sprY = dialog_open ? inY + inH + 4 : bubY + bubH + bubTailH + 4;
    winH = sprY + sprH + 4;
}

static void relayout(BOOL keep_bottom) {
    RECT rc, wa;
    int old_bottom, cx, nx;
    GetWindowRect(hwnd, &rc);
    old_bottom = rc.bottom;
    /* 立绘永远画在窗口水平正中，锚定窗口中心 = 锚定立绘。
       旧版把 x 钉回 wa.right-24，走路中任何 say/talk 结束触发重排都会瞬移回右边缘 */
    cx = (rc.left + rc.right) / 2;
    layout();
    rebuild_dib();
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    workarea = wa;
    if (keep_bottom) {
        nx = cx - winW / 2;
        if (nx < wa.left + 4)
            nx = wa.left + 4;
        if (nx + winW > wa.right - 4)
            nx = wa.right - 4 - winW;
        SetWindowPos(hwnd, NULL, nx, old_bottom - winH, winW, winH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowPos(hwnd, NULL, rc.left, rc.top, winW, winH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    render();
}

static void draw_rounded(GpGraphics *g, REAL x, REAL y, REAL w, REAL h, REAL r,
                         ARGB fill, ARGB line) {
    GpPath *path;
    GpSolidFill *fb;
    GpPen *pen;
    GdipCreatePath(FillModeAlternate, &path);
    GdipAddPathArc(path, x, y, r * 2, r * 2, 180, 90);
    GdipAddPathArc(path, x + w - r * 2, y, r * 2, r * 2, 270, 90);
    GdipAddPathArc(path, x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    GdipAddPathArc(path, x, y + h - r * 2, r * 2, r * 2, 90, 90);
    GdipClosePathFigure(path);
    GdipCreateSolidFill(fill, &fb);
    GdipFillPath(g, fb, path);
    GdipDeleteBrush(fb);
    GdipCreatePen1(line, 2.0f, UnitPixel, &pen);
    GdipDrawPath(g, pen, path);
    GdipDeletePen(pen);
    GdipDeletePath(path);
}

static void fill_ellipse(GpGraphics *g, GpBrush *b, INT x, INT y, INT w, INT h) {
    /* 单条 360° 弧首尾重合路径为空；拆两段 180°（draw_rounded 的四段 90° 是活证） */
    GpPath *p;
    GdipCreatePath(0, &p);
    GdipAddPathArc(p, (REAL)x, (REAL)y, (REAL)w, (REAL)h, 0.0f, 180.0f);
    GdipAddPathArc(p, (REAL)x, (REAL)y, (REAL)w, (REAL)h, 180.0f, 180.0f);
    GdipClosePathFigure(p);
    GdipFillPath(g, b, p);
    GdipDeletePath(p);
}

/* 本机 gdiplus 的 GdipDrawImageRectRect(I) 一调就崩（b9test 隔离复现 0xC0000005），
   九宫格只能在加载时把九块克隆成独立位图，绘制走验证过的 GdipDrawImageRectI */
static void build_bubble_slices(void) {
    static const int R[10][4] = {
        { BUB_SRC_X, BUB_SRC_Y, BUB_SL_L, BUB_SL_T },
        { BUB_SRC_X + BUB_SL_L, BUB_SRC_Y, BUB_SRC_W - BUB_SL_L - BUB_SL_R, BUB_SL_T },
        { BUB_SRC_X + BUB_SRC_W - BUB_SL_R, BUB_SRC_Y, BUB_SL_R, BUB_SL_T },
        { BUB_SRC_X, BUB_SRC_Y + BUB_SL_T, BUB_SL_L, BUB_SRC_H - BUB_SL_T - BUB_SL_B },
        { BUB_SRC_X + BUB_SL_L, BUB_SRC_Y + BUB_SL_T, BUB_SRC_W - BUB_SL_L - BUB_SL_R, BUB_SRC_H - BUB_SL_T - BUB_SL_B },
        { BUB_SRC_X + BUB_SRC_W - BUB_SL_R, BUB_SRC_Y + BUB_SL_T, BUB_SL_R, BUB_SRC_H - BUB_SL_T - BUB_SL_B },
        { BUB_SRC_X, BUB_SRC_Y + BUB_SRC_H - BUB_SL_B, BUB_SL_L, BUB_SL_B },
        { BUB_SRC_X + BUB_SL_L, BUB_SRC_Y + BUB_SRC_H - BUB_SL_B, BUB_SRC_W - BUB_SL_L - BUB_SL_R, BUB_SL_B },
        { BUB_SRC_X + BUB_SRC_W - BUB_SL_R, BUB_SRC_Y + BUB_SRC_H - BUB_SL_B, BUB_SL_R, BUB_SL_B },
        { QUIL_SRC_X, QUIL_SRC_Y, QUIL_SRC_W, QUIL_SRC_H },
    };
    int i;
    for (i = 0; i < 10; i++)
        if (GdipCloneBitmapAreaI(R[i][0], R[i][1], R[i][2], R[i][3],
                                 PF_32BPP_ARGB, img_bubble, &bub_slice[i]) != Ok)
            bub_slice[i] = NULL;
    if (!bub_slice[4])
        dbg("bubble slices incomplete, fall back to full stretch");
}

static void draw_slice(GpGraphics *g, int i, int dx, int dy, int dw, int dh) {
    if (dw <= 0 || dh <= 0 || !bub_slice[i])
        return;
    GdipDrawImageRectI(g, bub_slice[i], dx, dy, dw, dh);
}

static void draw_bubble9(GpGraphics *g, int x, int y, int w, int h) {
    double ds = (double)h / BUB_SRC_H;
    int dl = (int)(BUB_SL_L * ds), dr = (int)(BUB_SL_R * ds);
    int dt = (int)(BUB_SL_T * ds), db = (int)(BUB_SL_B * ds);
    int dmw, dmh, qw, qh;
    if (!bub_slice[4]) {
        GdipDrawImageRectI(g, img_bubble, x, y, w, h);
        return;
    }
    if (dl + dr > w) {
        dl = w * BUB_SL_L / (BUB_SL_L + BUB_SL_R);
        dr = w - dl;
    }
    if (dt + db > h) {
        dt = h * BUB_SL_T / (BUB_SL_T + BUB_SL_B);
        db = h - dt;
    }
    dmw = w - dl - dr;
    dmh = h - dt - db;
    draw_slice(g, 0, x, y, dl, dt);
    draw_slice(g, 1, x + dl, y, dmw, dt);
    draw_slice(g, 2, x + dl + dmw, y, dr, dt);
    draw_slice(g, 3, x, y + dt, dl, dmh);
    draw_slice(g, 4, x + dl, y + dt, dmw, dmh);
    draw_slice(g, 5, x + dl + dmw, y + dt, dr, dmh);
    draw_slice(g, 6, x, y + dt + dmh, dl, db);
    draw_slice(g, 7, x + dl, y + dt + dmh, dmw, db);
    draw_slice(g, 8, x + dl + dmw, y + dt + dmh, dr, db);
    qw = (int)(QUIL_SRC_W * ds);
    qh = (int)(QUIL_SRC_H * ds);
    draw_slice(g, 9, x + w / 2 - qw / 2, y + h, qw, qh);
}

/* 本机 gdiplus 没导出 GdipSetStringFormatLineSpacing（GetProcAddress 回 NULL），
   1.5 倍行距自己算：MeasureString 的 codepointsFitted 拆行，逐行画，整体垂直居中 */
static void draw_wrapped(GpGraphics *g, const WCHAR *text, GpFont *font,
                         GpStringFormat *fmt, GpBrush *brush, RectF tr, REAL line_h) {
    RectF bb;
    REAL y;
    int len = (int)wcslen(text);
    int rest = len, lines = 0, i;
    const WCHAR *q = text;
    while (rest > 0) {
        RectF mr = tr;
        INT fit = 0;
        mr.Height = 10000;
        GdipMeasureString(g, q, rest, font, &mr, fmt, &bb, &fit, NULL);
        if (fit <= 0)
            fit = 1;
        lines++;
        q += fit;
        rest -= fit;
    }
    y = tr.Y + (tr.Height - lines * line_h) / 2;
    if (y < tr.Y)
        y = tr.Y;
    q = text;
    rest = len;
    for (i = 0; i < lines && rest > 0; i++) {
        RectF mr = tr, lr = tr;
        INT fit = 0;
        mr.Height = 10000;
        GdipMeasureString(g, q, rest, font, &mr, fmt, &bb, &fit, NULL);
        if (fit <= 0)
            fit = 1;
        lr.Y = y;
        lr.Height = line_h;
        GdipDrawString(g, q, fit, font, &lr, fmt, brush);
        q += fit;
        rest -= fit;
        y += line_h;
    }
}

static void render(void) {
    GpGraphics *g;
    RECT rc;
    BLENDFUNCTION bf;
    POINT ptSrc, ptDst;
    SIZE sz;
    WCHAR fam_name[64];
    GpFontFamily *fam = NULL;
    int fi;
    GetWindowRect(hwnd, &rc);
    GdipGetImageGraphicsContext((GpImage *)canvas, &g);
    GdipGraphicsClear(g, 0x00000000);
    /* 原来写死 Microsoft YaHei 且不判返回值：非中文 Windows 没装雅黑时
       GdipCreateFontFamilyFromName 失败、fam 保持未初始化，后面拿它建字体就是野指针 */
    for (fi = 0; PET_FONTS[fi]; fi++) {
        utf8w(PET_FONTS[fi], fam_name, 64);
        if (GdipCreateFontFamilyFromName(fam_name, NULL, &fam) == Ok && fam)
            break;
        fam = NULL;
    }
    if (info_n && fam) {
        GpFont *font;
        GpStringFormat *fmt;
        GpSolidFill *brush;
        RectF lr;
        int i;
        draw_rounded(g, 0, 0, (REAL)winW, (REAL)panH, 8.0f, 0xD9FFFFFF, 0xFF8A7A6E);
        GdipCreateFont(fam, (REAL)fontIn * 0.8f, FontStyleRegular, UnitPixel, &font);
        GdipCreateStringFormat(0, 0, &fmt);
        GdipSetStringFormatAlign(fmt, StringAlignmentNear);
        GdipSetStringFormatLineAlign(fmt, StringAlignmentNear);
        GdipCreateSolidFill(0xFF4A4A4A, &brush);
        for (i = 0; i < info_n; i++) {
            lr.X = fontIn * 0.4f;
            lr.Y = 6 + i * (fontIn + 6);
            lr.Width = (REAL)(winW - fontIn * 0.8);
            lr.Height = (REAL)(fontIn + 6);
            GdipDrawString(g, info_lines[i], -1, font, &lr, fmt, brush);
        }
        GdipDeleteBrush(brush);
        GdipDeleteStringFormat(fmt);
        GdipDeleteFont(font);
    }
    if (alarm_set) {
        /* 规格：闹钟图标在桌宠右下角；draw_rounded 是唯一全验证过的填充路 */
        draw_rounded(g, (REAL)(winW - fontIn - 4), (REAL)(winH - fontIn - 4),
                     (REAL)(fontIn * 8 / 10), (REAL)(fontIn * 8 / 10),
                     4.0f, 0xFFE8B93C, 0xFF8A6A1C);
    }
    if (talking && fam) {
        GpFont *font;
        GpStringFormat *fmt;
        GpSolidFill *brush;
        RectF tr;
        double sc = (double)pct / 100 * (double)sys_dpi / 96;
        draw_bubble9(g, 0, bubY, bubW, bubH);
        GdipCreateFont(fam, (REAL)(BUB_FONT_PX * sc), FontStyleRegular, UnitPixel, &font);
        GdipCreateStringFormat(0, 0, &fmt);
        GdipSetStringFormatAlign(fmt, StringAlignmentCenter);
        GdipSetStringFormatLineAlign(fmt, StringAlignmentNear);
        GdipCreateSolidFill(0xFF6B5A4E, &brush);
        tr.X = bubW * 0.14f;
        tr.Y = bubY + bubH * 0.14f;
        tr.Width = bubW * 0.72f;
        tr.Height = bubH * 0.72f;
        draw_wrapped(g, talkText, font, fmt, brush, tr,
                     (REAL)(BUB_FONT_PX * sc * BUB_LINE_RATIO));
        GdipDeleteBrush(brush);
        GdipDeleteStringFormat(fmt);
        GdipDeleteFont(font);
    }
    if (dialog_open && fam) {
        GpFont *font;
        GpStringFormat *fmt;
        GpSolidFill *brush;
        RectF tr, box;
        RectF bb;
        WCHAR vis[64];
        int n = 0, i;
        int hx = inX, hy = inY, hw = inW, hh = inH;
        REAL pad, txoff;
        if (img_inbar) {
            /* 素材里输入框是透明洞（白底洪水从左尖角漏进吃掉了框内白），
               先垫不透明白底再叠素材，洞才变成可读的输入区 */
            hx = inX + inW * 974 / 10000;
            hy = inY + inH * 3797 / 10000;
            hw = inW * (8048 - 974) / 10000;
            hh = inH * (6606 - 3797) / 10000;
            draw_rounded(g, (REAL)hx, (REAL)hy, (REAL)hw, (REAL)hh,
                         4.0f, 0xFFFFFFFF, 0xFFFFFFFF);
            GdipDrawImageRectI(g, img_inbar, inX, inY, inW, inH);
        } else {
            draw_rounded(g, (REAL)inX, (REAL)inY, (REAL)inW, (REAL)inH,
                         (REAL)inH / 2.0f, 0xF2FFFFFF, 0xFF8A7A6E);
        }
        if (inp_len <= VIS_CHARS) {
            memcpy(vis, inp, inp_len * sizeof(WCHAR));
            n = inp_len;
        } else {
            for (i = 0; i < PIN_CHARS; i++)
                vis[n++] = inp[i];
            vis[n++] = 0x2026;
            for (i = inp_len - (VIS_CHARS - PIN_CHARS - 1); i < inp_len; i++)
                vis[n++] = inp[i];
        }
        vis[n] = 0;
        pad = img_inbar ? hh * 0.20f : fontIn * 0.6f;
        /* 羽化重抠后素材里印着的「跟白说：」标签活了下来：文字区从标签后起步，
           空框占位符由素材自己承担，代码不再叠一份 */
        txoff = img_inbar ? hw * 0.26f : pad;
        GdipCreateFont(fam, img_inbar ? hh * 0.66f : (REAL)fontIn,
                       FontStyleRegular, UnitPixel, &font);
        GdipCreateStringFormat(0, 0, &fmt);
        GdipSetStringFormatAlign(fmt, StringAlignmentNear);
        GdipSetStringFormatLineAlign(fmt, StringAlignmentNear);
        GdipCreateSolidFill(inp_len ? 0xFF333333 : 0xFF9A9A9A, &brush);
        tr.X = hx + txoff;
        tr.Y = (REAL)hy;
        tr.Width = (REAL)hw - txoff - pad * 0.5f;
        tr.Height = (REAL)hh;
        if (n) {
            GdipDrawString(g, vis, -1, font, &tr, fmt, brush);
        } else if (!img_inbar || alarm_input_mode) {
            RectF tr2 = tr;
            tr2.X += pad * 0.6f;
            tr2.Width -= pad * 0.6f;
            GdipDrawString(g, alarm_input_mode ? L"……时间，HH:MM。" : L"……说点什么。",
                           -1, font, &tr2, fmt, brush);
        }
        box.X = 0;
        box.Y = 0;
        box.Width = 10000;
        box.Height = 10000;
        GdipMeasureString(g, vis, n, font, &box, fmt, &bb, NULL, NULL);
        if (caret_on) {
            GpPen *pen;
            REAL cx = tr.X + bb.Width + 2;
            GdipCreatePen1(0xFF333333, 2.0f, UnitPixel, &pen);
            GdipDrawLineI(g, pen, (INT)cx, hy + hh / 5, (INT)cx, hy + hh * 4 / 5);
            GdipDeletePen(pen);
        }
        GdipDeleteBrush(brush);
        GdipDeleteStringFormat(fmt);
        GdipDeleteFont(font);
    }
    if (acting) {
        GpSolidFill *ab;
        int px = sprX - fontIn * 2 - 4, py = sprY + sprH / 2;
        if (px < 2)
            px = 2;
        GdipCreateSolidFill(acting == 1 ? 0xFFE88AA0 : acting == 2 ? 0xFF8A5A3E : 0xFF7EB6D9, &ab);
        if (acting == 1) {
            int off = step_phase ? 6 : -6;
            fill_ellipse(g, ab, px, py + off, fontIn, fontIn);
            fill_ellipse(g, ab, px + fontIn, py - off, fontIn * 7 / 10, fontIn * 7 / 10);
        } else if (acting == 2) {
            draw_rounded(g, (REAL)px, (REAL)py, (REAL)(fontIn * 2),
                         (REAL)(fontIn * 3 / 2), 2.0f, 0xFF8A5A3E, 0xFF8A5A3E);
        } else {
            fill_ellipse(g, ab, px, py, fontIn * 3 / 2, fontIn);
        }
        GdipDeleteBrush(ab);
    }
    if (q_mode) {
        if (img_q) {
            GdipDrawImageRectI(g, img_q, sprX, sprY, sprW, sprH);
        } else {
            GpSolidFill *wb, *db, *lb;
            int r = sprW * 35 / 100;
            int cx = sprX + sprW / 2;
            GdipCreateSolidFill(0xFFC9B6E4, &lb);
            GdipCreateSolidFill(0xFFFFFFFF, &wb);
            GdipCreateSolidFill(0xFF5A4A42, &db);
            fill_ellipse(g, lb, cx - r * 7 / 10, sprY + sprH - r, r * 14 / 10, r);
            fill_ellipse(g, wb, cx - r, sprY + sprH / 3, r * 2, r * 2);
            fill_ellipse(g, db, cx - r / 2, sprY + sprH / 3 + r * 8 / 10, r / 5, r / 4);
            fill_ellipse(g, db, cx + r / 2 - r / 5, sprY + sprH / 3 + r * 8 / 10, r / 5, r / 4);
            GdipDeleteBrush(lb);
            GdipDeleteBrush(wb);
            GdipDeleteBrush(db);
        }
    } else {
        int dstY = sprY, dstH = sprH;
        if (squash) {
            dstH = sprH * 85 / 100;
            dstY = sprY + sprH - dstH;
        }
        if (walk_on && walk_dir < 0) {
            GdipTranslateWorldTransform(g, (REAL)(sprX + sprW), (REAL)dstY, 0);
            GdipScaleWorldTransform(g, -1.0f, 1.0f, 0);
            GdipDrawImageRectI(g, cur, 0, 0, sprW, dstH);
            GdipResetWorldTransform(g);
        } else {
            GdipDrawImageRectI(g, cur, sprX, dstY, sprW, dstH);
        }
    }
    /* 头顶名牌：多实例时每只白挂自己的名字（fam 还没删，必须在 DeleteFontFamily 之前画） */
    if (fam) {
        GpFont *nf;
        GpSolidFill *nb;
        GpStringFormat *nfmt;
        RectF nr;
        REAL fs = 13.0f * (REAL)sys_dpi / 96.0f;
        GdipCreateFont(fam, fs, FontStyleRegular, UnitPixel, &nf);
        GdipCreateStringFormat(0, 0, &nfmt);
        GdipSetStringFormatAlign(nfmt, StringAlignmentFar);
        GdipCreateSolidFill(0xFF6B5A4E, &nb);
        nr.X = (REAL)sprX + (REAL)sprW * 0.52f;
        nr.Y = (REAL)sprY + 2.0f;
        nr.Width = (REAL)sprW * 0.48f;
        nr.Height = fs * 1.9f;
        GdipDrawString(g, selfName, -1, nf, &nr, nfmt, nb);
        GdipDeleteBrush((GpBrush *)nb);
        GdipDeleteStringFormat(nfmt);
        GdipDeleteFont(nf);
    }
    if (fam)
        GdipDeleteFontFamily(fam);
    GdipDeleteGraphics(g);
    {
        /* GDI+ 画到 HDC 不写 alpha，改锁 PARGB 位自己拷进 DIB */
        BitmapDataX bd;
        int y;
        GpStatus ls = GdipBitmapLockBits(canvas, NULL, 1, PF_32BPP_PARGB, &bd);
        if (ls != Ok) {
            /* 锁失败则 memcpy 跳过、DIB 保留上一帧——那才是重影真源；跳帧不送旧帧 */
            dbg("LOCK_FAIL st=%d skip frame", (int)ls);
            return;
        }
        for (y = 0; y < winH; y++)
            memcpy((char *)dib_bits + (size_t)y * winW * 4,
                   (char *)bd.Scan0 + (size_t)y * bd.Stride, (size_t)winW * 4);
        GdipBitmapUnlockBits(canvas, &bd);
    }
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    ptSrc.x = 0;
    ptSrc.y = 0;
    ptDst.x = rc.left;
    ptDst.y = rc.top;
    sz.cx = winW;
    sz.cy = winH;
    if (!UpdateLayeredWindow(hwnd, NULL, &ptDst, &sz, memDC, &ptSrc, 0, &bf, ULW_ALPHA)) {
        FILE *lf = _wfopen(g_log_path, L"a");
        if (lf) {
            fprintf(lf, "ULW_FAIL err=%lu\n", GetLastError());
            fclose(lf);
        }
    }
}

static void talk(void) {
    if (simple_mode)
        return;
    utf8w(LINES[rand() % LINE_COUNT], talkText, 1024);
    talking = TRUE;
    reply_mode = FALSE;
    SetTimer(hwnd, IDT_TALK, 2600, NULL);
    relayout(TRUE);
}

static void say(const WCHAR *text, int ms) {
    wcsncpy(talkText, text, 1023);
    talkText[1023] = 0;
    talking = TRUE;
    SetTimer(hwnd, IDT_TALK, ms, NULL);
    dbg("say len=%d ms=%d", (int)wcslen(talkText), ms);
    relayout(TRUE);
}

static ULONGLONG ft2ull(FILETIME ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static void refresh_info(void) {
    int i = 0;
    if (show_cpu) {
        FILETIME id, ke, us;
        ULONGLONG tot, idl;
        GetSystemTimes(&id, &ke, &us);
        tot = ft2ull(ke) + ft2ull(us);
        idl = ft2ull(id);
        if (cpu_prev_total && tot > cpu_prev_total) {
            ULONGLONG dt = tot - cpu_prev_total, di = idl - cpu_prev_idle;
            swprintf(info_lines[i], 64, L"CPU %d%%", (int)((dt - di) * 100 / dt));
        } else {
            swprintf(info_lines[i], 64, L"CPU --");
        }
        cpu_prev_total = tot;
        cpu_prev_idle = idl;
        i++;
    }
    if (show_mem) {
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        swprintf(info_lines[i], 64, L"内存 %d%%", (int)ms.dwMemoryLoad);
        i++;
    }
    if (show_disk) {
        static WCHAR drv[4];
        ULARGE_INTEGER fr, to;
        if (!drv[0])
            compat_data_drive(drv);     /* 原来写死 D:\，没 D 盘的机器永远显示 -- */
        if (GetDiskFreeSpaceExW(drv, &fr, &to, NULL) && to.QuadPart)
            swprintf(info_lines[i], 64, L"%c盘 %d%%", drv[0],
                     (int)(100 - fr.QuadPart * 100 / to.QuadPart));
        else
            swprintf(info_lines[i], 64, L"存储 --");
        i++;
    }
    if (show_net) {
        DWORD now = GetTickCount();
        ULONGLONG bytes = 0;
        /* 原来 static MIB_IFTABLE 只含 1 个 entry，多网卡机器上恒失败 */
        if (compat_net_bytes(&bytes) != 0)
            bytes = net_prev_bytes;                    /* 取不到就别编出个假速率 */
        if (net_prev_ms && now > net_prev_ms && bytes >= net_prev_bytes)
            swprintf(info_lines[i], 64, L"网速 %dKB/s",
                     (int)((bytes - net_prev_bytes) / (now - net_prev_ms)));
        else
            swprintf(info_lines[i], 64, L"网速 --");
        net_prev_bytes = bytes;
        net_prev_ms = now;
        i++;
    }
    info_n = i;
}

static void open_dialog(void) {
    if (dialog_open)
        return;
    dbg("dialog open");
    dialog_open = TRUE;
    caret_on = TRUE;
    SetTimer(hwnd, IDT_CARET, 500, NULL);
    relayout(TRUE);
}

static void close_dialog(void) {
    if (!dialog_open)
        return;
    dbg("dialog close");
    dialog_open = FALSE;
    KillTimer(hwnd, IDT_CARET);
    relayout(TRUE);
}

static void clip_copy(void) {
    HGLOBAL hg;
    WCHAR *p;
    if (!OpenClipboard(hwnd))
        return;
    EmptyClipboard();
    hg = GlobalAlloc(GMEM_MOVEABLE, (inp_len + 1) * sizeof(WCHAR));
    p = (WCHAR *)GlobalLock(hg);
    memcpy(p, inp, (inp_len + 1) * sizeof(WCHAR));
    GlobalUnlock(hg);
    SetClipboardData(CF_UNICODETEXT, hg);
    CloseClipboard();
}

static void clip_paste(void) {
    HGLOBAL hg;
    WCHAR *p;
    int have, take;
    if (!OpenClipboard(hwnd))
        return;
    hg = GetClipboardData(CF_UNICODETEXT);
    if (hg) {
        p = (WCHAR *)GlobalLock(hg);
        if (p) {
            have = (int)wcslen(p);
            take = 511 - inp_len;
            if (take > have)
                take = have;
            if (take > 0) {
                memcpy(inp + inp_len, p, take * sizeof(WCHAR));
                inp_len += take;
                inp[inp_len] = 0;
            }
            GlobalUnlock(hg);
        }
    }
    CloseClipboard();
    render();
}

static void flush_raw(const char *raw, int len, WCHAR **dst, int *dn, int *cap) {
    int need;
    if (len <= 0)
        return;
    need = MultiByteToWideChar(CP_UTF8, 0, raw, len, NULL, 0);
    if (*dn + need + 1 > *cap) {
        while (*dn + need + 1 > *cap)
            *cap *= 2;
        *dst = (WCHAR *)realloc(*dst, *cap * sizeof(WCHAR));
    }
    MultiByteToWideChar(CP_UTF8, 0, raw, len, *dst + *dn, need);
    *dn += need;
    (*dst)[*dn] = 0;
}

static WCHAR *parse_result(const char *json) {
    const char *p = NULL, *q, *s;
    WCHAR *out = NULL;
    int dn = 0, cap = 256;
    char raw[512];
    int rl = 0;
    out = (WCHAR *)malloc(cap * sizeof(WCHAR));
    out[0] = 0;
    s = json;
    while ((s = strstr(s, "\"type\":\"result\"")) != NULL) {
        p = s;
        s += 15;
    }
    if (!p)
        s = json;
    else
        s = p;
    q = strstr(s, "\"result\":\"");
    if (!q) {
        free(out);
        return NULL;
    }
    q += 10;
    while (*q && *q != '"') {
        if (*q == '\\') {
            flush_raw(raw, rl, &out, &dn, &cap);
            rl = 0;
            q++;
            switch (*q) {
            case 'n':
                out[dn++] = L'\n';
                out[dn] = 0;
                q++;
                break;
            case 't':
                out[dn++] = L'\t';
                out[dn] = 0;
                q++;
                break;
            case 'u': {
                unsigned v = 0;
                int i;
                for (i = 1; i <= 4 && q[i]; i++) {
                    v <<= 4;
                    if (q[i] >= '0' && q[i] <= '9') v |= q[i] - '0';
                    else if (q[i] >= 'a' && q[i] <= 'f') v |= q[i] - 'a' + 10;
                    else if (q[i] >= 'A' && q[i] <= 'F') v |= q[i] - 'A' + 10;
                }
                if (dn + 2 >= cap) {
                    cap *= 2;
                    out = (WCHAR *)realloc(out, cap * sizeof(WCHAR));
                }
                out[dn++] = (WCHAR)v;
                out[dn] = 0;
                q += 5;
                break;
            }
            default:
                if (dn + 2 >= cap) {
                    cap *= 2;
                    out = (WCHAR *)realloc(out, cap * sizeof(WCHAR));
                }
                out[dn++] = (WCHAR)(unsigned char)*q;
                out[dn] = 0;
                q++;
            }
        } else {
            if (rl == sizeof(raw)) {
                flush_raw(raw, rl, &out, &dn, &cap);
                rl = 0;
            }
            raw[rl++] = *q++;
        }
        if (dn > 300)
            break;
    }
    flush_raw(raw, rl, &out, &dn, &cap);
    if (dn > 300) {
        out[300] = 0;
        wcscat(out, L"……");
    }
    return out;
}

// 剥掉残留的过期 key，让 qwen 回落到 settings.json 里正确的那把
static WCHAR *build_clean_env(void) {
    WCHAR *e = GetEnvironmentStringsW();
    WCHAR *p = e;
    size_t cap = 65536, len = 0;
    WCHAR *out = (WCHAR *)malloc(cap * sizeof(WCHAR));
    while (*p) {
        size_t n = wcslen(p);
        if (wcsncmp(p, L"DASHSCOPE_API_KEY=", 18) != 0 &&
            wcsncmp(p, L"DASHSCPE_APL_KEY=", 17) != 0 &&
            wcsncmp(p, L"OPENAI_APL_KEY=", 15) != 0) {
            while (len + n + 2 > cap) {
                cap *= 2;
                out = (WCHAR *)realloc(out, cap * sizeof(WCHAR));
            }
            memcpy(out + len, p, (n + 1) * sizeof(WCHAR));
            len += n + 1;
        }
        p += n + 1;
    }
    out[len] = 0;
    FreeEnvironmentStringsW(e);
    return out;
}

static DWORD WINAPI qwen_thread(void *arg) {
    WCHAR *prompt = (WCHAR *)arg;
    SECURITY_ATTRIBUTES sa;
    HANDLE in_r, in_w, out_r, out_w, err_w;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char *ubuf;
    int ulen;
    DWORD got;
    char *buf;
    int bn = 0, bcap = 65536;
    WCHAR *res;
    WCHAR *env;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    CreatePipe(&in_r, &in_w, &sa, 0);
    CreatePipe(&out_r, &out_w, &sa, 0);
    /* 子进程若继承 in_w，stdin 永远等不到 EOF；out_r 同理 */
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    err_w = CreateFileW(g_errlog_path,
                        GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &sa, CREATE_ALWAYS, 0, NULL);
    if (err_w == INVALID_HANDLE_VALUE)
        err_w = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    dbg("handles in_r=%p in_w=%p out_r=%p out_w=%p err_w=%p",
        in_r, in_w, out_r, out_w, err_w);
    ulen = WideCharToMultiByte(CP_UTF8, 0, prompt, -1, NULL, 0, NULL, NULL);
    ubuf = (char *)malloc(ulen);
    WideCharToMultiByte(CP_UTF8, 0, prompt, -1, ubuf, ulen, NULL, NULL);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    memset(&pi, 0, sizeof(pi));
    {
        WCHAR cmdw[PET_PATH_LEN * 3];
        WCHAR args[512];
        cmdw[0] = 0;
        /* 命令行运行时拼：原来 .cmd 全路径写死在源码里，换用户名即失效 */
        if (g_qwen_path[0] && lstrlenW(g_qwen_path) + 16 < (int)(sizeof cmdw / sizeof cmdw[0])) {
            utf8w(QWEN_ARGS, args, 512);
            if (lstrlenW(g_qwen_path) + lstrlenW(args) + 16 < (int)(sizeof cmdw / sizeof cmdw[0])) {
                lstrcpyW(cmdw, L"cmd.exe /c \"");
                lstrcatW(cmdw, g_qwen_path);
                lstrcatW(cmdw, L"\" ");
                lstrcatW(cmdw, args);
            }
        }
        if (!cmdw[0]) {
            /* 兜底：这台机器没有 qwen CLI（启动期已把后端改成 local，正常走不到这） */
            dbg("qwen cmdline unavailable, abort request");
            PostMessageW(hwnd, WM_QWEN_DONE, 0, 0);
            free(ubuf);
            free(prompt);
            return 1;
        }
        env = build_clean_env();
        /* 句柄继承修复后重试无控制台：控制台窗口的根源直接去掉 */
        if (!CreateProcessW(NULL, cmdw, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                            env, NULL, &si, &pi)) {
            dbg("cp env fail %lu, retry NULL env", GetLastError());
            if (!CreateProcessW(NULL, cmdw, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                dbg("cp fail %lu", GetLastError());
                free(env);
                PostMessageW(hwnd, WM_QWEN_DONE, 0, 0);
                free(ubuf);
                free(prompt);
                return 1;
            }
        }
        free(env);
    }
    CloseHandle(in_r);
    CloseHandle(out_w);
    WriteFile(in_w, ubuf, ulen - 1, &got, NULL);
    dbg("stdin write ok bytes=%lu", got);
    WriteFile(in_w, "\n", 1, &got, NULL);
    CloseHandle(in_w);
    free(ubuf);
    buf = (char *)malloc(bcap);
    while (ReadFile(out_r, buf + bn, bcap - bn - 1, &got, NULL) && got > 0) {
        bn += got;
        if (bn > bcap - 4096) {
            bcap *= 2;
            buf = (char *)realloc(buf, bcap);
        }
    }
    buf[bn] = 0;
    dbg("qwen bytes=%d", bn);
    CloseHandle(out_r);
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    res = parse_result(buf);
    dbg("parse=%d", res != NULL);
    free(buf);
    PostMessageW(hwnd, WM_QWEN_DONE, 0, (LPARAM)res);
    free(prompt);
    return 0;
}

/* ---------- 本地推理管道（v2 接线）：spawn inference_backend.exe 流式收 token ---------- */

/* 近谈滚动窗：transcript 模式最近 8 轮。UI 线程写，worker 线程只读 */
static WCHAR histU[8][96], histA[8][96];
static int hist_n;

static void build_local_prompt(const WCHAR *userw, char *out, int cap, int transcript) {
    char u8[1600];
    int n, b, i;
    n = WideCharToMultiByte(CP_UTF8, 0, userw, -1, u8, (int)sizeof u8, NULL, NULL);
    if (n <= 0)
        u8[0] = 0;
    if (transcript && hist_n > 0) {
        b = snprintf(out, cap,
                     "你是桌宠「白」。用简洁中文口语回答，不超三句，纯文本，不要代码块和JSON。"
                     "下面是哥哥和白最近的对话记录，最后一行是哥哥现在说的，接着白的口吻回复：\n");
        if (b < 0)
            b = 0;
        if (b > cap - 8)
            b = cap - 8;
        for (i = 0; i < hist_n && b < cap - 700; i++) {
            char wu[300], wa[300];
            WCHAR tw[96], ta[96];
            lstrcpynW(tw, histU[i], 90);
            lstrcpynW(ta, histA[i], 90);
            WideCharToMultiByte(CP_UTF8, 0, tw, -1, wu, sizeof wu, NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, ta, -1, wa, sizeof wa, NULL, NULL);
            n = snprintf(out + b, cap - b, "哥哥：%s\n白：%s\n", wu, wa);
            if (n > 0) {
                b += n;
                if (b > cap - 8)      /* snprintf 返回"需要"的长度，截断时会超，硬夹 */
                    b = cap - 8;
            }
        }
        snprintf(out + b, cap - b, "哥哥：%s", u8);
    } else {
        snprintf(out, cap,
                 "你是桌宠「白」。用简洁中文回答，不要代码块，不要 JSON，"
                 "只输出纯文本，不超过三句话。\n用户说：%s", u8);
    }
}

/* 「记住：xxx」→ 追加进共享记忆（UTF-8 一行）。终端白读写同一文件 */
static void remember_append(void) {
    WCHAR *p = inp + 3;            /* 跳过「记住」+ 全/半角冒号 */
    char line[1024];
    FILE *f;
    int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, line, (int)sizeof line - 2, NULL, NULL);
    if (n <= 1)
        return;
    if (line[n - 2] != '\n') {
        line[n - 1] = '\n';
        line[n] = 0;
    }
    f = _wfopen(g_memfile, L"a");
    if (!f) {
        dbg("remember open fail %lu", GetLastError());
        return;
    }
    fputs(line, f);
    fclose(f);
    dbg("remember appended %d bytes", n);
}

/* ReadFile 会把 3 字节汉字劈成两块：只转完整 UTF-8 前缀，残字节留到下轮 */
static int utf8_complete_len(const char *buf, int len) {
    int lead = len - 1, back = 0;
    unsigned char c;
    int need, have;
    while (lead >= 0 && back < 3 && ((unsigned char)buf[lead] & 0xC0) == 0x80) {
        lead--;
        back++;
    }
    if (lead < 0)
        return len;
    c = (unsigned char)buf[lead];
    need = (c < 0x80) ? 1 : (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    have = len - lead;
    return (have < need) ? lead : len;
}

typedef struct { WCHAR *user; unsigned long long id; int is_ma; } LocalReq;

/* token 块转发给 UI：堆副本。PostMessage 异步入队，传栈指针 UI 处理时已翻篇 */
static void post_tokens(unsigned long long id, const char *u8, int len) {
    WCHAR *w;
    int wlen;
    if (len <= 0)
        return;
    wlen = MultiByteToWideChar(CP_UTF8, 0, u8, len, NULL, 0);
    if (wlen <= 0)
        return;
    w = (WCHAR *)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    if (!w)
        return;
    MultiByteToWideChar(CP_UTF8, 0, u8, len, w, wlen);
    w[wlen] = 0;
    if (!PostMessageW(hwnd, WM_STREAM_TOKEN, (WPARAM)id, (LPARAM)w))
        free(w);   /* 窗口已亡投递失败：线程侧自己 free，否则泄漏 */
}

static DWORD WINAPI local_pipe_thread(void *arg) {
    LocalReq *rq = (LocalReq *)arg;
    unsigned long long myid = rq->id;
    int use_ma = rq->is_ma;
    SECURITY_ATTRIBUTES sa;
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL, err_w = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR exe[512];
    char req[4096], head[80];
    char acc[2048];
    char buf[1024];
    int accLen = 0, hlen, plen;
    DWORD got, code = 0xFFFFFFFE;

    if (use_ma)
        /* 多智能体：主管自己会拆需求，喂原文，不套桌宠对话模板 */
        WideCharToMultiByte(CP_UTF8, 0, rq->user, -1, req, sizeof req, NULL, NULL);
    else
        build_local_prompt(rq->user, req, sizeof req, 1);
    free(rq->user);
    free(rq);

    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) {
        dbg("local pipe fail %lu", GetLastError());
        PostMessageW(hwnd, WM_STREAM_END, (WPARAM)myid, (LPARAM)code);
        return 1;
    }
    err_w = CreateFileW(g_errlog_path,
                        GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &sa, CREATE_ALWAYS, 0, NULL);
    if (err_w == INVALID_HANDLE_VALUE)
        err_w = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    /* 父端句柄摘继承位：子若继承 in_w，stdin 永不 EOF，双挂死（本机实测坑） */
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    lstrcpyW(exe, use_ma ? g_ma_path : g_backend_path);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;  /* 调试进文件，不污染 token 流 */
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(exe, NULL, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        dbg("local spawn fail %lu", GetLastError());
        CloseHandle(in_r); CloseHandle(in_w);
        CloseHandle(out_r); CloseHandle(out_w);
        if (err_w != INVALID_HANDLE_VALUE) CloseHandle(err_w);
        PostMessageW(hwnd, WM_STREAM_END, (WPARAM)myid, (LPARAM)code);
        return 1;
    }
    /* 父立刻关"子端"副本：留着 out_w，子退出后写端引用不归零，读取线程等不到 EOF */
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (err_w != INVALID_HANDLE_VALUE) CloseHandle(err_w);
    CloseHandle(pi.hThread);

    /* 长度前缀请求帧：两行 ASCII 参数 + prompt 原始字节（模板含 \n，零转义） */
    plen = (int)strlen(req);
    /* 第三个参数=多智能体角色（AgentRole 枚举）；inference_backend 的 sscanf 只取两个，无害 */
    hlen = snprintf(head, sizeof head, "%d %d %d\n%d\n", 256, 70,
                    use_ma ? ma_role : 0, plen);
    WriteFile(in_w, head, (DWORD)hlen, &got, NULL);
    WriteFile(in_w, req, (DWORD)plen, &got, NULL);
    CloseHandle(in_w);   /* 写完即关写端，子进程读完 prompt 即见 EOF */

    for (;;) {
        int use;
        if (!ReadFile(out_r, buf, sizeof buf, &got, NULL) || got == 0)
            break;       /* ERROR_BROKEN_PIPE / 0 字节 = EOS：管道结束即结束符 */
        if (accLen + (int)got > (int)sizeof acc)
            accLen = 0;  /* 溢出保护，正常 token 块大小到不了这里 */
        memcpy(acc + accLen, buf, got);
        accLen += (int)got;
        use = utf8_complete_len(acc, accLen);
        if (use > 0) {
            post_tokens(myid, acc, use);
            memmove(acc, acc + use, (size_t)(accLen - use));
            accLen -= use;
        }
    }
    if (accLen > 0)
        post_tokens(myid, acc, accLen);
    CloseHandle(out_r);
    /* v2 "子进程异常退出检测"的正解：留住 hProcess，等退出取码 */
    WaitForSingleObject(pi.hProcess, 15000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    dbg("local end id=%u code=%lu", (unsigned)myid, code);
    PostMessageW(hwnd, WM_STREAM_END, (WPARAM)myid, (LPARAM)code);
    return 0;
}

/* 提交后清空输入框文本：框留着不关，方便接着打下一句 */
static void input_clear(void) {
    inp_len = 0;
    inp[0] = 0;
    render();
}

static void submit(void) {
    WCHAR *prompt;
    if (alarm_input_mode) {
        int hh = -1, mm = -1;
        if (swscanf(inp, L"%d:%d", &hh, &mm) == 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
            alarm_set = TRUE;
            alarm_h = hh;
            alarm_m = mm;
            alarm_input_mode = FALSE;
            input_clear();
            close_dialog();
            say(L"……闹钟，设好了。", 2600);
            SetTimer(hwnd, IDT_ALARM, 1000, NULL);
        } else {
            say(L"……格式，HH:MM。笨蛋。", 2600);
        }
        render();
        return;
    }
    if (waiting || inp_len == 0)
        return;
    dbg("submit len=%d", inp_len);
    if (inp_len >= 4 && inp[0] == 0x8BB0 && inp[1] == 0x4F4F &&
        (inp[2] == 0xFF1A || inp[2] == L':')) {
        /* 「记住：xxx」全后端通用，落盘即完，不走模型 */
        remember_append();
        input_clear();
        say(L"……记住了。", 3000);
        return;
    }
    if (backend_local) {
        LocalReq *rq = (LocalReq *)malloc(sizeof *rq);
        rq->user = (WCHAR *)malloc((size_t)(inp_len + 1) * sizeof(WCHAR));
        memcpy(rq->user, inp, (size_t)(inp_len + 1) * sizeof(WCHAR));
        rq->id = ++req_counter;
        rq->is_ma = (backend_local == 2);
        if (backend_local == 1) {
            if (hist_n >= 8) {
                int i;
                for (i = 0; i < 7; i++) {
                    lstrcpyW(histU[i], histU[i + 1]);
                    lstrcpyW(histA[i], histA[i + 1]);
                }
                hist_n = 7;
            }
            lstrcpynW(histU[hist_n], inp, 96);
        }
        cur_req = rq->id;
        waiting = TRUE;
        reply_mode = FALSE;
        stream_started = 0;
        if (!simple_mode)
            say(L"……想着。", 60000);
        /* 多智能体一轮要 5~8 次 LLM 调用，超时放到 5 分钟 */
        SetTimer(hwnd, IDT_QWEN, backend_local == 2 ? 300000 : 60000, NULL);
        dbg("local submit id=%u len=%d", (unsigned)cur_req, inp_len);
        input_clear();
        CreateThread(NULL, 0, local_pipe_thread, rq, 0, NULL);
        return;
    }
    waiting = TRUE;
    prompt = (WCHAR *)malloc((inp_len + 1) * sizeof(WCHAR));
    memcpy(prompt, inp, (inp_len + 1) * sizeof(WCHAR));
    input_clear();
    reply_mode = FALSE;
    if (!simple_mode)
        say(L"……qwen 思考中。", 60000);
    SetTimer(hwnd, IDT_QWEN, 60000, NULL);
    CreateThread(NULL, 0, qwen_thread, prompt, 0, NULL);
}

/* 散步中任何操作 → 原地停 10 秒（IDT_WALK 50ms×200），再次操作重置计时 */
static void walk_hold(void) {
    if (walk_on)
        walk_pause = 200;
}

/* 眨眼帧选择：正面闭眼帧优先；只有正面睁眼没闭眼 → 不眨（切旧图会闪）；没正面图 → 旧侧视闭眼 */
/* 多实例启动参数。中文名字走 GetCommandLineW，不经任何 ANSI 代码页 */
static void parse_cmdline(void) {
    WCHAR *cmd = GetCommandLineW(), *p;
    int i;
    if ((p = wcsstr(cmd, L"--slot ")) != NULL) {
        selfSlot = _wtoi(p + 7);
        if (selfSlot < 0 || selfSlot > 5)
            selfSlot = 0;
    }
    if ((p = wcsstr(cmd, L"--name ")) != NULL) {
        p += 7;
        i = 0;
        while (*p && *p != L' ' && i < 22)
            selfName[i++] = *p++;
        selfName[i] = 0;
    }
    if ((p = wcsstr(cmd, L"--model ")) != NULL) {
        p += 8;
        i = 0;
        while (*p && *p != L' ' && i < 62)
            selfModel[i++] = *p++;
        selfModel[i] = 0;
        /* 子进程（inference_backend/ma）继承此环境变量=这只白的专属模型；
           key 不在这——三只同端点共用 settings.json 那把，无需逐只配置 */
        SetEnvironmentVariableW(L"PET_LLM_MODEL", selfModel);
    }
    if ((p = wcsstr(cmd, L"--backend ")) != NULL) {
        if (wcsncmp(p + 10, L"local", 5) == 0)
            backend_local = 1;
        else if (wcsncmp(p + 10, L"ma", 2) == 0)
            backend_local = 2;
        else
            backend_local = 0;
    }
    if ((p = wcsstr(cmd, L"--skin ")) != NULL) {
        p += 7;
        i = 0;
        while (*p && *p != L' ' && i < 62)
            skinArg[i++] = *p++;
        skinArg[i] = 0;
    }
    /* 多服务商接线：--base/--keyfile 写进环境变量，子进程后端自己读。
       base 指任何 OpenAI 兼容服务（DeepSeek 官方/智谱官方/本机 llama.cpp、Ollama）；
       keyfile 第一行=密钥原文。路径别带空格 */
    if ((p = wcsstr(cmd, L"--base ")) != NULL) {
        WCHAR vb[256];
        int k = 0;
        p += 7;
        while (*p && *p != L' ' && k < 255)
            vb[k++] = *p++;
        vb[k] = 0;
        SetEnvironmentVariableW(L"PET_LLM_BASE", vb);
    }
    if ((p = wcsstr(cmd, L"--keyfile ")) != NULL) {
        WCHAR vb[256];
        int k = 0;
        p += 10;
        while (*p && *p != L' ' && k < 255)
            vb[k++] = *p++;
        vb[k] = 0;
        SetEnvironmentVariableW(L"PET_LLM_KEYFILE", vb);
    }
    /* 皮肤目录默认跟名字走：assets\白2\ 里有谁就换谁，没有全用公共的 */
    {
        const WCHAR *sk = skinArg[0] ? skinArg : selfName;
        lstrcpyW(g_skin_dir, g_asset_dir);
        lstrcatW(g_skin_dir, sk);
        lstrcatW(g_skin_dir, L"\\");
    }
}

static GpBitmap *blink_art(void) {
    if (img_blinkf)
        return img_blinkf;
    if (img_front)
        return NULL;
    return img_stand2;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        POINT pt;
        pt.x = LOWORD(lp);
        pt.y = HIWORD(lp);
        dbg("down %d,%d", (int)pt.x, (int)pt.y);
        captured = TRUE;
        dragging = FALSE;
        downPt = pt;
        {
            /* 屏幕坐标抓取偏移：客户区坐标会随窗口移动自激振荡 */
            POINT cp;
            RECT rc;
            GetCursorPos(&cp);
            GetWindowRect(h, &rc);
            grabOff.x = cp.x - rc.left;
            grabOff.y = cp.y - rc.top;
        }
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt;
        if (!captured)
            return 0;
        pt.x = LOWORD(lp);
        pt.y = HIWORD(lp);
        if (!dragging && (abs(pt.x - downPt.x) > 6 || abs(pt.y - downPt.y) > 6)) {
            dragging = TRUE;
            bowing = FALSE;
            KillTimer(h, IDT_BOW);
            cur = img_drag;
        }
        if (dragging) {
            POINT cp;
            GetCursorPos(&cp);
            SetWindowPos(h, NULL, cp.x - grabOff.x, cp.y - grabOff.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            render();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt;
        pt.x = LOWORD(lp);
        pt.y = HIWORD(lp);
        if (captured) {
            ReleaseCapture();
            captured = FALSE;
        }
        if (dragging) {
            dragging = FALSE;
            cur = sitting ? img_kneel1
                : (walk_on && walk_n > 1) ? img_walk[step_phase % walk_n] : img_stand1;
            walk_hold();
            render();
            return 0;
        }
        if (pt.x >= sprX && pt.x < sprX + sprW && pt.y >= sprY && pt.y < sprY + sprH) {
            dbg("hit sprite y=%d head=%d", (int)pt.y,
                (int)(pt.y < sprY + (int)(sprH * HEAD_RATIO)));
            if (sitting) {
                /* 跪坐中再点一下 → 起来，不重新鞠躬 */
                sitting = FALSE;
                cur = (walk_on && walk_n > 1) ? img_walk[step_phase % walk_n] : img_stand1;
                walk_hold();
                render();
                talk();
                return 0;
            }
            if (pt.y < sprY + (int)(sprH * HEAD_RATIO)) {
                GpBitmap *ba = !(walk_on && walk_n > 1) ? blink_art() : NULL;
                if (ba) {
                    cur = ba;
                    SetTimer(h, IDT_BLINKOFF, 350, NULL);
                }
            } else {
                bowing = TRUE;
                cur = img_kneel1;
                SetTimer(h, IDT_BOW, 900, NULL);
            }
            walk_hold();
            talk();
            if (!simple_mode)
                open_dialog();
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        simple_mode = !simple_mode;
        if (simple_mode) {
            talking = FALSE;
            close_dialog();
        }
        relayout(TRUE);
        return 0;
    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) < 0 && wp == 'E') {
            q_mode = !q_mode;
            render();
            return 0;
        }
        if (walk_on && (wp == VK_LEFT || wp == VK_RIGHT)) {
            walk_dir = (wp == VK_LEFT) ? -1 : 1;
            walk_hold();
            return 0;
        }
        if (!dialog_open)
            return 0;
        if (GetKeyState(VK_CONTROL) < 0) {
            if (wp == 'C')
                clip_copy();
            else if (wp == 'V')
                clip_paste();
            return 0;
        }
        if (wp == VK_BACK) {
            if (inp_len > 0) {
                inp_len--;
                inp[inp_len] = 0;
                render();
            }
        } else if (wp == VK_RETURN) {
            submit();
        } else if (wp == VK_ESCAPE) {
            close_dialog();
        }
        return 0;
    case WM_CHAR:
        if (!dialog_open || wp < 32)
            return 0;
        dbg("char %u", (unsigned)wp);
        walk_hold();
        if (inp_len < 511) {
            inp[inp_len++] = (WCHAR)wp;
            inp[inp_len] = 0;
            render();
        }
        return 0;
    case WM_QWEN_DONE: {
        WCHAR *res = (WCHAR *)lp;
        dbg("done res=%d", res != NULL);
        waiting = FALSE;
        KillTimer(h, IDT_QWEN);
        KillTimer(h, IDT_TALK);
        if (res && res[0]) {
            if (!simple_mode) {
                reply_mode = TRUE;
                say(res, 9000);
            }
            free(res);
        } else {
            free(res);
            if (!simple_mode) {
                reply_mode = FALSE;
                say(L"……qwen 没回应。笨蛋机器。", 2600);
            }
        }
        return 0;
    }
    case WM_STREAM_TOKEN: {
        WCHAR *w = (WCHAR *)lp;
        if ((unsigned long long)wp != cur_req) {
            /* v2 请求 ID 校验的正解：旧请求迟到的 token 直接丢，不碰气泡 */
            free(w);
            return 0;
        }
        if (!stream_started) {
            talkText[0] = 0;   /* 首 token 顶掉"……想着。"占位 */
            stream_started = 1;
        }
        {
            int tl = (int)wcslen(talkText), wl = (int)wcslen(w), i = 0;
            while (i < wl && tl < 1020)
                talkText[tl++] = w[i++];
            talkText[tl] = 0;
        }
        free(w);   /* 契约：读取线程 malloc，UI 用完 free */
        if (!simple_mode) {
            talking = TRUE;
            relayout(TRUE);   /* 气泡宽高由 bubble.h 引擎按新字数重算 */
        }
        return 0;
    }
    case WM_STREAM_END: {
        unsigned long long id = (unsigned long long)wp;
        DWORD code = (DWORD)(UINT_PTR)lp;
        if (id != cur_req)
            return 0;
        waiting = FALSE;
        KillTimer(h, IDT_QWEN);
        KillTimer(h, IDT_TALK);
        if (code != 0 || !stream_started || !talkText[0]) {
            reply_mode = FALSE;
            say(L"……backend 没声音。笨蛋机器。", 2600);
        } else {
            reply_mode = TRUE;
            talking = TRUE;
            if (backend_local == 1 && hist_n < 8) {
                lstrcpynW(histA[hist_n], talkText, 96);
                hist_n++;
            }
            SetTimer(h, IDT_TALK, 8000, NULL);
            relayout(TRUE);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        HMENU menu, size_menu, tool_menu, backend_menu, role_menu;
        POINT pt;
        GetCursorPos(&pt);
        menu = CreatePopupMenu();
        size_menu = CreatePopupMenu();
        AppendMenuW(size_menu, MF_STRING, IDM_SIZE_BASE + 20, L"20%");
        AppendMenuW(size_menu, MF_STRING, IDM_SIZE_BASE + 40, L"40%");
        AppendMenuW(size_menu, MF_STRING, IDM_SIZE_BASE + 60, L"60%");
        AppendMenuW(size_menu, MF_STRING, IDM_SIZE_BASE + 80, L"80%");
        AppendMenuW(size_menu, MF_STRING, IDM_SIZE_BASE + 100, L"100%");
        CheckMenuRadioItem(size_menu, IDM_SIZE_BASE + 20, IDM_SIZE_BASE + 100,
                           IDM_SIZE_BASE + pct, MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)size_menu, L"大小");
        tool_menu = CreatePopupMenu();
        AppendMenuW(tool_menu, MF_STRING | (show_cpu ? MF_CHECKED : 0), IDM_CPU, L"CPU");
        AppendMenuW(tool_menu, MF_STRING | (show_mem ? MF_CHECKED : 0), IDM_MEM, L"内存");
        AppendMenuW(tool_menu, MF_STRING | (show_disk ? MF_CHECKED : 0), IDM_DISK, L"存储");
        AppendMenuW(tool_menu, MF_STRING | (show_net ? MF_CHECKED : 0), IDM_NET, L"网速");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)tool_menu, L"工具");
        AppendMenuW(menu, MF_STRING | (alarm_set ? MF_CHECKED : 0), IDM_ALARM, L"闹钟");
        AppendMenuW(menu, MF_STRING | (walk_on ? MF_CHECKED : 0), IDM_WALK, L"散步");
        backend_menu = CreatePopupMenu();
        AppendMenuW(backend_menu, MF_STRING, IDM_BACKEND_QWEN, L"qwen CLI");
        AppendMenuW(backend_menu, MF_STRING, IDM_BACKEND_LOCAL, L"本地管道");
        AppendMenuW(backend_menu, MF_STRING, IDM_BACKEND_MA, L"多智能体");
        CheckMenuRadioItem(backend_menu, IDM_BACKEND_QWEN, IDM_BACKEND_MA,
                           IDM_BACKEND_QWEN + backend_local, MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)backend_menu, L"推理后端");
        role_menu = CreatePopupMenu();
        AppendMenuW(role_menu, MF_STRING, IDM_ROLE_BASE, L"主管（拆解+验收）");
        AppendMenuW(role_menu, MF_STRING, IDM_ROLE_BASE + 1, L"员工A（单卡直通）");
        AppendMenuW(role_menu, MF_GRAYED, IDM_ROLE_BASE + 2, L"员工B（并行·阶段2）");
        AppendMenuW(role_menu, MF_GRAYED, IDM_ROLE_BASE + 3, L"员工C（并行·阶段2）");
        CheckMenuRadioItem(role_menu, IDM_ROLE_BASE, IDM_ROLE_BASE + 3,
                           IDM_ROLE_BASE + ma_role, MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)role_menu, L"多智能体角色");
        AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");
        SetForegroundWindow(h);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
        DestroyMenu(menu);
        return 0;
    }
    case WM_COMMAND:
        if (wp == IDM_EXIT) {
            if (!exiting) {
                exiting = TRUE;
                if (!simple_mode)
                    say(L"Bye!", 1500);
                SetTimer(h, IDT_EXITANIM, 30, NULL);
            }
        } else if (wp >= IDM_CPU && wp <= IDM_NET) {
            if (wp == IDM_CPU) show_cpu = !show_cpu;
            else if (wp == IDM_MEM) show_mem = !show_mem;
            else if (wp == IDM_DISK) show_disk = !show_disk;
            else show_net = !show_net;
            relayout(TRUE);
            if (info_n) {
                refresh_info();
                SetTimer(h, IDT_SYSINFO, 2000, NULL);
            } else {
                KillTimer(h, IDT_SYSINFO);
            }
            render();
        } else if (wp == IDM_ALARM) {
            if (!alarm_set) {
                alarm_input_mode = TRUE;
                inp_len = 0;
                inp[0] = 0;
                open_dialog();
            } else {
                alarm_set = FALSE;
                ringing = FALSE;
                KillTimer(h, IDT_ALARM);
                KillTimer(h, IDT_RING);
            }
            render();
        } else if (wp == IDM_WALK) {
            walk_on = !walk_on;
            if (walk_on) {
                /* 跪坐中开散步必须先起身：IDT_WALK 遇 sitting 直接 break，
                   否则菜单勾上了她却原地不动 */
                if (sitting) {
                    sitting = FALSE;
                    cur = img_stand1;
                }
                SetTimer(h, IDT_WALK, 50, NULL);
                SetTimer(h, IDT_STEP, walk_n > 1 ? WALK_CYCLE_MS / walk_n : 200, NULL);
                SetTimer(h, IDT_HOVER, 200, NULL);
            } else {
                KillTimer(h, IDT_WALK);
                KillTimer(h, IDT_STEP);
                KillTimer(h, IDT_HOVER);
                hover_region = -1;
                cur = img_stand1;
                render();
            }
        } else if (wp == IDM_BACKEND_QWEN) {
            backend_local = 0;
        } else if (wp == IDM_BACKEND_LOCAL) {
            backend_local = 1;
        } else if (wp == IDM_BACKEND_MA) {
            backend_local = 2;
        } else if (wp >= IDM_ROLE_BASE && wp <= IDM_ROLE_BASE + 1) {
            /* B/C 菜单已灰显；就算被程序化硬发命令也进不来，调度闸在 ma.exe 那边挡第二道 */
            ma_role = (int)(wp - IDM_ROLE_BASE);
        } else if (wp >= IDM_SIZE_BASE + 20 && wp <= IDM_SIZE_BASE + 100) {
            pct = (int)(wp - IDM_SIZE_BASE);
            relayout(TRUE);
        }
        return 0;
    case WM_TIMER:
        switch (wp) {
        case IDT_BLINK:
            /* 散步中不眨：会切到不同机位的图，闪一下；跪坐只有拿到同机位闭眼帧才眨 */
            if (!bowing && !dragging && !(walk_on && walk_n > 1)) {
                GpBitmap *ba = sitting ? img_kneelb : blink_art();
                if (ba) {
                    cur = ba;
                    render();
                    SetTimer(h, IDT_BLINKOFF, 220, NULL);
                }
            }
            SetTimer(h, IDT_BLINK, 2000 + rand() % 2500, NULL);
            break;
        case IDT_BLINKOFF:
            KillTimer(h, IDT_BLINKOFF);
            if (!bowing && !dragging && !(walk_on && walk_n > 1)) {
                cur = sitting ? img_kneel1 : img_stand1;
                render();
            }
            break;
        case IDT_TALK:
            KillTimer(h, IDT_TALK);
            talking = FALSE;
            dbg("talk end");
            relayout(TRUE);
            break;
        case IDT_BOW:
            KillTimer(h, IDT_BOW);
            bowing = FALSE;
            if (!dragging) {
                /* 哥哥：鞠躬结束后坐下保持跪坐，再点一下才起来 */
                sitting = TRUE;
                cur = img_kneel1;
                render();
            }
            break;
        case IDT_CARET:
            caret_on = !caret_on;
            render();
            break;
        case IDT_QWEN:
            KillTimer(h, IDT_QWEN);
            if (waiting) {
                waiting = FALSE;
                reply_mode = FALSE;
                say(L"……qwen 超时了。", 2600);
            }
            break;
        case IDT_SQUASH:
            KillTimer(h, IDT_SQUASH);
            squash = FALSE;
            cur = sitting ? img_kneel1
                : (walk_on && walk_n > 1) ? img_walk[step_phase % walk_n] : img_stand1;
            render();
            break;
        case IDT_TURN:
            KillTimer(h, IDT_TURN);
            dbg("turn %d", turn_stage);
            if (turn_stage == 1) {
                cur = img_front;
                turn_stage = 2;
                SetTimer(h, IDT_TURN, 380, NULL);
            } else {
                turn_stage = 0;
                render();
            }
            break;
        case IDT_WALK: {
            RECT rc;
            int nx;
            if (captured || dragging || sitting)
                break;
            if (walk_pause > 0) {
                walk_pause--;
                break;
            }
            GetWindowRect(h, &rc);
            nx = rc.left + walk_dir * (sprH / 60 + 2);
            if (nx < workarea.left + 4) {
                nx = workarea.left + 4;
                walk_dir = 1;
                dbg("bump left");
                if (img_front && img_back) {
                    turn_stage = 1;
                    cur = img_back;
                    walk_pause = 16;
                    SetTimer(h, IDT_TURN, 380, NULL);
                } else {
                    squash = TRUE;
                    SetTimer(h, IDT_SQUASH, 400, NULL);
                }
                say(L"……痛。墙，笨蛋。", 2600);
            } else if (nx + winW > workarea.right - 4) {
                nx = workarea.right - 4 - winW;
                walk_dir = -1;
                dbg("bump right");
                if (img_front && img_back) {
                    turn_stage = 1;
                    cur = img_back;
                    walk_pause = 16;
                    SetTimer(h, IDT_TURN, 380, NULL);
                } else {
                    squash = TRUE;
                    SetTimer(h, IDT_SQUASH, 400, NULL);
                }
                say(L"……痛。墙，笨蛋。", 2600);
            }
            SetWindowPos(h, NULL, nx, rc.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (rand() % 250 == 0)
                walk_pause = 20 + rand() % 40;
            if (rand() % 500 == 0)
                talk();
            render();
            break;
        }
        case IDT_STEP:
            if (!bowing && !dragging && !squash && !sitting && !turn_stage) {
                if (walk_n > 1) {
                    step_phase = (step_phase + 1) % walk_n;
                    cur = img_walk[step_phase];
                } else {
                    /* 没散步素材时退回两帧站立交替的老逻辑 */
                    step_phase ^= 1;
                    cur = step_phase ? img_stand2 : img_stand1;
                }
                render();
            }
            break;
        case IDT_HOVER: {
            POINT cp;
            int region = -1;
            GetCursorPos(&cp);
            ScreenToClient(h, &cp);
            if (cp.x >= sprX && cp.x < sprX + sprW && cp.y >= sprY && cp.y < sprY + sprH) {
                if (cp.y < sprY + (int)(sprH * HEAD_RATIO))
                    region = 0;
                else if (cp.y > sprY + sprH * 85 / 100)
                    region = 2;
                else
                    region = 1;
            }
            if (region != hover_region && !sitting) {
                hover_region = region;
                if (walk_on && region == 0) {
                    walk_pause = 15;
                    if (walk_n < 2)
                        cur = img_stand2;
                    say(L"……别、别摸头。害羞。", 2600);
                } else if (walk_on && region == 2) {
                    walk_pause = 20;
                    squash = TRUE;
                    SetTimer(h, IDT_SQUASH, 450, NULL);
                    say(L"哇——！绊、绊倒了。笨蛋脚。", 2600);
                } else if (walk_on && region == 1) {
                    if (walk_n < 2) {
                        cur = img_stand2;
                        SetTimer(h, IDT_BLINKOFF, 250, NULL);
                    }
                }
                render();
            }
            break;
        }
        case IDT_SYSINFO:
            if (info_n) {
                refresh_info();
                render();
            }
            break;
        case IDT_INTRO: {
            RECT rc;
            int target = workarea.bottom - winH - 8;
            GetWindowRect(h, &rc);
            if (rc.top > target) {
                int ny = rc.top - (winH / 20 + 2);
                if (ny < target)
                    ny = target;
                SetWindowPos(h, NULL, rc.left, ny, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                render();
            } else {
                intro = FALSE;
                KillTimer(h, IDT_INTRO);
                if (!simple_mode)
                    say(L"Hi!", 2000);
            }
            break;
        }
        case IDT_EXITANIM: {
            RECT rc;
            GetWindowRect(h, &rc);
            if (rc.top > workarea.bottom) {
                DestroyWindow(h);
                break;
            }
            SetWindowPos(h, NULL, rc.left, rc.top + winH / 20 + 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            render();
            break;
        }
        case IDT_RANDOM: {
            SYSTEMTIME st;
            SetTimer(h, IDT_RANDOM, 20000 + rand() % 20000, NULL);
            if (walk_on || dragging || exiting || intro || waiting || acting)
                break;
            GetLocalTime(&st);
            if (st.wMonth == 1 && st.wDay == 1) {
                acting = 4;
                say(L"……新年。洗衣服，甩干。", 2600);
            } else if (st.wMonth == 12 && st.wDay == 25) {
                acting = 4;
                say(L"……老虎机，jackpot ♪", 2600);
            } else {
                acting = 1 + rand() % 3;
                if (acting == 1)
                    say(L"♪ 跳舞，跳舞。", 2600);
                else if (acting == 2)
                    say(L"……看书。嘘。", 2600);
                else
                    say(L"……喝茶，歇会。", 2600);
            }
            SetTimer(h, IDT_ACT, 2200, NULL);
            render();
            break;
        }
        case IDT_ACT:
            acting = 0;
            render();
            break;
        case IDT_ALARM: {
            SYSTEMTIME st;
            if (!alarm_set)
                break;
            GetLocalTime(&st);
            if (!ringing && st.wHour == alarm_h && st.wMinute == alarm_m) {
                ringing = TRUE;
                ring_count = 0;
                SetTimer(h, IDT_RING, 500, NULL);
                say(L"……哥哥，时间到。起床。", 6000);
            }
            break;
        }
        case IDT_RING:
            squash = !squash;
            ring_count++;
            PlaySoundW(ring_count % 2 ? L"SystemExclamation" : L"SystemHand",
                       NULL, SND_ALIAS | SND_ASYNC);
            if (ring_count >= 8) {
                ringing = FALSE;
                squash = FALSE;
                KillTimer(h, IDT_RING);
            }
            render();
            break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cl, int show) {
    WNDCLASSW wc;
    RECT wa;
    ULONG_PTR token;
    GdiplusStartupInput si;
    MSG msg;
    (void)hp;
    (void)cl;
    (void)show;
    init_paths();
    parse_cmdline();
    {
        char sk[256];
        WideCharToMultiByte(CP_UTF8, 0, g_skin_dir, -1, sk, sizeof sk, NULL, NULL);
        dbg("boot slot=%d backend=%d skin=%s", selfSlot, backend_local, sk);
    }
    if (g_path_probe_fail)
        dbg("boot WARN exe dir probe failed, running from current dir");
    /* qwen CLI 运行时定位（原来 .cmd 全路径写死在源码里）。
       这台机器没有 qwen CLI 就把后端改成 local，别把请求丢进死路 */
    if (compat_find_qwen(g_qwen_path, (int)(sizeof g_qwen_path / sizeof g_qwen_path[0])))
        dbg("boot qwen cli found");
    else {
        dbg("boot qwen cli NOT found");
        if (backend_local == 0) {
            backend_local = 1;
            dbg("boot backend forced to local pipe");
        }
    }
    /* DPI 感知与取值全部运行时解析（compat.h）：老系统没有 Win10 1703+ 的导出，
       静态导入会让 exe 在 loader 阶段就起不来 */
    sys_dpi = compat_dpi_init();
    dbg("boot dpi=%u compat v%d", sys_dpi, PET_COMPAT_VER);
    memset(&si, 0, sizeof(si));
    si.GdiplusVersion = 1;
    GdiplusStartup(&token, &si, NULL);
    srand((unsigned)GetTickCount());
    img_stand1 = load_png("桌宠站立形象1.png");
    img_stand2 = load_png("桌宠站立形象2.png");
    img_kneel1 = load_png("桌宠跪坐形象1.png");
    img_drag = load_png("桌宠被鼠标拖动时形象.png");
    img_bubble = load_png("聊天气泡.png");
    build_bubble_slices();
    img_q = load_png_opt("桌宠Q版.png");
    img_inbar = load_png_opt("输入条.png");
    {
        int i;
        char nm[32];
        for (i = 0; i < WALK_MAX; i++) {
            snprintf(nm, sizeof nm, "walk\\walk%02d.png", i + 1);
            img_walk[i] = load_png_opt(nm);
            if (!img_walk[i])
                break;
            walk_n++;
        }
        dbg("boot walk=%d", walk_n);
    }
    img_front = load_png_opt("front.png");
    img_back = load_png_opt("back.png");
    img_blinkf = load_png_opt("blink.png");
    img_kneelb = load_png_opt("kneel_blink.png");
    if (img_front)
        img_stand1 = img_front;   /* 正面图兼任新待机站姿 */
    dbg("boot front=%p back=%p blink=%p kneelb=%p", (void *)img_front, (void *)img_back,
        (void *)img_blinkf, (void *)img_kneelb);
    dbg("boot inbar=%p dpi=%u", (void *)img_inbar, sys_dpi);
    cur = img_stand1;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"ShiroPetV2";
    RegisterClassW(&wc);
    memDC = CreateCompatibleDC(NULL);
    layout();
    rebuild_dib();
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    workarea = wa;
    hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                           L"ShiroPetV2", L"ShiroPet", WS_POPUP,
                           (wa.right - winW - 24 - selfSlot * (winW + 30)), wa.bottom - winH - 8,
                           winW, winH, NULL, NULL, hi, NULL);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, NULL, (wa.right - winW - 24 - selfSlot * (winW + 30)), wa.bottom, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    intro = TRUE;
    SetTimer(hwnd, IDT_INTRO, 30, NULL);
    SetTimer(hwnd, IDT_RANDOM, 20000 + rand() % 20000, NULL);
    render();
    SetTimer(hwnd, IDT_BLINK, 2000 + rand() % 2500, NULL);
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
