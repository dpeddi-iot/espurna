/*

THERMOSTAT MODULE

Copyright (C) 2017 by Dmitry Blinov <dblinov76 at gmail dot com>

https://github.com/xoseperez/espurna/pull/1603#issuecomment-469256254
*/

#include "thermostat.h"

#if THERMOSTAT_SUPPORT

#include "ntp.h"
#include "relay.h"
#include "sensor.h"
#include "mqtt.h"
#include "ws.h"

#if THERMOSTAT_DISPLAY_SUPPORT
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT
#include <gfxfont.h>
#include <static/Roboto_Thin9pt8b.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#else
#include <SSD1306.h> // alias for `#include "SSD1306Wire.h"`
#endif
#endif

const char* NAME_THERMOSTAT_ENABLED     = "thermostatEnabled";
const char* NAME_THERMOSTAT_MODE        = "thermostatMode";
const char* NAME_TEMP_RANGE_MIN         = "tempRangeMin";
const char* NAME_TEMP_RANGE_MAX         = "tempRangeMax";
const char* NAME_REMOTE_SENSOR_NAME     = "remoteSensorName";
const char* NAME_REMOTE_TEMP_MAX_WAIT   = "remoteTempMaxWait";
const char* NAME_ALONE_ON_TIME          = "aloneOnTime";
const char* NAME_ALONE_OFF_TIME         = "aloneOffTime";
const char* NAME_MAX_ON_TIME            = "maxOnTime";
const char* NAME_MIN_OFF_TIME           = "minOffTime";
const char* NAME_BURN_TOTAL             = "burnTotal";
const char* NAME_BURN_TODAY             = "burnToday";
const char* NAME_BURN_YESTERDAY         = "burnYesterday";
const char* NAME_BURN_THIS_MONTH        = "burnThisMonth";
const char* NAME_BURN_PREV_MONTH        = "burnPrevMonth";
const char* NAME_BURN_DAY               = "burnDay";
const char* NAME_BURN_MONTH             = "burnMonth";
const char* NAME_OPERATION_MODE         = "thermostatOperationMode";

unsigned long _thermostat_remote_temp_max_wait  = THERMOSTAT_REMOTE_TEMP_MAX_WAIT * MILLIS_IN_SEC;
unsigned long _thermostat_alone_on_time   = THERMOSTAT_ALONE_ON_TIME  * MILLIS_IN_MIN;
unsigned long _thermostat_alone_off_time  = THERMOSTAT_ALONE_OFF_TIME * MILLIS_IN_MIN;
unsigned long _thermostat_max_on_time     = THERMOSTAT_MAX_ON_TIME    * MILLIS_IN_MIN;
unsigned long _thermostat_min_off_time    = THERMOSTAT_MIN_OFF_TIME   * MILLIS_IN_MIN;
unsigned int  _thermostat_on_time_for_day = 0;
unsigned int  _thermostat_burn_total      = 0;
unsigned int  _thermostat_burn_today      = 0;
unsigned int  _thermostat_burn_yesterday  = 0;
unsigned int  _thermostat_burn_this_month = 0;
unsigned int  _thermostat_burn_prev_month = 0;
unsigned int  _thermostat_burn_day        = 0;
unsigned int  _thermostat_burn_month      = 0;

enum temperature_source_t {temp_none, temp_local, temp_remote};
struct thermostat_t {
  unsigned long last_update = 0;
  unsigned long last_switch = 0;
  String remote_sensor_name;
  unsigned int temperature_source = temp_none;
};

bool _thermostat_enabled = true;
bool _thermostat_mode_cooler = false;

temp_t _remote_temp;
temp_range_t _temp_range;
thermostat_t _thermostat;

enum thermostat_cycle_type {cooling, heating};
unsigned int _thermostat_cycle = heating;
String thermostat_remote_sensor_topic;

//------------------------------------------------------------------------------
const temp_t& thermostatRemoteTemp() {
    return _remote_temp;
}

//------------------------------------------------------------------------------
const temp_range_t& thermostatRange() {
    return _temp_range;
}

//------------------------------------------------------------------------------
void thermostatEnabled(bool enabled) {
    _thermostat_enabled = enabled;
}

//------------------------------------------------------------------------------
bool thermostatEnabled() {
    return _thermostat_enabled;
}

//------------------------------------------------------------------------------
void thermostatModeCooler(bool cooler) {
    _thermostat_mode_cooler = cooler;
}

//------------------------------------------------------------------------------
bool thermostatModeCooler() {
    return _thermostat_mode_cooler;
}

//------------------------------------------------------------------------------
std::vector<thermostat_callback_f> _thermostat_callbacks;

void thermostatRegister(thermostat_callback_f callback) {
    _thermostat_callbacks.push_back(callback);
}

//------------------------------------------------------------------------------
void updateRemoteTemp(bool remote_temp_actual) {
  #if WEB_SUPPORT
      char tmp_str[16];
      if (remote_temp_actual) {
        dtostrf(_remote_temp.temp, 1, 1, tmp_str);
      } else {
        strcpy(tmp_str, "\"?\"");
      }
      char buffer[128];
      snprintf_P(buffer, sizeof(buffer), PSTR("{\"thermostatVisible\": 1, \"remoteTmp\": %s}"), tmp_str);
      wsSend(buffer);
  #endif
}

