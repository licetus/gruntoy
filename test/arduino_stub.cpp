/*
 * ============================================================================
 *  test/arduino_stub.cpp —— 本机验证用的 Arduino 桩实现
 * ============================================================================
 */

#include "Arduino.h"

#include <cstdarg>

// ---------------------------------------------------------------------------
// 假时钟
// ---------------------------------------------------------------------------
uint32_t g_fake_ms     = 0;
uint32_t g_fake_micros = 0;

unsigned long millis() { return (unsigned long)g_fake_ms; }
unsigned long micros() { return (unsigned long)g_fake_micros; }

void delay(unsigned long ms) {
  // 假时钟推进：让"等待型"代码在本机测试里也能前进
  g_fake_ms     += (uint32_t)ms;
  g_fake_micros += (uint32_t)ms * 1000u;
}
void delayMicroseconds(unsigned int us) { g_fake_micros += us; }
void yield() { }

// ---------------------------------------------------------------------------
// 串口桩
// ---------------------------------------------------------------------------
FakeSerialClass Serial;

int FakeSerialClass::printf(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  m_out += buf;
  return n;
}

// ---------------------------------------------------------------------------
// GPIO / LEDC 桩
// ---------------------------------------------------------------------------
std::vector<LedcWriteRecord> g_ledcWrites;

static uint8_t  s_lastPin  = 0xFF;
static uint8_t  s_lastMode = 0;
static uint8_t  s_lastDigital = 0;

void pinMode(uint8_t pin, uint8_t mode)   { s_lastPin = pin; s_lastMode = mode; }
void digitalWrite(uint8_t pin, uint8_t v) { s_lastPin = pin; s_lastDigital = v; }
int  digitalRead(uint8_t pin)             { (void)pin; return 0; }

void ledcSetup(uint8_t channel, uint32_t freq, uint8_t resolutionBits) {
  (void)channel; (void)freq; (void)resolutionBits;
}
void ledcAttachPin(uint8_t pin, uint8_t channel) { (void)pin; (void)channel; }
void ledcAttach(uint8_t pin, uint32_t freq, uint8_t resolutionBits) {
  (void)pin; (void)freq; (void)resolutionBits;
}

void ledcWrite(uint8_t channel, uint32_t duty) {
  LedcWriteRecord r{channel, duty, g_fake_ms};
  g_ledcWrites.push_back(r);
  if (g_ledcWrites.size() > 200000) g_ledcWrites.erase(g_ledcWrites.begin());
}

// 统计某个通道在 [fromMs, toMs] 时间窗内写入占空比的平均值（0~maxDuty）
uint32_t g_ledcAverageDuty(uint8_t channel, uint32_t fromMs, uint32_t toMs) {
  uint64_t sum = 0;
  uint32_t n   = 0;
  for (const LedcWriteRecord& r : g_ledcWrites) {
    if (r.channel != channel) continue;
    if (r.atMs < fromMs || r.atMs > toMs) continue;
    sum += r.duty;
    n++;
  }
  return n ? (uint32_t)(sum / n) : 0;
}
