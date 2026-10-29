/*
 * arduino_entry.cpp - entry point for the wokwi.com web editor only.
 *
 * wokwi.com compiles "ESP-IDF" projects with the Arduino-ESP32 core, which
 * defines app_main() itself and then calls setup() and loop(). Those must be
 * C++ functions, so they live here. Native ESP-IDF builds (idf.py, GitHub
 * Actions, Wokwi for VS Code) do not compile this file and use app_main().
 */
#ifdef ARDUINO
#include <Arduino.h>

extern "C" void cocis_start(void);

void setup(void)
{
    cocis_start();
}

void loop(void)
{
    vTaskDelay(portMAX_DELAY);   /* all work happens in FreeRTOS tasks */
}
#endif
