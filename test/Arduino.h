/*
 * ============================================================================
 *  test/Arduino.h —— 本机验证用的最小 Arduino 桩（头文件）
 *
 *  目的：让固件的纯逻辑部分（参数校验、包络数学、心情状态机、呼吸触发链路、
 *        咕噜闸门、摇尾过渡、CLI 解析）能在电脑上用 g++ 编译并运行，
 *        不需要 ESP32 硬件，也不需要安装 Arduino 工具链。
 *
 *  ⚠ 这不是硬件仿真：它不模拟马达惯性、不模拟机构、不产生声音。
 *     它只回答"逻辑对不对"，不回答"手感好不好"。
 * ============================================================================
 */

#ifndef ARDUINO_H_STUB
#define ARDUINO_H_STUB

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 模拟 ESP32 Arduino Core 2.x 分支（决定 config.h 走 ledcSetup 路径）
// ---------------------------------------------------------------------------
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 0
#endif

#ifndef PI
#define PI 3.14159265358979323846f
#endif

// Arduino 的 F() 宏：把字符串常量放到 FLASH。本机验证直接退化为普通字面量。
#ifndef F
#define F(x) (x)
#endif

// 常用类型别名
typedef uint8_t  byte;
typedef bool     boolean;

// ---------------------------------------------------------------------------
// 假时钟：所有测试通过 g_fake_ms 推进时间，不依赖真实时间
// ---------------------------------------------------------------------------
extern uint32_t g_fake_ms;
extern uint32_t g_fake_micros;

unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
void yield();

// ---------------------------------------------------------------------------
// 串口桩：把输出收集到内存，便于断言；输入可由测试注入
// ---------------------------------------------------------------------------
class FakeSerialClass {
public:
  void begin(unsigned long baud) { (void)baud; }

  int  available() { return (int)(m_in.size() - m_inPos); }
  int  read()      { return (m_inPos < m_in.size()) ? (int)(unsigned char)m_in[m_inPos++] : -1; }

  void print(const char* s)         { m_out += s ? s : ""; }
  void print(char c)                { m_out += c; }
  void print(int v)                 { char b[24]; snprintf(b, sizeof(b), "%d", v); m_out += b; }
  void print(unsigned v)            { char b[24]; snprintf(b, sizeof(b), "%u", v); m_out += b; }
  void print(long v)                { char b[24]; snprintf(b, sizeof(b), "%ld", v); m_out += b; }
  void print(unsigned long v)       { char b[24]; snprintf(b, sizeof(b), "%lu", v); m_out += b; }
  void print(double v)              { char b[32]; snprintf(b, sizeof(b), "%.2f", v); m_out += b; }

  void println()                    { m_out += '\n'; }
  void println(const char* s)       { print(s); m_out += '\n'; }
  void println(char c)              { print(c); m_out += '\n'; }
  void println(int v)               { print(v); m_out += '\n'; }
  void println(unsigned v)          { print(v); m_out += '\n'; }
  void println(long v)              { print(v); m_out += '\n'; }
  void println(unsigned long v)     { print(v); m_out += '\n'; }

  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  // --- 测试辅助 ---
  // 追加输入（保留已消费位置，避免重复处理旧命令）
  void feed(const char* text) { m_in += text; }
  void clearInput()           { m_in.clear(); m_inPos = 0; }
  const std::string& output() const { return m_out; }
  std::string takeOutput()    { std::string s = m_out; m_out.clear(); return s; }
  void clearOutput()          { m_out.clear(); }
  bool outputHas(const char* needle) const { return m_out.find(needle) != std::string::npos; }
  size_t outputCount(const char* needle) const {
    size_t n = 0, pos = 0;
    const std::string nd(needle);
    while ((pos = m_out.find(nd, pos)) != std::string::npos) { n++; pos += nd.size(); }
    return n;
  }

private:
  std::string m_out;
  std::string m_in;
  size_t      m_inPos = 0;
};

extern FakeSerialClass Serial;

// ---------------------------------------------------------------------------
// GPIO / LEDC 桩
//   记录最近一次写入，供测试断言"驱动频率/占空比确实落到了输出上"
// ---------------------------------------------------------------------------
#define INPUT  0
#define OUTPUT 1
#define HIGH   1
#define LOW    0

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
int  digitalRead(uint8_t pin);

void ledcSetup(uint8_t channel, uint32_t freq, uint8_t resolutionBits);
void ledcAttachPin(uint8_t pin, uint8_t channel);
void ledcWrite(uint8_t channel, uint32_t duty);
// Core 3.x 风格 API（本机桩同时提供，保证两个分支都能编译）
void ledcAttach(uint8_t pin, uint32_t freq, uint8_t resolutionBits);

struct LedcWriteRecord { uint8_t channel; uint32_t duty; uint32_t atMs; };
extern std::vector<LedcWriteRecord> g_ledcWrites;
uint32_t g_ledcAverageDuty(uint8_t channel, uint32_t fromMs, uint32_t toMs);

#endif  // ARDUINO_H_STUB
