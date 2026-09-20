/*
 * ============================================================================
 *  haptic.h —— 体感引擎（呼吸 / 咕噜 / 裸测 / 扫频 / 提示震）
 *  对应 PRD v1.1：FR-35（呼吸）、FR-36（咕噜）、FR-40a（平滑过渡）
 *
 *  【设计要点】
 *   1. 完全非阻塞：所有动作在 tick() 中按 millis() 推进，不 delay()。
 *   2. 呼吸：正弦包络，谷底占空比 0%（消除低占空比机械嗡鸣），周期/幅度带随机抖动。
 *   3. 咕噜：驱动频率 P-32 通过"门控调制"真正落到输出——
 *        在每个 1/f 周期内，按目标平均占空比导通一段、断开一段。
 *        平均功率与占空比一致，同时在 f 上产生可听的震颤/咕噜声。
 *        ⚠ 若缺少这一步，P-32 与「扫频找猫感区间」实验都不产生实际物理差异。
 *   4. 咕噜叠加：基础占空比 × 强度抖动（P-34/P-35）× 长包络缓变（P-36）× 淡入淡出（P-22/P-23）。
 *   5. 互斥与优先级：咕噜 > 呼吸 > 待机；提示震（柔震/拒绝震）可打断呼吸但不打断咕噜。
 * ============================================================================
 */

#ifndef GRUNTOY_HAPTIC_H
#define GRUNTOY_HAPTIC_H

#include <Arduino.h>

// ---------------------------------------------------------------------------
// 震动模式
// ---------------------------------------------------------------------------
enum HapticMode : uint8_t {
  HM_IDLE = 0,      // 空闲
  HM_BREATH,        // 呼吸模式
  HM_PURR,          // 咕噜模式
  HM_BARE,          // 裸测（固定占空比长转，测起转阈值/噪声）
  HM_SWEEP,         // 扫频（自动遍历 25~50Hz，每频点停留固定时长）
  HM_BUZZ           // 短促提示震（柔震 / 拒绝震共用）
};

class HapticEngine {
public:
  void init();
  void tick();

  // --- 触发接口 ---
  bool startBreath(uint8_t level);              // level: 1=L1 2=L2 3=L3
  bool startPurr(uint32_t durationMs);          // 指定时长咕噜（时长已由映射决定）
  void startBare(uint8_t dutyPct);              // 裸测（长转）
  bool startSweep();                            // 扫频
  bool startBuzz(uint16_t ms, uint8_t dutyPct); // 短震（可打断呼吸，不可打断咕噜/扫频）
  void stop();                                  // 立即停止

  // --- 状态查询 ---
  HapticMode getMode() const        { return m_mode; }
  const char* modeName() const;
  bool isBusy() const               { return m_mode != HM_IDLE; }
  bool isPurring() const            { return m_mode == HM_PURR; }
  bool isSweeping() const           { return m_mode == HM_SWEEP; }
  uint32_t getElapsedMs() const     { return m_mode == HM_IDLE ? 0 : (millis() - m_startMs); }
  uint32_t getPurrTargetMs() const  { return m_purrDurationMs; }
  uint32_t getPurrRemainingMs() const;
  uint32_t getDriveFreqHz() const;               // 当前实际驱动频率（Hz）
  uint8_t  getCurrentDutyPct() const { return m_lastDutyPct; }
  uint8_t  getSweepIndex() const    { return m_sweepIdx; }
  uint32_t getSweepFreqHz() const   { return m_sweepFreqHz; }
  uint32_t getPurrCount() const     { return m_purrStarted; }
  uint32_t getBreathCount() const   { return m_breathStarted; }

  // --- 直接输出（供本机测试与裸测使用）---
  void setDutyDirect(uint8_t dutyPct);
  void off();

private:
  void   applyDuty(uint8_t dutyPct);
  // 门控调制：把"平均占空比 dutyPct"在 freqHz 上切成通/断脉冲
  void   applyGatedDuty(float dutyPct, uint32_t elapsedMs, uint32_t freqHz);
  float  randJitter(float pct);
  uint32_t nextRand();

  HapticMode m_mode      = HM_IDLE;
  uint32_t   m_startMs   = 0;
  uint32_t   m_lastTick  = 0;
  uint8_t    m_lastDutyPct = 0;

  // --- 呼吸状态 ---
  uint32_t m_breathPeriodMs = 4000;   // 本次呼吸周期（含抖动）
  uint8_t  m_breathPeakDuty = 40;     // 本次峰值占空比（含抖动）
  uint8_t  m_breathCycles   = 2;
  uint32_t m_breathTotalMs  = 0;

  // --- 咕噜状态 ---
  uint32_t m_purrDurationMs = 20000;
  uint32_t m_purrFreqHz     = 30;
  uint32_t m_purrFadeInMs   = 500;
  uint32_t m_purrFadeOutMs  = 800;

  // --- 裸测 / 短震 ---
  uint32_t m_bareDurationMs = 0;      // 0 = 长转不停

  // --- 扫频状态 ---
  uint8_t  m_sweepIdx       = 0;
  uint32_t m_sweepFreqHz    = 25;     // 本质是门控调制频率
  uint32_t m_sweepStepStart = 0;

  // --- 计数器（PoC 观测用）---
  uint32_t m_purrStarted   = 0;
  uint32_t m_breathStarted = 0;

  // --- 内部 RNG（自实现，便于复现）---
  uint32_t m_rngState = 12345;
};

extern HapticEngine g_haptic;

#endif  // GRUNTOY_HAPTIC_H
