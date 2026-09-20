/*
 * ============================================================================
 *  test_main.cpp —— 参数体系与校验规则验证
 *
 *  覆盖：
 *    · 参数表完整性（项数、分组合计、名称唯一、枚举与 META 顺序一致）
 *    · PRD 附录 C.8 的 C-01~C-18 中可在本机验证的部分
 *    · v1.0 扩展规则 X-01~X-03
 *    · 三套性格预设的自洽性
 *    · 参数名查找 / 枚举取值解析 / 越界钳制
 *    · NVS 持久化与 schema 版本守卫
 * ============================================================================
 */

#include "Arduino.h"
#include "Preferences.h"
#include "params.h"
#include "config.h"
#include "check.h"

#include <cstdio>

int main() {
  printf("\n===== gruntoy v1.0 参数体系与校验规则 =====\n");

  Preferences prefs;
  Params::init(&prefs);

  // -------------------------------------------------------------------------
  SECTION("1. 参数表完整性");
  {
    CHECK(P_COUNT == 75, "参数项数 = 75（P-01~P-58 展开 + v1.0 补齐 P-16/P-24）");

    int total = 0;
    const char* g[8];
    const int n = Params::groupNames(g, 8);
    for (int i = 0; i < n; i++) total += Params::countGroup(g[i]);
    char buf[96];
    snprintf(buf, sizeof(buf), "五组参数合计 %d == P_COUNT %d（无漏项/无重复归组）", total, (int)P_COUNT);
    CHECK(total == (int)P_COUNT, buf);

    bool unique = true;
    for (int i = 0; i < P_COUNT && unique; i++) {
      for (int j = i + 1; j < P_COUNT; j++) {
        if (strcmp(Params::getName((ParamId)i), Params::getName((ParamId)j)) == 0) { unique = false; break; }
      }
    }
    CHECK(unique, "参数名全局唯一");

    // 枚举与 META 顺序一致性抽查（顺序错位是本文件最容易犯的错）
    CHECK(!strcmp(Params::getName(P_MOOD_INITIAL), "mood_initial"), "P-01 顺序正确");
    CHECK(!strcmp(Params::getName(P_MOOD_LANDING_CURVE), "mood_landing_curve"), "P-16 顺序正确（v1.0 新增位）");
    CHECK(!strcmp(Params::getName(P_PURR_DURATION_MODE), "purr_duration_mode"), "P-24 顺序正确（v1.0 新增位）");
    CHECK(!strcmp(Params::getName(P_TAIL_PURR_EXIT_TRANSITION_MS), "tail_purr_exit_transition_ms"), "P-52 顺序正确");
    CHECK(!strcmp(Params::getName(P_CHAR_PRESET), "char_preset"), "P-58 顺序正确");

    CHECK(Params::validate() == nullptr, "默认参数集通过全部校验（C-01~C-18 + X-01~X-03）");
  }

  // -------------------------------------------------------------------------
  SECTION("2. C-02：restore < threshold（防无限咕噜死循环，最高优先级）");
  {
    CHECK(!strcmp(Params::validateSet(P_MOOD_RESTORE_VALUE, 100), "C-02"), "restore=100（==阈值）被 C-02 拒绝");
    CHECK(!strcmp(Params::validateSet(P_MOOD_RESTORE_VALUE, 105), "C-02"), "restore=105（>阈值）被 C-02 拒绝");
    CHECK(Params::validateSet(P_MOOD_RESTORE_VALUE, 99) == nullptr, "restore=99 通过");
    CHECK(Params::validateSet(P_MOOD_RESTORE_VALUE, 70) == nullptr, "restore=70 通过");
  }

  // -------------------------------------------------------------------------
  SECTION("3. C-16：摇尾缓降与心情降落对齐（±1000ms）");
  {
    CHECK(!strcmp(Params::validateSet(P_MOOD_LANDING_MS, 9000), "C-16"), "landing=9000 与 tail_exit=5000 差 4s，被 C-16 拒绝");
    CHECK(Params::validateSet(P_MOOD_LANDING_MS, 5500) == nullptr, "landing=5500（差 0.5s）通过");
    CHECK(Params::validateSet(P_MOOD_LANDING_MS, 6000) == nullptr, "landing=6000（差 1.0s 边界内）通过");
  }

  // -------------------------------------------------------------------------
  SECTION("4. C-18：呼吸波谷必须为 0（消除低占空比机械嗡鸣）");
  {
    CHECK(!strcmp(Params::validateSet(P_BREATH_TROUGH_DUTY_PCT, 20), "C-18"), "trough=20 被 C-18 拒绝");
    CHECK(!strcmp(Params::validateSet(P_BREATH_TROUGH_DUTY_PCT, 1), "C-18"), "trough=1 同样被拒绝（不留残量）");
    CHECK(Params::validateSet(P_BREATH_TROUGH_DUTY_PCT, 0) == nullptr, "trough=0 通过");
  }

  // -------------------------------------------------------------------------
  SECTION("5. 其余校验规则");
  {
    CHECK(!strcmp(Params::validateSet(P_MOOD_INITIAL, 100), "C-01"), "C-01：初始值 = 阈值 → 拒绝");
    CHECK(!strcmp(Params::validateSet(P_PURR_DURATION_MAX_MS, 10000), "C-03"), "C-03：上限 < 锚点最长时长 → 拒绝");
    CHECK(!strcmp(Params::validateSet(P_PURR_TIER_B4, 118), "C-05"), "C-05：末端分段点 != mood_max → 拒绝");
    CHECK(!strcmp(Params::validateSet(P_PURR_TIER_B1, 100), "C-05"), "C-05：分段点非严格递增 → 拒绝");
    CHECK(!strcmp(Params::validateSet(P_MOOD_DECAY_FLOOR, 80), "C-06"), "C-06：衰减地板 > 还原值 → 拒绝");
    CHECK(!strcmp(Params::validateSet(P_PET_GAIN_PER_STROKE, 0), "C-07"), "C-07：单次增量为 0 → 拒绝");
    CHECK(!strcmp(Params::validateSet(P_TAIL_TIER_B3, 110), "C-11"), "C-11：摇尾末档 != mood_max → 拒绝");
    CHECK(!strcmp(Params::validateSet(P_TAIL_FREQ_W4, 400), "C-12"), "C-12：W4 = 4.0Hz 超机构上限 → 拒绝");
    CHECK(!strcmp(Params::validateSet(P_TAIL_PURR_EXIT_TRANSITION_MS, 1000), "C-14"), "C-14：缓降时长 < 常规过渡 → 拒绝");
  }

  // -------------------------------------------------------------------------
  SECTION("6. v1.0 扩展规则 X-01~X-03");
  {
    CHECK(!strcmp(Params::validateSet(P_PURR_TRIGGER_THRESHOLD, 105), "X-01"),
          "X-01：抬阈值却不同步 purr_tier_b0 → 拒绝（否则最低档时长永不生效）");

    int32_t v = -1;
    CHECK(Params::parseChoice(P_MOOD_LANDING_CURVE, "ease_out", &v) && v == 2, "枚举解析：ease_out → 2");
    CHECK(Params::parseChoice(P_MOOD_LANDING_CURVE, "linear", &v) && v == 0, "枚举解析：linear → 0");
    CHECK(Params::parseChoice(P_PURR_DURATION_MODE, "linear", &v) && v == 1, "枚举解析：purr_duration_mode linear → 1");
    CHECK(!Params::parseChoice(P_MOOD_LANDING_CURVE, "no_such_curve", &v), "枚举解析：非法名被拒绝");
    CHECK(Params::getChoices(P_CHAR_PRESET) != nullptr, "char_preset 暴露合法取值列表（CLI 提示用）");
    CHECK(Params::getChoices(P_MOOD_LANDING_MS) == nullptr, "标量参数无取值列表");
  }

  // -------------------------------------------------------------------------
  SECTION("7. 三套性格预设自洽");
  {
    const char* names[] = {"gentle", "standard", "lively"};
    for (int i = 0; i < 3; i++) {
      const bool ok = Params::applyPreset(names[i]);
      char buf[180];
      snprintf(buf, sizeof(buf), "预设 %-8s 套用成功、参数自洽（threshold=%ld b0=%ld cooldown=%lds）",
               names[i], (long)Params::get(P_PURR_TRIGGER_THRESHOLD),
               (long)Params::get(P_PURR_TIER_B0),
               (long)(Params::get(P_COOLDOWN_DURATION_MS) / 1000));
      CHECK(ok && Params::validate() == nullptr, buf);
      // X-01 一致性：预设改阈值时必须同步映射起点
      snprintf(buf, sizeof(buf), "预设 %-8s 的映射起点与触发阈值一致（X-01）", names[i]);
      CHECK(Params::get(P_PURR_TIER_B0) == Params::get(P_PURR_TRIGGER_THRESHOLD), buf);
    }
    CHECK(!Params::applyPreset("no_such_preset"), "非法预设名返回失败");
    CHECK(Params::applyPreset("standard") && Params::get(P_CHAR_PRESET) == 1, "复位到标准预设");
  }

  // -------------------------------------------------------------------------
  SECTION("8. 参数名查找 / 钳制 / 规则说明");
  {
    CHECK(Params::findByName("purr_trigger_threshold") == P_PURR_TRIGGER_THRESHOLD, "按名查找成功");
    CHECK(Params::findByName("no_such_param_xyz") == P_COUNT, "不存在的参数名返回 P_COUNT");
    CHECK(Params::findByName(nullptr) == P_COUNT, "空指针安全");

    Params::set(P_COOLDOWN_DURATION_MS, 99999999);
    CHECK(Params::get(P_COOLDOWN_DURATION_MS) == Params::getMax(P_COOLDOWN_DURATION_MS), "越上限写入被钳制到 max");
    Params::set(P_COOLDOWN_DURATION_MS, -5);
    CHECK(Params::get(P_COOLDOWN_DURATION_MS) == Params::getMin(P_COOLDOWN_DURATION_MS), "越下限写入被钳制到 min");

    bool allText = true;
    for (int i = 1; i <= 18; i++) {
      char r[8];
      snprintf(r, sizeof(r), "C-%02d", i);
      const char* t = Params::ruleText(r);
      if (!t || strlen(t) < 4) allText = false;
    }
    CHECK(allText, "C-01~C-18 每条规则都有可读说明（文案与文档同源）");
    CHECK(strcmp(Params::getLayerName(), "") != 0, "配置层级名称可读（FR-38a）");
  }

  // -------------------------------------------------------------------------
  SECTION("9. NVS 持久化与 schema 版本守卫");
  {
    Params::resetGroup(GRP_PURR);           // 回到默认，避免受前面用例影响
    Params::set(P_COOLDOWN_DURATION_MS, 45000);
    CHECK(Params::save(), "SAVE 成功写入 NVS");

    Params::set(P_COOLDOWN_DURATION_MS, 600000);
    CHECK(Params::load(), "LOAD 成功读回 NVS");
    CHECK(Params::get(P_COOLDOWN_DURATION_MS) == 45000, "LOAD 恢复了已保存值（断电保持语义）");

    // schema 不匹配：必须忽略旧布局数据，回落硬编码默认值
    prefs.wipe();
    prefs.begin(PARAMS_NVS_NS, false);
    prefs.putUInt("ver", 99);
    int32_t zeros[P_COUNT];
    memset(zeros, 0, sizeof(zeros));
    prefs.putBytes("cfg", zeros, sizeof(zeros));
    Params::init(&prefs);
    CHECK(Params::get(P_COOLDOWN_DURATION_MS) == 90000,
          "schema 不匹配时忽略本地 blob（避免按新布局误读旧数据）");
    CHECK(!Params::hasStoredConfig(), "忽略后报告：未载入本地配置");
  }

  return report("参数体系与校验规则");
}
