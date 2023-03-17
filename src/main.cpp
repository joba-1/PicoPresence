/* 
Presence detection with LD2410 on a Raspberry Pico W
For now mostly the setupSensor example provided with the ld2410 library
*/

#include <Arduino.h>

#include <ld2410.h>
#include <radar.h>


ld2410 radar;
String command;
bool sensor_health = true;

static Stream &out = Serial;
// volatile uint32_t counter = 0;


void setup1() {
  // counter++; // show core 0 that core 1 has started
  Serial.begin(115200);
  while (!Serial && millis() < 2000);  // Sometimes Serial is false forever. The output is not _that_ important...
  Serial2.setRX(5);
  Serial2.setTX(4);
  Serial2.begin (256000, SERIAL_8N1);  // UART for monitoring the radar
  Serial.printf("%lu: LD2410 radar sensor initialising: ", millis());
  if(radar.begin(Serial2))
  {
    Serial.println("OK");
    Serial.println("Supported commands\nread: read current values from the sensor\nreadconfig: read the configuration from the sensor\nsetmaxvalues <motion gate> <stationary gate> <inactivitytimer>\nsetsensitivity <gate> <motionsensitivity> <stationarysensitivity>\nenableengineeringmode: enable engineering mode\ndisableengineeringmode: disable engineering mode\nrestart: restart the sensor\nreadversion: read firmware version\nfactoryreset: factory reset the sensor\n");
  }
  else
  {
    Serial.println("not connected");
  }
}


