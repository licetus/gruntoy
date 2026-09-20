/*
 * ============================================================================
 *  test/check.h —— 本机验证的最小断言框架
 * ============================================================================
 */

#ifndef GRUNTOY_CHECK_H
#define GRUNTOY_CHECK_H

#include <cstdio>
#include <cstring>

#include "Arduino.h"

inline int& chk_pass() { static int v = 0; return v; }
inline int& chk_fail() { static int v = 0; return v; }

#define CHECK(cond, msg)                                                        \
  do {                                                                          \
    if (cond) { chk_pass()++; printf("  \033[32mPASS\033[0m %s\n", (msg)); }     \
    else      { chk_fail()++; printf("  \033[31mFAIL\033[0m %s\n", (msg)); }     \
  } while (0)

#define SECTION(title) printf("\n[%s]\n", (title))

// 推进假时钟
inline void advance(uint32_t ms) {
  g_fake_ms     += ms;
  g_fake_micros += ms * 1000u;
}

// 跑 n 毫秒，每 stepMs 调一次 tick（模拟真实 loop 频率）
template <typename Fn>
inline void runFor(uint32_t ms, uint32_t stepMs, Fn tickFn) {
  for (uint32_t t = 0; t < ms; t += stepMs) {
    advance(stepMs);
    tickFn();
  }
}

inline int report(const char* title) {
  printf("\n===== %s：%d 通过 / %d 失败 =====\n\n", title, chk_pass(), chk_fail());
  return chk_fail() == 0 ? 0 : 1;
}

#endif  // GRUNTOY_CHECK_H
