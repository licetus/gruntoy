/*
 * ============================================================================
 *  cli.cpp —— 串口调试台实现
 *  gruntoy v1.0
 *
 *  【与 PoC 的差异】
 *   · t feed / t auto 改为非阻塞调度：不再用 delay() 阻塞主循环，
 *     否则状态机（咕噜时长、降落、冷却）在 feed 期间无法推进。
 *   · 抚摸间隔自动取 max(pet_debounce_ms, pet_valid_min_ms)，
 *     避免"看起来摸了 N 次、实际只有 N/3 被计入"的误导。
 *   · 枚举型参数支持按名写入（SET mood_landing_curve ease_in_out）。
 *   · 新增 t state / t tail / t cycle / INFO。
 * ============================================================================
 */

#include "cli.h"
#include "params.h"
#include "config.h"

CliConsole g_cli;

// ---------------------------------------------------------------------------
// FR-38c / RK-23：量产固件必须"不留运行期后门"，因此这里做的是**编译期整体移除**，
// 而不是把命令藏起来。关闭时只保留空的 init/poll —— 前者让 gruntoy.ino 的调用点
// 编译通过，后者让主循环的 g_cli.poll() 链接通过；其余 ~570 行命令实现完全不进
// 二进制，反编译也拿不到调参入口。两种配置都由 test/run_tests.sh 编译验证。
// ---------------------------------------------------------------------------
#if !ENABLE_DEBUG_CHANNEL

void CliConsole::init(HapticEngine*, MoodEngine*, TailDriver*, Preferences*) { }
void CliConsole::poll() { }

#else

void CliConsole::init(HapticEngine* haptic, MoodEngine* mood, TailDriver* tail, Preferences* prefs) {
  m_haptic = haptic;
  m_mood   = mood;
  m_tail   = tail;
  m_prefs  = prefs;
  m_len    = 0;

  Serial.println();
  Serial.println(F("[cli] 调试台就绪，输入 help 查看命令"));
  Serial.printf("[cli] 参数分组: mood=%d purr=%d breath=%d tail=%d sys=%d（合计 %d）\n",
                Params::countGroup(GRP_MOOD), Params::countGroup(GRP_PURR),
                Params::countGroup(GRP_BREATH), Params::countGroup(GRP_TAIL),
                Params::countGroup(GRP_SYS), (int)P_COUNT);
}

// ---------------------------------------------------------------------------
// 非阻塞调度 + 串口逐字节接收
// ---------------------------------------------------------------------------
void CliConsole::scheduleFeed(uint16_t n, uint32_t intervalMs) {
  m_autoRun        = true;
  m_autoRemaining  = n;               // 0 = 无限
  m_autoIntervalMs = intervalMs;
  m_autoNextMs     = millis();
  m_autoIdx        = 0;
}

void CliConsole::poll() {
  // --- 自动抚摸调度 ---
  if (m_autoRun) {
    const uint32_t now = millis();
    if (now >= m_autoNextMs) {
      m_mood->onStroke();
      m_autoIdx++;
      m_autoNextMs = now + m_autoIntervalMs;

      if (m_autoRemaining > 0) {
        m_autoRemaining--;
        if (m_autoRemaining == 0) {
          m_autoRun = false;
          Serial.printf("[feed] 完成 %u 次抚摸：mood=%d state=%s purr=%lu breath=%lu\n",
                        m_autoIdx, m_mood->getMood(), stateName(m_mood->getState()),
                        (unsigned long)m_mood->getPurrCount(),
                        (unsigned long)m_mood->getBreathCount());
        }
      }
      if (m_autoRun && m_autoIdx % 20 == 0) {
        Serial.printf("[auto] #%u mood=%d state=%s tail=W%d purr=%lu breath=%lu accum=%lus\n",
                      m_autoIdx, m_mood->getMood(), stateName(m_mood->getState()),
                      m_mood->getTailGear(), (unsigned long)m_mood->getPurrCount(),
                      (unsigned long)m_mood->getBreathCount(),
                      (unsigned long)(m_mood->getPurrAccumMs() / 1000));
      }
    }
  }

  // 逐字节接收（非阻塞）：\n 成行才分发，退格即时生效，空行不产生任何输出
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();

    if (c == '\r') continue;
    if (c == '\n') {
      m_buf[m_len] = '\0';
      if (m_len > 0) handleLine(m_buf);
      m_len = 0;
      continue;
    }
    if (c == 8 || c == 127) {            // 退格
      if (m_len > 0) m_len--;
      continue;
    }
    if (m_len < CLI_BUF_SIZE - 1) m_buf[m_len++] = c;
  }
}