//------------------------------------------------------------------------------
void updateOperationMode() {
  #if WEB_SUPPORT
    String message(F("{\"thermostatVisible\": 1, \"thermostatOperationMode\": \""));
    if (_thermostat.temperature_source == temp_remote) {
      message += F("remote temperature");
      updateRemoteTemp(true);
    } else if (_thermostat.temperature_source == temp_local) {
      message += F("local temperature");
      updateRemoteTemp(false);
    } else {
      message += F("autonomous");
      updateRemoteTemp(false);
    }
    message += F("\"}");
    wsSend(message.c_str());
  #endif
}

//------------------------------------------------------------------------------
// MQTT
//------------------------------------------------------------------------------
void thermostatMQTTCallback(unsigned int type, const char * topic, const char * payload) {

    if (type == MQTT_CONNECT_EVENT) {
      mqttSubscribeRaw(thermostat_remote_sensor_topic.c_str());
      mqttSubscribe(MQTT_TOPIC_HOLD_TEMP);
    }

    if (type == MQTT_MESSAGE_EVENT) {

        // Match topic
        String t = mqttMagnitude((char *) topic);

        if (strcmp(topic, thermostat_remote_sensor_topic.c_str()) != 0
         && !t.equals(MQTT_TOPIC_HOLD_TEMP))
           return;

        // Parse JSON input
        DynamicJsonBuffer jsonBuffer;
        JsonObject& root = jsonBuffer.parseObject(payload);
        if (!root.success()) {
            DEBUG_MSG_P(PSTR("[THERMOSTAT] Error parsing data\n"));
            return;
        }

        // Check rempte sensor temperature
        if (strcmp(topic, thermostat_remote_sensor_topic.c_str()) == 0) {
            if (root.containsKey(magnitudeTopic(MAGNITUDE_TEMPERATURE))) {
                String remote_temp = root[magnitudeTopic(MAGNITUDE_TEMPERATURE)];
                _remote_temp.temp = remote_temp.toFloat();
                _remote_temp.last_update = millis();
                _remote_temp.need_display_update = true;
                DEBUG_MSG_P(PSTR("[THERMOSTAT] Remote sensor temperature: %s\n"), remote_temp.c_str());
                updateRemoteTemp(true);
            }
        }

        // Check temperature range change
        if (t.equals(MQTT_TOPIC_HOLD_TEMP)) {
            if (root.containsKey(MQTT_TOPIC_HOLD_TEMP_MIN)) {
                int t_min = root[MQTT_TOPIC_HOLD_TEMP_MIN];
                int t_max = root[MQTT_TOPIC_HOLD_TEMP_MAX];
                if (t_min < THERMOSTAT_TEMP_RANGE_MIN_MIN || t_min > THERMOSTAT_TEMP_RANGE_MIN_MAX ||
                    t_max < THERMOSTAT_TEMP_RANGE_MAX_MIN || t_max > THERMOSTAT_TEMP_RANGE_MAX_MAX) {
                    DEBUG_MSG_P(PSTR("[THERMOSTAT] Hold temperature range error\n"));
                    return;
                }
                _temp_range.min = root[MQTT_TOPIC_HOLD_TEMP_MIN];
                _temp_range.max = root[MQTT_TOPIC_HOLD_TEMP_MAX];
                setSetting(NAME_TEMP_RANGE_MIN, _temp_range.min);
                setSetting(NAME_TEMP_RANGE_MAX, _temp_range.max);
                saveSettings();
                _temp_range.ask_interval = ASK_TEMP_RANGE_INTERVAL_REGULAR;
                _temp_range.last_update = millis();
                _temp_range.need_display_update = true;

                DEBUG_MSG_P(PSTR("[THERMOSTAT] Hold temperature range: (%d - %d)\n"), _temp_range.min, _temp_range.max);
                // Update websocket clients
                #if WEB_SUPPORT
                    char buffer[100];
                    snprintf_P(buffer, sizeof(buffer), PSTR("{\"thermostatVisible\": 1, \"tempRangeMin\": %d, \"tempRangeMax\": %d}"), _temp_range.min, _temp_range.max);
                    wsSend(buffer);
                #endif
            } else {
                DEBUG_MSG_P(PSTR("[THERMOSTAT] Error temperature range data\n"));
            }
        }
    }
}

//------------------------------------------------------------------------------
void notifyRangeChanged(bool min) {
  DEBUG_MSG_P(PSTR("[THERMOSTAT] notifyRangeChanged %s = %d\n"), min ? "MIN" : "MAX", min ? _temp_range.min : _temp_range.max);
  char tmp_str[6];
  sprintf(tmp_str, "%d", min ? _temp_range.min : _temp_range.max);

  mqttSend(min ? MQTT_TOPIC_NOTIFY_TEMP_RANGE_MIN : MQTT_TOPIC_NOTIFY_TEMP_RANGE_MAX, tmp_str, true);
}

