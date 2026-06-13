#include "ble_kbd.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <HIDTypes.h>
// 注意: 不直接 #include <esp_gap_ble_api.h>. 它由 <BLEDevice.h> 传递包含;
// 在 ESP32-S3 这套 core 下直接用 <> 引会找不到 (bt include 路径只在 BLE 库内部生效).
#if defined(CONFIG_BT_CLASSIC_ENABLED)
#include <esp_gap_bt_api.h>
#endif

#include <cstdlib>

// 清配对 (重新配对) 的底层 API 因蓝牙协议栈而异:
//   * ESP32-S3 (StickS3) 这套 core 用 **NimBLE** -> ble_store_clear() 一把清掉所有 bond.
//   * 经典 ESP32 (Atom Lite) 用 **Bluedroid** -> 枚举 bond 列表逐个 esp_ble_remove_bond_device.
// 两者函数名/类型完全不同, 故按当前编译的协议栈条件编译, 各自前置声明 (避免直接
// #include 那些只在库内部 include 路径可见的 bt 头).
#if defined(CONFIG_NIMBLE_ENABLED)
extern "C" int ble_store_clear(void);
#elif defined(CONFIG_BLUEDROID_ENABLED)
// 这几个 bond API 在 <esp_gap_ble_api.h> 里被 `#if (SMP_INCLUDED == TRUE)` 包着,
// SMP_INCLUDED 是预编译库 sdkconfig 的宏, 用户 TU 不一定可见, 故按原型自行声明;
// 类型 (esp_bd_addr_t / esp_ble_bond_dev_t) 在该 guard 之外, Bluedroid 下始终可见.
extern "C" {
int       esp_ble_get_bond_device_num(void);
esp_err_t esp_ble_get_bond_device_list(int* dev_num, esp_ble_bond_dev_t* dev_list);
esp_err_t esp_ble_remove_bond_device(esp_bd_addr_t bd_addr);
}
#endif

namespace ble_kbd {

namespace {

// HID 标准键盘 report map (report id 1, 8 字节: mods, reserved, key[6]).
const uint8_t HID_REPORT_MAP[] = {
  USAGE_PAGE(1),      0x01,        // Generic Desktop
  USAGE(1),           0x06,        // Keyboard
  COLLECTION(1),      0x01,        // Application
  REPORT_ID(1),       0x01,
  USAGE_PAGE(1),      0x07,        // Key Codes
  USAGE_MINIMUM(1),   0xE0,
  USAGE_MAXIMUM(1),   0xE7,
  LOGICAL_MINIMUM(1), 0x00,
  LOGICAL_MAXIMUM(1), 0x01,
  REPORT_SIZE(1),     0x01,
  REPORT_COUNT(1),    0x08,
  HIDINPUT(1),        0x02,        // modifiers
  REPORT_COUNT(1),    0x01,
  REPORT_SIZE(1),     0x08,
  HIDINPUT(1),        0x01,        // reserved
  REPORT_COUNT(1),    0x06,
  REPORT_SIZE(1),     0x08,
  LOGICAL_MINIMUM(1), 0x00,
  LOGICAL_MAXIMUM(1), 0x65,
  USAGE_PAGE(1),      0x07,
  USAGE_MINIMUM(1),   0x00,
  USAGE_MAXIMUM(1),   0x65,
  HIDINPUT(1),        0x00,        // keys
  END_COLLECTION(0)
};

BLEServer*         g_server = nullptr;
BLEHIDDevice*      g_hid = nullptr;
BLECharacteristic* g_input = nullptr;
volatile bool      g_conn = false;

class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    g_conn = true;
    Serial.printf("[kbd] connect, count=%d\n", s->getConnectedCount());
  }
  void onDisconnect(BLEServer* s) override {
    g_conn = (s->getConnectedCount() > 0);
    s->startAdvertising();
    Serial.printf("[kbd] disconnect, count=%d\n", s->getConnectedCount());
  }
};

class SecurityCb : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onConfirmPIN(uint32_t) override { return true; }
  bool onSecurityRequest() override {
    Serial.println("[kbd] onSecurityRequest -> accept");
    return true;
  }
#if defined(CONFIG_BLUEDROID_ENABLED)
  void onAuthenticationComplete(esp_ble_auth_cmpl_t desc) override {
    Serial.printf("[kbd] auth complete: success=%d fail_reason=0x%x\n",
                  desc.success, desc.fail_reason);
  }
#endif
};

}  // namespace