// ---------------------------------------------------------------------------
// 命令分发
// ---------------------------------------------------------------------------
void CliConsole::handleLine(char* line) {
  while (*line == ' ') line++;
  int n = strlen(line);
  while (n > 0 && line[n - 1] == ' ') line[--n] = '\0';
  if (n == 0) return;

  char cmd[24] = {0};
  const char* p = line;
  int ci = 0;
  while (*p && *p != ' ' && ci < 23) cmd[ci++] = *p++;
  while (*p == ' ') p++;
  const char* arg = p;

  for (int i = 0; cmd[i]; i++) cmd[i] = (char)toupper((unsigned char)cmd[i]);

  if (!strcmp(cmd, "HELP") || !strcmp(cmd, "?"))    { cmdHelp();   return; }
  if (!strcmp(cmd, "INFO"))                          { cmdInfo();   return; }
  if (!strcmp(cmd, "STATUS") || !strcmp(cmd, "ST"))  { cmdStatus(); return; }
  if (!strcmp(cmd, "GET"))                           { cmdGet(arg); return; }
  if (!strcmp(cmd, "SET"))                           { cmdSet(arg); return; }
  if (!strcmp(cmd, "RESET"))                         { cmdReset(arg); return; }
  if (!strcmp(cmd, "DUMP"))                          { cmdDump();   return; }
  if (!strcmp(cmd, "PRESET"))                        { cmdPreset(arg); return; }
  if (!strcmp(cmd, "T") || !strcmp(cmd, "TEST"))     { cmdTest(arg); return; }

  if (!strcmp(cmd, "SAVE")) {
    if (Params::save()) Serial.println(F("[cli] 已保存到 NVS"));
    return;
  }
  if (!strcmp(cmd, "LOAD")) {
    if (Params::load()) Serial.println(F("[cli] 已从 NVS 重载（重启后才会全局生效）"));
    return;
  }

  Serial.printf("[cli] 未知命令: %s（输入 help）\n", cmd);
}

// ---------------------------------------------------------------------------
void CliConsole::printHelpLine(const char* c, const char* d) {
  Serial.printf("  %-30s %s\n", c, d);
}

void CliConsole::cmdHelp() {
  Serial.println();
  Serial.println(F("=============== gruntoy v1.0 调试台 ==============="));
  Serial.println(F("[查询]"));
  printHelpLine("help | ?",                   "显示本帮助");
  printHelpLine("info",                       "固件与编译期开关信息");
  printHelpLine("status | st",                "状态快照（心情值/状态/档位/剩余时间/闸门）");
  printHelpLine("GET <group>.*",              "按组列参数（mood/purr/breath/tail/sys/ALL）");
  printHelpLine("GET <param>",                "查单个参数，如 GET purr_trigger_threshold");
  printHelpLine("DUMP",                       "导出全部参数（CSV，直接回填 PRD 附录 C）");
  Serial.println(F("[修改]"));
  printHelpLine("SET <param> <value>",        "改参数（自动跑 C-01~C-18 + X-01~X-03，非法即回滚）");
  printHelpLine("RESET <group>.* | ALL",      "恢复默认值（需 SAVE 落盘）");
  printHelpLine("SAVE / LOAD",                "手动存 / 读 NVS");
  printHelpLine("PRESET gentle|standard|lively", "套用性格档位预设（用户侧唯一可见项）");
  Serial.println(F("[体感测试]"));
  printHelpLine("t breath [1|2|3]",           "呼吸包络（Day4）");
  printHelpLine("t purr [seconds]",           "咕噜震动，不走状态机（Day5）");
  printHelpLine("t sweep / t sweepstop",      "咕噜扫频 25→50Hz（Day5 核心实验）");
  printHelpLine("t bare <duty> / t barestop", "裸马达定占空比（Day2）");
  printHelpLine("t trough <0|20>",            "波谷占空比 A/B 对比（Day2 关键实验）");
  printHelpLine("t gate <freqHz>",            "只改咕噜门控频率，快速找猫感区间");
  printHelpLine("t feed [n] [intervalMs]",    "非阻塞模拟 n 次抚摸，走完整状态机（Day7）");
  printHelpLine("t auto / t stop",            "无限自动抚摸循环（长时间稳定性）");
  printHelpLine("t mood <v>",                 "直接设心情值");
  printHelpLine("t state <名>",               "强制状态：accum/purr/landing/cooldown/lowpower/upgrading");
  printHelpLine("t tail <1-4>",               "直接设摇尾档位");
  printHelpLine("t cycle",                    "从当前心情值跑一次完整生命周期");
  Serial.println(F("==================================================="));
  Serial.println();
}

