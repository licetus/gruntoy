# gruntoy v1.0 —— 毛绒电子宠物 · 体感子系统基线固件

> 一句话定位：把 PRD v1.1 的「呼吸 / 咕噜 / 心情值 / 摇尾分级 / 平滑过渡」五项机制，
> 落成一套**可独立运行、可本机验证、可烧录调参**的 ESP32 基线固件。
> 硬件刻意只保留「一颗振动马达 + 一块 ESP32」——设计约束是用控制逻辑的丰富度替代硬件堆叠。

---

## 1. 快速验证（不需要硬件、不需要 Arduino 环境）

```bash
bash test/run_tests.sh            # 6 个套件，264 条断言
bash test/run_tests.sh --verbose  # 同时展示编译告警
```

预期输出结尾：

```
 套件         断言通过 断言失败
  test_main            55        0
  test_mood            75        0
  test_haptic          39        0
  test_cli             63        0
  build_debug          16        0
  build_release        16        0
 汇总: 套件 6 通过 / 0 失败
```

**它验证什么**：参数表与 18+3 条校验规则、状态机时序、呼吸包络数学、门控调制是否真的落到
PWM 输出、CLI 解析与拦截、`setup()/loop()` 真实入口链路，以及量产配置能否编译运行。

**它不验证什么**：马达惯性、机构手感、声音。呼吸有没有嗡鸣、咕噜像不像猫，只能烧录后听。
本机测试只回答「逻辑对不对」，不回答「手感好不好」。

---

## 2. 目录结构

```
gruntoy/
├── gruntoy.ino         入口：setup() / loop()，唯一持有 Preferences 句柄
├── config.h            硬件引脚、PWM 参数、编译期开关、NVS schema 版本
│                       （唯一允许出现硬件常量的文件）
├── params.h/.cpp       参数系统：75 项参数 + 校验规则 + NVS 持久化 + 三套预设
├── haptic.h/.cpp       体感引擎：呼吸 / 咕噜（含门控调制）/ 裸测 / 扫频 / 提示震
├── mood.h/.cpp         心情值引擎：6 态状态机、寿命周期、闸门、呼吸触发链路
├── tail.h/.cpp         摇尾通道：档位 → 频率/幅度 → 平滑过渡 → 输出
├── cli.h/.cpp          串口调试台：调参唯一入口，编译期可整体移除
├── docs/               ★ 交付文档（先读 01，再读 02）
│   ├── 01-设计意图与核心目标.md
│   ├── 02-关键结构与模块边界.md
│   ├── 03-参数体系与校验规则.md
│   ├── 04-验证与交接说明.md
│   └── 05-CHANGELOG-v1.0.md
├── memorys.md          ★ 项目进度与决策记录（新接手者先读这份）
└── test/               本机验证：Arduino 桩 + 断言框架 + 6 个套件 + run_tests.sh
```

---

## 3. 硬件接线

| 器件 | 接线 | 说明 |
|---|---|---|
| 振动马达 | `+` → DRV8833 `OUT1`（或 MOSFET 漏极）<br>`-` → DRV8833 `OUT2`（或 GND） | **切勿 GPIO 直驱** |
| 驱动输入 | 马达驱动模块 `IN` → `GPIO25` | ESP32 单脚上限 40mA，马达启动浪涌 >90mA |
| 触摸电极 ×3 | 铜箔 → `GPIO4` / `GPIO15` / `GPIO27` | 贴内衬塑料壳外壁，毛绒仅做装饰覆盖 |

> `GPIO25` 被占用时改用 `GPIO26`（`config.h` 的 `MOTOR_PWM_PIN_ALT`）。

**为什么必须经驱动模块**：ESP32 单引脚持续输出上限约 40mA，而微型振动马达堵转/启动浪涌
普遍在 90mA 以上。直连的后果不是「转得弱」，而是引脚永久损坏。推荐 `AO3400` / `IRLML6344`
栅极串 100Ω、栅源间加 10kΩ 下拉。

