/*
 * ============================================================================
 *  tail.h —— 摇尾通道（档位 → 频率/幅度 → 平滑过渡 → 输出）
 *  对应 PRD v1.1：FR-39 / FR-39a（摇尾分级与并存）、FR-40a（三通道同步过渡）
 *
 *  【职责边界】
 *    本模块只负责"把心情值档位翻译成一路频率+幅度，并保证切换平滑"。
 *    它不负责机构选型、连杆设计、限位标定——那些属《M1-PoC执行清单-摇尾机构
 *    与选型》的范畴。因此 v1.0 默认 ENABLE_TAIL_OUTPUT = 0，
 *    只输出状态与日志，硬件通道留出接口。
 *
 *  【关键约束】
 *    · 档位切换必须平滑：常规切换走 P-51，咕噜后缓降走 P-52。
 *    · 频率带 ±P-49 随机抖动，禁止精确周期重复（机械感）。
 *    · 无互动超过 P-53 后自动停摆（不归零心情值，只是停止动作）。
 *    · PURR 态由状态机直接锁 W4（FR-39 验收：咕噜期间摇尾为 W4）。
 * ============================================================================
 */

#ifndef GRUNTOY_TAIL_H
#define GRUNTOY_TAIL_H

#include <Arduino.h>

class TailDriver {
public:
  void init();
  void tick();

  // 设置目标档位（1~4）。usePurrExitTransition=true 时用 P-52 时长缓降。
  void setGear(uint8_t gear, bool usePurrExitTransition = false);
  // 立即对齐到目标档位（不做过过渡）：用于初始化与"状态驱动强制覆盖"。
  void snapToGear(uint8_t gear);

  // 交互通知：重置"无互动停摆"计时（抚摸/抱起时调用）
  void notifyInteraction();

  void stopNow();                      // 立即停摆（输出归零）

  // --- 查询 ---
  uint8_t getGear() const          { return m_gear; }
  bool    isActive() const         { return m_active; }
  bool    inTransition() const     { return m_transRemainMs > 0; }
  int32_t getTargetFreqHz100() const { return m_freqTo; }
  int32_t getFreqHz100() const     { return m_active ? m_freqNow : 0; }   // 平滑后的当前值
  int32_t getAmpX100() const       { return m_active ? m_ampNow : 0; }
  uint32_t getIdleMs() const       { return millis() - m_lastInteractionMs; }

  // 供本机测试注入：把输出通路记录为"最近一次写入值"
  uint8_t getLastOutputPct() const { return m_lastOutPct; }

private:
  int32_t targetFreqOf(uint8_t gear) const;
  int32_t targetAmpOf(uint8_t gear) const;
  uint32_t nextRand();
  void     writeOutput(uint8_t dutyPct);

  uint8_t  m_gear         = 1;      // 逻辑档位
  bool     m_active       = true;   // 是否处于摇摆状态

  int32_t  m_freqFrom     = 0;
  int32_t  m_freqTo       = 0;
  int32_t  m_freqNow      = 0;
  int32_t  m_ampFrom      = 0;
  int32_t  m_ampTo        = 0;
  int32_t  m_ampNow       = 0;

  uint32_t m_transStartMs = 0;
  uint32_t m_transTotalMs = 0;
  uint32_t m_transRemainMs = 0;

  uint32_t m_lastInteractionMs = 0;
  uint32_t m_lastJitterMs      = 0;
  float    m_jitterFactor      = 1.0f;

  float    m_phase             = 0.0f;   // 输出通路相位累加
  uint32_t m_lastTickMs        = 0;
  uint8_t  m_lastOutPct        = 0;

  uint32_t m_rngState          = 0x9E3779B9u;
};

extern TailDriver g_tail;

#endif  // GRUNTOY_TAIL_H
