/*
 * ============================================================================
 *  config.h —— 硬件与编译期配置
 *  gruntoy v1.0（体感子系统基线固件）
 *
 *  本文件是唯一允许出现"硬件相关常量"的地方。体感行为参数一律走
 *  params.cpp 的 META 表（FR-38 落地要求），不在代码里写死。
 * ============================================================================
 */

#ifndef GRUNTOY_CONFIG_H
#define GRUNTOY_CONFIG_H

#include <Arduino.h>

// ---------------------------------------------------------------------------
// 一、硬件引脚
// ---------------------------------------------------------------------------

// 振动马达 PWM 输出脚
// ⚠ 必须经 MOSFET 或 H 桥驱动模块，切勿直驱！
//    ESP32 单脚最大 40mA，震动马达启动浪涌 90mA 以上，直连必然烧引脚。
//    推荐接线：GPIO25 → 100Ω → MOSFET 栅极(AO3400/IRLML6344)
//              马达正极 → VCC(3.7~5V)，马达负极 → MOSFET 漏极
//              MOSFET 源极 → GND，栅极与源极之间加 10kΩ 下拉
#define MOTOR_PWM_PIN        25

// 备用 PWM 脚（GPIO25 被占用时改用）
#define MOTOR_PWM_PIN_ALT    26

// 摇尾通道输出脚（仅当 ENABLE_TAIL_OUTPUT = 1 时使用）
#define TAIL_PWM_PIN         18

// 电容触摸引脚（ESP32 内置触摸；铜箔电极贴内衬塑料壳外壁，毛绒只做装饰覆盖）
#define TOUCH_PIN_HEAD        4    // TOUCH0
#define TOUCH_PIN_BACK       15    // TOUCH3
#define TOUCH_PIN_TAIL       27    // TOUCH7

// 触摸判定阈值（需实测校准；ESP32 触摸读数与触摸程度负相关）
#define TOUCH_THRESHOLD      30
#define TOUCH_SAMPLE_INTERVAL_MS  50

// 板载 LED（状态指示）
#define STATUS_LED_PIN        2

// ---------------------------------------------------------------------------
// 二、PWM 参数
// ---------------------------------------------------------------------------
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define USE_LEDC_PWM 1      // Core 3.x：ledcAttach(pin, freq, res)
#else
  #define USE_LEDC_PWM 0      // Core 2.x：ledcSetup(channel,...) + ledcAttachPin
#endif

#define MOTOR_PWM_CHANNEL    0      // 仅 Core 2.x 使用
#define MOTOR_PWM_FREQ       20000  // 20kHz：高于人耳敏感区，抑制 PWM 载波啸叫
#define MOTOR_PWM_RES        10     // 10 位 → 0~1023
#define TAIL_PWM_CHANNEL     1      // 仅 Core 2.x 使用
#define TAIL_PWM_FREQ        50     // 摇尾驱动频率（伺服/电机由机构选型决定）
#define TAIL_PWM_RES         10

// ---------------------------------------------------------------------------
// 三、编译期功能开关
//   两个开关都用 #ifndef 包裹：既接受这里的默认值，也接受编译命令行
//   （-DENABLE_DEBUG_CHANNEL=0）覆盖。这样 CI 才能把"量产配置"和"调试配置"
//   两种形态都编译一遍——否则量产配置要等到真正提交审核前才会首次被验证。
// ---------------------------------------------------------------------------

// 调试通道（FR-38c / RK-23）：正式固件必须编译期关闭，不留运行期后门。
// 置 0 后 CLI 的实现体被条件编译移除（仅保留空的 init/poll 以满足链接）。
#ifndef ENABLE_DEBUG_CHANNEL
#define ENABLE_DEBUG_CHANNEL 1
#endif

// 摇尾通道物理输出。
//   0 = 仅计算档位/频率/幅度并打印（默认）——摇尾机构选型属另一份执行清单，
//       本基线不接机构，避免越界。
//   1 = 通过 TAIL_PWM_PIN 输出（需先完成机构选型与限位标定）。
#ifndef ENABLE_TAIL_OUTPUT
#define ENABLE_TAIL_OUTPUT   0
#endif

// 串口
#define SERIAL_BAUD          115200
#define CLI_BUF_SIZE         96

// ---------------------------------------------------------------------------
// 四、PoC 测试参数（扫频 / 谷底对比）
// ---------------------------------------------------------------------------
#define SWEEP_START_HZ       25     // 扫频起始频率
#define SWEEP_END_HZ         50     // 扫频结束频率
#define SWEEP_STEP_HZ        5      // 步进：25/30/35/40/45/50
#define SWEEP_POINTS         6      // 频点数量
#define SWEEP_DWELL_MS       10000  // 每频点停留 10s（够评分 + 测噪声）

// ---------------------------------------------------------------------------
// 五、NVS 参数布局版本
//  参数表结构变化（增删项）时必须 +1，否则旧 blob 会被按新布局误读。
//  见 params.cpp 的 save()/load()。
// ---------------------------------------------------------------------------
#define PARAMS_NVS_SCHEMA    2
#define PARAMS_NVS_NS        "gruntoy"

#endif  // GRUNTOY_CONFIG_H