//------------------------------------------------------------------------------
// Setup
//------------------------------------------------------------------------------
void commonSetup() {
  _thermostat_enabled     = getSetting(NAME_THERMOSTAT_ENABLED, THERMOSTAT_ENABLED_BY_DEFAULT);
  DEBUG_MSG_P(PSTR("[THERMOSTAT] _thermostat_enabled = %d\n"), _thermostat_enabled);

  _thermostat_mode_cooler = getSetting(NAME_THERMOSTAT_MODE, THERMOSTAT_MODE_COOLER_BY_DEFAULT);
  DEBUG_MSG_P(PSTR("[THERMOSTAT] _thermostat_mode_cooler = %d\n"), _thermostat_mode_cooler);
  
  _temp_range.min         = getSetting(NAME_TEMP_RANGE_MIN, THERMOSTAT_TEMP_RANGE_MIN);
  _temp_range.max         = getSetting(NAME_TEMP_RANGE_MAX, THERMOSTAT_TEMP_RANGE_MAX);
  DEBUG_MSG_P(PSTR("[THERMOSTAT] _temp_range.min = %d\n"), _temp_range.min);
  DEBUG_MSG_P(PSTR("[THERMOSTAT] _temp_range.max = %d\n"), _temp_range.max);

  _thermostat.remote_sensor_name = getSetting(NAME_REMOTE_SENSOR_NAME, THERMOSTAT_REMOTE_SENSOR_NAME);
  thermostat_remote_sensor_topic = _thermostat.remote_sensor_name + String("/") + String(MQTT_TOPIC_JSON);

  _thermostat_remote_temp_max_wait = getSetting(NAME_REMOTE_TEMP_MAX_WAIT, THERMOSTAT_REMOTE_TEMP_MAX_WAIT) * MILLIS_IN_SEC;
  _thermostat_alone_on_time   = getSetting(NAME_ALONE_ON_TIME,  THERMOSTAT_ALONE_ON_TIME)  * MILLIS_IN_MIN;
  _thermostat_alone_off_time  = getSetting(NAME_ALONE_OFF_TIME, THERMOSTAT_ALONE_OFF_TIME) * MILLIS_IN_MIN;
  _thermostat_max_on_time     = getSetting(NAME_MAX_ON_TIME,    THERMOSTAT_MAX_ON_TIME)    * MILLIS_IN_MIN;
  _thermostat_min_off_time    = getSetting(NAME_MIN_OFF_TIME,   THERMOSTAT_MIN_OFF_TIME)   * MILLIS_IN_MIN;
}

//------------------------------------------------------------------------------
void _thermostatReload() {
  int prev_temp_range_min = _temp_range.min;
  int prev_temp_range_max = _temp_range.max;

  commonSetup();
 
  if (_temp_range.min != prev_temp_range_min)
    notifyRangeChanged(true);
  if (_temp_range.max != prev_temp_range_max)
    notifyRangeChanged(false);
}

//------------------------------------------------------------------------------
void sendTempRangeRequest() {
  DEBUG_MSG_P(PSTR("[THERMOSTAT] sendTempRangeRequest\n"));
  mqttSend(MQTT_TOPIC_ASK_TEMP_RANGE, "", true);
}

//------------------------------------------------------------------------------
void setThermostatState(bool state) {
  DEBUG_MSG_P(PSTR("[THERMOSTAT] setThermostatState: %s\n"), state ? "ON" : "OFF");
  relayStatus(THERMOSTAT_RELAY, state, mqttForward(), false);
  _thermostat.last_switch = millis();
  // Send thermostat change state event to subscribers
  for (unsigned char i = 0; i < _thermostat_callbacks.size(); i++) {
      (_thermostat_callbacks[i])(state);
  }
}

//------------------------------------------------------------------------------
void debugPrintSwitch(bool state, double temp) {
  char tmp_str[16];
  dtostrf(temp, 1, 1, tmp_str);
  DEBUG_MSG_P(PSTR("[THERMOSTAT] switch %s, temp: %s, min: %d, max: %d, mode: %s, relay: %s, last switch %d\n"),
   state ? "ON" : "OFF", tmp_str, _temp_range.min, _temp_range.max, _thermostat_mode_cooler ? "COOLER" : "HEATER", relayStatus(THERMOSTAT_RELAY) ? "ON" : "OFF", millis() - _thermostat.last_switch);
}

//------------------------------------------------------------------------------
inline bool lastSwitchEarlierThan(unsigned int comparing_time) {
  return millis() - _thermostat.last_switch > comparing_time;
}

//------------------------------------------------------------------------------
inline void switchThermostat(bool state, double temp) {
    debugPrintSwitch(state, temp);
    setThermostatState(state);
}

//------------------------------------------------------------------------------
//----------- Main function that make decision ---------------------------------
//------------------------------------------------------------------------------
void checkTempAndAdjustRelay(double temp) {
  if (_thermostat_mode_cooler == false) { // Main operation mode. Thermostat is HEATER.
    // if thermostat switched ON and t > max - switch it OFF and start cooling
    if (relayStatus(THERMOSTAT_RELAY) && temp > _temp_range.max) {
      _thermostat_cycle = cooling;
      switchThermostat(false, temp);
    // if thermostat switched ON for max time - switch it OFF for rest
    } else if (relayStatus(THERMOSTAT_RELAY) && lastSwitchEarlierThan(_thermostat_max_on_time)) {
      switchThermostat(false, temp);
    // if t < min and thermostat switched OFF for at least minimum time - switch it ON and start
    } else if (!relayStatus(THERMOSTAT_RELAY) && temp < _temp_range.min
        && (_thermostat.last_switch == 0 || lastSwitchEarlierThan(_thermostat_min_off_time))) {
      _thermostat_cycle = heating;
      switchThermostat(true, temp);
    // if heating cycle and thermostat switchaed OFF for more than min time - switch it ON
    // continue heating cycle
    } else if (!relayStatus(THERMOSTAT_RELAY) && _thermostat_cycle == heating
        && lastSwitchEarlierThan(_thermostat_min_off_time)) {
      switchThermostat(true, temp);
    }
  } else { // Thermostat is COOLER. Inverse logic.
    // if thermostat switched ON and t < min - switch it OFF and start heating
    if (relayStatus(THERMOSTAT_RELAY) && temp < _temp_range.min) {
      _thermostat_cycle = heating;
      switchThermostat(false, temp);
    // if thermostat switched ON for max time - switch it OFF for rest
    } else if (relayStatus(THERMOSTAT_RELAY) && lastSwitchEarlierThan(_thermostat_max_on_time)) {
      switchThermostat(false, temp);
    // if t > max and thermostat switched OFF for at least minimum time - switch it ON and start
    } else if (!relayStatus(THERMOSTAT_RELAY) && temp > _temp_range.max
        && (_thermostat.last_switch == 0 || lastSwitchEarlierThan(_thermostat_min_off_time))) {
      _thermostat_cycle = cooling;
      switchThermostat(true, temp);
    // if cooling cycle and thermostat switchaed OFF for more than min time - switch it ON
    // continue cooling cycle
    } else if (!relayStatus(THERMOSTAT_RELAY) && _thermostat_cycle == cooling
        && lastSwitchEarlierThan(_thermostat_min_off_time)) {
      switchThermostat(true, temp);
    }
  }
}

