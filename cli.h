/*
 * ============================================================================
 *  cli.h —— 串口调试台
 *  对应 PRD v1.1：FR-38（参数全变量化）、FR-38a（三层配置）、FR-38b（写入校验）、
 *                FR-38c（调试通道编译期隔离）
 *
 *  【设计意图】
 *    这是 M1 阶段"调参"的唯一入口：所有能调的东西都在这里调，不需要重烧固件。
 *    参数值改动后立即生效；需要断电保持时执行 SAVE 落盘。
 *
 *  ⚠ FR-38c / RK-23：量产固件把 config.h 的 ENABLE_DEBUG_CHANNEL 置 0，
 *     整个 CLI 被条件编译移除——不留任何运行期后门。
 * ============================================================================
 */

#ifndef GRUNTOY_CLI_H
#define GRUNTOY_CLI_H

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "haptic.h"
#include "mood.h"
#include "tail.h"

class CliConsole {
public:
  void init(HapticEngine* haptic, MoodEngine* mood, TailDriver* tail, Preferences* prefs);
  void poll();                       // 每 loop 调一次；非阻塞逐字节收

private:
  void handleLine(char* line);
  void cmdHelp();
  void cmdInfo();
  void cmdStatus();
  void cmdGet(const char* arg);
  void cmdSet(const char* arg);
  void cmdReset(const char* arg);
  void cmdDump();
  void cmdPreset(const char* arg);
  void cmdTest(const char* arg);
  void printHelpLine(const char* cmd, const char* desc);
  void scheduleFeed(uint16_t n, uint32_t intervalMs);

  HapticEngine* m_haptic = nullptr;
  MoodEngine*   m_mood   = nullptr;
  TailDriver*   m_tail   = nullptr;
  Preferences*  m_prefs  = nullptr;

  char    m_buf[CLI_BUF_SIZE];
  uint8_t m_len = 0;

  // 自动抚摸模拟（t auto / t feed），非阻塞调度
  bool     m_autoRun        = false;
  uint16_t m_autoRemaining  = 0;     // 0 且 m_autoRun → 无限循环
  uint32_t m_autoIntervalMs = 3000;
  uint32_t m_autoNextMs     = 0;
  uint16_t m_autoIdx        = 0;
};

extern CliConsole g_cli;

#endif  // GRUNTOY_CLI_H
