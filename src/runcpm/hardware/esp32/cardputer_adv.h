#ifndef CARDPUTER_ADV_H
#define CARDPUTER_ADV_H

/*
 * RunCPM hardware definition for M5Stack Cardputer Adv
 *
 * microSD:
 *   SCK  = GPIO 40
 *   MISO = GPIO 39
 *   MOSI = GPIO 14
 *   CS   = GPIO 12
 */

SdFat SD;

#define SPIINIT 40,39,14,12
#define SDMHZ 20
#define SDINIT 12, SD_SCK_MHZ(SDMHZ)

#define BOARD "M5Stack Cardputer Adv"

#define board_esp32
#define board_digital_io
#define board_cardputer_setdef
#define board_cardputer_ifconfig
#define board_cardputer_telnetd
#define board_cardputer_mem
#define board_cardputer_ftpd
#define board_cardputer_aux
#define board_cardputer_battery
#define board_cardputer_dns
#define board_cardputer_ping
#define board_cardputer_telnet
#define board_cardputer_removable_media

/*
 * Cardputer removable-media slots.
 *
 * Logical CP/M drives A: and B: are physical-style removable slots.
 * Each slot can be mounted to a directory below MEDIA/ on the SD card.
 * The slots start empty on every power-up.
 */
#define CARDPUTER_MEDIA_ROOT "MEDIA"
#define CARDPUTER_MEDIA_NAME_MAX 31

static char cardputerMediaMount[2][CARDPUTER_MEDIA_NAME_MAX + 1] =
{
    "",
    ""
};

static bool cardputerMediaMounted(uint8 drive)
{
    return (
        drive < 2 &&
        cardputerMediaMount[drive][0] != 0
    );
}

static const char *cardputerMediaName(uint8 drive)
{
    if (
        drive >= 2 ||
        !cardputerMediaMounted(drive)
    )
    {
        return "";
    }

    return cardputerMediaMount[drive];
}

static bool cardputerMediaRootPath(
    uint8 drive,
    char *path,
    size_t pathSize
)
{
    if (
        drive >= 2 ||
        !cardputerMediaMounted(drive) ||
        !path ||
        pathSize == 0
    )
    {
        return false;
    }

    int written =
        snprintf(
            path,
            pathSize,
            "%s/%s",
            CARDPUTER_MEDIA_ROOT,
            cardputerMediaMount[drive]
        );

    return (
        written > 0 &&
        (size_t)written < pathSize
    );
}

/*
 * RunCPM Arduino abstraction expects an LED definition.
 * GPIO 2 is harmless for our current use.
 */
#define LED 2
#define LEDinv 0


/*
 * Implemented in main.cpp.
 *
 * RunCPM BDOS function 232 calls esp32bdos(DE).
 */
uint8 cardputerEsp32Bdos(uint16 value);
uint16 cardputerSetdefBdos(uint16 commandTail);
uint16 cardputerIfconfigBdos(uint16 commandTail);
uint16 cardputerTelnetdBdos(uint16 commandTail);
uint16 cardputerMemBdos();
uint16 cardputerFtpdBdos();
uint16 cardputerBatteryBdos();
uint16 cardputerDnsBdos(uint16 commandTail);
uint16 cardputerPingBdos(uint16 commandTail);
uint16 cardputerTelnetBdos(uint16 commandTail);

uint8 cardputerAuxRead();
void cardputerAuxWrite(uint8 value);
uint8 cardputerAuxInputReady();
uint8 cardputerAuxOutputReady();


uint8 esp32bdos(uint16 value)
{
    return cardputerEsp32Bdos(value);
}

#endif