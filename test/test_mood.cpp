/*
 * ============================================================================
 *  test_mood.cpp —— 体感交互状态机时序验证
 *
 *  用可控假时钟推进 millis()，覆盖 PRD §9.5 的完整生命周期：
 *    ACCUM → PURR → LANDING → COOLDOWN → ACCUM
 *  以及 v1.0 补齐能力：呼吸触发链路（含 P-43 节流）、咕噜闸门（P-30/P-31）、
 *  降落曲线（P-16）、PURR 态 W4 锁定、系统态优先级、心情值不持久化。
 * ============================================================================
 */

#include "Arduino.h"
#include "Preferences.h"
#include "params.h"
#include "haptic.h"
#include "mood.h"
#include "tail.h"
#include "config.h"
#include "check.h"

#include <cstdio>

int main() {
  printf("\n===== gruntoy v1.0 体感交互状态机时序验证 =====\n");
  printf("（使用假时钟推进，不依赖真实时间，不需要硬件）\n");

  Preferences prefs;
  Params::init(&prefs);
  Params::applyPreset("standard");

  const int16_t  th        = (int16_t)Params::get(P_PURR_TRIGGER_THRESHOLD);   // 100
  const int16_t  restore   = (int16_t)Params::get(P_MOOD_RESTORE_VALUE);       // 70
  const int16_t  initial   = (int16_t)Params::get(P_MOOD_INITIAL);             // 60
  const uint32_t landing   = (uint32_t)Params::get(P_MOOD_LANDING_MS);         // 5000
  const uint32_t cooldown  = (uint32_t)Params::get(P_COOLDOWN_DURATION_MS);    // 90000

  printf("[参数基线] 初始=%d 阈值=%d 降落终点=%d 降落=%lums 冷却=%lums\n",
         initial, th, restore, (unsigned long)landing, (unsigned long)cooldown);

  // =========================================================================
  SECTION("1. 抚摸累积至触发咕噜");
  {
    MoodEngine mood;
    mood.init(nullptr, nullptr);
    CHECK(mood.getMood() == initial, "初始心情值 = P-01（60）");
    CHECK(mood.getState() == HS_ACCUM, "初始状态为 ACCUM");

    int strokes = 0;
    while (mood.getMood() < th && strokes < 200) {
      advance(4000);            // 间隔 > max(P-05, P-06)，视为有效抚摸
      mood.onStroke();
      strokes++;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "经 %d 次抚摸达到阈值（mood=%d，含连续抚摸加速增益）", strokes, mood.getMood());
    CHECK(mood.getMood() >= th, buf);
    CHECK(strokes <= 25, "达到阈值所需抚摸次数在合理量级（≤25 次，对应 1~3 分钟手感）");
    CHECK(mood.getState() == HS_PURR, "达标后立即进入 PURR（无需外部 tick 驱动）");
    CHECK(mood.getPurrTargetMs() >= 20000, "咕噜时长映射到合理值（≥20s）");
  }

  // =========================================================================
  SECTION("2. 咕噜期间：不叠加、档位锁 W4");
  {
    MoodEngine mood;
    mood.init(nullptr, nullptr);
    mood.forceMood(th - 1);                 // 99
    advance(4000);
    mood.onStroke();                        // +2 → 101 → 触发
    CHECK(mood.getState() == HS_PURR, "触发成功");
    CHECK(mood.getTailGear() == 4, "PURR 态摇尾锁定 W4（FR-39 验收）");

    const int16_t before = mood.getMood();
    const bool counted = mood.onStroke();
    CHECK(!counted, "咕噜期间抚摸返回 false（时长已在触发瞬间锁定，FR-37a）");
    CHECK(mood.getMood() == before, "咕噜期间心情值不变");

    const uint8_t gear = mood.getTailGear();
    advance(10000);
    mood.tick();
    CHECK(mood.getTailGear() == gear, "咕噜期间摇尾档位稳定，不因抚摸或时间漂移");
  }

  // =========================================================================
  SECTION("3. 咕噜结束 → 平滑降落 → 冷却");
  {
    MoodEngine mood;
    mood.init(nullptr, nullptr);
    mood.forceMood(117);                    // 高档位触发，验证跨档降落
    advance(4000);
    mood.onStroke();
    CHECK(mood.getState() == HS_PURR, "高水平触发成功");
    CHECK(mood.getPurrTargetMs() == 60000, "mood=117 落在 b3~b4 平台，时长 = D3 = 60s");

    advance(mood.getPurrTargetMs() + 100);
    mood.tick();
    CHECK(mood.getState() == HS_LANDING, "咕噜结束后进入 LANDING");

    const int16_t v0 = mood.getMood();
    advance(landing / 2);
    mood.tick();
    const int16_t v1 = mood.getMood();
    advance(landing / 2 + 200);
    mood.tick();
    const int16_t v2 = mood.getMood();

    char buf[160];
    snprintf(buf, sizeof(buf), "降落过程 %d → %d → %d（连续下降，无阶跃）", v0, v1, v2);
    CHECK(v1 < v0 && v2 <= v1, buf);
    CHECK(v1 > restore, "降落中点是中间值，不是一步跳到底（FR-40 平滑性）");
    CHECK(v2 == restore, "降落终点精确落到还原值 70（FR-37b）");
    CHECK(mood.getState() == HS_COOLDOWN, "降落完成后进入 COOLDOWN");
  }

  // =========================================================================
  SECTION("4. 降落期间抚摸：不计分，但必须有可感知反馈");
  {
    HapticEngine haptic;
    haptic.init();
    MoodEngine mood;
    mood.init(&haptic, nullptr);

    mood.forceMood(110);
    advance(4000);
    mood.onStroke();
    advance(mood.getPurrTargetMs() + 100);
    mood.tick();
    CHECK(mood.getState() == HS_LANDING, "已进入降落");

    advance(1000);
    mood.tick();
    const int16_t before = mood.getMood();
    const bool counted = mood.onStroke();
    CHECK(!counted, "降落期间抚摸返回 false（FR-37c）");
    CHECK(haptic.getCurrentDutyPct() == (uint8_t)Params::get(P_BREATH_L1_DUTY_PCT),
          "已给出柔震反馈（强度取最轻档 L1，弱于冷却期拒绝震）");
    CHECK(mood.isLandingTickled(), "记录了降落期被抚摸（供观测/埋点）");

    advance(1000);
    mood.tick();
    CHECK(mood.getMood() <= before, "降落期间心情值不因抚摸回升");
  }

  // =========================================================================
  SECTION("5. 冷却期：拒绝并给更重的拒绝震");
  {
    HapticEngine haptic;
    haptic.init();
    MoodEngine mood;
    mood.init(&haptic, nullptr);

    mood.forceMood(105);
    advance(4000);
    mood.onStroke();
    advance(mood.getPurrTargetMs() + 100);
    mood.tick();
    advance(landing + 200);
    mood.tick();
    CHECK(mood.getState() == HS_COOLDOWN, "已进入冷却");
    CHECK(mood.getMood() == restore, "冷却期心情值锁定在 70");

    const bool counted = mood.onStroke();
    CHECK(!counted, "冷却期抚摸返回 false");
    CHECK(haptic.getCurrentDutyPct() == (uint8_t)Params::get(P_BREATH_L2_DUTY_PCT),
          "拒绝震强度高于降落柔震（L2 > L1，体感可区分）");

    advance(5000);
    mood.tick();
    CHECK(mood.getState() == HS_COOLDOWN, "5s 后仍在冷却中（冻结衰减，不受 P-10 影响）");

    advance(cooldown);
    mood.tick();
    CHECK(mood.getState() == HS_ACCUM, "冷却结束后自动回到 ACCUM，无需用户操作");
    CHECK(mood.getMood() == restore, "冷却结束后心情值保持 70（不清零、不惩罚）");
  }

  // =========================================================================
  SECTION("6. 摇尾档位随心情值切换（含 P-50 迟滞）");
  {
    MoodEngine mood;
    mood.init(nullptr, nullptr);
    const int16_t t0 = (int16_t)Params::get(P_TAIL_TIER_B0);   // 54
    const int16_t t1 = (int16_t)Params::get(P_TAIL_TIER_B1);   // 79
    const int16_t t2 = (int16_t)Params::get(P_TAIL_TIER_B2);   // 99

    mood.forceMood(t0 - 5);
    CHECK(mood.getTailGear() == 1, "低心情 → W1");
    mood.forceMood(t1 + 5);
    CHECK(mood.getTailGear() == 2, "中低心情 → W2");
    mood.forceMood(t2 + 5);
    CHECK(mood.getTailGear() == 3, "中高心情 → W3");
    mood.forceMood((int16_t)Params::get(P_MOOD_MAX));
    CHECK(mood.getTailGear() == 4, "满心情 → W4");

    mood.forceMood(t2 + 5 - 1);      // 刚跌破边界但未超迟滞带
    CHECK(mood.getTailGear() == 4, "边界内 1 点抖动不换档（迟滞生效，防 RK-21）");
  }

  // =========================================================================
  SECTION("7. 咕噜时长映射：单调、无断崖、两端可分辨");
  {
    MoodEngine mood;
    mood.init(nullptr, nullptr);

    const uint32_t da = mood.debugPurrDuration(100);
    const uint32_t db = mood.debugPurrDuration(110);
    const uint32_t dc = mood.debugPurrDuration(120);
    char buf[200];
    snprintf(buf, sizeof(buf), "阈值处(100)=%lus，110处=%lus，满值(120)=%lus（严格递增）",
             (unsigned long)(da / 1000), (unsigned long)(db / 1000), (unsigned long)(dc / 1000));
    CHECK(da < db && db < dc, buf);
    snprintf(buf, sizeof(buf), "两端差异 %lus（%.1f 倍），体感可分辨（FR-37a）",
             (unsigned long)((dc - da) / 1000), (double)dc / da);
    CHECK(dc > da * 2, buf);

    const uint32_t span = dc - da;
    uint32_t maxJump = 0;
    int16_t  jumpAt  = 0;
    for (int16_t m = 100; m < 120; m++) {
      const uint32_t x = mood.debugPurrDuration(m), y = mood.debugPurrDuration(m + 1);
      const uint32_t j = (y > x) ? (y - x) : (x - y);
      if (j > maxJump) { maxJump = j; jumpAt = m; }
    }
    snprintf(buf, sizeof(buf), "全程跨度 %lus，单点最大跳变 %lums @mood=%d（上限 %lus，无断崖）",
             (unsigned long)(span / 1000), (unsigned long)maxJump, jumpAt,
             (unsigned long)(span / 10));
    CHECK(maxJump <= span / 10, buf);   // 判定标准：单点跳变 ≤ 全程跨度的 1/10
    CHECK(mood.debugPurrDuration(110) < mood.debugPurrDuration(115), "末段插值区 110~115 仍有变化");

    // 线性模式（P-24）：斜率生效且被上限截断
    const char* e1 = Params::validateSet(P_PURR_DURATION_MODE, PDM_LINEAR);
    CHECK(e1 == nullptr, "切换到 linear 模式通过校验");
    Params::set(P_PURR_DURATION_MODE, PDM_LINEAR);
    const uint32_t l100 = mood.debugPurrDuration(100);
    const uint32_t l105 = mood.debugPurrDuration(105);
    const uint32_t l120 = mood.debugPurrDuration(120);
    snprintf(buf, sizeof(buf), "线性模式：100→%lus 105→%lus 120→%lus（约 4s/点，超上限则截断）",
             (unsigned long)(l100 / 1000), (unsigned long)(l105 / 1000), (unsigned long)(l120 / 1000));
    CHECK(l100 == 20000 && l105 > l100 && l120 == (uint32_t)Params::get(P_PURR_DURATION_MAX_MS), buf);
    Params::set(P_PURR_DURATION_MODE, PDM_TIERED);
    CHECK(mood.debugPurrDuration(100) == da, "切回 tiered 后映射恢复");
  }

  // =========================================================================
  SECTION("8. 自然衰减：30 分钟后开始，落到地板不归零");
  {
    MoodEngine mood;
    mood.init(nullptr, nullptr);
    mood.forceMood(90);
    const int16_t start = mood.getMood();

    advance((uint32_t)Params::get(P_MOOD_DECAY_START_MS) + 1000);
    mood.tick();
    CHECK(mood.getMood() < start, "静置超过 P-10 后心情值开始衰减");

    for (int i = 0; i < 200; i++) {
      advance((uint32_t)Params::get(P_MOOD_DECAY_INTERVAL_MS));
      mood.tick();
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "长期静置后停在 %d（地板 %ld，不归零、不惩罚）",
             mood.getMood(), (long)Params::get(P_MOOD_DECAY_FLOOR));
    CHECK(mood.getMood() == Params::get(P_MOOD_DECAY_FLOOR), buf);
  }

  // =========================================================================
  SECTION("9. 防抖：首次抚摸必响应，过快重复被过滤");
  {
    MoodEngine mood;
    mood.init(nullptr, nullptr);
    mood.forceMood(60);

    advance(5000);
    const bool c1 = mood.onStroke();
    CHECK(c1, "首次抚摸一定计入（开箱即响应）");
    const int16_t v1 = mood.getMood();

    advance(10);
    const bool c2 = mood.onStroke();
    CHECK(!c2, "10ms 内的重复抚摸属于抖动，被过滤");
    CHECK(mood.getMood() == v1, "被过滤的抚摸不改变心情值");
  }

  // =========================================================================
  SECTION("10. 呼吸触发链路（FR-35 / §9.6）与 P-43 节流");
  {
    HapticEngine haptic;
    haptic.init();
    MoodEngine mood;
    mood.init(&haptic, nullptr);

    advance(4000);
    mood.onStroke();                       // mood 62，未达阈值
    CHECK(mood.getState() == HS_ACCUM, "仍处于累积态（离阈值还远）");
    CHECK(haptic.getMode() == HM_BREATH, "抚摸后自动启动呼吸（原 PoC 缺失此链路）");
    CHECK(mood.getBreathCount() == 1, "呼吸计数 +1");

    haptic.stop();                          // 手动结束本次呼吸
    advance(4000);
    mood.onStroke();
    CHECK(mood.getBreathCount() == 1, "距上次呼吸 4s < P-43(30s)，被节流不重启");

    haptic.stop();
    advance(30000);
    mood.onStroke();
    CHECK(mood.getBreathCount() == 2, "累计间隔 > P-43 后重新触发呼吸");
  }

  // =========================================================================
  SECTION("11. 呼吸与咕噜互斥（咕噜优先）");
  {
    HapticEngine haptic;
    haptic.init();
    MoodEngine mood;
    mood.init(&haptic, nullptr);

    mood.forceMood(th - 1);
    advance(4000);
    mood.onStroke();                        // 直接触发咕噜
    CHECK(mood.getState() == HS_PURR, "触发咕噜");
    CHECK(haptic.getMode() == HM_PURR, "震动通道被咕噜占用");
    CHECK(mood.getBreathCount() == 0, "同一次抚摸不会先起呼吸再被咕噜抢占（链路顺序已修正）");

    mood.forceState(HS_LANDING);
    advance(4000);
    mood.onStroke();
    CHECK(mood.getBreathCount() == 0, "降落期不启动呼吸");
  }

  // =========================================================================
  SECTION("12. 咕噜轮次间隔闸门（P-30）");
  {
    MoodEngine mood;
    mood.init(nullptr, nullptr);

    mood.forceMood(th - 1);
    advance(4000);
    mood.onStroke();
    CHECK(mood.getPurrCount() == 1, "第 1 轮咕噜触发");

    advance(mood.getPurrTargetMs() + 100);
    mood.tick();                            // → LANDING
    advance(landing + 200);
    mood.tick();                            // → COOLDOWN，记录 m_lastPurrEndMs

    mood.forceState(HS_ACCUM);
    mood.forceMood(th - 1);
    advance(4000);
    mood.onStroke();
    CHECK(mood.getPurrCount() == 1, "距上一轮结束不足 P-30(15s)，本轮被闸门拦下");
    CHECK(mood.purrGateState() == PG_GAP, "闸门状态报告为 GAP");

    // 累计静默时长超过 P-30 后才能放行。注意此前已推进 4000ms，
    // 因此这里必须补足到 gap 以上，不能按绝对时间想当然取值。
    advance((uint32_t)Params::get(P_PURR_BETWEEN_GAP_MS));
    mood.onStroke();
    CHECK(mood.getPurrCount() == 2, "休息足够后闸门放行，可再次触发");
  }

  // =========================================================================
  SECTION("13. 累计连续运行上限（P-31）");
  {
    MoodEngine mood;
    mood.init(nullptr, nullptr);
    Params::set(P_PURR_CUMULATIVE_LIMIT_MS, 60000);   // 压到最小值便于验证

    mood.forceMood(118);                              // → 单轮 60s
    advance(4000);
    mood.onStroke();
    CHECK(mood.getPurrTargetMs() == 60000, "单轮时长 60s");
    CHECK(mood.getPurrAccumMs() == 60000, "累计运行时长已计入");
    CHECK(mood.isPurrLockedOut(), "达到 P-31 上限后置锁定标记（本轮跑完即强制休息）");
    CHECK(mood.purrGateState() == PG_CUMULATIVE, "闸门状态报告为 CUMULATIVE");

    Params::resetGroup(GRP_PURR);
  }

  // =========================================================================
  SECTION("14. 降落曲线三选一（P-16）");
  {
    MoodEngine mood;
    mood.init(nullptr, nullptr);

    const int16_t from = 120;
    const uint32_t at  = 1250;      // t = 0.25（用 1/4 处取样，三种曲线差异最明显）

    Params::set(P_MOOD_LANDING_CURVE, LC_LINEAR);
    const int16_t vLin = mood.debugLandingValue(from, at);
    Params::set(P_MOOD_LANDING_CURVE, LC_EASE_IN_OUT);
    const int16_t vInOut = mood.debugLandingValue(from, at);
    Params::set(P_MOOD_LANDING_CURVE, LC_EASE_OUT);
    const int16_t vOut = mood.debugLandingValue(from, at);
    Params::set(P_MOOD_LANDING_CURVE, LC_EASE_IN_OUT);

    char buf[180];
    snprintf(buf, sizeof(buf), "t=0.25 取值：ease_out=%d < linear=%d < ease_in_out=%d（三条曲线可区分）",
             vOut, vLin, vInOut);
    CHECK(vOut < vLin && vLin < vInOut, buf);
    CHECK(vOut < from, "ease_out 起步快（前期就明显下落）");
    CHECK(vInOut < from, "ease_in_out 起步慢但仍在下降（缓入缓出）");

    // 分段线性近似段数生效：段数越多越贴近解析曲线
    Params::set(P_MOOD_LANDING_SEGMENTS, 3);
    const int16_t v3 = mood.debugLandingValue(from, at);
    Params::set(P_MOOD_LANDING_SEGMENTS, 20);
    const int16_t v20 = mood.debugLandingValue(from, at);
    Params::set(P_MOOD_LANDING_SEGMENTS, 5);
    snprintf(buf, sizeof(buf), "段数 3 → %d，段数 20 → %d（P-17 生效，段数越多越平滑）", v3, v20);
    CHECK(v20 >= v3, buf);
  }

  // =========================================================================
  SECTION("15. 系统态优先级（PRD §9.5：安全类 > 咕噜 > 呼吸 > 待机）");
  {
    MoodEngine mood;
    mood.init(nullptr, nullptr);
    mood.forceState(HS_LOWPOWER);
    CHECK(!mood.onStroke(), "低电模式下一律不受理抚摸");
    CHECK(mood.purrGateState() == PG_SYSTEM, "闸门状态报告为 SYSTEM（禁止咕噜，保护电量）");
    mood.forceState(HS_UPGRADING);
    CHECK(!mood.onStroke(), "升级中不受理抚摸");
    mood.forceState(HS_ACCUM);
    CHECK(mood.getState() == HS_ACCUM, "退出系统态后回到累积中，计时已重置");
  }

  // =========================================================================
  SECTION("16. 心情值不做掉电持久化（FR-37d）");
  {
    MoodEngine m1;
    m1.init(nullptr, nullptr);
    m1.forceMood(95);
    CHECK(m1.getMood() == 95, "运行中心情值可被推到 95");
    CHECK(Params::findByName("mood_current") == P_COUNT,
          "参数表中不存在「当前心情值」项（心情值不是配置，也不构成养成数据）");

    Params::init(&prefs);                 // 模拟重启
    MoodEngine m2;
    m2.init(nullptr, nullptr);
    char buf[128];
    snprintf(buf, sizeof(buf), "重启后心情值归位初始值 %d（不是 95）", m2.getMood());
    CHECK(m2.getMood() == initial, buf);
  }

  return report("体感交互状态机时序验证");
}
