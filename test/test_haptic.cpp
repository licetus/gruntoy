/*
 * ============================================================================
 *  test_haptic.cpp —— 体感引擎（呼吸包络 / 咕噜门控 / 淡入淡出 / 扫频）验证
 *
 *  这是 v1.0 相对原 PoC 增补的关键测试：
 *  原 PoC 的驱动频率 P-32 只被读进变量、从未落到输出，导致「扫频找猫感区间」
 *  这项 M1 核心实验在物理上不产生任何差异。本套件断言门控调制确实生效。
 * ============================================================================
 */

#include "Arduino.h"
#include "Preferences.h"
#include "params.h"
#include "haptic.h"
#include "config.h"
#include "check.h"

#include <cstdio>

// 采集 [fromMs, toMs] 窗口内写出的占空比记录
static void collectLedc(uint8_t channel, uint32_t fromMs, uint32_t toMs,
                        std::vector<uint32_t>& out) {
  out.clear();
  for (const LedcWriteRecord& r : g_ledcWrites) {
    if (r.channel != channel) continue;
    if (r.atMs < fromMs || r.atMs > toMs) continue;
    out.push_back(r.duty);
  }
}

static double avgOf(const std::vector<uint32_t>& v) {
  if (v.empty()) return 0.0;
  double s = 0;
  for (uint32_t x : v) s += x;
  return s / (double)v.size();
}

