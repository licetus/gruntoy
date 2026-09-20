# 05 · CHANGELOG —— v1.0 相对 PoC 的差异

> 本文记录四类变更，编号可被 `01`~`04` 文档交叉引用：
>
> | 前缀 | 含义 |
> |---|---|
> | `GAP-nn` | PoC 缺失、v1.0 补齐的需求项 |
> | `FIX-nn` | v1.0 建设过程中发现并修复的缺陷（含 PoC 遗留与 v1.0 自身引入） |
> | `NEW-nn` | v1.0 新增的工程能力（PoC 无对应物） |
> | `OP-nn` | 待定项与显式设计选择（**不是缺陷**，是有意识的取舍） |

---

## 0. 总结

| 维度 | PoC | v1.0 |
|---|---|---|
| 源文件 | 6 个（`params` / `mood` / `haptic` / `cli` + 头文件 + `.ino`） | **12 个**（新增 `tail.*`、`config.h` 独立、`test/` 完整） |
| 参数项数 | 部分（P-32 未落地，缺 P-16/P-17/P-24/P-30/P-31） | **75 项，覆盖 P-01~P-58 全部展开** |
| 校验规则 | `C-01`~`C-18`（部分未生效） | **21 条**（`C-01`~`C-18` + `X-01`~`X-03`） |
| 驱动频率落地 | ❌ 只读入变量，从未影响输出 | ✅ 门控调制，有断言守住 |
| 测试可运行性 | ❌ `run_tests.sh` 引用未上传的桩文件 | ✅ 桩齐备，一行命令跑完 |
| 测试套件 | 2 个（`test_main` / `test_mood`） | **6 个**（+`test_haptic` / `test_cli` / `build_debug` / `build_release`） |
| 断言数 | 因桩缺失无法执行 | **264 条，全绿** |
| 真实入口验证 | 无 | ✅ `build_check` 直接调 `setup()` / `loop()` |
| 量产配置验证 | 无 | ✅ 关闭调试通道后编译、链接、运行 |
| 编译告警 | — | **0**（`-Wall -Wextra`） |

---

## 1. `GAP` —— 补齐的 PoC 功能缺口

### `GAP-01` 驱动频率 `P-32` 真正落到输出（**最实质的一处补齐**）

**PoC 状况**：`P-32 purr_drive_freq_hz` 被读入一个成员变量，但从未影响实际 PWM 输出。

**为什么这是严重问题**：M1 Day5 的核心实验是「扫频 25→50Hz 找猫感区间」。
若驱动频率不影响输出，操作者在 6 个频点上会听到**完全相同**的震动，
进而得出「频率不影响手感」这一错误结论，并可能据此放弃整个咕噜频率调优方向。

**v1.0 做法**：实现**门控调制**（gating）——在每个 `1/f` 周期内，按目标平均占空比
切成「导通满量程 / 完全断开」两段。平均功率与占空比一致，同时在 `f` 上产生震颤。

**验证**：`test_haptic.cpp` §2 两条互补断言——
① 输出样本只含 0 与满量程两种值；② 同样时长内 50Hz 的通断切换次数显著多于 20Hz。

**边界处理**：`duty ≤ 0` → 直接输出 0；`freq == 0` → 退化为纯 PWM；`duty ≥ 100` → 全程导通。

---

### `GAP-02` 呼吸自动触发链路（FR-35 / PRD §9.6）

**PoC 状况**：呼吸只能通过 CLI 手动 `t breath` 起，缺少从抚摸事件自动触发的链路。

**v1.0 做法**：`MoodEngine::maybeTriggerBreath(now)`，四项条件全部满足才启动：
① 状态为 `ACCUM`；② 震动通道空闲；③ 距上次呼吸 ≥ `P-43`；④ 档位由心情值决定。

**顺序细节**：该调用放在咕噜判定**之后**。若提前，本次抚摸直接触发咕噜时会出现
「先起一次呼吸、随即被咕噜抢占」的无效启停（听觉上是一次多余抖动）。

**验证**：`test_mood.cpp` §10（链路 + `P-43` 节流）、§11（与咕噜互斥，咕噜优先）。

---

### `GAP-03` `P-24` 线性映射模式

