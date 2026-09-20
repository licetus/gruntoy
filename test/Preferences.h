/*
 * ============================================================================
 *  test/Preferences.h —— 本机验证用的 NVS 桩
 *
 *  用进程内 map 模拟 NVS 的键值行为，语义与 ESP32 Preferences 对齐：
 *    · begin(ns, readOnly) 打开命名空间（本桩不做权限校验）
 *    · putUInt / putBytes 返回实际写入长度
 *    · getBytesLength 返回已存长度，0 表示不存在
 *  注意：进程退出后内容消失，这是故意的——"断电是否保持"必须由真实硬件验证。
 * ============================================================================
 */

#ifndef PREFERENCES_H_STUB
#define PREFERENCES_H_STUB

#include <cstdint>
#include <cstring>
#include <string>
#include <map>
#include <vector>

class Preferences {
public:
  bool begin(const char* name, bool readOnly = false) {
    m_ns = name ? name : "";
    m_readOnly = readOnly;
    return true;
  }
  void end() { }

  size_t putUInt(const char* key, uint32_t value) {
    std::vector<uint8_t>& b = slot(key);
    b.assign((const uint8_t*)&value, (const uint8_t*)&value + sizeof(value));
    return sizeof(value);
  }
  uint32_t getUInt(const char* key, uint32_t defaultValue = 0) {
    const std::vector<uint8_t>& b = slotRef(key);
    if (b.size() != sizeof(uint32_t)) return defaultValue;
    uint32_t v = 0;
    memcpy(&v, b.data(), sizeof(v));
    return v;
  }

  size_t putBytes(const char* key, const void* value, size_t len) {
    std::vector<uint8_t>& b = slot(key);
    b.assign((const uint8_t*)value, (const uint8_t*)value + len);
    return len;
  }
  size_t getBytesLength(const char* key) { return slotRef(key).size(); }
  size_t getBytes(const char* key, void* buf, size_t len) {
    const std::vector<uint8_t>& b = slotRef(key);
    if (len > b.size()) len = b.size();
    if (len > 0) memcpy(buf, b.data(), len);
    return len;
  }

  // --- 测试辅助：模拟"出厂空 NVS" ---
  void wipe() { m_data.clear(); }

private:
  std::string keyOf(const char* k) const { return m_ns + "/" + (k ? k : ""); }
  std::vector<uint8_t>& slot(const char* k) { return m_data[keyOf(k)]; }
  const std::vector<uint8_t>& slotRef(const char* k) {
    auto it = m_data.find(keyOf(k));
    if (it == m_data.end()) {
      static const std::vector<uint8_t> empty;
      return empty;
    }
    return it->second;
  }

  std::string m_ns;
  bool        m_readOnly = false;
  std::map<std::string, std::vector<uint8_t>> m_data;
};

#endif  // PREFERENCES_H_STUB
