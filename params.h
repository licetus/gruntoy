/*
 * ============================================================================
 *  params.h —— 体感参数系统
 *  对应 PRD v1.1 附录 C：参数总表（P-01~P-58）+ 校验规则（C-01~C-18）
 *
 *  【核心约定】
 *   1. 行为参数全部集中在本文件的 META 表，代码里不写死任何数值（FR-38）。
 *   2. 频率/幅度/比例类参数一律用整数存储，避免浮点误差在 NVS 往返后漂移：
 *        freq 单位 = Hz × 100      （30Hz → 3000，2.0Hz → 200）
 *        amp  单位 = 归一化值 × 100（0.6 → 60）
 *        bool 单位 = 0 / 1
 *        enum 单位 = 序号（见各参数 desc 与 choices 字段）
 *   3. 三层配置架构：云端 > 本地(NVS) > 硬编码默认值。
 *      本基线实现「本地 > 硬编码」两层，云端层留出接口但默认关闭（FR-38a）。
 *      关键约束：云端拉取失败时必须保持本地值，禁止回落硬编码默认值。
 * ============================================================================
 */

#ifndef GRUNTOY_PARAMS_H
#define GRUNTOY_PARAMS_H

#include <Arduino.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// 参数编号（P-xx 为 PRD 附录 C 的编号；数组型参数已展开为独立标量）
// ---------------------------------------------------------------------------
enum ParamId : uint8_t {
  // --- 组 A：心情值累积（12 项） ---
  P_MOOD_INITIAL = 0,             // P-01
  P_MOOD_MAX,                     // P-02
  P_MOOD_DECAY_FLOOR,             // P-03
  P_PET_GAIN_PER_STROKE,          // P-04
  P_PET_VALID_MIN_MS,             // P-05
  P_PET_DEBOUNCE_MS,              // P-06
  P_PET_CONTINUOUS_WINDOW_MS,     // P-07
  P_PET_CONTINUOUS_BONUS,         // P-08
  P_PET_CONTINUOUS_INTERVAL_MS,   // P-09
  P_MOOD_DECAY_START_MS,          // P-10
  P_MOOD_DECAY_INTERVAL_MS,       // P-11
  P_MOOD_DECAY_STEP,              // P-12

  // --- 组 B：触发、降落与冷却（11 项） ---
  P_PURR_TRIGGER_THRESHOLD,       // P-13
  P_MOOD_RESTORE_VALUE,           // P-14
  P_MOOD_LANDING_MS,              // P-15
  P_MOOD_LANDING_CURVE,           // P-16  v1.0 补齐：0=linear 1=ease_in_out 2=ease_out
  P_MOOD_LANDING_SEGMENTS,        // P-17
  P_MOOD_LANDING_TOUCH_BUZZ_MS,   // P-18
  P_MOOD_LANDING_BLOCK_GAIN,      // P-19
  P_COOLDOWN_DURATION_MS,         // P-20
  P_COOLDOWN_DENY_BUZZ_MS,        // P-21
  P_PURR_FADEIN_MS,               // P-22
  P_PURR_FADEOUT_MS,              // P-23

  // --- 组 C：咕噜时长映射（15 项） ---
  P_PURR_DURATION_MODE,           // P-24  v1.0 补齐：0=tiered 1=linear
  P_PURR_TIER_B0,                 // P-25 分段点（5 点 / 4 段）
  P_PURR_TIER_B1,
  P_PURR_TIER_B2,
  P_PURR_TIER_B3,
  P_PURR_TIER_B4,
  P_PURR_TIER_D0,                 // P-26 各段锚点时长（ms）
  P_PURR_TIER_D1,
  P_PURR_TIER_D2,
  P_PURR_TIER_D3,
  P_PURR_LINEAR_SLOPE,            // P-27 线性模式斜率（ms/点）
  P_PURR_LINEAR_INTERCEPT_MS,     // P-28 线性模式截距（ms）
  P_PURR_DURATION_MAX_MS,         // P-29 时长上限（截断）
  P_PURR_BETWEEN_GAP_MS,          // P-30 轮次间最小间隔
  P_PURR_CUMULATIVE_LIMIT_MS,     // P-31 累计连续运行上限

  // --- 组 D：咕噜体感（5 项） ---
  P_PURR_DRIVE_FREQ_HZ,           // P-32 驱动频率（Hz×100）
  P_PURR_DUTY_BASE_PCT,           // P-33 基础占空比
  P_PURR_DUTY_JITTER_PCT,         // P-34 强度抖动幅度
  P_PURR_JITTER_PERIOD_MS,        // P-35 抖动周期
  P_PURR_LONG_ENVELOPE_MS,        // P-36 长包络缓变周期

