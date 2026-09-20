/*
 * ============================================================================
 *  test_cli.cpp —— 串口调试台验证
 *
 *  原 PoC 的 run_tests.sh 显式排除了 cli.cpp（"依赖 Serial 输入流"）。
 *  v1.0 补齐了可注入的串口桩，因此 CLI 的解析、校验拦截、枚举取值、
 *  非阻塞抚摸模拟都被纳入本机验证范围——它是 M1 现场唯一的调参入口，
 *  不能只靠"上手试"来保证可用。
 * ============================================================================
 */

#include "Arduino.h"
#include "Preferences.h"
#include "params.h"
#include "haptic.h"
#include "mood.h"
#include "tail.h"
#include "cli.h"
#include "config.h"
#include "check.h"

#include <cstdio>

// 送一条命令并让 CLI 处理完
static void sendCommand(const char* line) {
  Serial.feed(line);
  Serial.feed("\n");
  g_cli.poll();
}

static bool expectOutput(const char* needle, const char* what) {
  const bool ok = Serial.outputHas(needle);
  CHECK(ok, what);
  return ok;
}

int main() {
  printf("\n===== gruntoy v1.0 串口调试台验证 =====\n");

  Preferences prefs;
  Params::init(&prefs);
  Params::applyPreset("standard");

  g_haptic.init();
  g_tail.init();
  g_mood.init(&g_haptic, &g_tail);
  g_cli.init(&g_haptic, &g_mood, &g_tail, &prefs);

  // =========================================================================
  SECTION("1. 帮助与固件信息");
  {
    Serial.clearOutput();
    sendCommand("help");
    expectOutput("gruntoy v1.0 调试台", "help 输出固件标识");
    expectOutput("t sweep", "help 列出扫频命令");
    expectOutput("PRESET", "help 列出性格预设命令");

    Serial.clearOutput();
    sendCommand("info");
    expectOutput("参数项数", "info 输出参数项数");
    expectOutput("ENABLE_DEBUG_CHANNEL", "info 暴露调试通道编译开关（FR-38c 可核查）");
    expectOutput("ENABLE_TAIL_OUTPUT", "info 暴露摇尾输出编译开关");
  }

  // =========================================================================
  SECTION("2. 参数查询");
  {
    Serial.clearOutput();
    sendCommand("GET purr.*");
    expectOutput("purr_trigger_threshold", "GET <group>.* 列出整组参数");
    expectOutput("purr_drive_freq_hz", "分组列表包含驱动频率");

    Serial.clearOutput();
    sendCommand("GET purr_trigger_threshold");
    expectOutput("触发咕噜的心情值阈值", "GET <param> 输出说明文案");
    expectOutput("100", "GET <param> 输出当前值");

    Serial.clearOutput();
    sendCommand("GET no_such_param");
    expectOutput("无此参数", "未知参数名给出明确提示");
  }

  // =========================================================================
  SECTION("3. 写入校验（FR-38b：非法写入必须被拦截并告知规则号）");
  {
    Serial.clearOutput();
    sendCommand("SET mood_restore_value 100");
    expectOutput("C-02", "restore=100 被拦截并报出 C-02");
    expectOutput("当前值保持", "拒绝时明确告知现值未被改动");
    CHECK(Params::get(P_MOOD_RESTORE_VALUE) == 70, "参数实际未被写入（真正回滚）");

    Serial.clearOutput();
    sendCommand("SET mood_restore_value 70");
    expectOutput("✓", "合法写入成功");

    Serial.clearOutput();
    sendCommand("SET mood_landing_ms 9000");
    expectOutput("C-16", "landing=9000 被拦截并报出 C-16");

    Serial.clearOutput();
    sendCommand("SET breath_trough_duty_pct 20");
    expectOutput("C-18", "波谷 20% 被拦截并报出 C-18");

    Serial.clearOutput();
    sendCommand("SET purr_trigger_threshold 105");
    expectOutput("X-01", "抬阈值不同步分段点被拦截并报出 X-01");
  }

  // =========================================================================
  SECTION("4. 枚举取值按名写入");
  {
    Serial.clearOutput();
    sendCommand("SET mood_landing_curve ease_out");
    expectOutput("✓", "枚举名 ease_out 可写入");

    Serial.clearOutput();
    sendCommand("GET mood_landing_curve");
    expectOutput("可选", "枚举型参数展示可选值列表");

    Serial.clearOutput();
    sendCommand("SET mood_landing_curve no_such_curve");
    expectOutput("取值非法", "非法枚举名被拒绝");

    Serial.clearOutput();
    sendCommand("SET mood_landing_curve ease_in_out");
    expectOutput("✓", "恢复默认曲线");
    CHECK(Params::get(P_MOOD_LANDING_CURVE) == LC_EASE_IN_OUT, "曲线参数回到 ease_in_out");
  }

  // =========================================================================
  SECTION("5. 导出与预设");
  {
    Serial.clearOutput();
    sendCommand("DUMP");
    expectOutput("PARAM_DUMP_BEGIN", "DUMP 有明确的起止标记（便于脚本抓取）");
    expectOutput("name,group,value,min,max", "DUMP 输出 CSV 表头");
    expectOutput("PARAM_DUMP_END", "DUMP 有结束标记");
    char buf[64];
    snprintf(buf, sizeof(buf), "# 共 %d 项", (int)P_COUNT);
    expectOutput(buf, "DUMP 报告项数，可与参数表核对");

    Serial.clearOutput();
    sendCommand("PRESET lively");
    expectOutput("lively", "预设切换成功");
    CHECK(Params::get(P_CHAR_PRESET) == 2, "预设序号已更新（P-58）");
    CHECK(Params::get(P_PURR_TIER_B0) == Params::get(P_PURR_TRIGGER_THRESHOLD),
          "预设切换后映射起点与阈值保持一致（X-01）");

    Serial.clearOutput();
    sendCommand("PRESET no_such_preset");
    expectOutput("无此预设", "非法预设名被拒绝");
  }

  // =========================================================================
  SECTION("6. 体感测试命令");
  {
    Serial.clearOutput();
    sendCommand("t breath 3");
    expectOutput("呼吸启动 L3", "呼吸测试命令可用");
    CHECK(g_haptic.getMode() == HM_BREATH, "震动通道确实进入 BREATH");

    g_haptic.stop();

    Serial.clearOutput();
    sendCommand("t tail 4");
    expectOutput("W4", "摇尾档位命令可用");
    CHECK(g_tail.getGear() == 4, "摇尾目标档位 = W4");

    Serial.clearOutput();
    sendCommand("t mood 118");
    expectOutput("force mood=118", "心情值直写可用");
    CHECK(g_mood.getMood() == 118, "心情值已更新");

    Serial.clearOutput();
    sendCommand("t state cooldown");
    expectOutput("COOLDOWN", "状态强制切换可用");
    CHECK(g_mood.getState() == HS_COOLDOWN, "状态机确实进入 COOLDOWN");

    Serial.clearOutput();
    sendCommand("t gate 45");
    expectOutput("45Hz", "门控频率快捷设置可用（Day5 找猫感区间）");
    CHECK(Params::get(P_PURR_DRIVE_FREQ_HZ) == 4500, "P-32 已更新为 45.00Hz");

    Serial.clearOutput();
    sendCommand("t gate 99");
    expectOutput("C-09", "越界门控频率被 C-09 拦截");

    Serial.clearOutput();
    sendCommand("t bare 37");
    expectOutput("duty=37%", "裸测命令可用");
    CHECK(g_haptic.getCurrentDutyPct() == 37, "裸测占空比已输出");
    g_haptic.stop();

    Serial.clearOutput();
    sendCommand("t trough 20");
    expectOutput("波谷占空比 = 20%", "波谷 A/B 对比命令可用");
    CHECK(Params::get(P_BREATH_TROUGH_DUTY_PCT) == 20,
          "波谷参数被临时改写（绕开 C-18，仅用于实验）");
    g_haptic.stop();
    Serial.clearOutput();
    sendCommand("t trough 0");
    CHECK(Params::get(P_BREATH_TROUGH_DUTY_PCT) == 0, "波谷参数可恢复为 0");
    g_haptic.stop();

    Serial.clearOutput();
    sendCommand("t cycle");
    expectOutput("完整生命周期", "生命周期演示命令可用");

    Serial.clearOutput();
    sendCommand("t bogus");
    expectOutput("未知 t 子命令", "未知子命令给出明确提示");
  }

  // =========================================================================
  SECTION("7. 非阻塞抚摸模拟（t feed / t auto）");
  {
    Params::applyPreset("standard");
    Params::resetGroup(GRP_MOOD);
    Params::resetGroup(GRP_PURR);
    Params::applyPreset("standard");
    g_mood.resetState();

    Serial.clearOutput();
    sendCommand("t feed 5 4000");
    expectOutput("开始模拟 5 次抚摸", "t feed 接受次数与间隔参数");

    // 推进时钟并驱动 loop（模拟真实主循环），期间状态机必须能正常推进
    const uint16_t before = g_mood.getCountedStrokes();
    for (int i = 0; i < 200; i++) {
      advance(100);
      g_cli.poll();
      g_haptic.tick();
      g_mood.tick();
      g_tail.tick();
    }
    const uint16_t after = g_mood.getCountedStrokes();
    char buf[160];
    snprintf(buf, sizeof(buf), "非阻塞调度完成 5 次抚摸（计入 %d 次，馈入前 %d 次）",
             after - before, before);
    CHECK(after - before == 5, buf);
    expectOutput("完成 5 次抚摸", "t feed 结束时打印汇总");

    // 关键：feed 期间状态机仍能推进（原 PoC 用 delay() 会阻塞状态机）
    CHECK(g_mood.getCountedStrokes() >= 5, "抚摸确实被计入（间隔未被防抖过滤）");

    Serial.clearOutput();
    sendCommand("t auto 200");
    expectOutput("自动抚摸循环启动", "t auto 启动");
    for (int i = 0; i < 50; i++) {
      advance(100);
      g_cli.poll();
      g_haptic.tick();
      g_mood.tick();
      g_tail.tick();
    }
    CHECK(g_mood.getCountedStrokes() > after, "自动循环持续产生抚摸事件");

    Serial.clearOutput();
    sendCommand("t stop");
    expectOutput("自动循环已停止", "t stop 停止循环");
    CHECK(g_haptic.getMode() == HM_IDLE, "停止后震动通道关闭");
  }

  // =========================================================================
  SECTION("8. 状态快照与容错");
  {
    Serial.clearOutput();
    sendCommand("status");
    expectOutput("状态快照", "status 输出快照标题");
    expectOutput("心情值", "快照含心情值");
    expectOutput("咕噜闸门", "快照含闸门状态（v1.0 新增观测项）");
    expectOutput("累计咕噜", "快照含咕噜/呼吸计数");

    Serial.clearOutput();
    sendCommand("FOOBAR");
    expectOutput("未知命令", "未知命令给出明确提示");

    Serial.clearOutput();
    sendCommand("   ");
    CHECK(Serial.output().empty(), "空行不产生输出（不会刷屏）");
  }

  return report("串口调试台验证");
}
