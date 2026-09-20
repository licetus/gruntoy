/*
 * ============================================================================
 *  haptic.cpp —— 体感引擎实现
 *  gruntoy v1.0
 * ============================================================================
 */

#include "haptic.h"
#include "params.h"
#include "config.h"

HapticEngine g_haptic;

// ---------------------------------------------------------------------------
// 硬件初始化
// ---------------------------------------------------------------------------
void HapticEngine::init() {
#if USE_LEDC_PWM
  // ESP32 Arduino Core 3.x
  ledcAttach(MOTOR_PWM_PIN, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
  ledcWrite(MOTOR_PWM_PIN, 0);
#else
  // Core 2.x 兼容路径
  ledcSetup(MOTOR_PWM_CHANNEL, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
  ledcAttachPin(MOTOR_PWM_PIN, MOTOR_PWM_CHANNEL);
  ledcWrite(MOTOR_PWM_CHANNEL, 0);
#endif
  m_lastTick = millis();
  m_rngState = micros() ^ 0xA5A5A5A5;
  m_lastDutyPct = 0;
  Serial.printf("[HAPTIC] 马达 PWM 就绪 (GPIO%d, 载波 %dHz, %dbit, 驱动器=%s)\n",
                MOTOR_PWM_PIN, MOTOR_PWM_FREQ, MOTOR_PWM_RES,
                USE_LEDC_PWM ? "LEDC(Core3)" : "LEDC-Channel(Core2)");
}

const char* HapticEngine::modeName() const {
  switch (m_mode) {
    case HM_IDLE:   return "IDLE";
    case HM_BREATH: return "BREATH";
    case HM_PURR:   return "PURR";
    case HM_BARE:   return "BARE";
    case HM_SWEEP:  return "SWEEP";
    case HM_BUZZ:   return "BUZZ";
    default:        return "UNKNOWN";
  }
}

uint32_t HapticEngine::getDriveFreqHz() const {
  switch (m_mode) {
    case HM_PURR:  return m_purrFreqHz;
    case HM_SWEEP: return m_sweepFreqHz;
    default:       return 0;   // 呼吸/裸测不走门控，驱动频率概念不适用
  }
}

uint32_t HapticEngine::getPurrRemainingMs() const {
  if (m_mode != HM_PURR) return 0;
  uint32_t el = millis() - m_startMs;
  return el >= m_purrDurationMs ? 0 : (m_purrDurationMs - el);
}

// ---------------------------------------------------------------------------
// xorshift32：自实现 RNG，避免平台 random() 差异导致本机测试不可复现
// ---------------------------------------------------------------------------
uint32_t HapticEngine::nextRand() {
  m_rngState ^= m_rngState << 13;
  m_rngState ^= m_rngState >> 17;
  m_rngState ^= m_rngState << 5;
  return m_rngState;
}

// 返回 [1-pct%, 1+pct%] 的乘性因子，例如 pct=15 → [0.85, 1.15]
float HapticEngine::randJitter(float pct) {
  float r = (float)(nextRand() % 10001) / 10000.0f;   // [0,1]
  float d = (r * 2.0f - 1.0f) * (pct / 100.0f);
  return 1.0f + d;
}

// ---------------------------------------------------------------------------
// 占空比输出：0~100% → PWM 满量程
// ---------------------------------------------------------------------------
void HapticEngine::applyDuty(uint8_t dutyPct) {
  if (dutyPct > 100) dutyPct = 100;
  m_lastDutyPct = dutyPct;

  const uint32_t maxDuty = (1u << MOTOR_PWM_RES) - 1u;
  const uint32_t v = (uint32_t)((uint64_t)dutyPct * maxDuty / 100u);

#if USE_LEDC_PWM
  ledcWrite(MOTOR_PWM_PIN, v);
#else
  ledcWrite(MOTOR_PWM_CHANNEL, v);
#endif
}

// ---------------------------------------------------------------------------
// 门控调制（v1.0 补齐）
//   把「平均占空比 dutyPct」在 freqHz 上切成通/断脉冲：
//     周期 T = 1000/freqHz ms；每周期内前 (dutyPct% × T) 导通、其余断开。
//   平均功率与 dutyPct 一致，同时在该频率上产生可听的机械震颤——
//   这正是 P-32「驱动频率 25~50Hz」在 ERM 马达上的物理实现方式，
//   也是 M1 执行清单 Day 5「扫频找猫感区间」得以成立的前提。
// ---------------------------------------------------------------------------
void HapticEngine::applyGatedDuty(float dutyPct, uint32_t elapsedMs, uint32_t freqHz) {
  if (dutyPct <= 0.0f)  { applyDuty(0);   return; }
  if (freqHz == 0)      { applyDuty((uint8_t)dutyPct); return; }   // 无频率要求 → 纯 PWM

  if (dutyPct >= 100.0f) { applyDuty(100); return; }

  const uint32_t periodMs = 1000u / freqHz;      // 30Hz → 33ms
  if (periodMs == 0)     { applyDuty((uint8_t)dutyPct); return; }

  const uint32_t onMs = (uint32_t)((float)periodMs * dutyPct / 100.0f + 0.5f);
  const uint32_t phase = elapsedMs % periodMs;

  applyDuty(phase < onMs ? 100 : 0);
}

void HapticEngine::off() {
  applyDuty(0);
  m_mode = HM_IDLE;
}

// ---------------------------------------------------------------------------
// 启动：呼吸（FR-35）
// ---------------------------------------------------------------------------
bool HapticEngine::startBreath(uint8_t level) {
  // 互斥：咕噜 / 扫频优先，不打断
  if (m_mode == HM_PURR || m_mode == HM_SWEEP) return false;

  uint8_t peak = 0;
  switch (level) {
    case 1:  peak = (uint8_t)Params::get(P_BREATH_L1_DUTY_PCT); break;
    case 2:  peak = (uint8_t)Params::get(P_BREATH_L2_DUTY_PCT); break;
    case 3:  peak = (uint8_t)Params::get(P_BREATH_L3_DUTY_PCT); break;
    default: peak = (uint8_t)Params::get(P_BREATH_PEAK_DUTY_PCT); break;
  }
  if (peak < 1) peak = 1;

  // 幅度随机抖动（P-39）：禁止精确机械重复
  const float ampJit = randJitter((float)Params::get(P_BREATH_AMPLITUDE_JITTER_PCT));
  int32_t p = (int32_t)(peak * ampJit);
  if (p < 1)   p = 1;
  if (p > 100) p = 100;

  // 周期随机抖动（P-38）
  const float perJit = randJitter((float)Params::get(P_BREATH_PERIOD_JITTER_PCT));
  m_breathPeriodMs = (uint32_t)(Params::get(P_BREATH_PERIOD_MS) * perJit);
  if (m_breathPeriodMs < 500) m_breathPeriodMs = 500;

  m_breathPeakDuty = (uint8_t)p;
  m_breathCycles   = (uint8_t)Params::get(P_BREATH_CYCLES_PER_TRIGGER);
  if (m_breathCycles < 1) m_breathCycles = 1;
  m_breathTotalMs  = m_breathPeriodMs * m_breathCycles;

  m_mode    = HM_BREATH;
  m_startMs = millis();
  m_breathStarted++;

  if (Params::get(P_DEBUG_LOG_ENABLED)) {
    Serial.printf("[HAPTIC] 呼吸启动 L%u: 周期=%lums(%+.1f%%) 峰值=%u%% 周期数=%u\n",
                  level, (unsigned long)m_breathPeriodMs,
                  (perJit - 1.0f) * 100.0f, m_breathPeakDuty, m_breathCycles);
  }
  return true;
}

// ---------------------------------------------------------------------------
// 启动：咕噜（FR-36）
// ---------------------------------------------------------------------------
bool HapticEngine::startPurr(uint32_t durationMs) {
  // 时长上限截断（P-29）
  const int32_t dmax = Params::get(P_PURR_DURATION_MAX_MS);
  if (durationMs > (uint32_t)dmax) durationMs = (uint32_t)dmax;
  if (durationMs < 1000)           durationMs = 1000;

  m_purrDurationMs = durationMs;
  m_purrFreqHz     = (uint32_t)Params::get(P_PURR_DRIVE_FREQ_HZ) / 100;   // Hz×100 → Hz
  if (m_purrFreqHz < 1) m_purrFreqHz = 1;
  m_purrFadeInMs   = (uint32_t)Params::get(P_PURR_FADEIN_MS);
  m_purrFadeOutMs  = (uint32_t)Params::get(P_PURR_FADEOUT_MS);

  m_mode    = HM_PURR;
  m_startMs = millis();
  m_purrStarted++;

  if (Params::get(P_DEBUG_LOG_ENABLED)) {
    Serial.printf("[HAPTIC] 咕噜启动: 时长=%lums 门控频率=%luHz 淡入=%lums 淡出=%lums\n",
                  (unsigned long)m_purrDurationMs, (unsigned long)m_purrFreqHz,
                  (unsigned long)m_purrFadeInMs, (unsigned long)m_purrFadeOutMs);
  }
  return true;
}

// ---------------------------------------------------------------------------
// 启动：裸测（固定占空比长转，测起转阈值与噪声基线）
// ---------------------------------------------------------------------------
void HapticEngine::startBare(uint8_t dutyPct) {
  m_mode           = HM_BARE;
  m_startMs        = millis();
  m_bareDurationMs = 0;      // 长转，直到手工停止
  applyDuty(dutyPct);
  if (Params::get(P_DEBUG_LOG_ENABLED)) {
    Serial.printf("[HAPTIC] 裸测: 占空比 %u%% 持续输出（t barestop 停止）\n", dutyPct);
  }
}

void HapticEngine::setDutyDirect(uint8_t dutyPct) { applyDuty(dutyPct); }

// ---------------------------------------------------------------------------
// 启动：扫频（门控频率 25→50Hz，每频点停留 SWEEP_DWELL_MS）
// ---------------------------------------------------------------------------
bool HapticEngine::startSweep() {
  if (m_mode == HM_PURR) return false;   // 咕噜优先

  m_mode           = HM_SWEEP;
  m_startMs        = millis();
  m_sweepIdx       = 0;
  m_sweepFreqHz    = SWEEP_START_HZ;
  m_sweepStepStart = millis();

  Serial.println(F("[HAPTIC] ========== 咕噜扫频开始 =========="));
  Serial.println(F("[HAPTIC] 每频点停留 10s，请逐点记录「猫感评分(1~5)」与「噪声(dB)」"));
  Serial.printf("[HAPTIC] 频点 %u/%u: %lu Hz\n",
                m_sweepIdx + 1, (unsigned)SWEEP_POINTS, (unsigned long)m_sweepFreqHz);
  applyDuty((uint8_t)Params::get(P_PURR_DUTY_BASE_PCT));
  return true;
}

// ---------------------------------------------------------------------------
// 启动：短促提示震（柔震 / 拒绝震共用）
// ---------------------------------------------------------------------------
bool HapticEngine::startBuzz(uint16_t ms, uint8_t dutyPct) {
  // 提示震可打断呼吸，但不可打断咕噜 / 扫频
  if (m_mode == HM_PURR || m_mode == HM_SWEEP) return false;
  if (ms == 0) return false;

  m_mode           = HM_BARE;
  m_startMs        = millis();
  m_bareDurationMs = ms;
  applyDuty(dutyPct);
  return true;
}

// ---------------------------------------------------------------------------
// 停止
// ---------------------------------------------------------------------------
void HapticEngine::stop() {
  off();
  if (Params::get(P_DEBUG_LOG_ENABLED)) Serial.println(F("[HAPTIC] 已停止"));
}

// ---------------------------------------------------------------------------
// tick —— 非阻塞推进
// ---------------------------------------------------------------------------
void HapticEngine::tick() {
  const uint32_t now = millis();
  uint32_t elapsed;

  switch (m_mode) {

    // ---------------- 呼吸 ----------------
    case HM_BREATH: {
      elapsed = now - m_startMs;
      if (elapsed >= m_breathTotalMs) {
        off();
        if (Params::get(P_DEBUG_LOG_ENABLED)) {
          Serial.printf("[HAPTIC] 呼吸结束（历时 %lums）\n", (unsigned long)elapsed);
        }
        break;
      }

      const uint32_t inCycle = elapsed % m_breathPeriodMs;
      const float    phase   = (float)inCycle / (float)m_breathPeriodMs;   // 0~1

      // 半波正弦包络：sin(π·phase) → 0 → 1 → 0（谷底严格为 0）
      const float env = sinf(PI * phase);

      // 谷底占空比（P-41，默认 0 = 完全断电）
      const float trough = (float)Params::get(P_BREATH_TROUGH_DUTY_PCT);
      const float dutyF  = trough + ((float)m_breathPeakDuty - trough) * env;

      // 呼吸不使用门控调制（周期 3~5s，门控无意义）
      applyDuty((uint8_t)dutyF);
      break;
    }

    // ---------------- 咕噜 ----------------
    case HM_PURR: {
      elapsed = now - m_startMs;
      if (elapsed >= m_purrDurationMs) {
        off();
        Serial.printf("[HAPTIC] 咕噜结束（历时 %lums）\n", (unsigned long)elapsed);
        break;
      }

      const float base = (float)Params::get(P_PURR_DUTY_BASE_PCT);

      // 强度抖动（P-34 + P-35）
      const uint32_t jitPeriod = (uint32_t)Params::get(P_PURR_JITTER_PERIOD_MS);
      const float jitPhase  = (jitPeriod ? (float)(elapsed % jitPeriod) / (float)jitPeriod : 0.0f);
      const float jitEnv    = sinf(2.0f * PI * jitPhase);
      const float jitPct    = (float)Params::get(P_PURR_DUTY_JITTER_PCT);
      const float jitFactor = 1.0f + jitEnv * (jitPct / 100.0f);

      // 长包络缓变（P-36）：模拟情绪起伏
      const uint32_t longPeriod = (uint32_t)Params::get(P_PURR_LONG_ENVELOPE_MS);
      const float longPhase  = (longPeriod ? (float)(elapsed % longPeriod) / (float)longPeriod : 0.0f);
      const float longEnv    = sinf(2.0f * PI * longPhase);
      // 长包络深度固定 ±15%：M1 若实测起伏不足/过多，再提升为独立参数（见 CHANGELOG OP-04）
      const float longFactor = 1.0f + longEnv * 0.15f;

      // 淡入（P-22）/ 淡出（P-23），禁止骤起骤停
      float fadeFactor = 1.0f;
      if (m_purrFadeInMs > 0 && elapsed < m_purrFadeInMs) {
        fadeFactor = (float)elapsed / (float)m_purrFadeInMs;
      }
      const uint32_t fadeOutStart = (m_purrDurationMs > m_purrFadeOutMs)
                                    ? (m_purrDurationMs - m_purrFadeOutMs) : 0;
      if (m_purrFadeOutMs > 0 && elapsed >= fadeOutStart) {
        fadeFactor = 1.0f - (float)(elapsed - fadeOutStart) / (float)m_purrFadeOutMs;
      }
      if (fadeFactor < 0.0f) fadeFactor = 0.0f;

      float dutyF = base * jitFactor * longFactor * fadeFactor;
      if (dutyF < 0.0f)   dutyF = 0.0f;
      if (dutyF > 100.0f) dutyF = 100.0f;

      applyGatedDuty(dutyF, elapsed, m_purrFreqHz);
      break;
    }

    // ---------------- 裸测 / 短震 ----------------
    case HM_BARE: {
      if (m_bareDurationMs > 0 && (now - m_startMs) >= m_bareDurationMs) {
        off();
        if (Params::get(P_DEBUG_LOG_ENABLED)) Serial.println(F("[HAPTIC] 短震结束"));
      }
      break;
    }

    // ---------------- 扫频 ----------------
    case HM_SWEEP: {
      const uint32_t dwell = now - m_sweepStepStart;
      if (dwell >= SWEEP_DWELL_MS) {
        m_sweepIdx++;
        const uint32_t nextFreq = SWEEP_START_HZ + (uint32_t)m_sweepIdx * SWEEP_STEP_HZ;

        if (nextFreq > SWEEP_END_HZ || m_sweepIdx >= SWEEP_POINTS) {
          Serial.println();
          Serial.println(F("[HAPTIC] ========== 扫频结束 =========="));
          Serial.println(F("[HAPTIC] 请汇总各频点的猫感评分与噪声值"));
          off();
          break;
        }

        m_sweepFreqHz    = nextFreq;
        m_sweepStepStart = now;
        Serial.printf("[HAPTIC] 频点 %u/%u: %lu Hz   ← 请评分并测噪声\n",
                      m_sweepIdx + 1, (unsigned)SWEEP_POINTS, (unsigned long)m_sweepFreqHz);
        break;   // 本 tick 不输出，下一 tick 起按新频率门控
      }

      // 频点内加轻微强度起伏，避免"死震"，更接近真实咕噜
      const float base = (float)Params::get(P_PURR_DUTY_BASE_PCT);
      const float ph   = (float)(dwell % 500) / 500.0f;
      const float jit  = 1.0f + sinf(2.0f * PI * ph) * 0.12f;
      float dutyF = base * jit;
      if (dutyF < 0.0f)   dutyF = 0.0f;
      if (dutyF > 100.0f) dutyF = 100.0f;

      applyGatedDuty(dutyF, dwell, m_sweepFreqHz);
      break;
    }

    // ---------------- 空闲 ----------------
    case HM_IDLE:
    default:
      break;
  }

  m_lastTick = now;
}