void CliConsole::cmdInfo() {
  Serial.println();
  Serial.println(F("------------- 固件信息 -------------"));
  Serial.printf("  固件版本      : gruntoy v1.0\n");
  Serial.printf("  参数项数      : %d（P-01~P-58 展开，含 v1.0 补齐项）\n", (int)P_COUNT);
  Serial.printf("  NVS schema    : %d（命名空间 %s）\n", PARAMS_NVS_SCHEMA, PARAMS_NVS_NS);
  Serial.printf("  调试通道      : %s（ENABLE_DEBUG_CHANNEL=%d，编译期）\n",
                ENABLE_DEBUG_CHANNEL ? "开启" : "已编译移除", ENABLE_DEBUG_CHANNEL);
  Serial.printf("  摇尾物理输出  : %s（ENABLE_TAIL_OUTPUT=%d）\n",
                ENABLE_TAIL_OUTPUT ? "开启" : "关闭（仅状态计算）", ENABLE_TAIL_OUTPUT);
  Serial.printf("  马达载波      : %dHz / %dbit（驱动频率由门控调制实现）\n",
                MOTOR_PWM_FREQ, MOTOR_PWM_RES);
  Serial.printf("  配置层级      : %s\n", Params::getLayerName());
  Serial.println(F("------------------------------------"));
  Serial.println();
}

