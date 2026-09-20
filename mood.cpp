/*
 * ============================================================================
 *  mood.cpp —— 心情值引擎实现
 *  gruntoy v1.0
 * ============================================================================
 */

#include "mood.h"
#include "params.h"
#include "config.h"

MoodEngine g_mood;

// ---------------------------------------------------------------------------
// 名称表
// ---------------------------------------------------------------------------
const char* stateName(HapticState s) {
  switch (s) {
    case HS_ACCUM:     return "ACCUM";
    case HS_PURR:      return "PURR";
    case HS_LANDING:   return "LANDING";
    case HS_COOLDOWN:  return "COOLDOWN";
    case HS_LOWPOWER:  return "LOWPOWER";
    case HS_UPGRADING: return "UPGRADING";
    default:           return "UNKNOWN";
  }
}

const char* purrGateName(PurrGate g) {
  switch (g) {
    case PG_OPEN:       return "OPEN";
    case PG_GAP:        return "GAP(P-30)";
    case PG_CUMULATIVE: return "CUMULATIVE(P-31)";
    case PG_SYSTEM:     return "SYSTEM";
    default:            return "?";
  }
}

// ---------------------------------------------------------------------------
// 降落曲线（P-16）
// ---------------------------------------------------------------------------
static float easeCurve(float t, int32_t curve) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  switch (curve) {
    case LC_LINEAR:
      return t;
    case LC_EASE_OUT: {            // 起步快、收尾慢
      const float u = 1.0f - t;
      return 1.0f - u * u * u;
    }
    case LC_EASE_IN_OUT:           // 两端导数为 0（默认）
    default:
      return t * t * (3.0f - 2.0f * t);
  }
}

// 按 N 段线性插值近似曲线（对应 P-17「分段线性近似段数」）
static float piecewiseCurve(float t, int N, int32_t curve) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  if (N < 1) N = 1;

  const float s = t * (float)N;
  int k = (int)s;
  if (k >= N) return 1.0f;

  const float frac = s - (float)k;
  const float f0 = easeCurve((float)k / (float)N, curve);
  const float f1 = easeCurve((float)(k + 1) / (float)N, curve);
  return f0 + (f1 - f0) * frac;
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
void MoodEngine::init(HapticEngine* haptic, TailDriver* tail) {
  m_haptic = haptic;
  m_tail   = tail;

  m_mood = (int16_t)Params::get(P_MOOD_INITIAL);
  const int16_t lo = (int16_t)Params::get(P_MOOD_DECAY_FLOOR);
  const int16_t hi = (int16_t)Params::get(P_MOOD_MAX);
  if (m_mood < lo) m_mood = lo;
  if (m_mood > hi) m_mood = hi;

  m_landingTo = (int16_t)Params::get(P_MOOD_RESTORE_VALUE);

  m_state      = HS_ACCUM;
  m_stateStart = millis();
  resetAccumulators(millis());

  m_tailGear = 1;
  updateTailGear(false);                 // 首次对齐不做过过渡
  if (m_tail) m_tail->snapToGear(m_tailGear);

  Serial.printf("[mood] init: mood=%d state=%s tail=W%d (threshold=%d restore=%d cooldown=%lus)\n",
                m_mood, stateName(m_state), m_tailGear,
                (int)Params::get(P_PURR_TRIGGER_THRESHOLD),
                (int)Params::get(P_MOOD_RESTORE_VALUE),
                (unsigned long)((uint32_t)Params::get(P_COOLDOWN_DURATION_MS) / 1000));
}

void MoodEngine::resetAccumulators(uint32_t now) {
  m_lastStrokeMs      = now;
  m_lastDecayMs       = now;
  m_continuousStartMs = now;
  m_lastBonusMs       = now;
}

// ---------------------------------------------------------------------------
// 状态切换（唯一入口）
// ---------------------------------------------------------------------------
void MoodEngine::enterState(HapticState s) {
  if (m_state == s) return;

  Serial.printf("[mood] state %s -> %s (mood=%d, elapsed=%lums)\n",
                stateName(m_state), stateName(s), m_mood,
                (unsigned long)(millis() - m_stateStart));

  const HapticState prev = m_state;
  m_state      = s;
  m_stateStart = millis();

  if (s == HS_PURR) m_purrCount++;

  // 进入冷却时冻结衰减计时（FR-37c：冷却期不受衰减影响）
  if (s == HS_COOLDOWN) {
    m_lastPurrEndMs = millis();
    resetAccumulators(millis());
  }
  // 从安全态回到累积态时重置计时，避免"回来就立刻衰减"
  if (s == HS_ACCUM && (prev == HS_LOWPOWER || prev == HS_UPGRADING)) {
    resetAccumulators(millis());
  }
}