//------------------------------------------------------------------------------
void updateCounters() {
  if (relayStatus(THERMOSTAT_RELAY)) {
    setSetting(NAME_BURN_TOTAL,      ++_thermostat_burn_total);
    setSetting(NAME_BURN_TODAY,      ++_thermostat_burn_today);
    setSetting(NAME_BURN_THIS_MONTH, ++_thermostat_burn_this_month);
  }

  if (ntpSynced()) {
    const auto ts = now();
    unsigned int now_day = day(ts);
    unsigned int now_month = month(ts);
    if (now_day != _thermostat_burn_day) {
      _thermostat_burn_yesterday = _thermostat_burn_today;
      _thermostat_burn_today = 0;
      _thermostat_burn_day = now_day;
      setSetting(NAME_BURN_YESTERDAY, _thermostat_burn_yesterday);
      setSetting(NAME_BURN_TODAY,     _thermostat_burn_today);
      setSetting(NAME_BURN_DAY,       _thermostat_burn_day);
    }
    if (now_month != _thermostat_burn_month) {
      _thermostat_burn_prev_month = _thermostat_burn_this_month;
      _thermostat_burn_this_month = 0;
      _thermostat_burn_month = now_month;
      setSetting(NAME_BURN_PREV_MONTH, _thermostat_burn_prev_month);
      setSetting(NAME_BURN_THIS_MONTH, _thermostat_burn_this_month);
      setSetting(NAME_BURN_MONTH,      _thermostat_burn_month);
    }
  }
}

//------------------------------------------------------------------------------
double getLocalTemperature() {
  #if SENSOR_SUPPORT
      for (byte i=0; i<magnitudeCount(); i++) {
          if (magnitudeType(i) == MAGNITUDE_TEMPERATURE) {
              double temp = magnitudeValue(i);
              char tmp_str[16];
              dtostrf(temp, 1, 1, tmp_str);
              DEBUG_MSG_P(PSTR("[THERMOSTAT] getLocalTemperature temp: %s\n"), tmp_str);
              return temp > -0.1 && temp < 0.1 ? DBL_MIN : temp;
          }
      }
  #endif
  return DBL_MIN;
}

//------------------------------------------------------------------------------
double getLocalHumidity() {
  #if SENSOR_SUPPORT
      for (byte i=0; i<magnitudeCount(); i++) {
          if (magnitudeType(i) == MAGNITUDE_HUMIDITY) {
              double hum = magnitudeValue(i);
              char tmp_str[16];
              dtostrf(hum, 1, 0, tmp_str);
              DEBUG_MSG_P(PSTR("[THERMOSTAT] getLocalHumidity hum: %s\%\n"), tmp_str);
              return hum > -0.1 && hum < 0.1 ? DBL_MIN : hum;
          }
      }
  #endif
  return DBL_MIN;
}

//------------------------------------------------------------------------------
// Loop
//------------------------------------------------------------------------------
void thermostatLoop(void) {

  if (!thermostatEnabled())
    return;

  // Update temperature range
  if (mqttConnected()) {
    if (millis() - _temp_range.ask_time > _temp_range.ask_interval) {
      _temp_range.ask_time = millis();
      sendTempRangeRequest();
    }
  }

  // Update thermostat state
  if (millis() - _thermostat.last_update > THERMOSTAT_STATE_UPDATE_INTERVAL) {
    _thermostat.last_update = millis();
    updateCounters();
    unsigned int last_temp_src = _thermostat.temperature_source;
    if (_remote_temp.last_update != 0 && millis() - _remote_temp.last_update < _thermostat_remote_temp_max_wait) {
      // we have remote temp
      _thermostat.temperature_source = temp_remote;
      DEBUG_MSG_P(PSTR("[THERMOSTAT] setup thermostat by remote temperature\n"));
      checkTempAndAdjustRelay(_remote_temp.temp);
    } else if (getLocalTemperature() != DBL_MIN) {
      // we have local temp
      _thermostat.temperature_source = temp_local;
      DEBUG_MSG_P(PSTR("[THERMOSTAT] setup thermostat by local temperature\n"));
      checkTempAndAdjustRelay(getLocalTemperature());
      // updateRemoteTemp(false);
    } else {
      // we don't have any temp - switch thermostat on for N minutes every hour
      _thermostat.temperature_source = temp_none;
      DEBUG_MSG_P(PSTR("[THERMOSTAT] setup thermostat by timeout\n"));
      if (relayStatus(THERMOSTAT_RELAY) && millis() - _thermostat.last_switch > _thermostat_alone_on_time) {
        setThermostatState(false);
      } else if (!relayStatus(THERMOSTAT_RELAY) && millis() - _thermostat.last_switch > _thermostat_alone_off_time) {
        setThermostatState(false);
      }
    }
    if (last_temp_src != _thermostat.temperature_source) {
      updateOperationMode();
    }
  }
}