**PoC 状况**：只有锚点分段一种映射，无法用单条公式快速试参。

**v1.0 做法**：`purr_duration_mode` 枚举（`tiered` / `linear`）。
线性模式：`duration = 截距 + 斜率 × (mood − purr_tier_b0)`，结果截断到 `P-29`。
新增 `X-02` 校验保证斜率 > 0 且区间内单调不减。

**验证**：`test_mood.cpp` §7 断言两种模式均可区分。

---

### `GAP-04` `P-16` 降落曲线选择 + `P-17` 段数实际使用

**PoC 状况**：`P-16` / `P-17` 存在但未真正参与降落计算。

**v1.0 做法**：`landingValue()` 走 `piecewiseCurve(t, N, curve)`，
支持 `linear` / `ease_in_out` / `ease_out` 三条曲线，`N` 取 `P-17`（默认 5 段）。

**验证**：`test_mood.cpp` §14 断言三条曲线在同一起点下取值可区分，
且 `P-17` 段数变化会影响中间取值。

---

### `GAP-05` `P-30` 轮次间隔 + `P-31` 累计上限（两道闸门）

**PoC 状况**：咕噜达到阈值即无限触发，无「休息」约束。

**v1.0 做法**：`purrGateState()` 返回四种状态：

| 状态 | 触发条件 |
|---|---|
| `PG_OPEN` | 可触发 |
| `PG_GAP` | 距上一轮咕噜结束不足 `P-30`（默认 15s） |
| `PG_CUMULATIVE` | 累计运行达 `P-31`（默认 300s），须休息 `P-30` 以上才解锁 |
| `PG_SYSTEM` | 处于 `LOWPOWER` / `UPGRADING` |

被拦下时不进入 `PURR`，但**仍正常更新摇尾档位**（避免档位卡住）。
`status` 命令显示当前闸门状态，便于现场观测。

**验证**：`test_mood.cpp` §12（`P-30`）、§13（`P-31`）。

---

### `GAP-06` PURR 态锁定摇尾 W4（FR-39 验收条件）

**PoC 状况**：咕噜期间摇尾档位仍按心情值 + 迟滞判定，会出现「咕噜期间摇尾降档」。

**v1.0 做法**：进入 `PURR` 时直接锁定 `m_tailGear = 4` 并 `setGear(4)`，
不依赖迟滞判定。

**验证**：`test_mood.cpp` §2。

---

### `GAP-07` 摇尾独立通道模块（新增 `tail.h` / `tail.cpp`）

**PoC 状况**：摇尾档位只在 `mood.cpp` 里算了个数字，没有独立通道，
也没有频率/幅度插值、抖动、停摆逻辑。

**v1.0 做法**：`TailDriver` 类，职责边界明确——**不做机构选型**。

| 能力 | 实现 |
|---|---|
| 档位 → 频率/幅度 | `targetFreqOf(gear)` / `targetAmpOf(gear)` |
| 平滑过渡 | 频率与幅度**同时**按 ease-in-out 插值：`f = t²(3−2t)` |
| 切换时长 | 常规 `P-51`，咕噜后缓降 `P-52` |
| 去机械感 | 频率带 ±`P-49` 随机抖动，每 500ms 刷新 |
| 自动停摆 | 无互动超 `P-53` 后停摆 |
| 物理输出 | `duty = 50 + sin(phase) × ampNorm × 25`；`ENABLE_TAIL_OUTPUT=0` 时只算不输出 |

**验证**：`test_mood.cpp` §6（档位切换与迟滞）、§15（系统态优先级）；
`build_check` §2 断言档位始终落在 W1~W4 合法区间。

---

## 2. `FIX` —— 修复的缺陷

### `FIX-01` 试写校验先钳制再校验，导致越界值被静默接受

**严重度**：高（会让 M1 调参得出错误结论）

**症状**：`SET purr_drive_freq_hz 9900`（99Hz，超出 20~60Hz 工作区间）
和 `t gate 99` 看起来设置成功，实际值被悄悄夹到 60Hz（`C-09` 的上界），
规则号从未报出。

**根因**：`Params::validateSet()` 先把候选值钳制到 `[min, max]`，再跑 `validate()`。
钳制后自然满足值域类规则，因此 `C-09` 永远不可能被触发。

