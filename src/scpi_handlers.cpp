#include "scpi_handlers.h"

#include "chatbot.h"
#include "servo_controller.h"
#include <M5Unified.h>
#include <Preferences.h>
#include <Vrekrer_scpi_parser.h>
#include <WiFi.h>

extern ServoController servo_controller;

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
 *    TORQ
 *      :OFF
 *      :ON
 *    CHAT
 *      :TEXT <text...>
 *      :START
 *      :STOP
 *      :ABORT
 *      :STAT?
 *      :FORMAT A|B
 *      :FORMAT?
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

  scpi.SetCommandTreeBase(F("TORQ"));
  scpi.RegisterCommand(F(":OFF"), [](SCPI_C commands, SCPI_P parameters,
                                          Stream &interface) {
    servo_controller.torqueOff();
    interface.println(F("OK"));
  });
  scpi.RegisterCommand(
      F(":ON"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        servo_controller.torqueOn();
        interface.println(F("OK"));
      });

  scpi.SetCommandTreeBase(F("CHAT"));
  scpi.RegisterCommand(
      F(":TEXT"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        String text;
        for (size_t i = 0; i < parameters.Size(); i++) {
          if (i > 0) text += " ";
          text += parameters[i];
        }
        text.trim();
        // 引用符で囲まれていたら剥がす
        if (text.length() >= 2 && text.startsWith("\"") &&
            text.endsWith("\"")) {
          text = text.substring(1, text.length() - 1);
        }
        if (text.isEmpty()) {
          interface.println(F("NG,empty text"));
          return;
        }
        if (chatbotSendText(text.c_str())) {
          interface.printf("OK,send %d bytes\r\n", (int)text.length());
        } else {
          interface.print(F("NG,"));
          interface.println(chatbotStatus());
        }
      });
  scpi.RegisterCommand(
      F(":START"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        (void)commands;
        (void)parameters;
        chatbotStart();
        interface.print(F("OK,"));
        interface.println(chatbotStatus());
      });
  scpi.RegisterCommand(
      F(":STOP"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        (void)commands;
        (void)parameters;
        chatbotStop();
        interface.print(F("OK,"));
        interface.println(chatbotStatus());
      });
  scpi.RegisterCommand(
      F(":ABORT"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        (void)commands;
        (void)parameters;
        chatbotAbort();
        interface.print(F("OK,"));
        interface.println(chatbotStatus());
      });
  scpi.RegisterCommand(
      F(":STAT?"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        (void)commands;
        (void)parameters;
        interface.println(chatbotStatus());
        interface.println(chatbotLastChat());
      });
  scpi.RegisterCommand(
      F(":FORMAT"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        const char* p0 = parameters[0];
        if (p0 != nullptr && (p0[0] == 'B' || p0[0] == 'b')) {
          chatbotSetFormatB(true);
          interface.println(F("OK,B"));
        } else if (p0 != nullptr && (p0[0] == 'A' || p0[0] == 'a')) {
          chatbotSetFormatB(false);
          interface.println(F("OK,A"));
        } else {
          interface.println(F("NG,use A or B"));
        }
      });
  scpi.RegisterCommand(
      F(":FORMAT?"), [](SCPI_C commands, SCPI_P parameters, Stream &interface) {
        (void)commands;
        (void)parameters;
        interface.println(chatbotGetFormatB() ? F("B") : F("A"));
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
