/* agent_config.h —— 多智能体后端角色配置 + 调度闸（ma.c 专用，header-only）
 *
 * 编译: gcc -O2 -o ma.exe ma.c -lwinhttp   （ma.c #include "agent_config.h"）
 * 设计原则：
 *   1. 角色差异全部收进一张 static const 表，代码里不撒 if(role==...) 的配置判断
 *   2. 资源检查集中在 schedule_task 一个闸门，三关全过才放行
 *   3. 任何计数器只走带 clamp 的入口，不裸 +=
 */
#ifndef AGENT_CONFIG_H
#define AGENT_CONFIG_H

#include <windows.h>

/* ---------------- 1. 类型定义 ---------------- */

typedef enum {
    ROLE_MANAGER = 0,   /* 主管：拆任务、验收、汇总            */
    ROLE_EMP_A,         /* 员工A：主执行者（串行，阶段1已上线）*/
    ROLE_EMP_B,         /* 员工B：并行工人（阶段2，先占位）    */
    ROLE_EMP_C,         /* 员工C：质检工人（阶段2，先占位）    */
    ROLE_COUNT
} AgentRole;

typedef enum {
    EXEC_SERIAL = 0,    /* 主管 / 员工A：一次一件事            */
    EXEC_PARALLEL = 1   /* 员工B / 员工C：一卡多任务           */
} ExecMode;

typedef enum {
    SCHED_OK = 0,       /* 放行                                */
    SCHED_BUSY,         /* 卡上任务满 / 角色未启用             */
    SCHED_THROTTLED,    /* 本轮模型调用配额用完                */
    SCHED_CTX_REFUSE,   /* 上下文过红线，拒开新任务            */
    SCHED_BAD_ROLE      /* 枚举越界：防御性分支                */
} ScheduleResult;

#define MAX_TASKS_PER_CARD      5
#define MAX_CALLS_PER_ROUND     8
#define CTX_TOTAL_TOKENS        1600000L          /* 1.6M */
#define CTX_WARN_TOKENS         (CTX_TOTAL_TOKENS * 75 / 100)   /* 黄线：开始裁剪 */
#define CTX_TRIM_TOKENS         (CTX_TOTAL_TOKENS * 85 / 100)   /* 橙线：强制裁剪 */
#define CTX_HARD_TOKENS         (CTX_TOTAL_TOKENS * 92 / 100)   /* 红线：只收不发 */

typedef struct {
    AgentRole role;
    ExecMode  mode;
    int  max_tasks;        /* 表值必须 <= MAX_TASKS_PER_CARD，init 里有断言 */
    int  calls_left;       /* 本轮剩余模型调用（每轮开始重新 init 即重置）  */
    long ctx_used;         /* tokens；改它只许走 ctx_add()                  */
    int  enabled;          /* 0 = 阶段未实现，菜单灰显，调度器直接 BUSY     */
} AgentConfig;

/* ---------------- 2. 配置映射表 ---------------- */

static const AgentConfig g_role_tbl[ROLE_COUNT] = {
    /* role          mode           tasks calls ctx enabled */
    { ROLE_MANAGER,  EXEC_SERIAL,     1, MAX_CALLS_PER_ROUND, 0, 1 },
    { ROLE_EMP_A,    EXEC_SERIAL,     5, MAX_CALLS_PER_ROUND, 0, 1 },
    { ROLE_EMP_B,    EXEC_PARALLEL,   5, MAX_CALLS_PER_ROUND, 0, 0 },  /* 阶段2 */
    { ROLE_EMP_C,    EXEC_PARALLEL,   5, MAX_CALLS_PER_ROUND, 0, 0 },  /* 阶段2 */
};

/* 编译期防御：表条数必须 == 枚举个数，加角色忘改表直接编译失败 */
typedef char agent_tbl_size_check[
    (sizeof(g_role_tbl) / sizeof(g_role_tbl[0]) == ROLE_COUNT) ? 1 : -1];

static int init_agent_config(AgentRole role, AgentConfig *out) {
    if (!out)
        return 0;
    if ((int)role < 0 || role >= ROLE_COUNT)
        return 0;                       /* 越界不猜，直接失败 */
    *out = g_role_tbl[role];
    if (out->max_tasks > MAX_TASKS_PER_CARD)
        out->max_tasks = MAX_TASKS_PER_CARD;   /* 表写错也不越帽 */
    return 1;
}

/* 上下文记账唯一入口：负数夹 0，超量夹顶，永不回绕 */
static long ctx_add(AgentConfig *cfg, long delta) {
    long v = cfg->ctx_used + delta;
    if (v < 0)
        v = 0;
    if (v > CTX_TOTAL_TOKENS)
        v = CTX_TOTAL_TOKENS;
    cfg->ctx_used = v;
    return v;
}

/* ---------------- 3. 调度器 ---------------- */

/* current_tasks：卡上在飞任务数。单线程读 int 即可；
 * 阶段2 并行后必须换 InterlockedAdd 读取，见下方注释占位 */
static ScheduleResult schedule_task(AgentConfig *cfg, int current_tasks) {
    if (!cfg)
        return SCHED_BAD_ROLE;

    switch (cfg->role) {
    case ROLE_MANAGER:
    case ROLE_EMP_A:
        /* 串行：任何时刻在飞 ≤1。>1 说明状态机出错，宁可拒派 */
        if (current_tasks > 1)
            return SCHED_BUSY;
        break;

    case ROLE_EMP_B:
    case ROLE_EMP_C:
        /* 并行：阶段1 未实现 → enabled=0 挡在闸外，逻辑先摆对 */
        if (!cfg->enabled)
            return SCHED_BUSY;
        if (current_tasks >= cfg->max_tasks)
            return SCHED_BUSY;
        /* [阶段2] 占坑必须 CAS，不能"先查后加"：
         *   if (InterlockedCompareExchange(&card.inflight, n+1, n) != n) goto busy;
         * 释放用 InterlockedDecrement；locks/信号量按方案留到那个阶段 */
        break;

    default:
        return SCHED_BAD_ROLE;          /* 枚举被外部写坏也走这 */
    }

    /* 三关共用闸：每轮调用配额 → 上下文水位 → 放行 */
    if (cfg->calls_left <= 0)
        return SCHED_THROTTLED;
    if (cfg->ctx_used >= CTX_HARD_TOKENS)
        return SCHED_CTX_REFUSE;
    /* 黄/橙线不在这里拦，交给调用方：WARN→裁剪历史，TRIM→强制压缩后再派 */

    cfg->calls_left--;
    return SCHED_OK;
}

#endif /* AGENT_CONFIG_H */
