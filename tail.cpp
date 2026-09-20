/*
 * ============================================================================
 *  tail.cpp —— 摇尾通道实现
 *  gruntoy v1.0
 * ============================================================================
 */

#include "tail.h"
#include "params.h"
#include "config.h"

TailDriver g_tail;

// ---------------------------------------------------------------------------
void TailDriver::init() {
  m_gear      = 1;
  m_active    = true;
  m_freqFrom = m_freqTo = m_freqNow = 0;
  m_ampFrom  = m_ampTo  = m_ampNow  = 0;
  m_transTotalMs = m_transRemainMs = 0;
  m_lastInteractionMs = millis();
  m_lastJitterMs      = millis();
  m_lastTickMs        = millis();
  m_rngState          = micros() ^ 0x85EBCA6Bu;

#if ENABLE_TAIL_OUTPUT
  #if USE_LEDC_PWM
    ledcAttach(TAIL_PWM_PIN, TAIL_PWM_FREQ, TAIL_PWM_RES);
    ledcWrite(TAIL_PWM_PIN, 0);
  #else
    ledcSetup(TAIL_PWM_CHANNEL, TAIL_PWM_FREQ, TAIL_PWM_RES);
    ledcAttachPin(TAIL_PWM_PIN, TAIL_PWM_CHANNEL);
    ledcWrite(TAIL_PWM_CHANNEL, 0);
  #endif
  Serial.printf("[TAIL] 摇尾通道就绪 (GPIO%d)\n", TAIL_PWM_PIN);
#else
  Serial.println(F("[TAIL] 摇尾通道：仅状态计算与日志（ENABLE_TAIL_OUTPUT=0）"));
#endif

  snapToGear(1);
}

uint32_t TailDriver::nextRand() {
  m_rngState ^= m_rngState << 13;
  m_rngState ^= m_rngState >> 17;
  m_rngState ^= m_rngState << 5;
  return m_rngState;
}

int32_t TailDriver::targetFreqOf(uint8_t gear) const {
  switch (gear) {
    case 1:  return Params::get(P_TAIL_FREQ_W1);
    case 2:  return Params::get(P_TAIL_FREQ_W2);
    case 3:  return Params::get(P_TAIL_FREQ_W3);
    case 4:  return Params::get(P_TAIL_FREQ_W4);
    default: return Params::get(P_TAIL_FREQ_W1);
  }
}

int32_t TailDriver::targetAmpOf(uint8_t gear) const {
  switch (gear) {
    case 1:  return Params::get(P_TAIL_AMP_W1);
    case 2:  return Params::get(P_TAIL_AMP_W2);
    case 3:  return Params::get(P_TAIL_AMP_W3);
    case 4:  return Params::get(P_TAIL_AMP_W4);
    default: return Params::get(P_TAIL_AMP_W1);
  }
}

// ---------------------------------------------------------------------------
// 立即对齐（不做过过渡）
// ---------------------------------------------------------------------------
void TailDriver::snapToGear(uint8_t gear) {
  if (gear < 1) gear = 1;
  if (gear > 4) gear = 4;

  m_gear = gear;
  m_freqTo = targetFreqOf(gear);
  m_ampTo  = targetAmpOf(gear);
  m_freqNow = m_freqTo;
  m_ampNow  = m_ampTo;
  m_freqFrom = m_freqTo;
  m_ampFrom  = m_ampTo;
  m_transTotalMs = m_transRemainMs = 0;
  m_active = (Params::get(P_TAIL_MODE_ENABLED) != 0);
  m_lastInteractionMs = millis();
}

// ---------------------------------------------------------------------------
// 设置目标档位（带平滑过渡）
// ---------------------------------------------------------------------------
void TailDriver::setGear(uint8_t gear, bool usePurrExitTransition) {
  if (gear < 1) gear = 1;
  if (gear > 4) gear = 4;

  if (Params::get(P_TAIL_MODE_ENABLED) == 0) {
    m_gear = gear;
    m_freqTo = m_ampTo = 0;
    m_freqNow = m_ampNow = 0;
    m_active = false;
    return;
  }

  // 从"当前实际值"出发，避免过渡中途改档时出现回跳
  m_freqFrom = m_freqNow;
  m_ampFrom  = m_ampNow;
  m_gear     = gear;
  m_freqTo   = targetFreqOf(gear);
  m_ampTo    = targetAmpOf(gear);

  const uint32_t dur = (uint32_t)(usePurrExitTransition
                                  ? Params::get(P_TAIL_PURR_EXIT_TRANSITION_MS)
                                  : Params::get(P_TAIL_TRANSITION_MS));
  m_transTotalMs  = dur;
  m_transStartMs  = millis();
  m_transRemainMs = dur;

  m_active = true;
  m_lastInteractionMs = millis();

  if (Params::get(P_DEBUG_LOG_ENABLED)) {
    Serial.printf("[TAIL] W%u -> W%u  (%ld.%02ldHz -> %ld.%02ldHz, 过渡 %lums%s)\n",
                  (unsigned)(m_gear), (unsigned)gear,
                  (long)(m_freqFrom / 100), (long)(m_freqFrom % 100),
                  (long)(m_freqTo / 100), (long)(m_freqTo % 100),
                  (unsigned long)dur, usePurrExitTransition ? " 咕噜后缓降" : "");
  }
}

