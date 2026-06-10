#include "ble_kbd.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <HIDTypes.h>

extern "C" int ble_store_clear(void);

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
  bool onSecurityRequest() override { return true; }
};

}  // namespace

void begin(const char* name) {
  BLEDevice::init(name);
  BLEDevice::setPower(ESP_PWR_LVL_P9);
  BLEDevice::setSecurityCallbacks(new SecurityCb());

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCb());

  g_hid = new BLEHIDDevice(g_server);
  g_input = g_hid->inputReport(1);
  g_hid->manufacturer()->setValue("agent_pet");
  g_hid->pnp(0x02, 0x05ac, 0x820a, 0x0100);   // 伪装成苹果蓝牙键盘
  g_hid->hidInfo(0x00, 0x01);
  g_hid->reportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  g_hid->startServices();
  g_hid->setBatteryLevel(100);

  BLESecurity* sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  sec->setCapability(ESP_IO_CAP_NONE);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->setAppearance(0x03C1);  // HID Keyboard
  adv->addServiceUUID(g_hid->hidService()->getUUID());
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
  int rc = ble_store_clear();
  Serial.printf("[kbd] ble_store_clear rc=%d\n", rc);
}

}  // namespace ble_kbd