void begin(const char* name) {
  BLEDevice::init(name);
  BLEDevice::setPower(ESP_PWR_LVL_P9);
  BLEDevice::setSecurityCallbacks(new SecurityCb());

#if defined(CONFIG_BT_CLASSIC_ENABLED)
  // 经典 ESP32 (如 Atom Lite 的 ESP32-PICO-D4) 蓝牙是双模. 这套 core 把控制器跑在
  // BTDM(经典+BLE), macOS 可能从经典 BT 那侧发现设备却给不出 BLE 配对入口
  // (表现: 看得到但"没有连接按钮"). 关掉经典 BT 的可发现/可连接, 强制只走 BLE.
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
#endif

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCb());

  g_hid = new BLEHIDDevice(g_server);
  g_input = g_hid->inputReport(1);
  g_hid->manufacturer()->setValue("agent_pet");
  // 伪装成苹果蓝牙键盘 (跳过 macOS 键盘识别向导). 但两台设备若用**完全相同**的
  // VID:PID, macOS 的 HID 层会按 VID:PID 缓存/去重, 第二台会被顶掉 ("点连接就消失").
  // 所以每个 target 用不同的 product id, 让 macOS 当成两台独立键盘.
#if defined(AGENTPET_ATOM)
  g_hid->pnp(0x02, 0x05ac, 0x820b, 0x0110);   // Atom Lite: 区别于 StickS3
#else
  g_hid->pnp(0x02, 0x05ac, 0x820a, 0x0100);   // StickS3
#endif
  g_hid->hidInfo(0x00, 0x01);
  g_hid->reportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  g_hid->startServices();
  g_hid->setBatteryLevel(100);

  BLESecurity* sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  sec->setCapability(ESP_IO_CAP_NONE);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  // 主广播包只放 flags + appearance + HID 服务 (很小), 名字放进 scan response.
  // 否则长名字 (如 "AgentPet-AtomLite") 会把主广播包撑过 31 字节上限, 导致
  // esp_ble_gap_config_adv_data 失败 -> macOS 识别不到键盘 / "没有连接按钮".
  BLEAdvertisementData advData;
  advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  advData.setAppearance(0x03C1);  // HID Keyboard
  advData.setCompleteServices(g_hid->hidService()->getUUID());

  BLEAdvertisementData scanResp;
  scanResp.setName(name);

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanResp);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.printf("[kbd] advertising as '%s' (pure HID keyboard)\n", name);
}

bool connected() {
  return g_conn && g_server && g_server->getConnectedCount() > 0;
}

void tap(uint8_t mods, uint8_t key) {
  if (!g_input) return;
  uint8_t rpt[8] = {0};
  rpt[0] = mods;
  rpt[2] = key;
  g_input->setValue(rpt, 8);
  g_input->notify();
  delay(12);
  uint8_t rel[8] = {0};
  g_input->setValue(rel, 8);
  g_input->notify();
  delay(8);
}

void clearBonds() {
#if defined(CONFIG_NIMBLE_ENABLED)
  // NimBLE (StickS3): 清空 bond store 里的全部配对密钥.
  int rc = ble_store_clear();
  Serial.printf("[kbd] NimBLE 清除全部蓝牙配对记录 (rc=%d)\n", rc);
#elif defined(CONFIG_BLUEDROID_ENABLED)
  // Bluedroid (Atom): 枚举已绑定设备逐个删除 (必须在 BLEDevice::init 之后调用).
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) { Serial.println("[kbd] 无配对记录可清除"); return; }
  esp_ble_bond_dev_t* list =
      (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * n);
  if (!list) { Serial.println("[kbd] clearBonds malloc 失败"); return; }
  if (esp_ble_get_bond_device_list(&n, list) == ESP_OK) {
    for (int i = 0; i < n; ++i) esp_ble_remove_bond_device(list[i].bd_addr);
    Serial.printf("[kbd] 已清除 %d 条蓝牙配对记录\n", n);
  } else {
    Serial.println("[kbd] 读取配对列表失败");
  }
  free(list);
#else
  Serial.println("[kbd] clearBonds: 当前协议栈无可用 bond API, 跳过");
#endif
}

void repair() {
  Serial.println("[kbd] 重新配对: 断开当前 host + 清 bond + 重广播");
  // 1. 主动断开所有已连接的 host (server 角色下对端按 client=false 记录).
  if (g_server) {
    auto peers = g_server->getPeerDevices(false);
    for (auto& kv : peers) g_server->disconnect(kv.first);
    if (!peers.empty()) delay(80);  // 给断开事件一点时间落地
  }
  g_conn = false;

  // 2. 清掉本机侧旧配对密钥 (macOS 那侧也要"移除此设备"才算干净重配).
  clearBonds();

  // 3. 重新开始广播, 让设备立刻回到可被发现/配对状态.
  if (g_server) g_server->startAdvertising();
  else          BLEDevice::startAdvertising();
  Serial.println("[kbd] 已重置, 重新广播中, 可在系统蓝牙里重新配对");
}

}  // namespace ble_kbd