---

## 4. 编译与烧录

### 4.1 Arduino IDE

1. 开发板：`ESP32 Dev Module`（Arduino-ESP32 Core 2.x 或 3.x 均可，见 `config.h` 的
   `ESP_ARDUINO_VERSION_MAJOR` 分支，两套 LEDC API 都支持）。
2. 打开 `gruntoy.ino`（同目录 `.h/.cpp` 会被自动编入）。
3. 波特率 `115200`，烧录后打开串口监视器，输入 `help`。

### 4.2 平台无关的编译形态切换

`config.h` 的两个开关都用 `#ifndef` 包裹，因此**既可在文件里改，也可从命令行覆盖**：

```bash
# 调试配置（默认）：CLI 可用，摇尾只算不输出
-DENABLE_DEBUG_CHANNEL=1 -DENABLE_TAIL_OUTPUT=0

# 量产配置：CLI 实现体整体编译移除，摇尾接机构后打开物理输出
-DENABLE_DEBUG_CHANNEL=0 -DENABLE_TAIL_OUTPUT=1
```

量产配置下 CLI 命令实现（约 570 行）完全不进二进制，实测产物缩小 **17.5 KB（约 15%）**，
且启动时明确打印「调试通道已编译移除」，不会静默失效。两种形态都由
`bash test/run_tests.sh` 的 `build_debug` / `build_release` 两个套件编译并运行验证。

> 设计取舍：`ENABLE_TAIL_OUTPUT` 默认为 `0`。摇尾机构选型属《M1-PoC执行清单-摇尾机构与选型》
> 的范畴，本基线只负责「档位 → 频率/幅度 → 平滑过渡」的计算与状态输出，不越界接机构。

---

## 5. 串口命令速查

### 查询

| 命令 | 作用 |
|---|---|
| `help` / `?` | 全部命令 |
| `info` | 固件版本、参数项数、NVS schema、编译期开关状态、配置来源层级 |
| `status` / `st` | 状态快照：心情值 / 状态 / 摇尾档位 / 剩余时间 / 闸门 / 计数 |
| `GET <group>.*` | 按组列参数（`mood` / `purr` / `breath` / `tail` / `sys` / `ALL`） |
| `GET <param>` | 查单个参数：当前值、允许区间、说明、枚举可选值 |
| `DUMP` | 导出全部 75 项参数为 CSV，可直接回填 PRD 附录 C |

### 修改

| 命令 | 作用 |
|---|---|
| `SET <param> <value>` | 改参数。自动跑全部校验规则，非法即拒绝并报规则号 + 说明 |
| `SET <param> <枚举名>` | 枚举型参数支持按名写入，如 `SET mood_landing_curve ease_out` |
| `RESET <group>.*` / `RESET ALL` | 恢复默认值（需 `SAVE` 落盘） |
| `SAVE` / `LOAD` | 手动存 / 读 NVS |
| `PRESET gentle\|standard\|lively` | 套用性格预设（用户侧唯一可见的体感选项） |

### 体感测试（M1 现场调参主力）

| 命令 | 对应天数 | 作用 |
|---|---|---|
| `t breath [1\|2\|3]` | Day4 | 呼吸包络，档位 1/2/3 对应 L1/L2/L3 峰值占空比 |
| `t purr [seconds]` | Day5 | 咕噜震动，**不走状态机**，用于纯体感对比 |
| `t sweep` / `t sweepstop` | Day5 | 咕噜扫频 25→50Hz，每频点停留 10s |
| `t gate <freqHz>` | Day5 | 只改咕噜门控频率，最快路径找「猫感区间」 |
| `t bare <duty>` / `t barestop` | Day2 | 裸马达定占空比，测起转阈值与噪声 |
| `t trough <0\|20>` | Day2 | 波谷占空比 A/B 对比（0 = 完全断电） |
| `t feed [n] [intervalMs]` | Day7 | **非阻塞**模拟 n 次抚摸，走完整状态机 |
| `t auto` / `t stop` | Day7 | 无限自动抚摸循环，用于长时间稳定性 |
| `t mood <v>` | — | 直接设心情值 |
| `t state <名>` | — | 强制状态：`accum` / `purr` / `landing` / `cooldown` / `lowpower` / `upgrading` |
| `t tail <1-4>` | — | 直接设摇尾档位 |
| `t cycle` | — | 从当前心情值跑一次完整生命周期 |

