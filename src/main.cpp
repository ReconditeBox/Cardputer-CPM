#include <Arduino.h>
#include <M5Cardputer.h>

/*
 * Arduino ESP32 defines NOP() itself.
 * RunCPM uses NOP as Z80 opcode 00h.
 */
#ifdef NOP
#undef NOP
#endif

#define CPU "runcpm/cpu1.h"

#include "runcpm/globals.h"

#include <SPI.h>

#define SDFAT_FILE_TYPE 1
#define DISABLE_FS_H_WARNING

#include <SdFat.h>

#include "runcpm/hardware/esp32/cardputer_adv.h"

/*
 * RunCPM's Arduino abstraction contains Serial-based
 * console routines.
 *
 * Rename those while including it, because we provide
 * our own Cardputer screen/keyboard versions below.
 */
#define _kbhit  _serial_kbhit
#define _getch  _serial_getch
#define _getche _serial_getche
#define _putch  _serial_putch
#define _clrscr _serial_clrscr

#include "runcpm/abstraction_arduino.h"

#undef _kbhit
#undef _getch
#undef _getche
#undef _putch
#undef _clrscr


/*
 * ----------------------------------------------------
 * Cardputer local terminal
 * ----------------------------------------------------
 */

M5Canvas terminal(&M5Cardputer.Display);

static uint8_t keyBuffer[64];
static uint8_t keyHead = 0;
static uint8_t keyTail = 0;


/*
 * Forward declarations for our RunCPM console functions.
 */
int _kbhit(void);
uint8 _getch(void);
uint8 _getche(void);
void _putch(uint8 ch);
void _clrscr(void);


static void terminalPush()
{
    terminal.pushSprite(0, 0);
}


static void terminalInit()
{
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);

    terminal.setColorDepth(8);

    terminal.createSprite(
        M5Cardputer.Display.width(),
        M5Cardputer.Display.height()
    );

    terminal.fillSprite(BLACK);

    terminal.setTextColor(GREEN, BLACK);
    terminal.setTextSize(1);

    terminal.setTextWrap(true);
    terminal.setTextScroll(true);

    terminal.setScrollRect(
        0,
        0,
        terminal.width(),
        terminal.height(),
        BLACK
    );

    terminal.setCursor(0, 0);

    terminalPush();
}


static void queueKey(uint8_t c)
{
    uint8_t next =
        (keyHead + 1) % sizeof(keyBuffer);

    if (next != keyTail)
    {
        keyBuffer[keyHead] = c;
        keyHead = next;
    }
}


static void pollKeyboard()
{
    M5Cardputer.update();

    if (!M5Cardputer.Keyboard.isChange())
        return;

    if (!M5Cardputer.Keyboard.isPressed())
        return;

    Keyboard_Class::KeysState status =
        M5Cardputer.Keyboard.keysState();

    /*
     * Printable characters.
     */
    for (auto c : status.word)
    {
        uint8_t ch = (uint8_t)c;

        /*
         * Ctrl+A ... Ctrl+Z become
         * CP/M control characters 01h ... 1Ah.
         */
        if (status.ctrl)
        {
            if (ch >= '@' && ch <= '_')
            {
                ch &= 0x1F;
            }
        }

        queueKey(ch);
    }

    if (status.enter)
        queueKey(0x0D);

    if (status.backspace || status.del)
        queueKey(0x08);

    if (status.tab)
        queueKey(0x09);

    if (status.esc)
        queueKey(0x1B);
}


/*
 * ----------------------------------------------------
 * RunCPM console abstraction
 * ----------------------------------------------------
 */

int _kbhit(void)
{
    pollKeyboard();

    return (keyHead != keyTail);
}


uint8 _getch(void)
{
    while (keyHead == keyTail)
    {
        pollKeyboard();
        delay(1);
    }

    uint8 ch = keyBuffer[keyTail];

    keyTail =
        (keyTail + 1) % sizeof(keyBuffer);

    return ch;
}


void _clrscr(void)
{
    terminal.fillSprite(BLACK);
    terminal.setCursor(0, 0);

    terminalPush();
}


void _putch(uint8 ch)
{
    /*
     * Form feed = clear screen.
     */
    if (ch == 0x0C)
    {
        _clrscr();
        return;
    }

    /*
     * Backspace.
     */
    if (ch == 0x08)
    {
        int x = terminal.getCursorX();
        int y = terminal.getCursorY();

        if (x >= 6)
        {
            x -= 6;

            terminal.setCursor(x, y);
            terminal.print(' ');

            terminal.setCursor(x, y);
        }

        terminalPush();
        return;
    }

    /*
     * Normal characters, CR and LF.
     */
    terminal.write(ch);

    terminalPush();
}


