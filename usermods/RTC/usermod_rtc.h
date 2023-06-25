#pragma once

#include <Wire.h>   // WLEDSR - make sure the "right" Wire object is availeable
#include "src/dependencies/time/DS1307RTC.h"
#include "wled.h"

#if !defined(HW_PIN_SDA) && !defined(HW_PIN_SCL) // WLEDSR - don't overwrite global pins
#ifdef ARDUINO_ARCH_ESP32
  #define HW_PIN_SCL 22
  #define HW_PIN_SDA 21
#else
  #define HW_PIN_SCL 5
  #define HW_PIN_SDA 4
#endif
#endif

//Connect DS1307 to standard I2C pins (ESP32: GPIO 21 (SDA)/GPIO 22 (SCL))

class RTCUsermod : public Usermod {
  private:
    unsigned long lastTime = 0;
    bool disabled = false;
  public:

    void setup() {
      PinManagerPinType pins[2] = { { HW_PIN_SCL, true }, { HW_PIN_SDA, true } };
      if (!pinManager.allocateMultiplePins(pins, 2, PinOwner::HW_I2C)) { disabled = true; return; }
      Wire.begin(pins[1].pin, pins[0].pin);  // WLEDSR this will silently fail if Wire is already started
      // Wire.setClock(400000);
      RTC.begin();
      time_t rtcTime = RTC.get();
      if (rtcTime) {
        toki.setTime(rtcTime,TOKI_NO_MS_ACCURACY,TOKI_TS_RTC);
        updateLocalTime();
      } else {
        if (!RTC.chipPresent()) disabled = true; //don't waste time if H/W error
      }
    }

    void loop() {
      if (strip.isUpdating()) return;
      if (!disabled && toki.isTick()) {
        time_t t = toki.second();
        if (abs(t - RTC.get())> 2) RTC.set(t); //set RTC to NTP/UI-provided value - WLEDSR allow up to 3 sec deviation
      }
    }

    /*
     * addToConfig() can be used to add custom persistent settings to the cfg.json file in the "um" (usermod) object.
     * It will be called by WLED when settings are actually saved (for example, LED settings are saved)
     * I highly recommend checking out the basics of ArduinoJson serialization and deserialization in order to use custom settings!
     */
    void addToConfig(JsonObject& root)
    {
      //JsonObject top = root.createNestedObject("RTC");
      //JsonArray pins = top.createNestedArray("pin");
      //pins.add(HW_PIN_SCL);
      //pins.add(HW_PIN_SDA);
    }

    uint16_t getId()
    {
      return USERMOD_ID_RTC;
    }
};