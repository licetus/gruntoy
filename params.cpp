/*
 * ============================================================================
 *  params.cpp —— 体感参数系统实现
 *  gruntoy v1.0
 * ============================================================================
 */

#include "params.h"
#include "config.h"

int32_t      Params::s_v[P_COUNT];
Preferences* Params::s_prefs = nullptr;
bool         Params::s_loaded = false;

// ---------------------------------------------------------------------------
// 参数元信息表
//   顺序必须与 ParamId 枚举严格一致（本机测试会断言这一点）。
//   freq 单位 Hz×100 / amp 单位 ×100 / bool 单位 0|1 / enum 单位 序号
// ---------------------------------------------------------------------------
static const ParamMeta META[P_COUNT] = {
  // --- 组 A：心情值累积 ---
  {"mood_initial",                 GRP_MOOD,   60,         0,   120, "开机初始心情值", nullptr},
  {"mood_max",                     GRP_MOOD,   120,      100,   999, "心情值上限（超出截断）", nullptr},
  {"mood_decay_floor",             GRP_MOOD,   40,         0,   120, "自然衰减下限（不归零）", nullptr},
  {"pet_gain_per_stroke",          GRP_MOOD,   2,          0,    20, "单次有效抚摸增量", nullptr},
  {"pet_valid_min_ms",             GRP_MOOD,   3000,     500, 10000, "有效抚摸最短持续时长(ms)", nullptr},
  {"pet_debounce_ms",              GRP_MOOD,   500,      100,  3000, "抚摸防抖间隔(ms)", nullptr},
  {"pet_continuous_window_ms",     GRP_MOOD,   30000,   5000,120000, "连续抚摸判定窗口(ms)", nullptr},
  {"pet_continuous_bonus",         GRP_MOOD,   1,          0,    10, "连续抚摸额外增量", nullptr},
  {"pet_continuous_interval_ms",   GRP_MOOD,   10000,   3000, 60000, "额外增量发放间隔(ms)", nullptr},
  {"mood_decay_start_ms",          GRP_MOOD,   1800000,    0,7200000, "无互动多久后开始衰减(ms，默认30分钟)", nullptr},
  {"mood_decay_interval_ms",       GRP_MOOD,   300000, 60000,1800000, "衰减间隔(ms，默认5分钟)", nullptr},
  {"mood_decay_step",              GRP_MOOD,   1,          0,    10, "每次衰减量", nullptr},

  // --- 组 B：触发、降落与冷却 ---
  {"purr_trigger_threshold",       GRP_PURR,   100,        0,   120, "触发咕噜的心情值阈值", nullptr},
  {"mood_restore_value",           GRP_PURR,   70,         0,   120, "降落终点（还原目标值）", nullptr},
  {"mood_landing_ms",              GRP_PURR,   5000,    1000, 30000, "降落总时长(ms)", nullptr},
  {"mood_landing_curve",           GRP_PURR,   1,          0,     2, "降落曲线类型", "linear|ease_in_out|ease_out"},
  {"mood_landing_segments",        GRP_PURR,   5,          3,    20, "分段线性近似段数（供曲线离散化参考）", nullptr},
  {"mood_landing_touch_buzz_ms",   GRP_PURR,   200,        0,  2000, "降落期抚摸柔震反馈时长(ms)", nullptr},
  {"mood_landing_block_gain",      GRP_PURR,   1,          0,     1, "降落期是否禁止增加心情值(1=禁止)", nullptr},
  {"cooldown_duration_ms",         GRP_PURR,   90000,      0,600000, "冷却时长(ms，0=无冷却)", nullptr},
  {"cooldown_deny_buzz_ms",        GRP_PURR,   300,        0,  2000, "冷却期抚摸拒绝震时长(ms)", nullptr},
  {"purr_fadein_ms",               GRP_PURR,   500,        0,  3000, "咕噜淡入时长(ms)", nullptr},
  {"purr_fadeout_ms",              GRP_PURR,   800,        0,  3000, "咕噜淡出时长(ms)", nullptr},

  // --- 组 C：咕噜时长映射 ---
  {"purr_duration_mode",           GRP_PURR,   0,          0,     1, "映射方式（锚点分段/线性）", "tiered|linear"},
  {"purr_tier_b0",                 GRP_PURR,   100,        0,   999, "分段点0（应等于触发阈值）", nullptr},
  {"purr_tier_b1",                 GRP_PURR,   105,        0,   999, "分段点1", nullptr},
  {"purr_tier_b2",                 GRP_PURR,   110,        0,   999, "分段点2", nullptr},
  {"purr_tier_b3",                 GRP_PURR,   115,        0,   999, "分段点3", nullptr},
  {"purr_tier_b4",                 GRP_PURR,   120,        0,   999, "分段点4（应等于 mood_max）", nullptr},
  {"purr_tier_d0",                 GRP_PURR,   20000,   5000,120000, "锚点时长0(ms) 对应 b0", nullptr},
  {"purr_tier_d1",                 GRP_PURR,   32000,   5000,120000, "锚点时长1(ms) 对应 b1", nullptr},
  {"purr_tier_d2",                 GRP_PURR,   46000,   5000,120000, "锚点时长2(ms) 对应 b2", nullptr},
  {"purr_tier_d3",                 GRP_PURR,   60000,   5000,120000, "锚点时长3(ms) 对应 b3~b4", nullptr},
  {"purr_linear_slope",            GRP_PURR,   4000,     100, 20000, "线性模式斜率(ms/点)", nullptr},
  {"purr_linear_intercept_ms",     GRP_PURR,   20000,      0,120000, "线性模式截距(ms)", nullptr},
  {"purr_duration_max_ms",         GRP_PURR,   60000,  10000,300000, "咕噜最长时长上限(ms)", nullptr},
  {"purr_between_gap_ms",          GRP_PURR,   15000,      0, 60000, "轮次间最小间隔(ms)", nullptr},
  {"purr_cumulative_limit_ms",     GRP_PURR,   300000, 60000,1800000, "累计连续运行上限(ms)", nullptr},

  // --- 组 D：咕噜体感 ---
  {"purr_drive_freq_hz",           GRP_PURR,   3000,    2000,  6000, "驱动频率(Hz×100，30Hz=3000)", nullptr},
  {"purr_duty_base_pct",           GRP_PURR,   65,        10,   100, "基础占空比(%)", nullptr},
  {"purr_duty_jitter_pct",         GRP_PURR,   15,         0,    50, "强度抖动幅度(%)", nullptr},
  {"purr_jitter_period_ms",        GRP_PURR,   500,      100,  5000, "抖动周期(ms)", nullptr},
  {"purr_long_envelope_ms",        GRP_PURR,   15000,   5000, 60000, "长包络缓变周期(ms)", nullptr},

  // --- 组 E：呼吸 ---
  {"breath_period_ms",             GRP_BREATH, 4000,    3000,  5000, "呼吸周期(ms)", nullptr},
  {"breath_period_jitter_pct",     GRP_BREATH, 15,         0,    50, "周期随机抖动(%)", nullptr},
  {"breath_amplitude_jitter_pct",  GRP_BREATH, 10,         0,    50, "幅度随机抖动(%)", nullptr},
  {"breath_peak_duty_pct",         GRP_BREATH, 40,        10,   100, "默认峰值占空比(%)", nullptr},
  {"breath_trough_duty_pct",       GRP_BREATH, 0,          0,    30, "★谷底占空比(%)，必须为0", nullptr},
  {"breath_cycles_per_trigger",    GRP_BREATH, 2,          1,     5, "单次触发周期数", nullptr},
  {"breath_min_interval_ms",       GRP_BREATH, 30000,      0,300000, "最小触发间隔(ms)", nullptr},
  {"breath_l1_duty_pct",           GRP_BREATH, 25,         0,   100, "L1 轻档峰值占空比(%)", nullptr},
  {"breath_l2_duty_pct",           GRP_BREATH, 40,         0,   100, "L2 中档峰值占空比(%)", nullptr},
  {"breath_l3_duty_pct",           GRP_BREATH, 60,         0,   100, "L3 重档峰值占空比(%)", nullptr},

  // --- 组 F：摇尾 ---
  {"tail_mode_enabled",            GRP_TAIL,   1,          0,     1, "摇尾功能总开关", nullptr},
  {"tail_tier_b0",                 GRP_TAIL,   54,         0,  1000, "W1/W2 分界", nullptr},
  {"tail_tier_b1",                 GRP_TAIL,   79,         0,  1000, "W2/W3 分界", nullptr},
  {"tail_tier_b2",                 GRP_TAIL,   99,         0,  1000, "W3/W4 分界", nullptr},
  {"tail_tier_b3",                 GRP_TAIL,   120,        0,  1000, "W4 上界（应等于 mood_max）", nullptr},
  {"tail_freq_w1",                 GRP_TAIL,   30,         0,  1000, "W1 频率(Hz×100)=0.30Hz", nullptr},
  {"tail_freq_w2",                 GRP_TAIL,   60,         0,  1000, "W2 频率(Hz×100)=0.60Hz", nullptr},
  {"tail_freq_w3",                 GRP_TAIL,   120,        0,  1000, "W3 频率(Hz×100)=1.20Hz", nullptr},
  {"tail_freq_w4",                 GRP_TAIL,   200,        0,  1000, "W4 频率(Hz×100)=2.00Hz", nullptr},
  {"tail_amp_w1",                  GRP_TAIL,   60,         0,   100, "W1 幅度(×100)=0.60", nullptr},
  {"tail_amp_w2",                  GRP_TAIL,   70,         0,   100, "W2 幅度(×100)=0.70", nullptr},
  {"tail_amp_w3",                  GRP_TAIL,   85,         0,   100, "W3 幅度(×100)=0.85", nullptr},
  {"tail_amp_w4",                  GRP_TAIL,   100,        0,   100, "W4 幅度(×100)=1.00", nullptr},
  {"tail_freq_jitter_pct",         GRP_TAIL,   10,         0,    30, "频率随机抖动(%)", nullptr},
  {"tail_hysteresis",              GRP_TAIL,   2,          0,    10, "档位迟滞带宽（心情值单位）", nullptr},
  {"tail_transition_ms",           GRP_TAIL,   2500,     500, 10000, "常规档位切换过渡时长(ms)", nullptr},
  {"tail_purr_exit_transition_ms", GRP_TAIL,   5000,    1000, 15000, "★咕噜后缓降时长(ms)，须与 mood_landing_ms 对齐", nullptr},
  {"tail_stop_idle_ms",            GRP_TAIL,   600000,      0,3600000, "无互动多久后停止摇摆(ms，默认10分钟)", nullptr},

  // --- 组 G：系统与调试 ---
  {"config_cloud_enabled",         GRP_SYS,    0,          0,     1, "云端参数通道开关（默认关闭）", nullptr},
  {"debug_mode",                   GRP_SYS,    1,          0,     1, "调试模式（正式固件编译期关闭）", nullptr},
  {"debug_log_enabled",            GRP_SYS,    1,          0,     1, "内部状态日志输出", nullptr},
  {"char_preset",                  GRP_SYS,    1,          0,     2, "性格档位预设（用户可见的唯一体感选项）", "gentle|standard|lively"},
};

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
bool Params::init(Preferences* prefs) {
  s_prefs = prefs;

  // 第一层：硬编码默认值（永久兜底，离线也能完整工作）
  for (int i = 0; i < P_COUNT; i++) s_v[i] = META[i].def;

  // 第二层：本地 NVS 覆盖
  s_loaded = load();

  Serial.printf("[PARAM] 生效层级: %s（共 %d 项）\n", getLayerName(), (int)P_COUNT);

  // 启动即自检：默认值本身不合法属于固件配置缺陷，必须显式报警
  const char* err = validate();
  if (err) {
    Serial.printf("[PARAM][ERROR] 启动参数集未通过校验: %s —— %s\n", err, ruleText(err));
  }
  return true;
}