**修复**（`params.cpp`）：**以原值试算，不预先钳制**。
若没有任何规则命中而值本身越界，返回新错误码 `E-LIMIT`。

```cpp
int32_t     old = s_v[id];
s_v[id] = v;                    // ★ 原值，不钳制
const char* err = validate();
s_v[id] = old;
if (err) return err;
if (v < META[id].min || v > META[id].max) return "E-LIMIT";
return nullptr;
```

**为什么必须这样改**：操作者用 `t gate 99` 找猫感区间时，若实际听到的是 60Hz，
他会把 60Hz 的听感记为「99Hz」—— 整个扫频记录不可信。

**注意区分**：`Params::set()`（内部写入，用于加载 NVS 与预设）**仍然钳制**；
`validateSet()`（外部试写）**不钳制**。两者语义不同，不可互换。

**验证**：`test_cli.cpp` §3 与 §6；`test_main.cpp` §2~§6。

---

### `FIX-02` CLI 把枚举解析器当通用值解析器，所有数值型参数都写不进去

**严重度**：高（CLI 是 M1 现场唯一的调参入口）

**症状**：`SET mood_restore_value 70`、`SET purr_trigger_threshold 105` 等
**全部**数值型参数写入被报「取值非法」，只有枚举型（3 项）能写。

**根因**：`cmdSet()` 调用 `Params::parseChoice(id, p, &raw)` 作为通用解析入口，
而 `parseChoice()` 的第 2 行就是 `if (!META[id].choices) return false;`——
它对标量参数（`choices == nullptr`）一律返回 `false`。

**修复**（`cli.cpp`）：数字优先，只有纯文本才交给 `parseChoice`。

```cpp
bool numeric = true;
for (const char* q = p; *q; q++) {
  if (isdigit((unsigned char)*q)) continue;
  if (*q == '-' && q == p) continue;
  numeric = false; break;
}
if (numeric)                               { raw = atol(p); parsed = true; }
else if (Params::parseChoice(id, p, &raw)) { parsed = true; }
```

**验证**：`test_cli.cpp` §3（数值写入与拦截）、§4（枚举按名写入与非法名拒绝）。

---

### `FIX-03` `t feed` / `t auto` 用 `delay()` 阻塞主循环

**严重度**：中（使一项关键观察无法进行）

**症状**：PoC 的 `t feed` 每次抚摸后 `delay(1200)`。馈入期间状态机完全停摆，
咕噜时长、降落曲线、冷却计时都不推进。操作者想「观察降落是否平滑」，实际什么也看不到。

**修复**（`cli.cpp` / `cli.h`）：改为基于 `millis()` 的非阻塞调度
（`m_autoRun` / `m_autoNextMs` / `m_autoRemaining` / `m_autoIntervalMs`），
在 `poll()` 中推进。抚摸间隔自动取 `max(pet_debounce_ms, pet_valid_min_ms)`，
避免「看起来摸了 N 次、实际只有 N/3 被计入」的误导。

**验证**：`test_cli.cpp` §7 用 `advance(100)` + 四路 tick 模拟真实 loop，
断言**馈入期间状态机仍能推进**。这条断言在 `delay()` 实现下必然失败——
它把「非阻塞」从代码规范变成了可执行的约束。

---

### `FIX-04` 首次抚摸被防抖误拦

**严重度**：中（影响开箱第一印象）

**症状**：`init()` 时 `m_lastStrokeMs = 0`，若用户开机后 `P-06`（默认 500ms）内
就摸一下，会被防抖逻辑过滤。

**为什么是产品问题而非测试问题**：用户得到的第一印象是「这玩具没反应」，
且**不会摸第二下**。

**修复**（`mood.cpp` / `mood.h`）：新增 `m_everStroked` 成员，
**首次抚摸跳过防抖判定**；`resetState()` 中复位。

**验证**：`test_mood.cpp` §9。

---

### `FIX-05` 呼吸链路与咕噜判定顺序导致无效启停

**严重度**：低（影响听感细节）

**症状**：呼吸触发检查若排在咕噜判定之前，本次抚摸触发咕噜时，
会先起一次呼吸再被咕噜抢占，产生一次多余的启停抖动。