int main() {
  printf("\n===== gruntoy v1.0 体感引擎验证 =====\n");

  Preferences prefs;
  Params::init(&prefs);
  Params::applyPreset("standard");

  const uint32_t PWM_MAX = (1u << MOTOR_PWM_RES) - 1u;   // 1023

  // =========================================================================
  SECTION("1. 呼吸包络：谷底 0%、峰值达标、曲线平滑、周期在目标区间");
  {
    HapticEngine h;
    h.init();
    // 关掉随机抖动，便于精确断言
    Params::set(P_BREATH_PERIOD_JITTER_PCT, 0);
    Params::set(P_BREATH_AMPLITUDE_JITTER_PCT, 0);

    const uint32_t period = (uint32_t)Params::get(P_BREATH_PERIOD_MS);
    const uint32_t peak   = (uint32_t)Params::get(P_BREATH_L2_DUTY_PCT);

    g_ledcWrites.clear();
    const uint32_t t0 = g_fake_ms;
    h.startBreath(2);
    runFor(period, 5, [&]() { h.tick(); });
    const uint32_t t1 = g_fake_ms;

    std::vector<uint32_t> v;
    collectLedc(MOTOR_PWM_CHANNEL, t0, t1, v);
    CHECK(!v.empty(), "呼吸期间确实写入了 PWM 值");

    uint32_t mn = 0xFFFFFFFFu, mx = 0;
    for (uint32_t d : v) { if (d < mn) mn = d; if (d > mx) mx = d; }

    char buf[180];
    snprintf(buf, sizeof(buf), "波谷占空比 = %.1f%%（必须为 0，完全断电 —— C-18 的物理前提）",
             100.0 * mn / PWM_MAX);
    CHECK(mn == 0, buf);

    snprintf(buf, sizeof(buf), "峰值占空比 = %.1f%%（L2 设定 %lu%%）",
             100.0 * mx / PWM_MAX, (unsigned long)peak);
    CHECK(fabs(100.0 * mx / PWM_MAX - (double)peak) < 1.0, buf);

    // 平滑性：包络是正弦，相邻采样点差值应很小
    double maxStep = 0;
    for (size_t i = 1; i < v.size(); i++) {
      const double s = fabs((double)v[i] - (double)v[i - 1]);
      if (s > maxStep) maxStep = s;
    }
    snprintf(buf, sizeof(buf), "相邻 5ms 采样最大步进 %.2f%%（曲线平滑，无阶跃）",
             100.0 * maxStep / PWM_MAX);
    CHECK(maxStep < 0.05 * PWM_MAX, buf);

    // 中间值样本数应占多数（证明是连续包络而不是通断方波）
    int mid = 0;
    for (uint32_t d : v) if (d > 0 && d < PWM_MAX) mid++;
    snprintf(buf, sizeof(buf), "中间值样本 %d / %d（呼吸走模拟包络，不使用门控调制）",
             mid, (int)v.size());
    CHECK(mid > (int)v.size() * 8 / 10, buf);

    snprintf(buf, sizeof(buf), "呼吸周期 %lums 落在 3000~5000ms 目标区间（FR-35）",
             (unsigned long)period);
    CHECK(period >= 3000 && period <= 5000, buf);

    // 单次触发总时长 = 周期 × P-42，到点自动结束
    h.stop();
    h.startBreath(2);
    const uint32_t total = (uint32_t)Params::get(P_BREATH_PERIOD_MS) *
                           (uint32_t)Params::get(P_BREATH_CYCLES_PER_TRIGGER);
    runFor(total - 200, 50, [&]() { h.tick(); });
    CHECK(h.getMode() == HM_BREATH, "总时长到达前仍处于呼吸状态");
    runFor(400, 50, [&]() { h.tick(); });
    CHECK(h.getMode() == HM_IDLE, "呼吸在 周期×P-42 之后自行结束（非阻塞，无需外部停止）");
  }

  // =========================================================================
  SECTION("2. 咕噜门控调制：P-32 确实落到输出（这是原 PoC 的缺口）");
  {
    HapticEngine h;
    h.init();
    Params::set(P_PURR_DUTY_JITTER_PCT, 0);        // 关闭强度抖动，便于断言平均值
    Params::set(P_PURR_DRIVE_FREQ_HZ, 3000);       // 30Hz

    g_ledcWrites.clear();
    const uint32_t t0 = g_fake_ms;
    h.startPurr(20000);

    // ⚠ getDriveFreqHz() 只在咕噜/扫频期间有意义，咕噜结束后返回 0，
    //   因此必须在咕噜仍进行时断言，不能等整段跑完再查。
    CHECK(h.getDriveFreqHz() == 30, "驱动频率 = P-32/100 = 30Hz（咕噜进行中）");

    runFor(18500, 5, [&]() { h.tick(); });   // 停在 20s 之前，保持 HM_PURR
    CHECK(h.getMode() == HM_PURR, "18500ms 时仍处于咕噜状态（未提前结束）");
    CHECK(h.getPurrCount() == 1, "咕噜计数 +1");

    std::vector<uint32_t> v;
    // 取中段（避开淡入淡出）
    collectLedc(MOTOR_PWM_CHANNEL, t0 + 1000, g_fake_ms - 1000, v);

    bool binaryOnly = true;
    for (uint32_t d : v) if (d != 0 && d != PWM_MAX) { binaryOnly = false; break; }
    char buf[200];
    snprintf(buf, sizeof(buf),
             "门控输出仅含「断开 0%%」与「导通 100%%」两种状态（%d 个样本，证明按 %luHz 切脉冲）",
             (int)v.size(), (unsigned long)h.getDriveFreqHz());
    CHECK(binaryOnly, buf);

    const double avgPct = 100.0 * avgOf(v) / PWM_MAX;
    const double base = (double)Params::get(P_PURR_DUTY_BASE_PCT);
    snprintf(buf, sizeof(buf), "门控后平均占空比 %.1f%%（基础占空比 %.0f%%，长包络 ±15%% 内波动）",
             avgPct, base);
    CHECK(avgPct > base * 0.75 && avgPct < base * 1.25, buf);

    // 改变门控频率后，输出节奏应随之改变（同样时长内通断次数变化）
    g_ledcWrites.clear();
    Params::set(P_PURR_DRIVE_FREQ_HZ, 5000);       // 50Hz
    const uint32_t t2 = g_fake_ms;
    h.startPurr(5000);
    runFor(5000, 1, [&]() { h.tick(); });
    std::vector<uint32_t> v50;
    collectLedc(MOTOR_PWM_CHANNEL, t2 + 1000, g_fake_ms, v50);

    int transitions50 = 0;
    for (size_t i = 1; i < v50.size(); i++) if (v50[i] != v50[i - 1]) transitions50++;

    g_ledcWrites.clear();
    Params::set(P_PURR_DRIVE_FREQ_HZ, 2000);       // 20Hz
    const uint32_t t3 = g_fake_ms;
    h.startPurr(5000);
    runFor(5000, 1, [&]() { h.tick(); });
    std::vector<uint32_t> v20;
    collectLedc(MOTOR_PWM_CHANNEL, t3 + 1000, g_fake_ms, v20);

    int transitions20 = 0;
    for (size_t i = 1; i < v20.size(); i++) if (v20[i] != v20[i - 1]) transitions20++;

    snprintf(buf, sizeof(buf), "50Hz 通断切换 %d 次 > 20Hz 的 %d 次（频率参数真实生效）",
             transitions50, transitions20);
    CHECK(transitions50 > transitions20, buf);

    Params::resetGroup(GRP_PURR);
    Params::set(P_PURR_DUTY_JITTER_PCT, 0);
    Params::set(P_PURR_DRIVE_FREQ_HZ, 3000);
  }

  // =========================================================================
  SECTION("3. 咕噜淡入 / 淡出（FR-36⑧：禁止骤起骤停）");
  {
    HapticEngine h;
    h.init();
    Params::set(P_PURR_DUTY_JITTER_PCT, 0);

    g_ledcWrites.clear();
    const uint32_t t0 = g_fake_ms;
    h.startPurr(20000);
    runFor(20000, 5, [&]() { h.tick(); });

    std::vector<uint32_t> head, mid, tail;
    collectLedc(MOTOR_PWM_CHANNEL, t0, t0 + 400, head);                 // 淡入窗口前段
    collectLedc(MOTOR_PWM_CHANNEL, t0 + 1000, t0 + 15000, mid);        // 稳定中段
    collectLedc(MOTOR_PWM_CHANNEL, g_fake_ms - 400, g_fake_ms, tail);  // 淡出窗口末段

    const double aHead = 100.0 * avgOf(head) / PWM_MAX;
    const double aMid  = 100.0 * avgOf(mid)  / PWM_MAX;
    const double aTail = 100.0 * avgOf(tail) / PWM_MAX;

    char buf[200];
    snprintf(buf, sizeof(buf), "淡入前 400ms 平均 %.1f%% 明显低于中段 %.1f%%", aHead, aMid);
    CHECK(aHead < aMid * 0.7, buf);
    snprintf(buf, sizeof(buf), "淡出末 400ms 平均 %.1f%% 明显低于中段 %.1f%%", aTail, aMid);
    CHECK(aTail < aMid * 0.7, buf);

    Params::resetGroup(GRP_PURR);
  }

  // =========================================================================
  SECTION("4. 时长截断与保护（P-29 / 下限）");
  {
    HapticEngine h;
    h.init();
    h.startPurr(999999);
    CHECK(h.getPurrTargetMs() == (uint32_t)Params::get(P_PURR_DURATION_MAX_MS),
          "超过 P-29 的时长被截断到上限");
    h.startPurr(10);
    CHECK(h.getPurrTargetMs() == 1000, "过短时长被抬到 1s 下限，避免无意义的瞬震");
  }

  // =========================================================================
  SECTION("5. 互斥与优先级（咕噜 > 呼吸 > 待机；提示震可打断呼吸）");
  {
    HapticEngine h;
    h.init();

    CHECK(h.startPurr(5000), "咕噜启动");
    CHECK(!h.startBreath(2), "咕噜期间呼吸被拒绝（互斥，FR-35⑧）");
    CHECK(!h.startBuzz(200, 30), "咕噜期间提示震被拒绝（不打断咕噜）");
    CHECK(!h.startSweep(), "咕噜期间扫频被拒绝");
    CHECK(h.getMode() == HM_PURR, "模式仍为 PURR");

    h.stop();
    CHECK(h.getCurrentDutyPct() == 0, "stop() 后占空比归 0");

    CHECK(h.startBreath(2), "空闲时呼吸启动");
    CHECK(h.startBuzz(200, 30), "呼吸期间提示震可打断（降落柔震/冷却拒绝震依赖此行为）");
    CHECK(h.getMode() == HM_BARE, "提示震使用短震通道");

    h.stop();
    CHECK(h.startSweep(), "空闲时扫频启动");
    CHECK(!h.startBreath(2), "扫频期间呼吸被拒绝");
  }

  // =========================================================================
  SECTION("6. 扫频推进与自动收尾（Day5 核心实验的载体）");
  {
    HapticEngine h;
    h.init();
    Params::set(P_PURR_DRIVE_FREQ_HZ, 3000);

    g_ledcWrites.clear();
    CHECK(h.startSweep(), "扫频启动");
    CHECK(h.getSweepFreqHz() == SWEEP_START_HZ, "起始频点 = 25Hz");
    CHECK(h.getDriveFreqHz() == SWEEP_START_HZ, "门控频率跟随扫频当前频点");

    runFor((uint32_t)SWEEP_DWELL_MS, 50, [&]() { h.tick(); });
    char buf[128];
    snprintf(buf, sizeof(buf), "停留 %dms 后推进到第 2 个频点 %luHz",
             SWEEP_DWELL_MS, (unsigned long)h.getSweepFreqHz());
    CHECK(h.getSweepFreqHz() == SWEEP_START_HZ + SWEEP_STEP_HZ, buf);

    runFor((uint32_t)SWEEP_DWELL_MS * (SWEEP_POINTS + 1), 50, [&]() { h.tick(); });
    CHECK(h.getMode() == HM_IDLE, "遍历完全部频点后自动结束");
  }

  // =========================================================================
  SECTION("7. 裸测与占空比直写（Day2 基线实验）");
  {
    HapticEngine h;
    h.init();

    h.startBare(37);
    CHECK(h.getMode() == HM_BARE, "裸测模式");
    CHECK(h.getCurrentDutyPct() == 37, "裸测占空比按输入直接输出（不走门控）");
    CHECK(h.getDriveFreqHz() == 0, "裸测不属于门控模式，驱动频率为 0");

    runFor(1000, 10, [&]() { h.tick(); });
    CHECK(h.getMode() == HM_BARE, "长转模式不会自行结束（需 t barestop）");

    h.setDutyDirect(0);
    CHECK(h.getCurrentDutyPct() == 0, "直写占空比 0 生效");
    h.stop();
  }

  return report("体感引擎验证");
}