// ---------------------------------------------------------------------------
// 读写
// ---------------------------------------------------------------------------
int32_t Params::get(ParamId id) {
  if (id >= P_COUNT) return 0;
  return s_v[id];
}

void Params::set(ParamId id, int32_t v) {
  if (id >= P_COUNT) return;
  if (v < META[id].min) v = META[id].min;
  if (v > META[id].max) v = META[id].max;
  s_v[id] = v;
}

const char* Params::getName(ParamId id)    { return (id < P_COUNT) ? META[id].name    : "?"; }
const char* Params::getGroup(ParamId id)   { return (id < P_COUNT) ? META[id].group   : "?"; }
int32_t     Params::getMin(ParamId id)     { return (id < P_COUNT) ? META[id].min     : 0; }
int32_t     Params::getMax(ParamId id)     { return (id < P_COUNT) ? META[id].max     : 0; }
const char* Params::getDesc(ParamId id)    { return (id < P_COUNT) ? META[id].desc    : "?"; }
const char* Params::getChoices(ParamId id) { return (id < P_COUNT) ? META[id].choices : nullptr; }

ParamId Params::findByName(const char* name) {
  if (!name) return P_COUNT;
  for (int i = 0; i < P_COUNT; i++) {
    if (strcmp(META[i].name, name) == 0) return (ParamId)i;
  }
  return P_COUNT;
}

