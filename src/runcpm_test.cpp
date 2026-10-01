#include <Arduino.h>
#include <M5Cardputer.h>

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setTextColor(GREEN, BLACK);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(0, 0);

    M5Cardputer.Display.println("RUNCPM ENV TEST");
    M5Cardputer.Display.println();
    M5Cardputer.Display.println("Display: OK");
    M5Cardputer.Display.println("Firmware: runcpm-test");

    Serial.begin(115200);

    delay(2000);

    Serial.println();
    Serial.println("RUNCPM ENV TEST");
    Serial.println("Serial: OK");
}

void loop()
{
    M5Cardputer.update();
    delay(10);
}