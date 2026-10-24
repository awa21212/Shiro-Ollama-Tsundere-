/* bubble.h —— 聊天气泡尺寸计算引擎（纯计算，不依赖 Win32/GDI+，可单测）
 *
 * 规格来源：哥哥 2026-10-20 给的「虚拟伴侣渲染引擎」ChatBubble_t / RenderBubble 伪代码。
 * 相对规格的两处工程修正，都在下面标了 [FIX]：
 *   [FIX-1] 规格用 strlen()。strlen 数的是**字节**，台词是 UTF-8，一个汉字 3 字节，
 *           "……陪我玩。" 6 个字会被数成 18，150+18*20=510px，比正确值 270px 宽 89%。
 *           所以 text_content 保持 char*（和 pet.c 的 LINES[] 一致），但长度走
 *           bubble_charlen() 数代码点。struct 类型没改，语义改对了。
 *   [FIX-2] 宽度上限规格自相矛盾：伪代码写死 >600，执行指令#3 写"屏幕宽度的 80%"。
 *           两个都实现，取 min(600, screen_w*0.80)。screen_w 传 0 表示只用 600。
 */
#ifndef PET_BUBBLE_H
#define PET_BUBBLE_H

#include <wchar.h>

/* 情感标签（规格定义，未定义视觉表现；pet.c 里映射成文字颜色） */
#define EMO_LOST   1   /* 失落 */
#define EMO_CALM   2   /* 平静 */
#define EMO_HAPPY  3   /* 开心 */

/* 设计单位：96 DPI / pct=100 基准下的像素，全部取自规格原值 */
#define BUB_BASE_W        150
#define BUB_W_PER_CHAR     20
#define BUB_BASE_H          80
#define BUB_H_PER_10CH      15   /* char_length/10*15，整除，有死区 */
#define BUB_MAX_W          600
#define BUB_WRAP_H_BONUS    30   /* 触发上限强制换行时补的高度 */
#define BUB_FONT_PX         18   /* 原规格恒定 14px；16→18 是哥哥 2026-10-21 要求再放大一些 */
#define BUB_LINE_RATIO    1.5f   /* 执行指令#4：行距 1.5 倍 */
#define BUB_TAIL_STRETCH_W 400   /* 执行指令#4：超过此宽度尾部需平滑拉伸 */
#define BUB_SCREEN_CAP   0.80f   /* 执行指令#3：不超过屏宽 80% */

/* 文字区占气泡的比例（沿用 pet.c render() 现有的 0.72 / 0.14 内边距） */
#define BUB_TEXT_W_RATIO 0.72f
#define BUB_TEXT_H_RATIO 0.72f

typedef struct {
    char *text_content;   /* 台词内容，UTF-8 */
    int char_length;      /* 字符长度（代码点数，不是字节数） */
    int bubble_width;     /* 气泡宽度 px */
    int bubble_height;    /* 气泡高度 px */
    int emotion_type;     /* 1:失落 2:平静 3:开心 */
} ChatBubble_t;

/* [FIX-1] 数 UTF-8 代码点：跳过 0x80-0xBF 的后续字节。
 * 单字节 ASCII、2/3/4 字节序列的首字节都满足 (b & 0xC0) != 0x80。 */
static int bubble_charlen(const char *s) {
    int n = 0;
    if (!s)
        return 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80)
            n++;
    return n;
}

/* 规格 RenderBubble：基础尺寸 + 字数增量，超上限则封顶并补高。 */
static void bubble_apply(ChatBubble_t *b, int screen_w) {
    int cap, w, h;
    w = BUB_BASE_W + b->char_length * BUB_W_PER_CHAR;
    h = BUB_BASE_H + (b->char_length / 10) * BUB_H_PER_10CH;

    /* [FIX-2] 两套上限取小 */
    cap = BUB_MAX_W;
    if (screen_w > 0) {
        int s80 = (int)(screen_w * BUB_SCREEN_CAP);
        if (s80 < cap)
            cap = s80;
    }
    if (w > cap) {
        w = cap;
        h += BUB_WRAP_H_BONUS;
    }
    b->bubble_width = w;
    b->bubble_height = h;
}

static void RenderBubble(ChatBubble_t *b, int screen_w) {
    if (!b)
        return;
    b->char_length = bubble_charlen(b->text_content);
    bubble_apply(b, screen_w);
}

/* pet.c 的台词已是 UTF-16（talkText），BMP 中文一码元一字，用 wcslen。
 * 规格 struct 的 char* 留给 UTF-8 输入路径（RenderBubble）。 */
static void RenderBubbleW(ChatBubble_t *b, const wchar_t *w, int screen_w) {
    if (!b)
        return;
    b->text_content = NULL;
    b->char_length = w ? (int)wcslen(w) : 0;
    bubble_apply(b, screen_w);
}

/* 规格没给的配套计算：按字号估算折行后需要多高，用来检验公式会不会切字。
 * CJK 全角字形宽约 1em，所以每行字数 ≈ 文字区宽 / 字号。
 * 返回需要的行数；*need_h 回填这些行按 1.5 倍行距占的高度。 */
static int bubble_wrap_lines(const ChatBubble_t *b, int font_px, int *need_h) {
    int text_w = (int)(b->bubble_width * BUB_TEXT_W_RATIO);
    int cpl = text_w / (font_px > 0 ? font_px : BUB_FONT_PX);
    int lines;
    if (cpl < 1)
        cpl = 1;
    if (b->char_length <= 0) {
        if (need_h) *need_h = 0;
        return 0;
    }
    lines = (b->char_length + cpl - 1) / cpl;
    if (need_h)
        *need_h = (int)(lines * font_px * BUB_LINE_RATIO);
    return lines;
}

/* 气泡是否已宽到需要按执行指令#4 处理尾部拉伸 */
static int bubble_tail_stretch(const ChatBubble_t *b) {
    return b && b->bubble_width > BUB_TAIL_STRETCH_W;
}

#endif /* PET_BUBBLE_H */
