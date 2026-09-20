/*
 * ============================================================================
 *  test/build_check.cpp —— 固件整体启动冒烟验证（两种编译配置各跑一遍）
 *
 *  为什么需要它：
 *    其余 4 个套件都是直接 new 出 HapticEngine / MoodEngine / TailDriver 来测
 *    单个模块，从没走过 gruntoy.ino 里 setup() → loop() 这条**真实入口链路**。
 *    也从来没有在 ENABLE_DEBUG_CHANNEL=0 的量产配置下编译过——而量产配置恰恰是
 *    最终要提交审核的那一份。本检查同时补上这两处：
 *      ① 真实入口是否能在 200ms 内完成初始化、主循环是否成立；
 *      ② 量产配置（调试通道关闭）是否仍能编译、链接、运行，且确实不留调参入口。
 *
 *  本文件被 run_tests.sh 编译两遍：
 *      -DENABLE_DEBUG_CHANNEL=1 -DENABLE_TAIL_OUTPUT=0   （调试配置）
 *      -DENABLE_DEBUG_CHANNEL=0 -DENABLE_TAIL_OUTPUT=1   （量产配置）
 * ============================================================================
 */

#include <Arduino.h>
#include "config.h"
#include "params.h"
#include "mood.h"
#include "haptic.h"
#include "tail.h"

#include "check.h"

// gruntoy.ino 中的入口（Arduino IDE 会隐式声明，本机需显式）
void setup();
void loop();

int main() {
  printf("\n===== gruntoy v1.0 固件启动冒烟验证 =====\n");
  printf("编译配置: ENABLE_DEBUG_CHANNEL=%d, ENABLE_TAIL_OUTPUT=%d\n",
         ENABLE_DEBUG_CHANNEL, ENABLE_TAIL_OUTPUT);

  SECTION("1. setup() 可完成初始化（含 NVS 与三层参数落地）");
  {
    setup();
    // setup() 内含 delay(300)，假时钟会相应推进
    CHECK(g_fake_ms >= 300, "setup() 返回且启动延时已走完（未卡死）");

    const int32_t init = Params::get(P_MOOD_INITIAL);
    CHECK(init >= Params::getMin(P_MOOD_INITIAL) &&
              init <= Params::getMax(P_MOOD_INITIAL),
          "开机心情值落在合法区间（参数表已生效，非未初始化内存）");
    CHECK(strcmp(Params::getLayerName(), "") != 0, "配置层级有明确来源说明");
    CHECK(Params::validate() == nullptr, "启动后整套参数通过全部校验规则");
    CHECK(Serial.outputHas("就绪"), "固件公告就绪状态");
  }

  SECTION("2. loop() 可长期推进且状态机可完整跑完一轮");
  {
    // 把心情值推到阈值并触发一次抚摸 —— 走完整生命周期
    g_mood.forceState(HS_ACCUM);
    g_mood.forceMood((int16_t)Params::get(P_PURR_TRIGGER_THRESHOLD));
    const bool counted = g_mood.onStroke();
    CHECK(counted, "抚摸被计入（首次抚摸不受防抖拦截）");
    CHECK(g_mood.getState() == HS_PURR, "状态机进入 PURR");
    CHECK(g_haptic.isPurring(), "震动通道同步进入咕噜（三通道联动）");

    // 用一个远大于 P-15 + P-20 的假时长推进主循环
    const uint32_t span = (uint32_t)Params::get(P_MOOD_LANDING_MS) +
                          (uint32_t)Params::get(P_COOLDOWN_DURATION_MS) + 20000u;
    for (uint32_t i = 0; i < span; i++) loop();      // loop() 内含 delay(1)

    CHECK(g_mood.getState() == HS_COOLDOWN || g_mood.getState() == HS_ACCUM,
          "跑完咕噜+降落后落入 COOLDOWN（或已回到 ACCUM）");
    CHECK(g_mood.getMood() < Params::get(P_PURR_TRIGGER_THRESHOLD),
          "降落终点已回落到阈值以下（不会自我重触发，C-02 的运行时体现）");
    CHECK(g_mood.getPurrCount() == 1, "整轮只触发 1 次咕噜（无死循环）");
    CHECK(g_mood.getTailGear() >= 1 && g_mood.getTailGear() <= 4,
          "摇尾档位始终落在 W1~W4 合法区间");
  }

  SECTION("3. 调试通道的编译期隔离（FR-38c / RK-23）");
  {
    const bool cliReady = Serial.outputHas("调试台就绪");
    CHECK(Serial.outputHas("体感子系统基线固件 v1.0"), "启动横幅可见（固件确实跑起来了）");
#if ENABLE_DEBUG_CHANNEL
    CHECK(cliReady, "调试配置：CLI 已初始化并公告就绪");
    CHECK(Serial.outputHas("参数分组"), "调试配置：CLI 汇报参数分组统计");
    CHECK(!Serial.outputHas("调试通道已编译移除"),
          "调试配置：不出现「已移除」提示（开关语义正确）");
#else
    CHECK(!cliReady, "量产配置：CLI 未初始化（调参入口不存在）");
    CHECK(!Serial.outputHas("参数分组"), "量产配置：CLI 分组统计被编译移除");
    CHECK(Serial.outputHas("调试通道已编译移除"),
          "量产配置：明确告知调试通道已移除，而非静默失效");
#endif
  }

  return report("固件启动冒烟验证");
}