// 枚举型参数支持按名写入（SET mood_landing_curve ease_in_out）
bool Params::parseChoice(ParamId id, const char* text, int32_t* out) {
  if (id >= P_COUNT || !META[id].choices || !text || !*text) return false;

  // 先接受纯数字
  bool numeric = true;
  for (const char* q = text; *q; q++) {
    if (!isdigit((unsigned char)*q) && !(*q == '-' && q == text)) { numeric = false; break; }
  }
  if (numeric) { *out = atol(text); return true; }

  int idx = 0;
  const char* p = META[id].choices;
  while (*p) {
    const char* bar = strchr(p, '|');
    size_t len = bar ? (size_t)(bar - p) : strlen(p);
    if (strlen(text) == len && strncasecmp(p, text, len) == 0) { *out = idx; return true; }
    if (!bar) break;
    p = bar + 1;
    idx++;
  }
  return false;
}

// ---------------------------------------------------------------------------
// 打印
// ---------------------------------------------------------------------------
static void printOne(int i) {
  const char* ch = META[i].choices;
  if (ch) {
    Serial.printf("  %-31s = %6ld   [%ld~%ld]  %s <%s>\n",
                  META[i].name, (long)Params::get((ParamId)i),
                  (long)META[i].min, (long)META[i].max, META[i].desc, ch);
  } else {
    Serial.printf("  %-31s = %6ld   [%ld~%ld]  %s\n",
                  META[i].name, (long)Params::get((ParamId)i),
                  (long)META[i].min, (long)META[i].max, META[i].desc);
  }
}