//------------------------------------------------------------------------------
String getBurnTimeStr(unsigned int burn_time) {
  char burnTimeStr[18] = { 0 };
  if (burn_time < 60) {
    sprintf(burnTimeStr, "%d мин.", burn_time);
  } else {
    sprintf(burnTimeStr, "%d ч. %d мин.", (int)floor(burn_time / 60), burn_time % 60);
  }
  return String(burnTimeStr);
}

//------------------------------------------------------------------------------
void resetBurnCounters() {
  DEBUG_MSG_P(PSTR("[THERMOSTAT] resetBurnCounters\n"));
  setSetting(NAME_BURN_TOTAL,      0);
  setSetting(NAME_BURN_TODAY,      0);
  setSetting(NAME_BURN_YESTERDAY,  0);
  setSetting(NAME_BURN_THIS_MONTH, 0);
  setSetting(NAME_BURN_PREV_MONTH, 0);
  _thermostat_burn_total       = 0;
  _thermostat_burn_today       = 0;
  _thermostat_burn_yesterday   = 0;
  _thermostat_burn_this_month  = 0;
  _thermostat_burn_prev_month  = 0;
}

//#######################################################################
//  ___   _            _             
// |   \ (_) ___ _ __ | | __ _  _  _ 
// | |) || |(_-<| '_ \| |/ _` || || |
// |___/ |_|/__/| .__/|_|\__,_| \_, |
//              |_|             |__/ 
//#######################################################################

#if THERMOSTAT_DISPLAY_SUPPORT

#define wifi_on_width 16
#define wifi_on_height 16
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT
const char wifi_on_bits[] PROGMEM = {
  0x00, 0x00, 0x70, 0x00, 0x7E, 0x00, 0x7F, 0x80, 0x07, 0xC0, 0x01, 0xE0,
  0x40, 0xF0, 0x78, 0x78, 0x7C, 0x38, 0x1E, 0x1C, 0x07, 0x1C, 0x03, 0x8C,
  0x63, 0x8E, 0x71, 0x8E, 0x71, 0xCE, 0x00, 0x00, };
#else
const char wifi_on_bits[] PROGMEM = {
  0x00, 0x00, 0x0E, 0x00, 0x7E, 0x00, 0xFE, 0x01, 0xE0, 0x03, 0x80, 0x07,
  0x02, 0x0F, 0x1E, 0x1E, 0x3E, 0x1C, 0x78, 0x38, 0xE0, 0x38, 0xC0, 0x31,
  0xC6, 0x71, 0x8E, 0x71, 0x8E, 0x73, 0x00, 0x00, };
#endif

#define mqtt_width 16
#define mqtt_height 16
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT
const char mqtt_bits[] PROGMEM = {
  0x00, 0x00, 0x00, 0x10, 0x00, 0x18, 0x00, 0x1C, 0x57, 0xFE, 0x57, 0xFE,
  0x00, 0x1C, 0x08, 0x18, 0x18, 0x10, 0x38, 0x00, 0x7F, 0xEA, 0x7F, 0xEA,
  0x38, 0x00, 0x18, 0x00, 0x08, 0x00, 0x00, 0x00, };
#else
const char mqtt_bits[] PROGMEM = {
  0x00, 0x00, 0x00, 0x08, 0x00, 0x18, 0x00, 0x38, 0xEA, 0x7F, 0xEA, 0x7F,
  0x00, 0x38, 0x10, 0x18, 0x18, 0x08, 0x1C, 0x00, 0xFE, 0x57, 0xFE, 0x57,
  0x1C, 0x00, 0x18, 0x00, 0x10, 0x00, 0x00, 0x00, };
#endif

#define remote_temp_width 16
#define remote_temp_height 16
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT
const char remote_temp_bits[] PROGMEM = {
  0x00, 0x00, 0x07, 0x18, 0x08, 0xA4, 0x08, 0xA4, 0x09, 0x98, 0x0A, 0x80,
  0x0A, 0x80, 0x0B, 0x80, 0x0A, 0x80, 0x0A, 0x80, 0x0B, 0x80, 0x0A, 0x80,
  0x07, 0x00, 0x07, 0x00, 0x07, 0x00, 0x00, 0x00
};
#else
const char remote_temp_bits[] PROGMEM = {
  0x00, 0x00, 0xE0, 0x18, 0x10, 0x25, 0x10, 0x25, 0x90, 0x19, 0x50, 0x01,
  0x50, 0x01, 0xD0, 0x01, 0x50, 0x01, 0x50, 0x01, 0xD0, 0x01, 0x50, 0x01,
  0xE0, 0x00, 0xE0, 0x00, 0xE0, 0x00, 0x00, 0x00, };
#endif

#define server_width 16
#define server_height 16
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT
const char server_bits[] PROGMEM = {
  0x00, 0x00, 0x1F, 0xF8, 0x3F, 0xFC, 0x30, 0x0C, 0x30, 0x0C, 0x30, 0x0C,
  0x30, 0x0C, 0x30, 0x0C, 0x30, 0x0C, 0x1F, 0xF8, 0x3F, 0xFC, 0x7F, 0xFE,
  0x78, 0x1E, 0x7F, 0xFE, 0x3F, 0xFC, 0x00, 0x00, };