void CliConsole::cmdStatus() {
  const HapticState s = m_mood->getState();
  const int16_t mood  = m_mood->getMood();
  const int32_t mx    = Params::get(P_MOOD_MAX);

  Serial.println();
  Serial.println(F("------------------ 状态快照 ------------------"));
  Serial.printf("  心情值      : %d / %ld\n", mood, (long)mx);

  const int bars = (int)((int32_t)mood * 30 / (mx ? mx : 1));
  Serial.print(F("  进度        : ["));
  for (int i = 0; i < 30; i++) Serial.print(i < bars ? '#' : '.');
  Serial.printf("] %d%%\n", (int)((int32_t)mood * 100 / (mx ? mx : 1)));

  Serial.printf("  状态        : %s（已持续 %lus）\n",
                stateName(s), (unsigned long)(m_mood->getStateElapsedMs() / 1000));

  if (s == HS_PURR) {
    Serial.printf("  咕噜剩余    : %lus / 总 %lus\n",
                  (unsigned long)(m_mood->getPurrTargetMs() > m_mood->getStateElapsedMs()
                                  ? (m_mood->getPurrTargetMs() - m_mood->getStateElapsedMs()) / 1000 : 0),
                  (unsigned long)(m_mood->getPurrTargetMs() / 1000));
  } else if (s == HS_LANDING) {
    const uint32_t dur = (uint32_t)Params::get(P_MOOD_LANDING_MS);
    Serial.printf("  降落剩余    : %lums（曲线=%ld，段数=%ld）\n",
                  (unsigned long)(dur > m_mood->getStateElapsedMs() ? dur - m_mood->getStateElapsedMs() : 0),
                  (long)Params::get(P_MOOD_LANDING_CURVE),
                  (long)Params::get(P_MOOD_LANDING_SEGMENTS));
  } else if (s == HS_COOLDOWN) {
    const uint32_t dur = (uint32_t)Params::get(P_COOLDOWN_DURATION_MS);
    Serial.printf("  冷却剩余    : %lus\n",
                  (unsigned long)((dur > m_mood->getStateElapsedMs() ? dur - m_mood->getStateElapsedMs() : 0) / 1000));
  }

  Serial.printf("  摇尾档位    : W%d @ %ld.%02ldHz  实际 %ld.%02ldHz %s\n",
                m_mood->getTailGear(),
                (long)(m_mood->getTailFreqHz100() / 100), (long)(m_mood->getTailFreqHz100() % 100),
                m_tail ? (long)(m_tail->getFreqHz100() / 100) : 0L,
                m_tail ? (long)(m_tail->getFreqHz100() % 100) : 0L,
                (m_tail && !m_tail->isActive()) ? "[已停摆]" : "");

  Serial.printf("  震动模式    : %s", m_haptic->modeName());
  if (m_haptic->getDriveFreqHz()) Serial.printf(" @ %luHz 门控", (unsigned long)m_haptic->getDriveFreqHz());
  Serial.printf("  当前占空比 %u%%\n", m_haptic->getCurrentDutyPct());

  Serial.printf("  咕噜闸门    : %s（累计 %lus / 上限 %lus）\n",
                purrGateName(m_mood->purrGateState()),
                (unsigned long)(m_mood->getPurrAccumMs() / 1000),
                (unsigned long)((uint32_t)Params::get(P_PURR_CUMULATIVE_LIMIT_MS) / 1000));

  Serial.printf("  累计抚摸    : %u 次（计入 %u）\n",
                m_mood->getTotalStrokes(), m_mood->getCountedStrokes());
  Serial.printf("  累计咕噜    : %lu 次 | 呼吸 %lu 次\n",
                (unsigned long)m_mood->getPurrCount(), (unsigned long)m_mood->getBreathCount());
  Serial.printf("  参数层      : %s | 预设: %s\n",
                Params::getLayerName(), Params::getPresetName());
  Serial.println(F("---------------------------------------------"));
  Serial.println();
}

// ---------------------------------------------------------------------------
void CliConsole::cmdGet(const char* arg) {
  if (!*arg) { Serial.println(F("[cli] 用法: GET <param> 或 GET <group>.*")); return; }

  if (!strcasecmp(arg, "ALL") || !strcmp(arg, "*")) { Params::printAll(); return; }

  char g[8] = {0};
  int gi = 0;
  for (const char* q = arg; *q && *q != '.' && gi < 7; q++) g[gi++] = *q;

  if (strchr(arg, '.') || Params::countGroup(g) > 0) { Params::printGroup(g); return; }

  const ParamId id = Params::findByName(arg);
  if (id == P_COUNT) { Serial.printf("[cli] 无此参数: %s\n", arg); return; }

  const char* ch = Params::getChoices(id);
  Serial.printf("  %-31s = %ld  [%ld ~ %ld]  组 %s\n    %s%s%s\n",
                Params::getName(id), (long)Params::get(id),
                (long)Params::getMin(id), (long)Params::getMax(id),
                Params::getGroup(id), Params::getDesc(id),
                ch ? "  可选: " : "", ch ? ch : "");
}