void Params::printGroup(const char* group) {
  int n = 0;
  for (int i = 0; i < P_COUNT; i++) {
    if (strcmp(META[i].group, group) == 0) { printOne(i); n++; }
  }
  if (n == 0) Serial.printf("  (无此分组: %s)\n", group);
}

int Params::groupNames(const char** out, int cap) {
  static const char* g[] = {GRP_MOOD, GRP_PURR, GRP_BREATH, GRP_TAIL, GRP_SYS};
  int n = 5;
  if (n > cap) n = cap;
  for (int i = 0; i < n; i++) out[i] = g[i];
  return n;
}

void Params::printAll() {
  const char* g[8];
  int n = groupNames(g, 8);
  for (int i = 0; i < n; i++) {
    Serial.printf("\n--- 组: %s (%d 项) ---\n", g[i], countGroup(g[i]));
    printGroup(g[i]);
  }
}

void Params::resetGroup(const char* group) {
  int n = 0;
  for (int i = 0; i < P_COUNT; i++) {
    if (strcmp(META[i].group, group) == 0) { s_v[i] = META[i].def; n++; }
  }
  Serial.printf("[PARAM] 已重置分组 %s（%d 项）\n", group, n);
}

int Params::countGroup(const char* group) {
  int n = 0;
  for (int i = 0; i < P_COUNT; i++) if (strcmp(META[i].group, group) == 0) n++;
  return n;
}

// ---------------------------------------------------------------------------
// 持久化（NVS）
//   写入：version + 整块 blob
//   读取：version 不匹配或长度不符一律视为"无可用本地配置"，
//         此时保持硬编码默认值（属于首次开机路径，不是"拉取失败回落"）。
// ---------------------------------------------------------------------------
bool Params::save() {
  if (!s_prefs) return false;
  if (!s_prefs->begin(PARAMS_NVS_NS, false)) {
    Serial.println(F("[PARAM][ERROR] NVS 打开失败（写入）"));
    return false;
  }
  uint32_t ver = PARAMS_NVS_SCHEMA;
  size_t wv = s_prefs->putUInt("ver", ver);
  size_t wb = s_prefs->putBytes("cfg", s_v, sizeof(s_v));
  s_prefs->end();
  if (wv == sizeof(uint32_t) && wb == sizeof(s_v)) {
    Serial.printf("[PARAM] 已保存 %d 项参数到 NVS (schema=%lu)\n",
                  (int)P_COUNT, (unsigned long)ver);
    return true;
  }
  Serial.println(F("[PARAM][ERROR] NVS 写入长度不符"));
  return false;
}

