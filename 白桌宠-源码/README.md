# 桌宠「白」· Shiro Pet

> 纯 C（Win32 + GDI+）写的 Windows 桌面宠物，无第三方运行库。  
> 会站立、跪坐、眨眼、散步、被拎起来，也能聊天、记事、报 CPU/网速、当闹钟。

[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D4)](清单.md#1-运行环境)
[![Language](https://img.shields.io/badge/language-C-555)](#编译)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

## 特性

- **真·桌宠**：分层窗口逐像素透明，不占任务栏、不进 Alt-Tab
- **三种推理后端**：qwen CLI / 本地管道（任意 OpenAI 兼容端点）/ 多智能体
- **流式聊天**：token 逐块打进卷轴气泡，气泡随字数长大
- **共享记忆**：三只桌宠 + 终端里的白读同一份 `memory/about_bro.txt`
- **多只同屏**：`--slot/--name/--model/--skin`，一键起三只
- **散步 / 随机动作 / 节日台词 / 工具面板 / 闹钟 / Q 版 / 皮肤替换**
- **多智能体**：主管拆解 → 员工A 执行 → 三级验收 → 汇总；支持主管与员工分端点
- **绿色免安装**：路径全部相对 exe，整个目录拷走就能跑

完整功能、环境要求、踩过的坑见 **[清单.md](清单.md)**。

## 快速开始

1. 下载 Release 里的 `ShiroPet-v21-portable.zip`，解压到任意目录（路径可含中文）。
2. 双击 `启动白.cmd`（或 `pet.exe`）。
3. 首次启动会从屏幕下方升起，说 `Hi!`。

没有配置模型 key 也能跑：本地管道会回落 mock，一句固定台词，链路不断。

### 配置模型

右键 →「推理后端」：

| 后端 | 说明 |
|---|---|
| qwen CLI | 无头 `qwen-code`，需要本机安装并登录 |
| 本地管道（推荐） | 任意 OpenAI 兼容端点：DashScope / DeepSeek / 智谱 / 本机 Ollama |
| 多智能体 | `ma.exe`：主管拆解、员工A 执行、三级验收 |

密钥优先级：

```
PET_LLM_KEY > PET_LLM_KEYFILE（文件第一行）> %USERPROFILE%\.qwen\settings.json > 同名环境变量
```

换服务商时用 `--base` 和 `--keyfile`：

```bat
pet.exe --backend local --base https://api.deepseek.com/v1 --keyfile D:\keys\deepseek.txt
```

`--model` 只影响这只白的模型名，写进 `PET_LLM_MODEL`，子进程继承。

### 本机 Ollama

```bat
pet.exe --backend local --base http://127.0.0.1:11434/v1 --model qwen2.5:7b-instruct
```

Ollama 不校验 key，但后端要求 key 非空（否则回落 mock），随便给一个占位即可：

```bat
set PET_LLM_KEY=local
```

### 多智能体双端点

主管（拆解/判卷/汇总）和员工A（产出文件）可以挂不同服务商：

```bat
set PET_LLM_BASE=http://127.0.0.1:11434/v1
set PET_LLM_MODEL=qwen2.5:7b-instruct
set PET_LLM_KEY=local

set MA_MANAGER_BASE=https://dashscope.aliyuncs.com/compatible-mode/v1
set MA_MANAGER_MODEL=qwen3.8-max
set MA_MANAGER_KEY=sk-...
```

不配 `MA_MANAGER_*` 时主管沿用员工A，行为和旧版一致。

## 编译

需要 MinGW-w64（gcc / windres）。仓库里直接跑 `build.bat`（会自动进入 `src/`）；手工编译等价于：

```bat
cd src
windres -i pet.rc -O coff -o pet_res.res
gcc -O2 -mwindows -o pet.exe pet.c pet_res.res -lgdiplus -liphlpapi -lwinmm
gcc -O2 -o inference_backend.exe inference_backend.c -lwinhttp
gcc -O2 -o ma.exe ma.c -lwinhttp
```

> **必须带 `pet_res.res`**，否则图标和版本信息会丢。  
> 目录含中文时 gcc 8.1 可能找不到 `cc1.exe`，解决：在 ASCII 路径建目录联接再编译，见 [清单.md](清单.md#编译相关的坑)。

## 使用

| 动作 | 反应 |
|---|---|
| 点头部 | 眨眼 + 台词 + 输入框 |
| 点身体 | 鞠躬 → 跪坐；再点起身 |
| 拖动 | 「被拎起」姿势，跟手 1:1 |
| 双击 | 简易模式（静默） |
| 右键 | 菜单 |

键盘：`Ctrl+E` 切 Q 版，`←/→` 散步转向，`Enter` 发送，`Esc` 关输入框。

对任何一只白说「记住：xxx」会写入共享记忆，不走模型。

## 项目结构

```
.
├─ src/
│  ├─ pet.c                  桌宠本体（单文件）
│  ├─ bubble.h               气泡尺寸引擎
│  ├─ inference_backend.c    本地管道推理子进程
│  ├─ ma.c                   多智能体引擎
│  ├─ agent_config.h         角色表 + 调度闸
│  ├─ compat.h               路径/端点/DPI 运行时适配
│  ├─ pet_main.c             聊天管道示例
│  ├─ bubble_test.c          气泡引擎测试台
│  ├─ pet.rc / pet.ico       图标与版本信息
│  └─ launch_three.py        一键三只
├─ docs/
│  ├─ 功能介绍.md            完整功能与用法手册
│  └─ 多智能体调度规格_v2.md  阶段2 施工依据
├─ assets/README.md          素材说明（仓库不含美术资源）
├─ memory/about_bro.example.txt
├─ build.bat
├─ 清单.md
└─ LICENSE
```

运行时只需要 `pet.exe + inference_backend.exe + ma.exe + assets\`；日志和 `blackboard\` 自动生成。

## 已知限制

- 员工 B/C 并行调度未实现（菜单灰显，调度闸拦截）
- 鞠躬动画用跪坐姿态顶替
- 闹钟音乐、铃铛/Q 版真素材待补
- 多智能体判卷依赖模型能力，小模型容易误判
- 发布包与 `pet.rc` 版本号需随版本更新

## 素材与版权

- 代码以 MIT 发布。
- `assets/` 下的角色美术为 AI 生成，**不随本仓库分发**；请使用你自己的素材，或按 `assets/README.md` 的清单放入。
- 「白」角色版权归《游戏人生》原作者/权利方，本项目仅作个人学习与桌宠实现，与官方无关。

## License

[MIT](LICENSE)