  // --- 组 E：呼吸（10 项） ---
  P_BREATH_PERIOD_MS,             // P-37
  P_BREATH_PERIOD_JITTER_PCT,     // P-38
  P_BREATH_AMPLITUDE_JITTER_PCT,  // P-39
  P_BREATH_PEAK_DUTY_PCT,         // P-40
  P_BREATH_TROUGH_DUTY_PCT,       // P-41 ★ 默认 0（完全断电，消除嗡鸣）
  P_BREATH_CYCLES_PER_TRIGGER,    // P-42
  P_BREATH_MIN_INTERVAL_MS,       // P-43
  P_BREATH_L1_DUTY_PCT,           // P-44 L1/L2/L3 三档峰值占空比
  P_BREATH_L2_DUTY_PCT,
  P_BREATH_L3_DUTY_PCT,

  // --- 组 F：摇尾（18 项） ---
  P_TAIL_MODE_ENABLED,            // P-45
  P_TAIL_TIER_B0,                 // P-46 4 档分界（3 个内边界 + 1 个上界）
  P_TAIL_TIER_B1,
  P_TAIL_TIER_B2,
  P_TAIL_TIER_B3,
  P_TAIL_FREQ_W1,                 // P-47 各档频率（Hz×100）
  P_TAIL_FREQ_W2,
  P_TAIL_FREQ_W3,
  P_TAIL_FREQ_W4,
  P_TAIL_AMP_W1,                  // P-48 各档幅度（×100）
  P_TAIL_AMP_W2,
  P_TAIL_AMP_W3,
  P_TAIL_AMP_W4,
  P_TAIL_FREQ_JITTER_PCT,         // P-49
  P_TAIL_HYSTERESIS,              // P-50
  P_TAIL_TRANSITION_MS,           // P-51
  P_TAIL_PURR_EXIT_TRANSITION_MS, // P-52 ★ 须与 P-15 对齐（C-16）
  P_TAIL_STOP_IDLE_MS,            // P-53

  // --- 组 G：系统与调试（4 项） ---
  P_CONFIG_CLOUD_ENABLED,         // P-55
  P_DEBUG_MODE,                   // P-56
  P_DEBUG_LOG_ENABLED,            // P-57
  P_CHAR_PRESET,                  // P-58 0=gentle 1=standard 2=lively

  P_COUNT                         // 参数总数
};

// 分组名（用于 GET <group>.* / RESET <group>.*）
#define GRP_MOOD    "mood"
#define GRP_PURR    "purr"
#define GRP_BREATH  "breath"
#define GRP_TAIL    "tail"
#define GRP_SYS     "sys"

// P_MOOD_LANDING_CURVE 取值
enum LandingCurve : int32_t { LC_LINEAR = 0, LC_EASE_IN_OUT = 1, LC_EASE_OUT = 2 };
// P_PURR_DURATION_MODE 取值
enum PurrDurationMode : int32_t { PDM_TIERED = 0, PDM_LINEAR = 1 };

// ---------------------------------------------------------------------------
// 参数元信息
// ---------------------------------------------------------------------------
struct ParamMeta {
  const char* name;
  const char* group;
  int32_t     def;
  int32_t     min;
  int32_t     max;
  const char* desc;
  const char* choices;   // 枚举型参数的合法取值（'|' 分隔）；标量参数为 nullptr
};

class Params {
public:
  // 返回 true 表示成功（NVS 值被采纳或确认无可用值）；false 表示存储异常
  static bool init(Preferences* prefs);

  static int32_t get(ParamId id);
  static void    set(ParamId id, int32_t v);          // 带 [min,max] 钳制

  static const char* getName(ParamId id);
  static const char* getGroup(ParamId id);
  static int32_t getMin(ParamId id);
  static int32_t getMax(ParamId id);
  static const char* getDesc(ParamId id);
  static const char* getChoices(ParamId id);
  static ParamId findByName(const char* name);        // 未找到返回 P_COUNT
  static bool    parseChoice(ParamId id, const char* text, int32_t* out);

  static void printGroup(const char* group);
  static void printAll();
  static void resetGroup(const char* group);
  static int  countGroup(const char* group);
  static int  groupNames(const char** out, int cap);

  // 持久化（NVS）
  static bool save();
  static bool load();
  static bool hasStoredConfig();

  // 校验规则 C-01 ~ C-18
  //   validate()    返回 nullptr = 全部通过；否则返回违反的规则编号（如 "C-02"）
  //   validateSet() 试写校验：通过则不改动并返回 nullptr；失败返回规则编号。
  //                 ⚠ 以**原值**校验、不预先钳制——越界值必须被拒绝而不是被悄悄
  //                 夹到边界，否则 CLI 调参会得到"看起来成功但实际未生效"的假象。
  //                 值域类约束优先以 C-09 等规则报出；无规则覆盖的越界值报 E-LIMIT。
  static const char* validate();
  static const char* validateSet(ParamId id, int32_t v);
  // 规则说明（供 CLI 与文档统一引用）
  static const char* ruleText(const char* rule);

  // 性格预设（用户侧唯一可见的体感选项，P-58）
  static bool applyPreset(const char* name);
  static const char* getPresetName();
  static const char* presetNameByIndex(int32_t idx);

  static const char* getLayerName();
  static bool isDebug();

private:
  static int32_t      s_v[P_COUNT];
  static Preferences* s_prefs;
  static bool         s_loaded;   // true = 当前生效值来自 NVS
};

#endif  // GRUNTOY_PARAMS_H