bool Params::load() {
  if (!s_prefs) return false;
  if (!s_prefs->begin(PARAMS_NVS_NS, true)) return false;

  uint32_t ver = s_prefs->getUInt("ver", 0);
  if (ver != PARAMS_NVS_SCHEMA) {
    s_prefs->end();
    Serial.printf("[PARAM] NVS schema 不匹配（存储=%lu，当前=%lu），忽略本地值\n",
                  (unsigned long)ver, (unsigned long)PARAMS_NVS_SCHEMA);
    return false;
  }
  size_t len = s_prefs->getBytesLength("cfg");
  if (len != sizeof(s_v)) {
    s_prefs->end();
    Serial.println(F("[PARAM] NVS 数据长度不符，忽略本地值"));
    return false;
  }
  size_t rd = s_prefs->getBytes("cfg", s_v, sizeof(s_v));
  s_prefs->end();
  if (rd != sizeof(s_v)) return false;

  // 载入后按当前元信息钳制，防止旧值越界
  for (int i = 0; i < P_COUNT; i++) {
    if (s_v[i] < META[i].min) s_v[i] = META[i].min;
    if (s_v[i] > META[i].max) s_v[i] = META[i].max;
  }
  return true;
}

bool Params::hasStoredConfig() { return s_loaded; }

