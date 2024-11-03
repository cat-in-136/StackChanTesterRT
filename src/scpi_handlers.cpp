#include "scpi_handlers.h"

#include <M5Unified.h>
#include <Preferences.h>
#include <Vrekrer_scpi_parser.h>
#include <WiFi.h>

SCPI_Parser scpi;

/**
 * Setup SCPI Command Handler
 *
 * Supported Commands:
 *    SYSTem
 *      :WIFI
 *        :NETwork SSID,PASS
 *        :NETwork?
 *        :SCAN
 *      :TIME?
 *        :TZ TZ
 *        :TZ?
 *      :PDOWn
 *    *IDN?
 *    *LRN?
 *    *TST?
 */
void setupSCPI(void) {
  scpi.SetCommandTreeBase(F("SYSTem"));
  scpi.RegisterCommand(
      F(":WIFI:NETwork"),
      [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        const char *ssid = parameters[0];
        const char *pass = parameters[1];
        if ((ssid != nullptr) && (pass != nullptr)) {
          Preferences preferences;
          preferences.begin("wifi-config");
          preferences.putString("WIFI_SSID", ssid);
          preferences.putString("WIFI_PASSWD", pass);
          preferences.end();
          interface.println(F("OK"));
        } else {
          interface.println(F("NG,SSID and PASS not specified"));
        }
      });
  scpi.RegisterCommand(
      F(":WIFI:NETwork?"),
      [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        Preferences preferences;
        preferences.begin("wifi-config");
        String ssid = preferences.getString("WIFI_SSID");
        String passwd = preferences.getString("WIFI_PASSWD");
        preferences.end();

        interface.print(F("SSID="));
        interface.println(ssid);
        interface.print(F("PASS="));
        interface.println(passwd);
      });
  scpi.RegisterCommand(F(":WIFI:SCAN"), [](SCPI_C commands, SCPI_P parameters,
                                           Stream &interface) {
    interface.println(F("Scan WiFi ... "));
    interface.flush();

    const int n = WiFi.scanNetworks();
    if (n == 0) {
      interface.println(F("NG,No network found"));
      M5.update();
    } else {
      for (int i = 0; i < n; i++) {
        // Print SSID and RSSI for each network found
        interface.printf("%2d,", i + 1);
        interface.print(WiFi.SSID(i));
        interface.printf(",%d,", WiFi.RSSI(i));
        interface.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN)
                              ? F(",Open")
                              : F(",Enc"));
      }
      M5.update();
      interface.println(F("OK"));
    }
  });
  scpi.RegisterCommand(
      F(":TIME?"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        struct tm tm_info;
        getLocalTime(&tm_info);
        Serial.println(&tm_info, "%c");
      });
  scpi.RegisterCommand(
      F(":TIME:TZ"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        Preferences preferences;
        preferences.begin("system-time");
        preferences.putString("TZ", parameters[0]);
        preferences.end();
        setenv("TZ", parameters[0], 1);
        tzset(); // Assign the local timezone from setenv for mktime()
        interface.println(F("OK"));
      });
  scpi.RegisterCommand(F(":TIME:TZ?"), [](SCPI_C commands, SCPI_P parameters,
                                          Stream &interface) {
    const char *tz = getenv("TZ");
    if (tz == nullptr) {
      interface.println(F("NG,TZ not specified"));
    } else {
      interface.print(F("OK,"));
      interface.println(tz);
    }
  });
  scpi.RegisterCommand(
      F(":PDOWn"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        interface.println(F("Enter deepsleep forever..."));
        interface.flush();
        vTaskDelay(1);
        esp_deep_sleep_start();
      });

  scpi.SetCommandTreeBase(F(""));
  scpi.RegisterCommand(
      F("*IDN?"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        const uint64_t efuseMac = ESP.getEfuseMac();
        interface.printf("@cat_in_136,StackChanS3RT,#%08lx,v0.0.0", efuseMac);
        interface.println();
      });
  scpi.RegisterCommand(
      F("*LRN?"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        Preferences preferences;

        preferences.begin("wifi-config");
        const String ssid = preferences.getString("WIFI_SSID");
        const String pass = preferences.getString("WIFI_PASSWD");
        preferences.end();
        const char *tz = getenv("TZ");
        preferences.end();

        if (ssid != nullptr && pass != nullptr) {
          interface.printf("SYST:WIFI:NET %s,%s", ssid.c_str(), pass.c_str());
          interface.println();
        }
        if (tz != nullptr) {
          interface.printf("SYST:TIME:TZ %s", tz);
          interface.println();
        }
      });
  scpi.RegisterCommand(
      F("*TST"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        interface.println("0"); // Not supported as of now
      });

  scpi.SetErrorHandler(
      [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        interface.print(F("SCPI COMMAND ERROR,"));
        for (size_t i = 0; i < commands.Size(); i++) {
          interface.print(commands[i]);
          if (i < commands.Size() - 1) {
            interface.print(':');
          }
        }
        interface.println();
      });
}

void processSCPI(void) {
  char *message = scpi.GetMessage(Serial, "\n");
  if (message != NULL) {
    // remove CR to handler properly even if CR + LF sent from the client.
    if (message[strlen(message) - 1] == '\r') {
      message[strlen(message) - 1] = 0;
    }
    M5_LOGD("SCPI Command: %s", message);
    scpi.Execute(message, Serial);
  }
}