// ---------------------------------------------------------------------------
// 咕噜时长映射（FR-37a）
//
//  【tiered 模式】D0~D3 是分段线性曲线上的四个锚点：
//    · 端点严格落在 D0 / D3（调参时可精确指定首尾手感）
//    · 全程无跳变（初版"档内插值 + 档间跳变"在 104→105 处单点跳 10s，
//      违反 FR-40，已在 PoC 阶段修正）
//    · 仍只由 4 个时长参数控制，不增加参数数量
//    · bx3~b4 段刻意保持 D3（平台）：继续外推就需要"终点时长"参数，
//      会让 D3 的语义变模糊；平台是显式设计选择，见 CHANGELOG OP-02
//
//  【linear 模式】duration = intercept + slope × (mood − b0)，截断到上限
// ---------------------------------------------------------------------------
uint32_t MoodEngine::computePurrDuration(int16_t mood) {
  const int32_t dmax = Params::get(P_PURR_DURATION_MAX_MS);
  const int16_t b0   = (int16_t)Params::get(P_PURR_TIER_B0);

  // ---- 线性模式 ----
  if (Params::get(P_PURR_DURATION_MODE) == PDM_LINEAR) {
    int32_t d = Params::get(P_PURR_LINEAR_INTERCEPT_MS) +
                Params::get(P_PURR_LINEAR_SLOPE) * ((int32_t)mood - (int32_t)b0);
    if (d < 1000) d = 1000;
    if (d > dmax) d = dmax;
    return (uint32_t)d;
  }

  // ---- 锚点分段模式 ----
  const int16_t bx[4] = {
    (int16_t)Params::get(P_PURR_TIER_B0),
    (int16_t)Params::get(P_PURR_TIER_B1),
    (int16_t)Params::get(P_PURR_TIER_B2),
    (int16_t)Params::get(P_PURR_TIER_B3)
  };
  const int16_t  bEnd = (int16_t)Params::get(P_PURR_TIER_B4);
  const uint32_t dy[4] = {
    (uint32_t)Params::get(P_PURR_TIER_D0),
    (uint32_t)Params::get(P_PURR_TIER_D1),
    (uint32_t)Params::get(P_PURR_TIER_D2),
    (uint32_t)Params::get(P_PURR_TIER_D3)
  };

  if (mood <= bx[0]) return dy[0];
  if (mood >= bEnd)  return dy[3];

  int i = 0;
  while (i < 3 && mood >= bx[i + 1]) i++;
  if (i >= 3) return dy[3];              // 高位平台段

  const int16_t  lo_ = bx[i];
  const int16_t  hi_ = bx[i + 1];
  const uint32_t dLo = dy[i];
  const uint32_t dHi = dy[i + 1];
  if (hi_ <= lo_) return dLo;            // 防御：边界相等时不做除法

  const uint32_t span = (uint32_t)(hi_ - lo_);
  const uint32_t off  = (uint32_t)(mood - lo_);
  return dLo + (uint32_t)((int64_t)((int32_t)dHi - (int32_t)dLo) * (int64_t)off / (int64_t)span);
}

// ---------------------------------------------------------------------------
// 降落插值（FR-40）
// ---------------------------------------------------------------------------
int16_t MoodEngine::landingValue(uint32_t elapsedMs) {
  const uint32_t dur = (uint32_t)Params::get(P_MOOD_LANDING_MS);
  if (dur == 0) return m_landingTo;

  const float t = (float)elapsedMs / (float)dur;
  if (t >= 1.0f) return m_landingTo;

  const int32_t N = Params::get(P_MOOD_LANDING_SEGMENTS);
  const int32_t curve = Params::get(P_MOOD_LANDING_CURVE);
  const float f = piecewiseCurve(t, (int)N, curve);

  const int32_t v = (int32_t)m_landingFrom +
                    (int32_t)((float)(m_landingTo - m_landingFrom) * f);
  return (int16_t)v;
}