// ---------------------------------------------------------------------------
// 校验规则 C-01 ~ C-18（PRD 附录 C.8）
//   执行时机：写入参数时立即执行全部校验；任一项不通过则拒绝写入。
//   另含 v1.0 扩展规则 X-01~X-03（不在 PRD 编号内，用于覆盖 v1.0 补齐的参数）。
//   返回 nullptr 表示通过。
// ---------------------------------------------------------------------------
const char* Params::validate() {
  // ---- C-01：mood_initial < purr_trigger_threshold（否则开机立即触发咕噜）
  if (s_v[P_MOOD_INITIAL] >= s_v[P_PURR_TRIGGER_THRESHOLD]) return "C-01";

  // ---- C-02：★最高优先级 mood_restore_value < purr_trigger_threshold
  //      若降落终点 ≥ 触发阈值，咕噜结束瞬间会立即重新触发 → 无限咕噜死循环
  if (s_v[P_MOOD_RESTORE_VALUE] >= s_v[P_PURR_TRIGGER_THRESHOLD]) return "C-02";

  // ---- C-03：purr_duration_max_ms ≥ 各锚点时长最大值（否则时长被意外截断）
  {
    int32_t mx = s_v[P_PURR_TIER_D0];
    if (s_v[P_PURR_TIER_D1] > mx) mx = s_v[P_PURR_TIER_D1];
    if (s_v[P_PURR_TIER_D2] > mx) mx = s_v[P_PURR_TIER_D2];
    if (s_v[P_PURR_TIER_D3] > mx) mx = s_v[P_PURR_TIER_D3];
    if (s_v[P_PURR_DURATION_MAX_MS] < mx) return "C-03";
  }

  // ---- C-04：分段点个数 = 时长个数 + 1
  //      本实现将数组展开为 5 个 b* 与 4 个 d* 标量，长度关系由编译期结构保证。
  //      （规则保留，运行时恒真）

  // ---- C-05：分段点严格递增，末值 = mood_max
  if (!(s_v[P_PURR_TIER_B0] < s_v[P_PURR_TIER_B1] &&
        s_v[P_PURR_TIER_B1] < s_v[P_PURR_TIER_B2] &&
        s_v[P_PURR_TIER_B2] < s_v[P_PURR_TIER_B3] &&
        s_v[P_PURR_TIER_B3] < s_v[P_PURR_TIER_B4])) return "C-05";
  if (s_v[P_PURR_TIER_B4] != s_v[P_MOOD_MAX]) return "C-05";

  // ---- C-06：mood_decay_floor ≤ mood_restore_value
  if (s_v[P_MOOD_DECAY_FLOOR] > s_v[P_MOOD_RESTORE_VALUE]) return "C-06";

  // ---- C-07：pet_gain_per_stroke > 0 且 pet_valid_min_ms > 0
  if (s_v[P_PET_GAIN_PER_STROKE] <= 0 || s_v[P_PET_VALID_MIN_MS] <= 0) return "C-07";

  // ---- C-08：cooldown_duration_ms ≥ 0（=0 表示无冷却）—— 由 min 钳制保证，恒真

  // ---- C-09：purr_drive_freq_hz 落在所选马达工作区间（20~60Hz → 2000~6000）
  if (s_v[P_PURR_DRIVE_FREQ_HZ] < 2000 || s_v[P_PURR_DRIVE_FREQ_HZ] > 6000) return "C-09";

  // ---- C-10：摇尾三组数组长度一致 —— 结构固定为 4，恒真

  // ---- C-11：摇尾分界严格递增，末值 = mood_max
  if (!(s_v[P_TAIL_TIER_B0] < s_v[P_TAIL_TIER_B1] &&
        s_v[P_TAIL_TIER_B1] < s_v[P_TAIL_TIER_B2] &&
        s_v[P_TAIL_TIER_B2] < s_v[P_TAIL_TIER_B3])) return "C-11";
  if (s_v[P_TAIL_TIER_B3] != s_v[P_MOOD_MAX]) return "C-11";

  // ---- C-12：摇尾各档频率 ≤ 机构允许最大摆频（3.0Hz → 300）
  {
    const int32_t LIMIT = 300;
    if (s_v[P_TAIL_FREQ_W1] > LIMIT || s_v[P_TAIL_FREQ_W2] > LIMIT ||
        s_v[P_TAIL_FREQ_W3] > LIMIT || s_v[P_TAIL_FREQ_W4] > LIMIT) return "C-12";
  }

  // ---- C-13：摇尾各档幅度 ≤ 1.0（机械限位）
  if (s_v[P_TAIL_AMP_W1] > 100 || s_v[P_TAIL_AMP_W2] > 100 ||
      s_v[P_TAIL_AMP_W3] > 100 || s_v[P_TAIL_AMP_W4] > 100) return "C-13";

  // ---- C-14：tail_purr_exit_transition_ms ≥ tail_transition_ms
  if (s_v[P_TAIL_PURR_EXIT_TRANSITION_MS] < s_v[P_TAIL_TRANSITION_MS]) return "C-14";

  // ---- C-15：mood_landing_ms > 0（否则退化为阶跃）
  if (s_v[P_MOOD_LANDING_MS] <= 0) return "C-15";

  // ---- C-16：★tail_purr_exit_transition_ms 与 mood_landing_ms 差异 ≤ 1000ms
  {
    int32_t d = s_v[P_TAIL_PURR_EXIT_TRANSITION_MS] - s_v[P_MOOD_LANDING_MS];
    if (d < 0) d = -d;
    if (d > 1000) return "C-16";
  }

  // ---- C-17：mood_landing_segments ≥ 3
  if (s_v[P_MOOD_LANDING_SEGMENTS] < 3) return "C-17";

  // ---- C-18：breath_trough_duty_pct = 0（保留低占空比残量将产生机械嗡鸣）
  if (s_v[P_BREATH_TROUGH_DUTY_PCT] != 0) return "C-18";

  // ===== v1.0 扩展校验（X-01~X-03）=====

  // ---- X-01：映射起点必须与触发阈值一致
  //      否则心情值刚达阈值时会被映射到"中间档时长"，最低档 D0 永不生效。
  if (s_v[P_PURR_TIER_B0] != s_v[P_PURR_TRIGGER_THRESHOLD]) return "X-01";

  // ---- X-02：线性模式下映射在 [阈值, mood_max] 区间单调不减
  if (s_v[P_PURR_DURATION_MODE] == PDM_LINEAR) {
    if (s_v[P_PURR_LINEAR_SLOPE] <= 0) return "X-02";
    // 值域取 [阈值, mood_max]：lo = 阈值处取值（= 截距），hi = 上限处取值
    const int32_t lo = s_v[P_PURR_LINEAR_INTERCEPT_MS];
    const int32_t hi = lo + s_v[P_PURR_LINEAR_SLOPE] * (s_v[P_MOOD_MAX] - s_v[P_PURR_TRIGGER_THRESHOLD]);
    if (hi <= lo) return "X-02";
  }

  // ---- X-03：枚举型参数取值必须合法
  if (s_v[P_MOOD_LANDING_CURVE] < LC_LINEAR || s_v[P_MOOD_LANDING_CURVE] > LC_EASE_OUT) return "X-03";
  if (s_v[P_PURR_DURATION_MODE] < PDM_TIERED || s_v[P_PURR_DURATION_MODE] > PDM_LINEAR) return "X-03";
  if (s_v[P_CHAR_PRESET] < 0 || s_v[P_CHAR_PRESET] > 2) return "X-03";

  return nullptr;
}