#else
const char server_bits[] PROGMEM = {
  0x00, 0x00, 0xF8, 0x1F, 0xFC, 0x3F, 0x0C, 0x30, 0x0C, 0x30, 0x0C, 0x30,
  0x0C, 0x30, 0x0C, 0x30, 0x0C, 0x30, 0xF8, 0x1F, 0xFC, 0x3F, 0xFE, 0x7F,
  0x1E, 0x78, 0xFE, 0x7F, 0xFC, 0x3F, 0x00, 0x00, };
#endif

#define LOCAL_TEMP_UPDATE_INTERVAL 60000
#define LOCAL_HUM_UPDATE_INTERVAL  61000

#if THERMOSTAT_DISPLAY_ST7735_SUPPORT
/*#define cs   D8
#define dc   D3
#define rst  D4*/
#define cs   D8
#define dc   D4
#define rst  -1
Adafruit_ST7735 display = Adafruit_ST7735(cs, dc, rst);
#else
SSD1306  display(0x3c, 1, 3);
#endif

unsigned long _local_temp_last_update = 0xFFFF;
unsigned long _local_hum_last_update = 0xFFFF;
unsigned long _thermostat_display_off_interval = THERMOSTAT_DISPLAY_OFF_INTERVAL * MILLIS_IN_SEC;
unsigned long _thermostat_display_on_time = millis();
bool _thermostat_display_is_on = true;
bool _display_wifi_status   = true;
bool _display_mqtt_status   = true;
bool _display_server_status = true;
bool _display_remote_temp_status = true;
bool _display_need_refresh  = true;
bool _temp_range_need_update = true;

//------------------------------------------------------------------------------
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT

void drawIco(int16_t x, int16_t y, const char *ico, bool on = true) {
  display.drawBitmap(x, y, (const uint8_t*) ico, 16, 16, ((on == true) ? ST7735_WHITE : ST77XX_BLACK));
  _display_need_refresh = true;
}

#else

void drawIco(int16_t x, int16_t y, const char *ico, bool on = true) {
  display.drawIco16x16(x, y, ico, !on); //FIXME
  _display_need_refresh = true;
}

#endif

//------------------------------------------------------------------------------
void display_wifi_status(bool on) {
  _display_wifi_status = on;
  drawIco(0, 0, wifi_on_bits, on);
}

//------------------------------------------------------------------------------
void display_mqtt_status(bool on) {
  _display_mqtt_status = on;
  drawIco(17, 0, mqtt_bits, on);
}

//------------------------------------------------------------------------------
void display_server_status(bool on) {
  _display_server_status = on;
  drawIco(34, 0, server_bits, on);
}

//------------------------------------------------------------------------------
void display_remote_temp_status(bool on) {
  _display_remote_temp_status = on;
  drawIco(51, 0, remote_temp_bits, on);
}

//------------------------------------------------------------------------------
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT

void display_temp_range() {
  _temp_range.need_display_update = false;
  display.fillRect(68, 0, ST7735_TFTHEIGHT_160 - 60, 16, ST7735_BLACK);
  display.setTextColor(ST77XX_WHITE);
  display.setTextWrap(true);
  display.setCursor(90, 0 + 13); 
  display.setFont(&Roboto_Thin9pt8b);
  String temp_range = String(_temp_range.min) + "\xB0- " + String(_temp_range.max) + "\xB0";
  display.print(temp_range);
  _display_need_refresh = true;
}

#else

void display_temp_range() {
  _temp_range.need_display_update = false;
  display.setColor(BLACK);
  display.fillRect(68, 0, 60, 16);
  display.setColor(WHITE);
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.setFont(ArialMT_Plain_16);
  String temp_range = String(_temp_range.min) + "°- " + String(_temp_range.max) + "°";
  display.drawString(128 /*x*/, 0/*y*/, temp_range);
  _display_need_refresh = true;
}

#endif

//------------------------------------------------------------------------------
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT

void display_remote_temp() {
  _remote_temp.need_display_update = false;
  display.fillRect(0, 16, ST7735_TFTHEIGHT_160, 16, ST77XX_BLACK);
  
  display.setTextColor(ST77XX_WHITE);
  display.setTextWrap(true);
  display.setFont(&Roboto_Thin9pt8b);
  
  display.setCursor(0, 16 + 13);
  display.print(F("Remote t"));

  display.setCursor(75, 16 + 13);
  String temp_range_vol = String("= ") + (_display_remote_temp_status ? String(_remote_temp.temp, 1) : String("?")) + "\xB0";
  display.print(temp_range_vol);
  _display_need_refresh = true;
}

#else

void display_remote_temp() {
  _remote_temp.need_display_update = false;
  display.setColor(BLACK);
  display.fillRect(0, 16, 128, 16);
  display.setColor(WHITE);
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 16, F("Remote  t"));

  String temp_range_vol = String("= ") + (_display_remote_temp_status ? String(_remote_temp.temp, 1) : String("?")) + "°";
  display.drawString(75, 16, temp_range_vol);
  _display_need_refresh = true;
}
#endif

//------------------------------------------------------------------------------
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT

void display_local_temp() {
  display.fillRect(0, 32, ST7735_TFTHEIGHT_160, 16, ST77XX_BLACK);

  display.setTextColor(ST77XX_WHITE);
  display.setTextWrap(true);
  display.setFont(&Roboto_Thin9pt8b);
  
  display.setCursor(0, 32 + 13);
  display.print(F("Local      t"));

  display.setCursor(75, 32 + 13);
  String local_temp_vol = String("= ") + (getLocalTemperature() != DBL_MIN ? String(getLocalTemperature(), 1) : String("?")) + "\xB0";
  display.print(local_temp_vol);
  _display_need_refresh = true;
}