int16_t MoodEngine::debugLandingValue(int16_t from, uint32_t elapsedMs) {
  const int16_t saved = m_landingFrom;
  m_landingFrom = from;
  const int16_t v = landingValue(elapsedMs);
  m_landingFrom = saved;
  return v;
}

// ---------------------------------------------------------------------------
// 摇尾档位更新（FR-39 + P-50 迟滞）
//   allowTransition=false 时只重算档位不触发过渡（初始化用）。
//   purrExit=true 时该次切换使用 P-52 时长（咕噜后缓降）。
//   迟滞：升档需 mood ≥ 边界 + h，降档需 mood ≤ 边界 − h，
//         带宽 2h 内维持原档，防止边界抖动导致档位反复横跳（RK-21）。
// ---------------------------------------------------------------------------
void MoodEngine::updateTailGear(bool allowTransition, bool purrExit) {
  if (Params::get(P_TAIL_MODE_ENABLED) == 0) return;

  const int16_t t1 = (int16_t)Params::get(P_TAIL_TIER_B0);   // 升入 W2
  const int16_t t2 = (int16_t)Params::get(P_TAIL_TIER_B1);   // 升入 W3
  const int16_t t3 = (int16_t)Params::get(P_TAIL_TIER_B2);   // 升入 W4
  const int16_t h  = (int16_t)Params::get(P_TAIL_HYSTERESIS);

  uint8_t g = m_tailGear;
  const int16_t m = m_mood;

  switch (m_tailGear) {
    case 1: if (m >= t1 + h) g = 2; break;
    case 2: if (m >= t2 + h)      g = 3;
            else if (m <= t1 - h) g = 1;
            break;
    case 3: if (m >= t3 + h)      g = 4;
            else if (m <= t2 - h) g = 2;
            break;
    case 4: if (m <= t3 - h)      g = 3; break;
    default: g = 1; break;
  }

  if (g == m_tailGear) return;

  const uint8_t old = m_tailGear;
  m_tailGear = g;
  if (m_tail) m_tail->setGear(g, purrExit);

  if (allowTransition) {
    Serial.printf("[mood] tail W%u -> W%u (mood=%d, %ld.%02ldHz)\n",
                  (unsigned)old, (unsigned)g, m,
                  (long)(getTailFreqHz100() / 100), (long)(getTailFreqHz100() % 100));
  }
}

int32_t MoodEngine::getTailFreqHz100() const {
  if (m_tail) return m_tail->getTargetFreqHz100();
  switch (m_tailGear) {
    case 1:  return Params::get(P_TAIL_FREQ_W1);
    case 2:  return Params::get(P_TAIL_FREQ_W2);
    case 3:  return Params::get(P_TAIL_FREQ_W3);
    case 4:  return Params::get(P_TAIL_FREQ_W4);
    default: return Params::get(P_TAIL_FREQ_W1);
  }
}

// ---------------------------------------------------------------------------
// 呼吸触发链路（FR-35 + PRD §9.6）
//   条件：① 处于 ACCUM（咕噜/降落/冷却一律不启动呼吸）
//         ② 震动通道未被咕噜/扫频占用，且当前不在呼吸中
//         ③ 距上次呼吸触发 ≥ P-43
//   档位：按心情值落在哪一段摇尾分界上取 L1/L2/L3，
//         复用已有的活动度分界，避免再引入一套阈值。
// ---------------------------------------------------------------------------
uint8_t MoodEngine::breathLevelFor(int16_t mood) const {
  if (mood < (int16_t)Params::get(P_TAIL_TIER_B0)) return 1;
  if (mood < (int16_t)Params::get(P_TAIL_TIER_B1)) return 2;
  return 3;
}

void MoodEngine::maybeTriggerBreath(uint32_t now) {
  if (!m_haptic) return;
  if (m_state != HS_ACCUM) return;
  if (m_haptic->isPurring() || m_haptic->isSweeping()) return;
  if (m_haptic->getMode() == HM_BREATH) return;      // 已在呼吸，不叠加

  const uint32_t minInt = (uint32_t)Params::get(P_BREATH_MIN_INTERVAL_MS);
  if (m_lastBreathMs != 0 && (now - m_lastBreathMs) < minInt) {
    if (Params::get(P_DEBUG_LOG_ENABLED)) {
      Serial.printf("[mood] breath deferred (距上次 %lums < %lums, P-43)\n",
                    (unsigned long)(now - m_lastBreathMs), (unsigned long)minInt);
    }
    return;
  }

  const uint8_t lv = breathLevelFor(m_mood);
  if (m_haptic->startBreath(lv)) {
    m_lastBreathMs = now;
    m_breathCount++;
    Serial.printf("[mood] breath triggered L%u (mood=%d)\n", lv, m_mood);
  }
}