// 试写校验：以候选值**原值**试算整套规则，不改动当前生效值
const char* Params::validateSet(ParamId id, int32_t v) {
  if (id >= P_COUNT) return "E-RANGE";

  // ⚠ 这里**不做钳制**：试写校验要回答的是"这个值能不能用"，不是"夹到边界后
  //   能不能用"。若先钳制再校验，越界值会被悄悄夹进合法区间，规则永远报不出来——
  //   典型后果：`t gate 99`（超出 20~60Hz 工作区间）看起来设置成功，实际被夹到
  //   60Hz，操作者据此测出的"猫感区间"是错的。因此必须以原值走完整套 validate()，
  //   让 C-09 之类"值域即规则"的条目如实报出。
  //   仅在一条规则都没命中、而值本身越界时，才退回到通用的 E-LIMIT。
  int32_t     old = s_v[id];
  s_v[id] = v;
  const char* err = validate();
  s_v[id] = old;
  if (err) return err;

  if (v < META[id].min || v > META[id].max) return "E-LIMIT";
  return nullptr;
}

const char* Params::ruleText(const char* rule) {
  if (!rule) return "";
  if (!strcmp(rule, "C-01")) return "mood_initial 必须 < purr_trigger_threshold，否则开机立即触发咕噜";
  if (!strcmp(rule, "C-02")) return "mood_restore_value 必须 < purr_trigger_threshold，否则咕噜结束后立即重触发（死循环）";
  if (!strcmp(rule, "C-03")) return "purr_duration_max_ms 必须 ≥ 各锚点时长的最大值，否则时长被意外截断";
  if (!strcmp(rule, "C-04")) return "分段点个数必须 = 时长个数 + 1";
  if (!strcmp(rule, "C-05")) return "分段点必须严格递增且末值 = mood_max，否则高档位永不触发";
  if (!strcmp(rule, "C-06")) return "mood_decay_floor 必须 ≤ mood_restore_value，否则状态错乱";
  if (!strcmp(rule, "C-07")) return "pet_gain_per_stroke 与 pet_valid_min_ms 必须 > 0，否则无法累积";
  if (!strcmp(rule, "C-08")) return "cooldown_duration_ms 必须 ≥ 0（=0 表示无冷却）";
  if (!strcmp(rule, "C-09")) return "purr_drive_freq_hz 必须在马达工作区间内（20~60Hz），否则不转或异响";
  if (!strcmp(rule, "C-10")) return "摇尾分界/频率/幅度三组长度必须一致";
  if (!strcmp(rule, "C-11")) return "摇尾分界必须严格递增且末值 = mood_max";
  if (!strcmp(rule, "C-12")) return "摇尾各档频率不得超过机构允许的最大摆频";
  if (!strcmp(rule, "C-13")) return "摇尾各档幅度必须 ≤ 1.0（机械限位）";
  if (!strcmp(rule, "C-14")) return "tail_purr_exit_transition_ms 必须 ≥ tail_transition_ms，否则咕噜后过渡反而更快";
  if (!strcmp(rule, "C-15")) return "mood_landing_ms 必须 > 0，否则退化为阶跃，违背平滑设计";
  if (!strcmp(rule, "C-16")) return "tail_purr_exit_transition_ms 与 mood_landing_ms 差异必须 ≤ 1000ms，否则内外错位";
  if (!strcmp(rule, "C-17")) return "mood_landing_segments 必须 ≥ 3，否则缓入缓出无法体现";
  if (!strcmp(rule, "C-18")) return "breath_trough_duty_pct 必须 = 0，保留残量将产生机械嗡鸣";
  if (!strcmp(rule, "X-01")) return "[v1.0] 咕噜映射起点 purr_tier_b0 必须 = purr_trigger_threshold";
  if (!strcmp(rule, "X-02")) return "[v1.0] 线性映射斜率必须 > 0 且在区间内单调不减";
  if (!strcmp(rule, "X-03")) return "[v1.0] 枚举型参数取值非法";
  if (!strcmp(rule, "E-RANGE")) return "参数编号越界";
  if (!strcmp(rule, "E-LIMIT")) return "取值超出该参数允许范围（见 GET <param> 输出的 [min~max]）";
  return "未知校验规则";
}