**修复**（`mood.cpp`）：`maybeTriggerBreath()` 移到咕噜判定之后。

**验证**：`test_mood.cpp` §11。

---

### `FIX-06` `cli.cpp` 引用不存在的宏

**症状**：`PARAMS_NVS_NVS_NS_UNUSED_` 未定义 → 编译失败。

**修复**：改为 `PARAMS_NVS_NS`（定义于 `config.h`）。

---

### `FIX-07` `tail.cpp` 残留无效标签

**症状**：`if (m_active) { disable_again:; stopNow(); }` 中 `disable_again:` 是编辑残留。

**修复**：简化为 `if (m_active) stopNow();`。

---

### `FIX-08` `X-02` 校验式书写晦涩且易误读

**症状**：初版写作 `s_v[P_MOOD_MAX] - s_v[P_PURR_LINEAR_SLOPE] * 0 - s_v[P_PURR_TRIGGER_THRESHOLD]`。

**修复**：拆成有语义的局部变量。

```cpp
const int32_t lo = s_v[P_PURR_LINEAR_INTERCEPT_MS];
const int32_t hi = lo + s_v[P_PURR_LINEAR_SLOPE] * (s_v[P_MOOD_MAX] - s_v[P_PURR_TRIGGER_THRESHOLD]);
if (hi <= lo) return "X-02";
```

---

### `FIX-09` 调试通道声称「编译期整体移除」，实际只隐藏了串口读取

**严重度**：中（违反 FR-38c / RK-23）

**症状**：`config.h` 注释称「置 0 后整个 CLI 被条件编译移除」，
但 `cli.cpp` 中只有 `Serial.available()` 的读取循环被 `#if` 包裹，
其余约 570 行命令实现（含 `help` / `SET` / `DUMP` / `PRESET` 全部逻辑）**始终参与编译**。

**为什么严重**：FR-38c / RK-23 要求量产不留运行期后门。
「代码里还在、只是没人调用」不满足该要求——反编译仍可拿到完整调参入口。

**修复**（`cli.cpp`）：以 `#if !ENABLE_DEBUG_CHANNEL` 包裹整个实现体，
关闭时仅保留空的 `init()` / `poll()`（满足 `gruntoy.ino` 的调用点链接）。

**实测证据**：量产配置产物比调试配置小 **17.5 KB（约 15%）**。

**验证**：`build_check` 在 `ENABLE_DEBUG_CHANNEL=0` 下断言
「CLI 未初始化 + 分组统计不存在 + 明确打印已移除提示」。

---

### `FIX-10` `config.h` 的编译期开关无法从外部覆盖

**症状**：`ENABLE_DEBUG_CHANNEL` / `ENABLE_TAIL_OUTPUT` 为裸 `#define`，
命令行 `-DENABLE_DEBUG_CHANNEL=0` 会触发宏重定义告警且行为不确定，
导致**量产配置无法被自动验证**。

**修复**：两个开关改为 `#ifndef ... #define ... #endif` 形式，
既接受文件内默认值，也接受命令行覆盖。

**为什么重要**：量产配置是最终提交审核的那一份，
不能等到交付前才第一次被编译。修复后 `build_release` 套件每次跑测试都会编译并运行它。

---

### `FIX-11` 测试桩不完整，`run_tests.sh` 实际不可运行

**症状**：PoC 的 `run_tests.sh` 引用 `test/arduino_stub.cpp`、`test/Arduino.h`、
`test/Preferences.h`，但这三个文件**并未随源码提供**。`bash test/run_tests.sh`
在 PoC 交付物上直接失败。

**修复**：从零补齐这套桩：
`test/Arduino.h`（假时钟 + 串口桩 + LEDC 写入记录 + GPIO/LEDC 声明，兼容 Core 2.x/3.x）、
`test/Preferences.h`（以 `ns/key` 索引的 NVS 内存桩，支持 `wipe()` 模拟出厂空 NVS）、
`test/arduino_stub.cpp`（桩实现，`delay()` 也推进假时钟）、
`test/check.h`（`CHECK` 断言 + `SECTION` 分组 + `runFor` 步进推进）。

---