// ---------------------------------------------------------------------------
// 咕噜闸门（FR-36 ⑤⑥）
// ---------------------------------------------------------------------------
PurrGate MoodEngine::purrGateState() const {
  if (m_state == HS_LOWPOWER || m_state == HS_UPGRADING) return PG_SYSTEM;
  if (m_purrLockedOut) return PG_CUMULATIVE;

  const uint32_t gap = (uint32_t)Params::get(P_PURR_BETWEEN_GAP_MS);
  if (m_purrCount > 0 && (millis() - m_lastPurrEndMs) < gap) return PG_GAP;
  return PG_OPEN;
}

// ---------------------------------------------------------------------------
// 抚摸事件（核心交互入口）
// ---------------------------------------------------------------------------
bool MoodEngine::onStroke() {
  const uint32_t now = millis();
  m_totalStrokes++;

  // 任何一次交互都重置"摇尾无互动停摆"计时
  if (m_tail) m_tail->notifyInteraction();

  // --- 咕噜中：不叠加（时长在触发瞬间已锁定，FR-37a）---
  if (m_state == HS_PURR) return false;

  // --- 降落中：按 FR-37c / FR-40④ 不计分，但给一次柔震表示"感受到了" ---
  if (m_state == HS_LANDING) {
    if (Params::get(P_MOOD_LANDING_BLOCK_GAIN) != 0) {
      m_landingTouchDuring = true;
      if (m_haptic) {
        m_haptic->startBuzz((uint16_t)Params::get(P_MOOD_LANDING_TOUCH_BUZZ_MS),
                            (uint8_t)Params::get(P_BREATH_L1_DUTY_PCT));
      }
      Serial.println(F("[mood] stroke ignored (LANDING) - soft feedback given"));
      return false;
    }
  }

  // --- 冷却中：拒绝，给比柔震更重的拒绝震（FR-37b③）---
  if (m_state == HS_COOLDOWN) {
    if (m_haptic) {
      m_haptic->startBuzz((uint16_t)Params::get(P_COOLDOWN_DENY_BUZZ_MS),
                          (uint8_t)Params::get(P_BREATH_L2_DUTY_PCT));
    }
    const uint32_t left = ((uint32_t)Params::get(P_COOLDOWN_DURATION_MS) > getStateElapsedMs())
                          ? ((uint32_t)Params::get(P_COOLDOWN_DURATION_MS) - getStateElapsedMs()) : 0;
    Serial.printf("[mood] stroke denied (COOLDOWN, %lus left)\n", (unsigned long)(left / 1000));
    return false;
  }

  // --- 低电 / 升级中：一律不受理（PRD §9.5 优先级块）---
  if (m_state == HS_LOWPOWER || m_state == HS_UPGRADING) {
    Serial.println(F("[mood] stroke ignored (system state)"));
    return false;
  }

  // ===== 以下为 ACCUM：正常累积 =====

  // 防抖 / 有效性判定：
  //   基线以"事件"模拟抚摸（没有真实触摸时长），因此取 P-06 与 P-05 的较大者
  //   作为两次有效抚摸的最小间隔。接入真实触摸通道后，P-05 应改由
  //   触摸驱动的"按下持续时长 ≥ pet_valid_min_ms"判定（见 CHANGELOG OP-01）。
  //   首次抚摸不做防抖判定——开箱第一下必须响应，否则用户判定设备无反应。
  uint32_t minInterval = (uint32_t)Params::get(P_PET_DEBOUNCE_MS);
  const uint32_t validMin = (uint32_t)Params::get(P_PET_VALID_MIN_MS);
  if (validMin > minInterval) minInterval = validMin;

  const uint32_t gapMs = (uint32_t)Params::get(P_MOOD_DECAY_START_MS);
  const bool     firstStroke = !m_everStroked;

  if (now - m_lastStrokeMs > gapMs) {
    m_continuousStartMs = now;          // 中断过久，连续抚摸重新计时
    m_lastBonusMs       = now;
  } else if (!firstStroke && (now - m_lastStrokeMs) < minInterval) {
    if (Params::get(P_DEBUG_LOG_ENABLED)) {
      Serial.println(F("[mood] stroke filtered (too fast, debounce)"));
    }
    return false;
  }
  m_lastStrokeMs = now;
  m_everStroked  = true;

  // 连续抚摸加速增益（P-07/P-08/P-09）
  int16_t gain = (int16_t)Params::get(P_PET_GAIN_PER_STROKE);
  if (now - m_continuousStartMs >= (uint32_t)Params::get(P_PET_CONTINUOUS_WINDOW_MS)) {
    if (now - m_lastBonusMs >= (uint32_t)Params::get(P_PET_CONTINUOUS_INTERVAL_MS)) {
      gain += (int16_t)Params::get(P_PET_CONTINUOUS_BONUS);
      m_lastBonusMs = now;
      if (Params::get(P_DEBUG_LOG_ENABLED)) Serial.println(F("[mood] continuous bonus applied"));
    }
  }

  const int16_t maxv = (int16_t)Params::get(P_MOOD_MAX);
  m_mood += gain;
  if (m_mood > maxv) m_mood = maxv;
  m_countedStrokes++;

  Serial.printf("[mood] stroke +%d -> mood=%d (state=%s)\n", gain, m_mood, stateName(m_state));

  // --- 达标触发咕噜（FR-37a）---
  const int16_t threshold = (int16_t)Params::get(P_PURR_TRIGGER_THRESHOLD);
  if (m_mood >= threshold) {
    const PurrGate gate = purrGateState();
    if (gate != PG_OPEN) {
      Serial.printf("[mood] PURR suppressed by %s (mood=%d, accum=%lums)\n",
                    purrGateName(gate), m_mood, (unsigned long)m_purrAccumMs);
      updateTailGear(true);
    } else {
      m_purrTargetMs = computePurrDuration(m_mood);
      m_purrAccumMs += m_purrTargetMs;
      if (m_purrAccumMs >= (uint32_t)Params::get(P_PURR_CUMULATIVE_LIMIT_MS)) {
        m_purrLockedOut = true;   // 本轮照常跑完，之后强制休息
      }

      enterState(HS_PURR);

      // PURR 态锁 W4（FR-39 验收 / §9.5 状态机规则表）
      if (Params::get(P_TAIL_MODE_ENABLED) != 0 && m_tailGear != 4) {
        const uint8_t old = m_tailGear;
        m_tailGear = 4;
        if (m_tail) m_tail->setGear(4);
        Serial.printf("[mood] tail W%u -> W4 (PURR 态锁定, %ld.%02ldHz)\n",
                      (unsigned)old,
                      (long)(getTailFreqHz100() / 100), (long)(getTailFreqHz100() % 100));
      }

      if (m_haptic) m_haptic->startPurr(m_purrTargetMs);

      Serial.printf("[mood] PURR trigger! mood=%d dur=%lums mode=%s accum=%lums lockout=%d\n",
                    m_mood, (unsigned long)m_purrTargetMs,
                    Params::get(P_PURR_DURATION_MODE) == PDM_LINEAR ? "linear" : "tiered",
                    (unsigned long)m_purrAccumMs, m_purrLockedOut ? 1 : 0);
    }
  }

  // 呼吸链路（FR-35 / §9.6）放在咕噜判定之后：
  // 若本次抚摸直接触发咕噜，状态已切到 PURR，breath 会被互斥规则挡下，
  // 避免"先起呼吸、随即被咕噜抢占"的无效启停。
  maybeTriggerBreath(now);

  return true;
}