// ---------------------------------------------------------------------------
void CliConsole::cmdSet(const char* arg) {
  if (!*arg) { Serial.println(F("[cli] 用法: SET <param> <value>")); return; }

  char name[40] = {0};
  int ni = 0;
  const char* p = arg;
  while (*p && *p != ' ' && ni < 39) name[ni++] = *p++;
  while (*p == ' ') p++;

  if (!*p) { Serial.printf("[cli] 缺少值: SET %s <value>\n", name); return; }

  const ParamId id = Params::findByName(name);
  if (id == P_COUNT) { Serial.printf("[cli] 无此参数: %s\n", name); return; }

  // 值解析：数字优先，非数字文本再按枚举名解析。
  // ⚠ 不能直接调 parseChoice()：它对标量参数（choices == nullptr）一律返回 false，
  //   若用它当通用解析入口，所有数值型参数都会写不进去——而数值型恰恰是 M1
  //   调参的绝大多数。这里先做数字判定，只有纯文本才交给 parseChoice。
  int32_t raw    = 0;
  bool    parsed = false;          // ★ 必须从 false 起，缺值已在上面拦掉
  {
    bool numeric = true;
    for (const char* q = p; *q; q++) {
      if (isdigit((unsigned char)*q)) continue;
      if (*q == '-' && q == p) continue;      // 允许前导负号
      numeric = false;
      break;
    }
    if (numeric)                                { raw = (int32_t)atol(p); parsed = true; }
    else if (Params::parseChoice(id, p, &raw))  { parsed = true; }
  }

  if (!parsed) {
    Serial.printf("[cli] ✗ 取值非法: %s = %s", name, p);
    const char* ch = Params::getChoices(id);
    if (ch) Serial.printf("（可选: %s）", ch);
    Serial.println();
    return;
  }

  const char* failRule = Params::validateSet(id, raw);
  if (failRule) {
    Serial.printf("[cli] ✗ SET 被拒绝: %s = %ld\n", name, (long)raw);
    Serial.printf("      违反规则 %s：%s\n", failRule, Params::ruleText(failRule));
    Serial.printf("      当前值保持: %ld\n", (long)Params::get(id));
    return;
  }

  Params::set(id, raw);   // 通过校验后正式写入（含钳制）
  const int32_t applied = Params::get(id);
  Serial.printf("[cli] ✓ %s = %ld  [%ld ~ %ld]\n", name, (long)applied,
                (long)Params::getMin(id), (long)Params::getMax(id));
  if (applied != raw) Serial.printf("      （输入 %ld 已被钳制到合法区间）\n", (long)raw);

  if (id == P_MOOD_RESTORE_VALUE) {
    Serial.printf("      注意: restore(%ld) 必须 < threshold(%ld)，否则形成无限咕噜循环（C-02）\n",
                  (long)applied, (long)Params::get(P_PURR_TRIGGER_THRESHOLD));
  }
  if (id == P_PURR_TRIGGER_THRESHOLD) {
    Serial.printf("      提示: 阈值改变后，purr_tier_b0 需同步（X-01），否则 SET 会被拒绝\n");
  }
}

void CliConsole::cmdReset(const char* arg) {
  if (!*arg) { Serial.println(F("[cli] 用法: RESET <group>.*  或 RESET ALL")); return; }

  if (!strcasecmp(arg, "ALL")) {
    const char* g[8];
    const int n = Params::groupNames(g, 8);
    for (int i = 0; i < n; i++) Params::resetGroup(g[i]);
    Serial.println(F("[cli] 全部参数已恢复默认。若需断电保持请执行 SAVE"));
    return;
  }

  char g[16] = {0};
  int gi = 0;
  for (const char* q = arg; *q && *q != '.' && gi < 15; q++) g[gi++] = *q;
  Params::resetGroup(g);
  Serial.println(F("[cli] 若需断电保持请执行 SAVE"));
}

void CliConsole::cmdDump() {
  Serial.println();
  Serial.println(F("### PARAM_DUMP_BEGIN ###"));
  Serial.printf("# firmware=gruntoy-v1.0 layer=%s preset=%s count=%d\n",
                Params::getLayerName(), Params::getPresetName(), (int)P_COUNT);
  Serial.println(F("name,group,value,min,max"));
  for (uint8_t i = 0; i < P_COUNT; i++) {
    const ParamId id = (ParamId)i;
    Serial.printf("%s,%s,%ld,%ld,%ld\n",
                  Params::getName(id), Params::getGroup(id),
                  (long)Params::get(id), (long)Params::getMin(id), (long)Params::getMax(id));
  }
  Serial.println(F("### PARAM_DUMP_END ###"));
  Serial.printf("# 共 %d 项\n", (int)P_COUNT);
  Serial.println();
}

