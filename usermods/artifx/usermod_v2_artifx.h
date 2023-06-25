#pragma once

#include "wled.h"

#include "arti_wled.h"

class ARTIFXUserMod : public Usermod {
  private:
    // strings to reduce flash memory usage (used more than twice)
    static const char _name[]; //usermod name

    char errorMessage[100] = "";

    bool     enabled = false;
    bool     initDone = false;

  public:

    void setup() {
      // if (!initDone)
      //   strip.addEffect(FX_MODE_ARTIFX, &mode_ARTIFX, _data_FX_MODE_ARTIFX);
      initDone = true;
      enabled = true;
    }

    void connected() {
    }

    void loop() {
    }

    /*
     * addToJsonInfo() can be used to add custom entries to the /json/info part of the JSON API.
     * Creating an "u" object allows you to add custom key/value pairs to the Info section of the WLED web UI.
     * Below it is shown how this could be used for e.g. a light sensor
     */
    void addToJsonInfo(JsonObject& root)
    {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");

      JsonArray infoArr = user.createNestedArray(FPSTR(_name));
      infoArr.add(errorMessage); //value
      // infoArr.add(""); //unit
    }


    /*
     * addToJsonState() can be used to add custom entries to the /json/state part of the JSON API (state object).
     * Values in the state object may be modified by connected clients
     */
    void addToJsonState(JsonObject& root)
    {
      //root["user0"] = userVar0;
      if (!initDone) return;  // prevent crash on boot applyPreset()
      JsonObject usermod = root[FPSTR(_name)];
      if (usermod.isNull()) {
        usermod = root.createNestedObject(FPSTR(_name));
      }
      usermod["on"] = enabled;
    }


    /*
     * readFromJsonState() can be used to receive data clients send to the /json/state part of the JSON API (state object).
     * Values in the state object may be modified by connected clients
     */
    void readFromJsonState(JsonObject& root)
    {
      // userVar0 = root["user0"] | userVar0; //if "user0" key exists in JSON, update, else keep old value
      //if (root["bri"] == 255) Serial.println(F("Don't burn down your garage!"));
    }


    void addToConfig(JsonObject& root)
    {
    }


    bool readFromConfig(JsonObject& root)
    {
      JsonObject top = root[FPSTR(_name)];

      bool configComplete = !top.isNull();


      //  * Return true in case the config values returned from Usermod Settings were complete, or false if you'd like WLED to save your defaults to disk (so any missing values are editable in Usermod Settings)
      return configComplete;
    }

    void appendConfigData()
    {
    }

    /*
     * handleOverlayDraw() is called just before every show() (LED strip update frame) after effects have set the colors.
     * Use this to blank out some LEDs or set them to a different color regardless of the set effect mode.
     * Commonly used for custom clocks (Cronixie, 7 segment)
     */
    void handleOverlayDraw()
    {
      //strip.setPixelColor(0, RGBW32(0,0,0,0)) // set the first pixel to black
    }

   
    /*
     * getId() allows you to optionally give your V2 usermod an unique ID (please define it in const.h!).
     * This could be used in the future for the system to determine whether your usermod is installed.
     */
    uint16_t getId()
    {
      return USERMOD_ID_ARTIFX;
    }
};

// strings to reduce flash memory usage (used more than twice)
const char ARTIFXUserMod::_name[]       PROGMEM = "ARTIFX";