### `FIX-12` `test/Preferences.h` 初版设计混乱（v1.0 自身引入）

**症状**：残留 `store()` / `m_keyOfLastCall` 与未使用的 `slot()` / `slotR()` 混用。

**修复**：整体重写为单一 `keyOf()` / `slot()` / `slotRef()` 方案。

---

### `FIX-13` 测试代码中的无效断言与语法错误（v1.0 自身引入）

| 问题 | 修复 |
|---|---|
| `test_mood.cpp` 用 `vInOut == from + (restore-from)*(4/5)/1 \|\| ...` 断言，整数除法 + 恒真或式，无意义 | 改为 `CHECK(vOut < from, ...)` / `CHECK(vInOut < from, ...)` |
| `test_haptic.cpp` 呼吸自动结束断言在推进前未重新启动 | 改为 `h.stop(); h.startBreath(2);` 并分两段推进 |
| `test_haptic.cpp` `collectLedc` 用运行前保存的 `g_fake_ms - 400` 作窗口起点 | 改为使用运行结束后的 `g_fake_ms` |
| `test_mood.cpp` §10 第二次节流断言缺 `haptic.stop()`，首个呼吸仍在进行会提前 return | 推进前补 `haptic.stop()` |
| `test_main.cpp` 用 `Params::getName(...) == std::string(...)` 比较（指针 vs 对象） | 改为 `!strcmp(Params::getName(...), "...")` |
| `test_main.cpp` 缺 `#include "config.h"`（用到 `PARAMS_NVS_NS`） | 补上 |
| 多处 C++ 字符串字面量内嵌中文引号，导致语法错误 | 统一改为中文直角引号 `「」` |

---

### `FIX-14` 测试运行脚本的断言计数产生重复行（v1.0 自身引入）

**症状**：`grep -c 'PASS'` 在无匹配时既输出 `0` 又返回退出码 1，
`grep -c ... || echo 0` 因此打印两行 `0`，汇总表错位。

**修复**：改为 `grep -c ... | tr -d '\n'` 配合 `${var:-0}` 兜底。

---

### `FIX-15` 测试中的时序假设与实现语义不符（v1.0 自身引入）

| 套件 | 症状 | 修复 |
|---|---|---|
| `test_mood.cpp` §12 | 断言「休息足够后闸门放行」，但只推进了 10000ms，而前文已推进 4000ms，合计 14000 < `P-30`（15000ms） | 改为推进 `Params::get(P_PURR_BETWEEN_GAP_MS)` |
| `test_haptic.cpp` §2 | 断言 `getDriveFreqHz() == 30`，但整段 20000ms 跑完后咕噜已结束，该接口返回 0 | 改为在咕噜**仍在进行时**（18500ms 处）断言 |
| `test_cli.cpp` §3/§4 | 值解析器初值误写为 `parsed = (*p != '\0')`，导致非法枚举名被放行、数值参数被误拒 | 改为 `parsed = false` 起 |

---

## 3. `NEW` —— v1.0 新增的工程能力

### `NEW-01` 固件整体启动冒烟验证（`test/build_check.cpp`）

**PoC 无对应物**：原有测试都是直接 `new` 出引擎对象，从未走过
`gruntoy.ino` 的 `setup()` → `loop()` 这条**真实入口链路**。

**v1.0 做法**：新增 `build_check.cpp`，直接声明并调用 `setup()` / `loop()`，
推进真实假时钟跑完一整轮生命周期（咕噜 → 降落 → 冷却），
并断言三段：初始化、状态机完整闭环、调试通道隔离。

**以两种编译配置各跑一遍**（见 `FIX-10`）：

```bash
-DENABLE_DEBUG_CHANNEL=1 -DENABLE_TAIL_OUTPUT=0   # debug
-DENABLE_DEBUG_CHANNEL=0 -DENABLE_TAIL_OUTPUT=1   # release
```

---

### `NEW-02` CLI 纳入本机测试（PoC 显式排除）

**PoC 状况**：`run_tests.sh` 注释写明「cli.cpp 依赖 Serial 输入流，不纳入本机测试」。

**v1.0 做法**：串口桩支持输入注入（`Serial.feed()`），因此 CLI 的解析、校验拦截、
枚举取值、非阻塞抚摸模拟全部可测。新增 `test_cli.cpp`，63 条断言。

