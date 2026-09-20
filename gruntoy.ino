/*
 * ============================================================================
 *  gruntoy.ino —— 毛绒电子宠物 · 体感子系统基线固件 v1.0
 *
 *  用途
 *    把 PRD v1.1 的「呼吸 / 咕噜 / 心情值 / 摇尾分级 / 平滑过渡」五项机制，
 *    以及支撑它们的参数化能力（FR-35~FR-40 + 附录 C），落地为一套可独立运行、
 *    可本机验证、可烧录验证的基线固件。硬件部分刻意只保留"一颗振动马达 + 一个
 *    ESP32"，因为本模块的设计约束是：用控制逻辑的丰富度替代硬件的堆叠。
 *
 *  硬件接线（详见 README.md §3）
 *    振动马达 +  →  DRV8833 OUT1（或 MOSFET 漏极）
 *    振动马达 -  →  DRV8833 OUT2（或 GND）
 *    驱动输入    →  GPIO25（PWM，经 H 桥/MOSFET，切勿 GPIO 直驱）
 *    触摸电极    →  GPIO4 / GPIO15 / GPIO27（铜箔贴内衬塑料壳外壁）
 *
 *  两步验证
 *    ① 本机（不需要硬件、不需要 Arduino 环境）：bash test/run_tests.sh
 *    ② 烧录：见 README.md §4
 *
 *  串口命令（115200，输入 help 查看全部）
 *    info / status / GET <param> / SET <param> <v> / DUMP / PRESET / SAVE
 *    t breath | purr | sweep | bare | trough | gate | feed | auto | mood
 *    t state | tail | cycle | stop
 *
 *  版本：v1.0
 * ============================================================================
 */

#include <Arduino.h>
#include <Preferences.h>

#include "config.h"
#include "params.h"
#include "haptic.h"
#include "mood.h"
#include "tail.h"
#include "cli.h"

// ---------------------------------------------------------------------------
// 全局对象
//   各对象的定义分别在对应的 .cpp 内（haptic.cpp / mood.cpp / tail.cpp / cli.cpp）。
//   本文件只持有 Preferences（NVS 句柄），避免链接期 multiple definition。
// ---------------------------------------------------------------------------
Preferences g_prefs;

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);

  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F("  毛绒电子宠物 · 体感子系统基线固件 v1.0"));
  Serial.println(F("  gruntoy / vibration haptic baseline"));
  Serial.println(F("=============================================="));
  Serial.println(F("输入 help 查看命令；t cycle 跑一次完整生命周期"));
  Serial.println();

  // 第 1 层参数系统：硬编码默认值 → 本地 NVS 覆盖
  Params::init(&g_prefs);

  // 第 2 层：执行器与状态机
  g_haptic.init();
  g_tail.init();
  g_mood.init(&g_haptic, &g_tail);

  // 第 3 层：调试台（编译期可移除）
#if ENABLE_DEBUG_CHANNEL
  g_cli.init(&g_haptic, &g_mood, &g_tail, &g_prefs);
#else
  Serial.println(F("[SYS] 调试通道已编译移除（ENABLE_DEBUG_CHANNEL=0）"));
#endif

  Serial.printf("[SYS] 参数配置层级: %s\n", Params::getLayerName());
  Serial.printf("[SYS] 用户可见体感选项: %s\n", Params::getPresetName());
  Serial.println(F("[SYS] 就绪。"));
  Serial.println();
}

// ---------------------------------------------------------------------------
void loop() {
  // 完全非阻塞：三件事各推进一小步
  g_cli.poll();       // 串口命令 + 自动抚摸调度
  g_haptic.tick();    // 震动包络与门控调制
  g_mood.tick();      // 心情状态机
  g_tail.tick();      // 摇尾过渡与输出

  delay(1);           // 让出 CPU，降低空转功耗
}