// ---------------------------------------------------------------------------
// 主循环 tick
// ---------------------------------------------------------------------------
void MoodEngine::tick() {
  const uint32_t now = millis();

  switch (m_state) {

    // ===== 累积中：自然衰减 + 档位跟随 + 闸门复位 =====
    case HS_ACCUM: {
      const uint32_t delayMs = (uint32_t)Params::get(P_MOOD_DECAY_START_MS);
      const uint32_t intvMs  = (uint32_t)Params::get(P_MOOD_DECAY_INTERVAL_MS);
      const int16_t  step    = (int16_t)Params::get(P_MOOD_DECAY_STEP);
      const int16_t  floorv  = (int16_t)Params::get(P_MOOD_DECAY_FLOOR);

      if (now - m_lastStrokeMs > delayMs) {
        if (now - m_lastDecayMs >= intvMs) {
          m_lastDecayMs = now;
          if (m_mood > floorv) {
            m_mood -= step;
            if (m_mood < floorv) m_mood = floorv;
            if (Params::get(P_DEBUG_LOG_ENABLED)) {
              Serial.printf("[mood] decay -%d -> mood=%d\n", step, m_mood);
            }
          }
        }
      } else {
        m_lastDecayMs = now;   // 有人在互动就不衰减
      }

      updateTailGear(true);

      // 闸门复位：休息时间达到 P-30 后清空累计运行时长与锁定标记
      const uint32_t gap = (uint32_t)Params::get(P_PURR_BETWEEN_GAP_MS);
      if (m_purrCount > 0 && (m_purrAccumMs > 0 || m_purrLockedOut) &&
          (now - m_lastPurrEndMs) >= gap) {
        if (m_purrLockedOut) {
          Serial.printf("[mood] purr lockout cleared after %lums rest (P-31)\n",
                        (unsigned long)((now - m_lastPurrEndMs) / 1000));
        }
        m_purrLockedOut = false;
        m_purrAccumMs   = 0;
      }
      break;
    }

    // ===== 咕噜中：等待锁定时长 =====
    case HS_PURR: {
      if (now - m_stateStart >= m_purrTargetMs) {
        m_landingFrom        = m_mood;
        m_landingTo          = (int16_t)Params::get(P_MOOD_RESTORE_VALUE);
        m_landingTouchDuring = false;
        m_lastPurrEndMs      = now;
        enterState(HS_LANDING);
        if (m_haptic) m_haptic->stop();
      }
      break;
    }

    // ===== 降落中：按 P-16 曲线平滑降到 restore =====
    case HS_LANDING: {
      const uint32_t dur = (uint32_t)Params::get(P_MOOD_LANDING_MS);
      const uint32_t el  = now - m_stateStart;

      if (el >= dur) {
        m_mood = m_landingTo;
        enterState(HS_COOLDOWN);
        Serial.printf("[mood] landing done -> mood=%d, cooldown %lus\n",
                      m_mood, (unsigned long)((uint32_t)Params::get(P_COOLDOWN_DURATION_MS) / 1000));
      } else {
        m_mood = landingValue(el);
      }

      // 摇尾同步缓降：本次切换统一用 P-52 时长，与心情降落对齐（FR-40a / C-16）
      updateTailGear(true, /*purrExit=*/true);
      break;
    }

    // ===== 冷却中：锁定 restore，冻结衰减 =====
    case HS_COOLDOWN: {
      if (now - m_stateStart >= (uint32_t)Params::get(P_COOLDOWN_DURATION_MS)) {
        enterState(HS_ACCUM);
        Serial.printf("[mood] cooldown done -> ACCUM (mood=%d)\n", m_mood);
      }
      break;
    }

    // ===== 低电 / 升级：v1.0 占位（需接入电池 ADC / OTA 后启用）=====
    case HS_LOWPOWER:
    case HS_UPGRADING:
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// 调试接口
// ---------------------------------------------------------------------------
void MoodEngine::forceMood(int16_t v) {
  const int16_t lo = (int16_t)Params::get(P_MOOD_DECAY_FLOOR);
  const int16_t hi = (int16_t)Params::get(P_MOOD_MAX);
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  m_mood = v;
  updateTailGear(true);
  Serial.printf("[mood] force mood=%d (state=%s)\n", m_mood, stateName(m_state));
}

void MoodEngine::forceState(HapticState s) {
  enterState(s);
  Serial.printf("[mood] force state=%s (mood=%d)\n", stateName(m_state), m_mood);
}

void MoodEngine::resetState() {
  m_mood  = (int16_t)Params::get(P_MOOD_INITIAL);
  m_state = HS_ACCUM;
  m_stateStart = millis();
  resetAccumulators(millis());
  m_purrAccumMs   = 0;
  m_purrLockedOut = false;
  m_everStroked   = false;
  m_purrCount     = 0;
  m_breathCount   = 0;
  m_totalStrokes  = 0;
  m_countedStrokes= 0;
  m_lastBreathMs  = 0;
  updateTailGear(false);
  if (m_tail) m_tail->snapToGear(m_tailGear);
  Serial.printf("[mood] state reset -> mood=%d state=%s\n", m_mood, stateName(m_state));
}