**为什么值得做**：CLI 是 M1 现场唯一的调参入口，只靠「上手试」保证其可用性
风险过高——`FIX-01` / `FIX-02` 两个高危缺陷正是这套测试抓出来的。

---

### `NEW-03` 独立 `config.h`

**PoC 状况**：硬件常量分散在 `.ino` 与各 `.cpp` 中。

**v1.0 做法**：`config.h` 是**唯一允许出现硬件相关常量**的文件
（引脚、PWM 载波、编译期开关、NVS schema 版本）。
约定：行为参数一律走 `params.cpp` 的 META 表（FR-38 落地要求）。

---

### `NEW-04` NVS schema 版本守卫

**PoC 状况**：无版本号，参数表结构变化后旧 blob 会被按新布局误读。

**v1.0 做法**：`PARAMS_NVS_SCHEMA`（当前 `2`）与 NVS 中记录值比对，
不匹配则**忽略整份旧 blob**并回落默认值。

```cpp
prefs.putUInt("ver", PARAMS_NVS_SCHEMA);
prefs.putBytes("cfg", s_v, sizeof(s_v));
// load()：版本不符 → return false，不使用旧 blob
```

**约定**：未来任何参数增删都必须 `PARAMS_NVS_SCHEMA + 1`。

---

### `NEW-05` 新增 CLI 命令

| 命令 | 作用 |
|---|---|
| `info` | 固件版本、参数项数、NVS schema、编译期开关状态、配置来源层级 |
| `status` / `st` | 状态快照（含 `GAP-05` 的闸门状态与咕噜/呼吸计数） |
| `t gate <freqHz>` | 只改门控频率，最快路径找猫感区间（`GAP-01` 的现场工具） |
| `t state <名>` | 强制状态，用于验证系统态优先级与降级路径 |
| `t tail <1-4>` | 直接设摇尾档位（无机构时验证逻辑） |
| `t cycle` | 从当前心情值跑一次完整生命周期 |
| `RESET <group>.*` / `RESET ALL` | 按组或全量恢复默认 |

---

### `NEW-06` 测试运行脚本增强

- 汇总明细表（每个套件的断言通过/失败数）
- 编译失败与断言失败分别计数
- `--verbose` 展示编译告警
- 零告警基线：`-Wall -Wextra` 下 0 条告警

---

## 4. `OP` —— 待定项与显式设计选择

> 以下**不是缺陷**，是有意识的取舍或已排期的后续工作。列出以免被误读为遗漏。

### `OP-01` `P-05 pet_valid_min_ms` 的真实语义待接入触摸通道后启用

**现状**：v1.0 以「事件」模拟抚摸（没有真实触摸持续时长），因此
`onStroke()` 取 `max(P-06 pet_debounce_ms, P-05 pet_valid_min_ms)` 作为
两次有效抚摸的最小间隔。

**目标语义**：接入真实触摸通道后，`P-05` 应改由触摸驱动的
「按下持续时长 ≥ `pet_valid_min_ms`」判定——即「轻碰一下不算，得摸够 3 秒」。

**为何现在不实现**：v1.0 未接入触摸采样（见 `04` 已知限制 1）。
先把接口语义定下来在文档里，避免后来者误以为 `P-05` 是死参数。

**影响面**：`mood.cpp` 的 `onStroke()` 开头一段；`resonance` 无。

---

### `OP-02` `purr_tier_d3` 在 `b3`~`b4` 段保持平台（而非继续外推）

**选择**：`b3`（115）~ `b4`（120）段刻意保持 `D3`（默认 60s），形成平台。

**备选方案**：继续线性外推到 `b4`。但这需要引入第五个锚点时长参数
（或让 `D3` 同时承担「b3 处取值」与「b4 处取值」两个语义）。

**为何选平台**：
1. 不增加参数数量（`D4` 会让参数表多一项、映射说明复杂一倍）；
2. 平台段本身就是合理设计——心情值从 115 到 120 已是「极度兴奋」，
   再拉长咕噜时长收益递减，反而容易让用户觉得玩具「卡住了」；
3. 端点语义清晰：`D0` 对应阈值处，`D3` 对应最高处。