#else

void display_local_temp() {
  display.setColor(BLACK);
  display.fillRect(0, 32, 128, 16);
  display.setColor(WHITE);
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  display.drawString(0, 32, F("Local      t"));

  String local_temp_vol = String("= ") + (getLocalTemperature() != DBL_MIN ? String(getLocalTemperature(), 1) : String("?")) + "°";
  display.drawString(75, 32, local_temp_vol);
  _display_need_refresh = true;
}
#endif

//------------------------------------------------------------------------------
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT

void display_local_humidity() {
  display.fillRect(0, 48, ST7735_TFTHEIGHT_160, 16, ST77XX_BLACK);

  display.setTextColor(ST77XX_WHITE);
  display.setTextWrap(true);
  
  display.setCursor(0, 48 + 13);
  display.print(F("Local      h "));

  display.setCursor(75, 48 + 13);
  String local_hum_vol = String("= ") + (getLocalHumidity() != DBL_MIN ? String(getLocalHumidity(), 0) : String("?")) + "%";
  display.print(local_hum_vol);
  _display_need_refresh = true;
}

#else

void display_local_humidity() {
  display.setColor(BLACK);
  display.fillRect(0, 48, 128, 16);
  display.setColor(WHITE);
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  display.drawString(0, 48, F("Local      h "));

  String local_hum_vol = String("= ") + (getLocalHumidity() != DBL_MIN ? String(getLocalHumidity(), 0) : String("?")) + "%";
  display.drawString(75, 48, local_hum_vol);
  _display_need_refresh = true;
}
#endif

//------------------------------------------------------------------------------
void displayOn() {
  DEBUG_MSG_P(PSTR("[THERMOSTAT] Display is On.\n"));
  _thermostat_display_on_time = millis();
  _thermostat_display_is_on = true;
  _display_need_refresh = true;
  display_wifi_status(_display_wifi_status);
  display_mqtt_status(_display_mqtt_status);
  display_server_status(_display_server_status);
  display_remote_temp_status(_display_remote_temp_status);
  _temp_range.need_display_update = true;
  _remote_temp.need_display_update = true;
  display_local_temp();
  display_local_humidity();
}

//------------------------------------------------------------------------------
// Setup
//------------------------------------------------------------------------------
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT

void displaySetup() {
  display.initR(INITR_BLACKTAB);        // Initialize ST7735R screen
  display.setRotation(1);
  display.fillScreen(ST77XX_BLACK);
  display.cp437(true);

  displayOn();

  espurnaRegisterLoop(displayLoop);
}

#else

void displaySetup() {
  display.init();
  display.flipScreenVertically();

  displayOn();

  espurnaRegisterLoop(displayLoop);
}
#endif

//------------------------------------------------------------------------------
void displayLoop() {
  if (THERMOSTAT_DISPLAY_OFF_INTERVAL > 0 && millis() - _thermostat_display_on_time > _thermostat_display_off_interval) {
    if (_thermostat_display_is_on) {
      DEBUG_MSG_P(PSTR("[THERMOSTAT] Display Off by timeout\n"));
      _thermostat_display_is_on = false;
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT
//FIXME
#else
      display.resetDisplay();
#endif
    }
    return;
  }

  //------------------------------------------------------------------------------
  // Indicators
  //------------------------------------------------------------------------------
  if (!_display_wifi_status) {
    if (wifiConnected() && WiFi.getMode() != WIFI_AP)
      display_wifi_status(true);
  } else if (!wifiConnected() || WiFi.getMode() == WIFI_AP) {
    display_wifi_status(false);
  }

  if (!_display_mqtt_status) {
    if (mqttConnected())
      display_mqtt_status(true);
  } else if (!mqttConnected()) {
    display_mqtt_status(false);
  }

  if (_temp_range.last_update != 0 && millis() - _temp_range.last_update < THERMOSTAT_SERVER_LOST_INTERVAL) {
    if (!_display_server_status)
      display_server_status(true);
  } else if (_display_server_status) {
    display_server_status(false);
  }

  if (_remote_temp.last_update != 0 && millis() - _remote_temp.last_update < _thermostat_remote_temp_max_wait) {
    if (!_display_remote_temp_status)
      display_remote_temp_status(true);
  } else if (_display_remote_temp_status) {
    display_remote_temp_status(false);
    display_remote_temp();
  }

  //------------------------------------------------------------------------------
  // Temp range
  //------------------------------------------------------------------------------
  if (_temp_range.need_display_update) {
      display_temp_range();
  }

  //------------------------------------------------------------------------------
  // Remote temp
  //------------------------------------------------------------------------------
  if (_remote_temp.need_display_update) {
      display_remote_temp();
  }

  //------------------------------------------------------------------------------
  // Local temp
  //------------------------------------------------------------------------------
  if (millis() - _local_temp_last_update > LOCAL_TEMP_UPDATE_INTERVAL) {
      _local_temp_last_update = millis();
      display_local_temp();
  }

  //------------------------------------------------------------------------------
  // Local temp
  //------------------------------------------------------------------------------
  if (millis() - _local_hum_last_update > LOCAL_HUM_UPDATE_INTERVAL) {
      _local_hum_last_update = millis();
      display_local_humidity();
  }

  //------------------------------------------------------------------------------
  // Display update
  //------------------------------------------------------------------------------
  if (_display_need_refresh) {
    yield();
#if THERMOSTAT_DISPLAY_ST7735_SUPPORT
//FIXME
#else
    display.display();
#endif
    _display_need_refresh = false;
  }
}

