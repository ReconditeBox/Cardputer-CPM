#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>

#define SD_SPI_SCK_PIN   40
#define SD_SPI_MISO_PIN  39
#define SD_SPI_MOSI_PIN  14
#define SD_SPI_CS_PIN    12

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);   // true = enable keyboard

    Serial.begin(115200);

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setTextColor(GREEN, BLACK);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(0, 0);

    M5Cardputer.Display.println("CARDPUTER CPM TEST");
    M5Cardputer.Display.println();
    M5Cardputer.Display.println("Display  : OK");
    M5Cardputer.Display.println("Keyboard : ready");

    SPI.begin(
        SD_SPI_SCK_PIN,
        SD_SPI_MISO_PIN,
        SD_SPI_MOSI_PIN,
        SD_SPI_CS_PIN
    );

    if (SD.begin(SD_SPI_CS_PIN, SPI, 25000000))
    {
        M5Cardputer.Display.println("SD Card  : OK");
    }
    else
    {
        M5Cardputer.Display.println("SD Card  : FAILED");
    }

    M5Cardputer.Display.println();
    M5Cardputer.Display.println("Type something:");
    M5Cardputer.Display.print("> ");
}

void loop()
{
    M5Cardputer.update();

    if (M5Cardputer.Keyboard.isChange())
    {
        if (M5Cardputer.Keyboard.isPressed())
        {
            Keyboard_Class::KeysState status =
                M5Cardputer.Keyboard.keysState();

            for (auto c : status.word)
            {
                M5Cardputer.Display.print(c);
            }

            if (status.enter)
            {
                M5Cardputer.Display.println();
                M5Cardputer.Display.print("> ");
            }

            if (status.del)
            {
                M5Cardputer.Display.print("<BS>");
            }
        }
    }

    delay(5);
}