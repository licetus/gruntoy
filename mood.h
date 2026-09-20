/*
 * ============================================================================
 *  mood.h —— 心情值引擎（体感交互状态机）
 *  对应 PRD v1.1：
 *    FR-35   呼吸式震动（触发链路，§9.6）
 *    FR-36   咕噜式震动（轮次间隔、累计运行上限）
 *    FR-37/37a~37d  心情值累积 / 咕噜触发与时长映射 / 还原与冷却 / 自然衰减
 *    FR-39/39a      摇尾分级（PURR 态锁 W4）
 *    FR-40/40a      平滑过渡（降落曲线 / 三通道同步）
 *
 *  【状态机】对应 PRD §9.5（6 态）
 *      ACCUM ──触发──▶ PURR ──时长到达──▶ LANDING ──▶ COOLDOWN ──▶ ACCUM
 *                （安全类 LOWPOWER / UPGRADING 优先级最高，可打断任意态）
 *
 *  【设计要点】
 *   1. 所有数值从 Params 读取，代码内不出现行为常量（FR-38）。
 *   2. 状态切换一律经 enterState()，保证 m_stateStart 与副作用同步。
 *   3. 降落期与冷却期的抚摸"一律不计分，但必须给出可感知反馈"——
 *      否则用户会判定玩具失灵（FR-37c / FR-40④ / PRD §12 异常22）。
 *   4. 咕噜触发受两道闸门约束：轮次最小间隔（P-30）与累计运行上限（P-31）。
 * ============================================================================
 */

#ifndef GRUNTOY_MOOD_H
#define GRUNTOY_MOOD_H

#include <Arduino.h>
#include "haptic.h"
#include "tail.h"

// ---------------------------------------------------------------------------
// 体感状态（PRD §9.5）
// ---------------------------------------------------------------------------
enum HapticState : uint8_t {
  HS_ACCUM = 0,     // 待机 / 累积中
  HS_PURR,          // 咕噜中
  HS_LANDING,       // 降落中
  HS_COOLDOWN,      // 冷却中
  HS_LOWPOWER,      // 低电模式（v1.0 占位：需接入电池 ADC）
  HS_UPGRADING      // 升级中（v1.0 占位：需接入 OTA）
};

const char* stateName(HapticState s);

// 咕噜被闸门拦下的原因（用于日志与观测）
enum PurrGate : uint8_t {
  PG_OPEN = 0,        // 闸门开启，可触发
  PG_GAP,             // 距上一轮咕噜太近（P-30）
  PG_CUMULATIVE,      // 累计运行已达上限（P-31）
  PG_SYSTEM           // 系统态（低电/升级）
};

const char* purrGateName(PurrGate g);

class MoodEngine {
public:
  void init(HapticEngine* haptic, TailDriver* tail);

  // 主循环：状态驱动。必须在 loop 内每轮调用。
  void tick();

  // --- 交互输入 ---
  // 一次「抚摸」事件。返回 true 表示本次计入心情值。
  // 返回 false 时说明处于咕噜/降落/冷却/系统态，或间隔过短被防抖过滤，
  // 且已按规则给出对应的可感知反馈（轻震或拒绝震）。
  bool onStroke();

  // --- 查询 ---
  int16_t     getMood() const          { return m_mood; }
  HapticState getState() const         { return m_state; }
  uint8_t     getTailGear() const      { return m_tailGear; }
  int32_t     getTailFreqHz100() const;
  uint32_t    getStateElapsedMs() const{ return millis() - m_stateStart; }
  uint32_t    getPurrTargetMs() const  { return m_purrTargetMs; }
  uint16_t    getTotalStrokes() const  { return m_totalStrokes; }
  uint16_t    getCountedStrokes() const{ return m_countedStrokes; }
  uint32_t    getPurrCount() const     { return m_purrCount; }
  uint32_t    getBreathCount() const   { return m_breathCount; }
  uint32_t    getPurrAccumMs() const   { return m_purrAccumMs; }
  bool        isPurrLockedOut() const  { return m_purrLockedOut; }
  bool        isLandingTickled() const { return m_landingTouchDuring; }

  // 咕噜触发闸门当前状态（供 CLI / 测试观测，不改变任何状态）
  PurrGate purrGateState() const;

  // --- 调试与测试接口 ---
  void forceMood(int16_t v);
  void forceState(HapticState s);          // 仅用于调试与测试
  void resetState();
  // 心情值 → 咕噜时长映射（与运行时同一实现，避免测试与实现两套公式）
  uint32_t debugPurrDuration(int16_t mood) { return computePurrDuration(mood); }
  // 降落曲线取值（与运行时同一实现）
  int16_t  debugLandingValue(int16_t from, uint32_t elapsedMs);
  // 呼吸档位选择（1~3）
  uint8_t  debugBreathLevel(int16_t mood) const { return breathLevelFor(mood); }

private:
  void     enterState(HapticState s);
  void     updateTailGear(bool allowTransition, bool purrExit = false);
  uint32_t computePurrDuration(int16_t mood);
  int16_t  landingValue(uint32_t elapsedMs);
  uint8_t  breathLevelFor(int16_t mood) const;
  void     maybeTriggerBreath(uint32_t now);
  void     resetAccumulators(uint32_t now);

  HapticEngine* m_haptic = nullptr;
  TailDriver*   m_tail   = nullptr;

  // --- 心情值 ---
  int16_t  m_mood          = 60;
  uint32_t m_lastDecayMs   = 0;
  uint32_t m_lastStrokeMs  = 0;
  uint32_t m_lastPurrEndMs = 0;
  bool     m_everStroked   = false;  // 首次抚摸不做防抖判定（开箱即响应）

  // --- 连续抚摸加速增益 ---
  uint32_t m_continuousStartMs = 0;
  uint32_t m_lastBonusMs       = 0;

  // --- 状态机 ---
  HapticState m_state       = HS_ACCUM;
  uint32_t    m_stateStart  = 0;
  uint32_t    m_purrTargetMs = 0;
  int16_t     m_landingFrom = 0;
  int16_t     m_landingTo   = 70;
  bool        m_landingTouchDuring = false;

  // --- 咕噜闸门 ---
  uint32_t m_purrAccumMs    = 0;      // 近期累计咕噜运行时长（P-31 口径）
  bool     m_purrLockedOut  = false;  // 已达累计上限，需休息

  // --- 摇尾 ---
  uint8_t  m_tailGear       = 2;

  // --- 呼吸节流 ---
  uint32_t m_lastBreathMs   = 0;

  // --- 统计 ---
  uint16_t m_totalStrokes   = 0;
  uint16_t m_countedStrokes = 0;
  uint32_t m_purrCount      = 0;
  uint32_t m_breathCount    = 0;
};

extern MoodEngine g_mood;

#endif  // GRUNTOY_MOOD_H