void loop1() {
  // counter++;  // show core 0 that core 1 is processing the loop1

  radar.read();  // Always read frames from the sensor

  // RadarStatus always contains latest valid values updated every 10ms
  const uint32_t interval = 10;
  static uint32_t prev_ms = 0;
  uint32_t now = millis();
  if (now - prev_ms > interval) {
    prev_ms = now;
    RadarStatus.update(radar);
  }

  // for testing sensor settings...
  if(out.available())
  {
    char typedCharacter = out.read();
    if(typedCharacter == '\r' || typedCharacter == '\n')
    {
      command.trim();
      if(command.equals("read"))
      {
        command = "";
        out.print(F("Reading from sensor: "));
        if(radar.isConnected())
        {
          out.println(F("OK"));
          if(radar.presenceDetected())
          {
            if(radar.stationaryTargetDetected())
            {
              out.print(F("Stationary target: "));
              out.print(radar.stationaryTargetDistance());
              out.print(F("cm energy: "));
              out.println(radar.stationaryTargetEnergy());
            }
            if(radar.movingTargetDetected())
            {
              out.print(F("Moving target: "));
              out.print(radar.movingTargetDistance());
              out.print(F("cm energy: "));
              out.println(radar.movingTargetEnergy());
            }
          }
          else
          {
            out.println(F("nothing detected"));
          }
        }
        else
        {
          out.println(F("failed to read"));
        }
      }
      else if(command.equals("readconfig"))
      {
        command = "";
        out.print(F("Reading configuration from sensor: "));
        if(radar.requestCurrentConfiguration())
        {
          out.println(F("OK"));
          out.print(F("Maximum gate ID: "));
          out.println(radar.max_gate);
          out.print(F("Maximum gate for moving targets: "));
          out.println(radar.max_moving_gate);
          out.print(F("Maximum gate for stationary targets: "));
          out.println(radar.max_stationary_gate);
          out.print(F("Idle time for targets: "));
          out.println(radar.sensor_idle_time);
          out.println(F("Gate sensitivity"));
          for(uint8_t gate = 0; gate <= radar.max_gate; gate++)
          {
            out.print(F("Gate "));
            out.print(gate);
            out.print(F(" moving targets: "));
            out.print(radar.motion_sensitivity[gate]);
            out.print(F(" stationary targets: "));
            out.println(radar.stationary_sensitivity[gate]);
          }
        }
        else
        {
          out.println(F("Failed"));
        }
      }
      else if(command.startsWith("setmaxvalues"))
      {
        uint8_t firstSpace = command.indexOf(' ');
        uint8_t secondSpace = command.indexOf(' ',firstSpace + 1);
        uint8_t thirdSpace = command.indexOf(' ',secondSpace + 1);
        uint8_t newMovingMaxDistance = (command.substring(firstSpace,secondSpace)).toInt();
        uint8_t newStationaryMaxDistance = (command.substring(secondSpace,thirdSpace)).toInt();
        uint16_t inactivityTimer = (command.substring(thirdSpace,command.length())).toInt();
        if(newMovingMaxDistance > 0 && newStationaryMaxDistance > 0 && newMovingMaxDistance <= 8 && newStationaryMaxDistance <= 8)
        {
          out.print(F("Setting max values to gate "));
          out.print(newMovingMaxDistance);
          out.print(F(" moving targets, gate "));
          out.print(newStationaryMaxDistance);
          out.print(F(" stationary targets, "));
          out.print(inactivityTimer);
          out.print(F("s inactivity timer: "));
          command = "";
          if(radar.setMaxValues(newMovingMaxDistance, newStationaryMaxDistance, inactivityTimer))
          {
            out.println(F("OK, now restart to apply settings"));
          }
          else
          {
            out.println(F("failed"));
          }
        }
        else
        {
          out.print(F("Can't set distances to "));
          out.print(newMovingMaxDistance);
          out.print(F(" moving "));
          out.print(newStationaryMaxDistance);
          out.println(F(" stationary, try again"));
          command = "";
        }
      }
      else if(command.startsWith("setsensitivity"))
      {
        uint8_t firstSpace = command.indexOf(' ');
        uint8_t secondSpace = command.indexOf(' ',firstSpace + 1);
        uint8_t thirdSpace = command.indexOf(' ',secondSpace + 1);
        uint8_t gate = (command.substring(firstSpace,secondSpace)).toInt();
        uint8_t motionSensitivity = (command.substring(secondSpace,thirdSpace)).toInt();
        uint8_t stationarySensitivity = (command.substring(thirdSpace,command.length())).toInt();
        if(motionSensitivity >= 0 && stationarySensitivity >= 0 && motionSensitivity <= 100 && stationarySensitivity <= 100)
        {
          out.print(F("Setting gate "));
          out.print(gate);
          out.print(F(" motion sensitivity to "));
          out.print(motionSensitivity);
          out.print(F(" & stationary sensitivity to "));
          out.print(stationarySensitivity);
          out.println(F(": "));
          command = "";
          if(radar.setGateSensitivityThreshold(gate, motionSensitivity, stationarySensitivity))
          {
            out.println(F("OK, now restart to apply settings"));
          }
          else
          {
            out.println(F("failed"));
          }
        }
        else
        {
          out.print(F("Can't set gate "));
          out.print(gate);
          out.print(F(" motion sensitivity to "));
          out.print(motionSensitivity);
          out.print(F(" & stationary sensitivity to "));
          out.print(stationarySensitivity);
          out.println(F(", try again"));
          command = "";
        }
      }
      else if(command.equals("enableengineeringmode"))
      {
        command = "";
        out.print(F("Enabling engineering mode: "));
        if(radar.requestStartEngineeringMode())
        {
          out.println(F("OK"));
        }
        else
        {
          out.println(F("failed"));
        }
      }
      else if(command.equals("disableengineeringmode"))
      {
        command = "";
        out.print(F("Disabling engineering mode: "));
        if(radar.requestEndEngineeringMode())
        {
          out.println(F("OK"));
        }
        else
        {
          out.println(F("failed"));
        }
      }
      else if(command.equals("restart"))
      {
        command = "";
        out.print(F("Restarting sensor: "));
        if(radar.requestRestart())
        {
          out.println(F("OK"));
        }
        else
        {
          out.println(F("failed"));
        }
      }
      else if(command.equals("readversion"))
      {
        command = "";
        out.print(F("Requesting firmware version: "));
        if(radar.requestFirmwareVersion())
        {
          out.print('v');
          out.print(radar.firmware_major_version);
          out.print('.');
          out.print(radar.firmware_minor_version);
          out.print('.');
          out.println(radar.firmware_bugfix_version,HEX);
        }
        else
        {
          out.println(F("Failed"));
        }
      }
      else if(command.equals("factoryreset"))
      {
        command = "";
        out.print(F("Factory resetting sensor: "));
        if(radar.requestFactoryReset())
        {
          out.println(F("OK, now restart sensor to take effect"));
        }
        else
        {
          out.println(F("failed"));
        }
      }
      else
      {
        out.print(F("Unknown command: "));
        out.println(command);
        command = "";
      }
    }
    else
    {
      command += typedCharacter;
    }
  }
}