**若后续确实需要外推**：新增 `purr_tier_d4`，`PARAMS_NVS_SCHEMA + 1`，
并补 `test_mood.cpp` §7 的单调性断言。

---

### `OP-03` `LOWPOWER` / `UPGRADING` 两个安全态仅实现状态与优先级

**选择**：状态、优先级（最高，可打断任意态）、`purrGateState() == PG_SYSTEM`
全部实现且可经 `t state lowpower` / `t state upgrading` 验证；
但**触发源**（电池 ADC / OTA）未接入。

**为何这样切**：接入 ADC 需要确定分压电阻与阈值（硬件相关，属另一份清单）；
接入 OTA 需要 WiFi 与签名方案（安全相关，工作量大）。
先把状态机的「降级路径」建好并测通，硬件就绪后接上触发源即可。

**验证**：`test_mood.cpp` §15（系统态优先级）。

---

### `OP-04` 云端参数层只留接口，不启用

**选择**：三层架构「云端 > 本地 NVS > 硬编码」中，云端层保留接口，
`config_cloud_enabled` 默认 `0`。

**已落实的关键约束**：**云端拉取失败时必须保持本地值，禁止回落硬编码默认值。**
这是三层架构里唯一容易写错的地方——常见错误是「拉取失败 → 直接 return」
但参数已被重置为默认值。正确语义是失败什么都不做。

**为何暂不实现**：需要确定下发协议、鉴权方式、失败重试策略，
且 M1 阶段的核心是本地调参，云端下发的收益要等到量产铺开后才能体现。

---

### `OP-05` 摇尾物理输出默认关闭

**选择**：`ENABLE_TAIL_OUTPUT` 默认 `0`，只计算不输出。

**理由**：摇尾机构选型、连杆设计、限位标定属
《M1-PoC执行清单-摇尾机构与选型》的范畴。
本基线把逻辑层做完整（含输出通路与测试），但不替机构做决定。

**接机构后**：置 `1`，并用 `C-12`（频率上限）/ `C-13`（幅度上限）
按机构实际能力收紧参数。

---

### `OP-06` 假时钟机制要求「禁止 `delay()` 阻塞」这一全局约定

**不是缺陷，是架构约束**：因为 `runFor()` 靠假时钟步进推进，
任何 `delay()` 都会让状态机在测试中无法被推进。

**收益**：把「非阻塞」从口头规范变成可执行的约束——
`test_cli.cpp` §7 的「馈入期间状态机仍能推进」断言在 `delay()` 实现下必然失败。

**代价**：所有涉及等待的逻辑必须写成状态机形式（如 `t feed` 的调度），
初次阅读代码时不如 `delay()` 直观。

---

## 5. 验证结果快照

```
$ bash test/run_tests.sh

 套件         断言通过 断言失败
  test_main            55        0
  test_mood            75        0
  test_haptic          39        0
  test_cli             63        0
  build_debug          16        0
  build_release        16        0

 汇总: 套件 6 通过 / 0 失败
 全部通过。可以进入烧录阶段。
```

- 编译器：`g++ -std=c++17 -Wall -Wextra`，**0 条告警**
- 量产配置产物：95,648 字节；调试配置：113,184 字节（差 17.5 KB，验证 `FIX-09`）

---

## 6. 后续版本建议

| 优先级 | 事项 | 依赖 |
|---|---|---|
| P0 | 接入触摸采样，桥接到 `onStroke()`，启用 `OP-01` 的真实语义 | 触摸电极装配与阈值标定 |
| P0 | L3 真机体感验证（呼吸嗡鸣 / 咕噜猫感 / 摇尾过渡） | 烧录 + 人工试听 |
| P1 | 摇尾机构选型与 `ENABLE_TAIL_OUTPUT=1` 联调 | 机构清单 |
| P1 | 电池 ADC 接入，启用 `LOWPOWER`（`OP-03`） | 硬件分压电路 |
| P2 | 云端参数下发通道（`OP-04`） | 协议与鉴权方案 |
| P2 | NFC 周边联动 | 产品方向确认 |
| P3 | 若实测需要，新增长咕噜时长的外推段（`OP-02`） | M1 主观评分数据 |