void CliConsole::cmdPreset(const char* arg) {
  if (!*arg) {
    Serial.printf("[cli] 当前预设: %s\n", Params::getPresetName());
    Serial.println(F("[cli] 可选: gentle / standard / lively"));
    return;
  }
  if (Params::applyPreset(arg)) {
    Serial.printf("[cli] ✓ 已套用预设: %s\n", Params::getPresetName());
    Serial.printf("      threshold=%ld restore=%ld cooldown=%lus tier_b0=%ld\n",
                  (long)Params::get(P_PURR_TRIGGER_THRESHOLD),
                  (long)Params::get(P_MOOD_RESTORE_VALUE),
                  (long)(Params::get(P_COOLDOWN_DURATION_MS) / 1000),
                  (long)Params::get(P_PURR_TIER_B0));
    Serial.println(F("      （参数需 SAVE 才会断电保持）"));
  } else {
    Serial.printf("[cli] ✗ 无此预设: %s（可选 gentle / standard / lively）\n", arg);
  }
}

// ---------------------------------------------------------------------------
void CliConsole::cmdTest(const char* arg) {
  if (!*arg) {
    Serial.println(F("[cli] t 子命令: breath | purr | sweep | sweepstop | bare | barestop |"));
    Serial.println(F("      trough | gate | feed | auto | stop | mood | state | tail | cycle"));
    return;
  }

  char sub[16] = {0};
  int si = 0;
  const char* p = arg;
  while (*p && *p != ' ' && si < 15) sub[si++] = *p++;
  while (*p == ' ') p++;
  for (int i = 0; sub[i]; i++) sub[i] = (char)tolower((unsigned char)sub[i]);

  // --- t breath [level] ---
  if (!strcmp(sub, "breath")) {
    uint8_t lv = 2;
    if (*p) lv = (uint8_t)atoi(p);
    if (lv < 1 || lv > 3) { Serial.println(F("[cli] 呼吸档位需 1~3")); return; }
    if (m_haptic->startBreath(lv)) {
      Serial.printf("[cli] 呼吸启动 L%d —— 周期 %.2fs，波谷占空比 %ld%%（0=完全断电）\n",
                    lv, (double)Params::get(P_BREATH_PERIOD_MS) / 1000.0,
                    (long)Params::get(P_BREATH_TROUGH_DUTY_PCT));
    } else {
      Serial.println(F("[cli] 呼吸未启动：咕噜/扫频占用通道（互斥，咕噜优先）"));
    }
    return;
  }

  // --- t purr [seconds] ---
  if (!strcmp(sub, "purr")) {
    uint32_t sec = *p ? (uint32_t)atol(p) : 30;
    if (sec == 0) sec = 30;
    const uint32_t maxMs = (uint32_t)Params::get(P_PURR_DURATION_MAX_MS);
    if (sec * 1000UL > maxMs) {
      Serial.printf("[cli] 超过上限 %lus，已截断\n", (unsigned long)(maxMs / 1000));
      sec = maxMs / 1000;
    }
    m_haptic->startPurr(sec * 1000UL);
    Serial.printf("[cli] 咕噜启动 %lus —— 门控 %ld.%02ldHz，基准占空比 %ld%%，抖动 %.0f%%/%.1fs，长包络 %.0fs\n",
                  (unsigned long)sec,
                  (long)(Params::get(P_PURR_DRIVE_FREQ_HZ) / 100),
                  (long)(Params::get(P_PURR_DRIVE_FREQ_HZ) % 100),
                  (long)Params::get(P_PURR_DUTY_BASE_PCT),
                  (double)Params::get(P_PURR_DUTY_JITTER_PCT),
                  (double)Params::get(P_PURR_JITTER_PERIOD_MS) / 1000.0,
                  (double)Params::get(P_PURR_LONG_ENVELOPE_MS) / 1000.0);
    return;
  }

  // --- t sweep / t sweepstop ---
  if (!strcmp(sub, "sweep")) {
    if (m_haptic->startSweep()) {
      Serial.println(F("[cli] 咕噜扫频开始（每点 10s，请逐点记录）"));
      Serial.println(F("      评价维度：像猫咕噜(1~5) / 噪声(dB) / 是否有杂音"));
    } else {
      Serial.println(F("[cli] 扫频未启动：咕噜占用通道（咕噜优先）"));
    }
    return;
  }
  if (!strcmp(sub, "sweepstop")) {
    m_haptic->stop();
    Serial.println(F("[cli] 扫频已停止"));
    return;
  }

  // --- t bare / t barestop ---
  if (!strcmp(sub, "bare")) {
    if (!*p) { Serial.println(F("[cli] 用法: t bare <duty 0~100>")); return; }
    int d = atoi(p);
    if (d < 0) d = 0;
    if (d > 100) d = 100;
    m_haptic->startBare((uint8_t)d);
    Serial.printf("[cli] 裸马达启动 duty=%d%%（持续输出，t barestop 停止）\n", d);
    Serial.println(F("      Day2 任务：从 5% 起每次 +5%，找到「起转阈值」并记录"));
    return;
  }
  if (!strcmp(sub, "barestop")) {
    m_haptic->stop();
    Serial.println(F("[cli] 裸马达已停止（占空比归 0）"));
    return;
  }

  // --- t trough <0|20> ---
  if (!strcmp(sub, "trough")) {
    if (!*p) {
      Serial.println(F("[cli] 用法: t trough <0|20>"));
      Serial.println(F("      0  = 波谷完全断电（预期：无嗡嗡声）"));
      Serial.println(F("      20 = 波谷保留 20% 占空比（预期：可听嗡鸣）"));
      Serial.println(F("      对应 PRD §12 异常19 / RK-17 与校验规则 C-18"));
      return;
    }
    const int tv = atoi(p);
    if (tv != 0 && tv != 20) { Serial.println(F("[cli] 只支持 0 或 20")); return; }

    const int32_t oldTrough = Params::get(P_BREATH_TROUGH_DUTY_PCT);
    // ⚠ 这里绕过 validateSet 直接写入，仅用于 A/B 对比实验；
    //   注意 C-18 会拒绝 trough != 0，因此实验中途不能执行 SAVE。
    Params::set(P_BREATH_TROUGH_DUTY_PCT, tv);

    m_haptic->startBreath(2);
    Serial.printf("[cli] 波谷对比测试：波谷占空比 = %d%%（其余参数不变）\n", tv);
    Serial.println(F("      请贴近耳朵听 30s，再用另一个值重复；RESET breath.* 可恢复"));
    Serial.printf("      （原值 %ld）\n", (long)oldTrough);
    return;
  }

  // --- t gate <freqHz> ---
  if (!strcmp(sub, "gate")) {
    if (!*p) {
      Serial.printf("[cli] 用法: t gate <25~50>（当前 %ldHz）\n",
                    (long)(Params::get(P_PURR_DRIVE_FREQ_HZ) / 100));
      return;
    }
    const int32_t hz = atol(p);
    const char* err = Params::validateSet(P_PURR_DRIVE_FREQ_HZ, hz * 100);
    if (err) {
      Serial.printf("[cli] ✗ 被拒绝，违反 %s：%s\n", err, Params::ruleText(err));
      return;
    }
    Params::set(P_PURR_DRIVE_FREQ_HZ, hz * 100);
    Serial.printf("[cli] ✓ 咕噜门控频率 = %ldHz\n", (long)hz);
    return;
  }

  // --- t feed [n] [intervalMs] ---
  if (!strcmp(sub, "feed")) {
    uint16_t n = 40;
    uint32_t iv = 0;                 // 0 = 自动取 max(pet_debounce, pet_valid_min)
    if (*p) {
      n = (uint16_t)atoi(p);
      const char* q = strchr(p, ' ');
      if (q) { while (*q == ' ') q++; if (*q) iv = (uint32_t)atol(q); }
    }
    if (n == 0) n = 40;
    if (iv == 0) {
      iv = (uint32_t)Params::get(P_PET_DEBOUNCE_MS);
      const uint32_t v = (uint32_t)Params::get(P_PET_VALID_MIN_MS);
      if (v > iv) iv = v;
    }
    scheduleFeed(n, iv);
    Serial.printf("[cli] 开始模拟 %u 次抚摸，间隔 %lums（非阻塞，主循环持续运行）\n",
                  (unsigned)n, (unsigned long)iv);
    Serial.println(F("      观察点：是否在阈值处触发、时长是否符合映射、"));
    Serial.println(F("              降落是否平滑、冷却期是否拒绝计入、呼吸是否按 P-43 节流"));
    return;
  }

  // --- t auto [intervalMs] / t stop ---
  if (!strcmp(sub, "auto")) {
    uint32_t iv = 3000;
    if (*p) iv = (uint32_t)atol(p);
    if (iv < 100) iv = 100;
    scheduleFeed(0, iv);   // 0 = 无限
    Serial.printf("[cli] 自动抚摸循环启动（间隔 %lums，每 20 次打印摘要）；t stop 停止\n",
                  (unsigned long)iv);
    return;
  }
  if (!strcmp(sub, "stop")) {
    m_autoRun = false;
    m_haptic->stop();
    if (m_tail) m_tail->stopNow();
    Serial.println(F("[cli] 自动循环已停止，震动与摇尾已停"));
    return;
  }

  // --- t mood <v> ---
  if (!strcmp(sub, "mood")) {
    if (!*p) { Serial.println(F("[cli] 用法: t mood <值>")); return; }
    m_mood->forceMood((int16_t)atoi(p));
    return;
  }

  // --- t state <name> ---
  if (!strcmp(sub, "state")) {
    if (!*p) {
      Serial.println(F("[cli] 可选: accum purr landing cooldown lowpower upgrading"));
      return;
    }
    if      (!strcmp(p, "accum"))     m_mood->forceState(HS_ACCUM);
    else if (!strcmp(p, "purr"))      m_mood->forceState(HS_PURR);
    else if (!strcmp(p, "landing"))   m_mood->forceState(HS_LANDING);
    else if (!strcmp(p, "cooldown"))  m_mood->forceState(HS_COOLDOWN);
    else if (!strcmp(p, "lowpower"))  m_mood->forceState(HS_LOWPOWER);
    else if (!strcmp(p, "upgrading")) m_mood->forceState(HS_UPGRADING);
    else Serial.printf("[cli] 未知状态: %s\n", p);
    return;
  }

  // --- t tail <1-4> ---
  if (!strcmp(sub, "tail")) {
    if (!*p) { Serial.println(F("[cli] 用法: t tail <1-4>")); return; }
    const int g = atoi(p);
    if (g < 1 || g > 4) { Serial.println(F("[cli] 档位需 1~4")); return; }
    if (m_tail) {
      m_tail->setGear((uint8_t)g);
      Serial.printf("[cli] 摇尾目标 W%d → %ld.%02ldHz（过渡 %ldms）\n",
                    g, (long)(m_tail->getTargetFreqHz100() / 100),
                    (long)(m_tail->getTargetFreqHz100() % 100),
                    (long)Params::get(P_TAIL_TRANSITION_MS));
    }
    return;
  }

  // --- t cycle：从当前心情值跑一次完整生命周期 ---
  if (!strcmp(sub, "cycle")) {
    const int16_t th = (int16_t)Params::get(P_PURR_TRIGGER_THRESHOLD);
    m_mood->forceState(HS_ACCUM);
    m_mood->forceMood(th);
    Serial.println(F("[cli] 已把心情值推到阈值，下一步抚摸将触发完整生命周期："));
    Serial.println(F("      PURR → LANDING(5s，摇尾同步缓降) → COOLDOWN(90s) → ACCUM"));
    Serial.println(F("      用 status 观察各阶段；t mood <v> 可在其间查看降落曲线"));
    return;
  }

  Serial.printf("[cli] 未知 t 子命令: %s\n", sub);
}

#endif  // ENABLE_DEBUG_CHANNEL