> `t feed` / `t auto` 是**非阻塞**的：馈入期间主循环照常推进，状态机（咕噜时长、降落、冷却）
> 正常演进。原 PoC 用 `delay(1200)` 阻塞实现，导致喂入期间状态机完全停摆，
> 「观察降落是否平滑」这件事根本观察不到。

---

## 6. 典型调参流程

```text
# ① 确认固件与层级
info
status

# ② 找猫感区间（Day5 核心）：反复微调门控频率，每次贴近耳朵听 30s
t gate 30
t gate 35
t gate 40
SET purr_drive_freq_hz 3800     # 找到后用正式参数写入（单位 Hz×100）

# ③ 确认波谷必须是 0（否则有机械嗡鸣）
t trough 0
t trough 20                     # 对比试听；此命令绕过 C-18，仅供实验
SET breath_trough_duty_pct 20   # 想真正改：会被 C-18 拒绝，这是设计意图

# ④ 跑完整生命周期，观察是否平滑、冷却是否正确拒绝
t cycle
t feed 40
status

# ⑤ 定稿后落盘
SAVE
```

---

## 7. 文档索引

| 文档 | 内容 |
|---|---|
| [`docs/01-设计意图与核心目标.md`](docs/01-设计意图与核心目标.md) | 为什么这样设计、要达成什么、边界在哪 |
| [`docs/02-关键结构与模块边界.md`](docs/02-关键结构与模块边界.md) | 三层配置架构、6 态状态机、模块职责与数据流 |
| [`docs/03-参数体系与校验规则.md`](docs/03-参数体系与校验规则.md) | 75 项参数分组说明、18+3 条校验规则、三套预设 |
| [`docs/04-验证与交接说明.md`](docs/04-验证与交接说明.md) | 6 个套件覆盖矩阵、M1 判据对应、已知限制、下一步 |
| [`docs/05-CHANGELOG-v1.0.md`](docs/05-CHANGELOG-v1.0.md) | 相对 PoC 的差异、修复的缺陷、未决项 |
| [`memorys.md`](memorys.md) | 项目进度与决策记录：上游材料处理路径、8 条工程约定、设计决策台账、下一步待办 |

---

## 8. 版本与已知限制

**版本**：v1.0（体感子系统基线固件）
**参数表 schema**：`PARAMS_NVS_SCHEMA = 2`（与 PoC 不兼容，首次烧录建议先 `RESET ALL` + `SAVE`）

已知限制（详见 `docs/04`）：

1. **触摸判定尚未接入**：`config.h` 已定义 `TOUCH_PIN_*` 与 `TOUCH_THRESHOLD`，但 v1.0 未实现
   触摸采样到 `onStroke()` 的桥接——M1 阶段用 `t feed` 从串口注入抚摸事件替代。
2. **低电 / 升级两个安全态是占位**：`HS_LOWPOWER` / `HS_UPGRADING` 状态与优先级已实现且可经
   `t state` 强制进入验证，但触发源（电池 ADC / OTA）尚未接入。
3. **云端参数层未启用**：三层架构中「云端 > 本地」的云端层留出接口，
   `config_cloud_enabled` 默认 0。已落实的关键约束是：**拉取失败必须保持本地值，禁止回落硬编码**。
4. **摇尾只有逻辑没有机构**：`ENABLE_TAIL_OUTPUT=0`，输出通路已在 `tail.cpp` 中实现并被测试
   覆盖，但未接硬件。
5. **本机测试不覆盖真实体感**：见 §1。