void TailDriver::notifyInteraction() {
  m_lastInteractionMs = millis();
  if (!m_active && Params::get(P_TAIL_MODE_ENABLED) != 0) {
    // 被冷落后重新互动：恢复摇摆（走常规过渡）
    m_active = true;
    m_freqFrom = 0;
    m_ampFrom  = 0;
    m_freqTo   = targetFreqOf(m_gear);
    m_ampTo    = targetAmpOf(m_gear);
    m_transTotalMs  = (uint32_t)Params::get(P_TAIL_TRANSITION_MS);
    m_transStartMs  = millis();
    m_transRemainMs = m_transTotalMs;
  }
}

void TailDriver::stopNow() {
  m_active = false;
  m_freqNow = m_ampNow = 0;
  m_freqFrom = m_freqTo = m_ampFrom = m_ampTo = 0;
  m_transTotalMs = m_transRemainMs = 0;
  writeOutput(0);
}

void TailDriver::writeOutput(uint8_t dutyPct) {
  m_lastOutPct = dutyPct;
#if ENABLE_TAIL_OUTPUT
  const uint32_t maxDuty = (1u << TAIL_PWM_RES) - 1u;
  const uint32_t v = (uint32_t)((uint64_t)dutyPct * maxDuty / 100u);
  #if USE_LEDC_PWM
    ledcWrite(TAIL_PWM_PIN, v);
  #else
    ledcWrite(TAIL_PWM_CHANNEL, v);
  #endif
#endif
}

// ---------------------------------------------------------------------------
// tick
// ---------------------------------------------------------------------------
void TailDriver::tick() {
  const uint32_t now = millis();
  uint32_t dt = now - m_lastTickMs;
  m_lastTickMs = now;
  if (dt > 200) dt = 200;      // 长间隔后不做大步长积分，避免相位跳跃

  // --- 总开关关闭 ---
  if (Params::get(P_TAIL_MODE_ENABLED) == 0) {
    if (m_active) stopNow();
    return;
  }

  // --- 无互动停摆（P-53）---
  const uint32_t idleLimit = (uint32_t)Params::get(P_TAIL_STOP_IDLE_MS);
  if (m_active && idleLimit > 0 && (now - m_lastInteractionMs) >= idleLimit) {
    stopNow();
    Serial.printf("[TAIL] 无互动 %lus，停止摇摆（P-53）\n", (unsigned long)(idleLimit / 1000));
    return;
  }
  if (!m_active) return;

  // --- 频率抖动因子（P-49）：每 500ms 刷新一次，避免高频噪声 ---
  if (now - m_lastJitterMs >= 500) {
    m_lastJitterMs = now;
    const float pct = (float)Params::get(P_TAIL_FREQ_JITTER_PCT) / 100.0f;
    const float r   = (float)(nextRand() % 10001) / 10000.0f;   // [0,1]
    m_jitterFactor  = 1.0f + (r * 2.0f - 1.0f) * pct;
  }

  // --- 过渡插值（频率与幅度同时插值，避免"快而小/慢而大"的怪异组合）---
  if (m_transRemainMs > 0) {
    const uint32_t elapsed = now - m_transStartMs;
    if (elapsed >= m_transTotalMs) {
      m_freqNow = m_freqTo;
      m_ampNow  = m_ampTo;
      m_transRemainMs = 0;
    } else {
      const float t = (float)elapsed / (float)m_transTotalMs;
      const float f = t * t * (3.0f - 2.0f * t);   // 缓入缓出，两端导数为 0
      m_freqNow = m_freqFrom + (int32_t)((float)(m_freqTo - m_freqFrom) * f);
      m_ampNow  = m_ampFrom  + (int32_t)((float)(m_ampTo  - m_ampFrom)  * f);
      m_transRemainMs = m_transTotalMs - elapsed;
    }
  } else {
    m_freqNow = m_freqTo;
    m_ampNow  = m_ampTo;
  }

  // --- 输出通路：按频率振荡，摆幅由幅度决定 ---
  const float freqHz = (float)m_freqNow / 100.0f * m_jitterFactor;
  m_phase += 2.0f * PI * freqHz * (float)dt / 1000.0f;
  while (m_phase >= 2.0f * PI) m_phase -= 2.0f * PI;

  // 归一化到 0~100：中位 50，摆幅 ±(幅度×25)。幅度 1.0 → 25%~75%
  const float ampNorm = (float)m_ampNow / 100.0f;
  float dutyF = 50.0f + sinf(m_phase) * ampNorm * 25.0f;
  if (dutyF < 0.0f)   dutyF = 0.0f;
  if (dutyF > 100.0f) dutyF = 100.0f;

  writeOutput((uint8_t)dutyF);
}