#endif // THERMOSTAT_DISPLAY_SUPPORT

#if WEB_SUPPORT
//------------------------------------------------------------------------------
void _thermostatWebSocketOnConnected(JsonObject& root) {
  root["thermostatEnabled"] = thermostatEnabled();
  root["thermostatMode"] = thermostatModeCooler();
  root["thermostatVisible"] = 1;
  root[NAME_TEMP_RANGE_MIN] = _temp_range.min;
  root[NAME_TEMP_RANGE_MAX] = _temp_range.max;
  root[NAME_REMOTE_SENSOR_NAME] = _thermostat.remote_sensor_name;
  root[NAME_REMOTE_TEMP_MAX_WAIT]    = _thermostat_remote_temp_max_wait / MILLIS_IN_SEC;
  root[NAME_MAX_ON_TIME]     = _thermostat_max_on_time    / MILLIS_IN_MIN;
  root[NAME_MIN_OFF_TIME]    = _thermostat_min_off_time   / MILLIS_IN_MIN;
  root[NAME_ALONE_ON_TIME]   = _thermostat_alone_on_time  / MILLIS_IN_MIN;
  root[NAME_ALONE_OFF_TIME]  = _thermostat_alone_off_time / MILLIS_IN_MIN;
  root[NAME_BURN_TODAY]      = _thermostat_burn_today;
  root[NAME_BURN_YESTERDAY]  = _thermostat_burn_yesterday;
  root[NAME_BURN_THIS_MONTH] = _thermostat_burn_this_month;
  root[NAME_BURN_PREV_MONTH] = _thermostat_burn_prev_month;
  root[NAME_BURN_TOTAL]      = _thermostat_burn_total;
  if (_thermostat.temperature_source == temp_remote) {
    root[NAME_OPERATION_MODE] = "remote temperature";
    root["remoteTmp"]     = _remote_temp.temp;
  } else if (_thermostat.temperature_source == temp_local) {
    root[NAME_OPERATION_MODE] = "local temperature";
    root["remoteTmp"]     = "?";
  } else {
    root[NAME_OPERATION_MODE] = "autonomous";
    root["remoteTmp"]     = "?";
  }
}

//------------------------------------------------------------------------------
bool _thermostatWebSocketOnKeyCheck(const char * key, JsonVariant& value) {
    if (strncmp(key, NAME_THERMOSTAT_ENABLED,   strlen(NAME_THERMOSTAT_ENABLED))   == 0) return true;
    if (strncmp(key, NAME_THERMOSTAT_MODE,      strlen(NAME_THERMOSTAT_MODE))      == 0) return true;
    if (strncmp(key, NAME_TEMP_RANGE_MIN,       strlen(NAME_TEMP_RANGE_MIN))       == 0) return true;
    if (strncmp(key, NAME_TEMP_RANGE_MAX,       strlen(NAME_TEMP_RANGE_MAX))       == 0) return true;
    if (strncmp(key, NAME_REMOTE_SENSOR_NAME,   strlen(NAME_REMOTE_SENSOR_NAME))   == 0) return true;
    if (strncmp(key, NAME_REMOTE_TEMP_MAX_WAIT, strlen(NAME_REMOTE_TEMP_MAX_WAIT)) == 0) return true;
    if (strncmp(key, NAME_MAX_ON_TIME,          strlen(NAME_MAX_ON_TIME))          == 0) return true;
    if (strncmp(key, NAME_MIN_OFF_TIME,         strlen(NAME_MIN_OFF_TIME))         == 0) return true;
    if (strncmp(key, NAME_ALONE_ON_TIME,        strlen(NAME_ALONE_ON_TIME))        == 0) return true;
    if (strncmp(key, NAME_ALONE_OFF_TIME,       strlen(NAME_ALONE_OFF_TIME))       == 0) return true;
    return false;
}

//------------------------------------------------------------------------------
void _thermostatWebSocketOnAction(uint32_t client_id, const char * action, JsonObject& data) {
    if (strcmp(action, "thermostat_reset_counters") == 0) resetBurnCounters();
}
#endif

//------------------------------------------------------------------------------
void thermostatSetup() {
  commonSetup();

  _thermostat.temperature_source = temp_none;
  _thermostat_burn_total      = getSetting(NAME_BURN_TOTAL, 0);
  _thermostat_burn_today      = getSetting(NAME_BURN_TODAY, 0);
  _thermostat_burn_yesterday  = getSetting(NAME_BURN_YESTERDAY, 0);
  _thermostat_burn_this_month = getSetting(NAME_BURN_THIS_MONTH, 0);
  _thermostat_burn_prev_month = getSetting(NAME_BURN_PREV_MONTH, 0);
  _thermostat_burn_day        = getSetting(NAME_BURN_DAY, 0);
  _thermostat_burn_month      = getSetting(NAME_BURN_MONTH, 0);

  #if MQTT_SUPPORT
    mqttRegister(thermostatMQTTCallback);
  #endif

  // Websockets
  #if WEB_SUPPORT
      wsRegister()
          .onConnected(_thermostatWebSocketOnConnected)
          .onKeyCheck(_thermostatWebSocketOnKeyCheck)
          .onAction(_thermostatWebSocketOnAction);
  #endif

  espurnaRegisterLoop(thermostatLoop);
  espurnaRegisterReload(_thermostatReload);
}

#endif // THERMOSTAT_SUPPORT
