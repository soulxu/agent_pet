#include "app_prefs.h"

#include <Preferences.h>

namespace app_prefs {

namespace {
constexpr const char* NS = "apprefs";
constexpr const char* KEY_BRIGHT = "bri";
constexpr const char* KEY_SOUND = "snd";
constexpr const char* KEY_HOST = "host";

uint8_t g_bright = 128;
bool    g_sound = true;
uint8_t g_host = 0;
bool    g_loaded = false;

void load() {
  if (g_loaded) return;
  Preferences p;
  if (p.begin(NS, true /*ro*/)) {
    g_bright = p.getUChar(KEY_BRIGHT, 128);
    g_sound = p.getBool(KEY_SOUND, true);
    g_host = p.getUChar(KEY_HOST, 0);
    p.end();
  }
  g_loaded = true;
}
}  // namespace

void begin() { load(); }

uint8_t brightness() {
  load();
  return g_bright;
}

void setBrightness(uint8_t v) {
  load();
  g_bright = v;
  Preferences p;
  if (p.begin(NS, false)) { p.putUChar(KEY_BRIGHT, v); p.end(); }
}

bool soundOn() {
  load();
  return g_sound;
}

void setSoundOn(bool v) {
  load();
  g_sound = v;
  Preferences p;
  if (p.begin(NS, false)) { p.putBool(KEY_SOUND, v); p.end(); }
}

uint8_t hostSlot() {
  load();
  return g_host;
}

void setHostSlot(uint8_t v) {
  load();
  g_host = v;
  Preferences p;
  if (p.begin(NS, false)) { p.putUChar(KEY_HOST, v); p.end(); }
}

}  // namespace app_prefs