uint8 _getche(void)
{
    uint8 ch = _getch();

    _putch(ch);

    return ch;
}


/*
 * ----------------------------------------------------
 * RunCPM auxiliary devices
 * ----------------------------------------------------
 */

#ifdef USE_PUN

File pun_dev;
int pun_open = FALSE;

#endif


#ifdef USE_LST

File lst_dev;
int lst_open = FALSE;

#endif


/*
 * ----------------------------------------------------
 * RunCPM core
 * ----------------------------------------------------
 */

#include "runcpm/ram.h"
#include "runcpm/console.h"

#include CPU

#include "runcpm/disk.h"
#include "runcpm/host.h"
#include "runcpm/cpm.h"

#ifdef CCP_INTERNAL
#include "runcpm/ccp.h"
#endif


/*
 * ----------------------------------------------------
 * Start CP/M
 * ----------------------------------------------------
 */

void setup()
{
    /*
     * Start Cardputer hardware.
     */
    auto cfg = M5.config();

    M5Cardputer.begin(cfg, true);

    terminalInit();

    _puts("CARDPUTER CP/M\r\n");
    _puts("--------------\r\n");
    _puts("\r\n");

    _puts("Initializing SPI...\r\n");

    SPI.begin(SPIINIT);

    _puts("Initializing SD card...\r\n");

    if (!SD.begin(SDINIT))
    {
        _puts("\r\n");
        _puts("SD CARD FAILED\r\n");

        return;
    }

    _puts("SD card OK\r\n");

    _puts("RunCPM ");
    _puts(VERSION);
    _puts("\r\n");

    _puts("Board: ");
    _puts(BOARD);
    _puts("\r\n");

    _puts("CPU: ");
    _puts(CPU_IS);
    _puts("\r\n");

    _puts("\r\nStarting CP/M...\r\n\r\n");


#ifndef DEBUG

    Z80estimateClock();

#endif


#ifdef DEBUGLOG

    _sys_deletefile((uint8 *)LogName);

#endif


    /*
     * Check that a CCP is available.
     */
    if (!(VersionCCP >= 0x10 ||
          SD.exists(CCPname)))
    {
        _puts("Unable to load CP/M CCP.\r\n");
        return;
    }


#ifdef ABDOS

    _PatchBIOS();

#endif


    /*
     * Main CP/M / CCP loop.
     */
    while (true)
    {
        _puts(CCPHEAD);

        _PatchCPM();

        Status = STATUS_RUNNING;


#ifdef CCP_INTERNAL

        _ccp();

#else

        if (!_RamLoad(
                (uint8 *)CCPname,
                CCPaddr,
                0))
        {
            _puts(
                "Unable to load CCP.\r\n"
            );

            break;
        }


        if (firstBoot)
        {
            if (_sys_exists(
                    (uint8 *)AUTOEXEC))
            {
                uint16 cmd =
                    CCPaddr + 8;

                uint8 bytesread =
                    (uint8)_RamLoad(
                        (uint8 *)AUTOEXEC,
                        cmd,
                        125
                    );

                uint8 blen = 0;

                while (
                    blen < bytesread &&
                    _RamRead(
                        cmd + blen
                    ) > 31
                )
                {
                    blen++;
                }

                _RamWrite(
                    cmd + blen,
                    0x00
                );

                _RamWrite(
                    --cmd,
                    blen
                );
            }

            if (BOOTONLY)
            {
                firstBoot = FALSE;
            }
        }


        Z80reset();

        SET_LOW_REGISTER(
            BC,
            _RamRead(DSKByte)
        );

        PC = CCPaddr;

        Z80run(
            cpuDelayInstructions
        );

#endif


        if (Status == STATUS_EXIT)
        {
            break;
        }


#ifdef USE_PUN

        if (pun_dev)
        {
            _sys_fflush(pun_dev);
        }

#endif


#ifdef USE_LST

        if (lst_dev)
        {
            _sys_fflush(lst_dev);
        }

#endif
    }


    _puts("\r\nCP/M halted.\r\n");
}


void loop()
{
    pollKeyboard();
    delay(5);
}