// ---------------------------------------------------------------------------
// 性格预设（P-58）
//   用户侧唯一可见的体感选项。三套预设必须自身参数自洽并通过全部校验。
//   调参建议（PRD C.10）：内测阶段只在这三套间切换 + 微调
//   P-04 / P-13 / P-14 / P-20 / P-52，其余保持不动。
// ---------------------------------------------------------------------------
bool Params::applyPreset(const char* name) {
  if (!name) return false;

  if (strcmp(name, "gentle") == 0) {
    s_v[P_CHAR_PRESET]            = 0;
    s_v[P_PURR_TRIGGER_THRESHOLD] = 105;
    s_v[P_COOLDOWN_DURATION_MS]   = 120000;
    s_v[P_PURR_DUTY_BASE_PCT]     = 55;
    s_v[P_BREATH_MIN_INTERVAL_MS] = 45000;
    s_v[P_TAIL_FREQ_W1] = 25;  s_v[P_TAIL_FREQ_W2] = 50;
    s_v[P_TAIL_FREQ_W3] = 100; s_v[P_TAIL_FREQ_W4] = 170;
    // 分段点随阈值整体左移，保持"起点 = 触发阈值"（X-01）
    s_v[P_PURR_TIER_B0] = 105; s_v[P_PURR_TIER_B1] = 107;
    s_v[P_PURR_TIER_B2] = 110; s_v[P_PURR_TIER_B3] = 115;
  } else if (strcmp(name, "standard") == 0) {
    s_v[P_CHAR_PRESET]            = 1;
    s_v[P_PURR_TRIGGER_THRESHOLD] = 100;
    s_v[P_COOLDOWN_DURATION_MS]   = 90000;
    s_v[P_PURR_DUTY_BASE_PCT]     = 65;
    s_v[P_BREATH_MIN_INTERVAL_MS] = 30000;
    s_v[P_TAIL_FREQ_W1] = 30;  s_v[P_TAIL_FREQ_W2] = 60;
    s_v[P_TAIL_FREQ_W3] = 120; s_v[P_TAIL_FREQ_W4] = 200;
    s_v[P_PURR_TIER_B0] = 100; s_v[P_PURR_TIER_B1] = 105;
    s_v[P_PURR_TIER_B2] = 110; s_v[P_PURR_TIER_B3] = 115;
  } else if (strcmp(name, "lively") == 0) {
    s_v[P_CHAR_PRESET]            = 2;
    s_v[P_PURR_TRIGGER_THRESHOLD] = 95;
    s_v[P_COOLDOWN_DURATION_MS]   = 60000;
    s_v[P_PURR_DUTY_BASE_PCT]     = 70;
    s_v[P_BREATH_MIN_INTERVAL_MS] = 20000;
    s_v[P_TAIL_FREQ_W1] = 40;  s_v[P_TAIL_FREQ_W2] = 80;
    s_v[P_TAIL_FREQ_W3] = 150; s_v[P_TAIL_FREQ_W4] = 220;
    s_v[P_PURR_TIER_B0] = 95;  s_v[P_PURR_TIER_B1] = 103;
    s_v[P_PURR_TIER_B2] = 110; s_v[P_PURR_TIER_B3] = 116;
  } else {
    return false;
  }

  // 预设可能改动阈值，必须同步两个"末值 = mood_max"的分段点
  s_v[P_PURR_TIER_B4] = s_v[P_MOOD_MAX];
  s_v[P_TAIL_TIER_B3] = s_v[P_MOOD_MAX];

  const char* err = validate();
  if (err) {
    Serial.printf("[PARAM][ERROR] 预设 %s 校验失败: %s —— %s\n", name, err, ruleText(err));
    return false;
  }
  Serial.printf("[PARAM] 已应用性格预设: %s\n", getPresetName());
  return true;
}

const char* Params::presetNameByIndex(int32_t idx) {
  switch (idx) {
    case 0: return "gentle";
    case 1: return "standard";
    case 2: return "lively";
    default: return "unknown";
  }
}

const char* Params::getPresetName() {
  switch (s_v[P_CHAR_PRESET]) {
    case 0: return "gentle (温和)";
    case 1: return "standard (标准)";
    case 2: return "lively (活泼)";
    default: return "unknown";
  }
}

const char* Params::getLayerName() {
  if (s_v[P_CONFIG_CLOUD_ENABLED]) return "云端 (cloud) [v1.0 未接入]";
  if (s_loaded)                    return "本地 (local/NVS)";
  return "默认 (hardcoded)";
}

bool Params::isDebug() { return s_v[P_DEBUG_MODE] != 0; }
