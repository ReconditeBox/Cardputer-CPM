#include <Arduino.h>
#include <M5Cardputer.h>
#include <string.h>

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
 * Rename RunCPM's original Serial console functions.
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
 * ====================================================
 * LOCAL CARDPUTER TERMINAL
 * ====================================================
 */

#define TERM_COLS 40
#define TERM_ROWS 16

#define CHAR_W 6
#define CHAR_H 8

M5Canvas terminal(&M5Cardputer.Display);

static char termBuffer[TERM_ROWS][TERM_COLS];

static int cursorX = 0;
static int cursorY = 0;

static int savedCursorX = 0;
static int savedCursorY = 0;


/*
 * ANSI parser.
 */
enum
{
    ANSI_NORMAL,
    ANSI_ESC,
    ANSI_CSI
};

static int ansiState = ANSI_NORMAL;

static int ansiParam[4];
static int ansiParamIndex = 0;


/*
 * Shared CP/M keyboard buffer.
 *
 * Both the Cardputer keyboard and USB CDC
 * put characters into this buffer.
 */
static uint8_t keyBuffer[128];
static uint8_t keyHead = 0;
static uint8_t keyTail = 0;


/*
 * Used to avoid converting CR/LF from a PC terminal
 * into two separate CP/M Enter presses.
 */
static bool lastUSBWasCR = false;


/*
 * Forward declarations.
 */
int _kbhit(void);
uint8 _getch(void);
uint8 _getche(void);
void _putch(uint8 ch);
void _clrscr(void);


/*
 * ====================================================
 * DISPLAY
 * ====================================================
 */

static void terminalPush()
{
    terminal.pushSprite(0, 0);
}


static void terminalRenderAll()
{
    terminal.fillSprite(BLACK);

    for (int row = 0; row < TERM_ROWS; row++)
    {
        terminal.setCursor(
            0,
            row * CHAR_H
        );

        for (int col = 0; col < TERM_COLS; col++)
        {
            terminal.write(
                (uint8_t)termBuffer[row][col]
            );
        }
    }

    terminalPush();
}


static void terminalRenderCell(
    int col,
    int row
)
{
    if (
        col < 0 ||
        col >= TERM_COLS ||
        row < 0 ||
        row >= TERM_ROWS
    )
    {
        return;
    }

    terminal.fillRect(
        col * CHAR_W,
        row * CHAR_H,
        CHAR_W,
        CHAR_H,
        BLACK
    );

    terminal.setCursor(
        col * CHAR_W,
        row * CHAR_H
    );

    terminal.write(
        (uint8_t)termBuffer[row][col]
    );

    terminalPush();
}


static void terminalClearBuffer()
{
    for (int row = 0; row < TERM_ROWS; row++)
    {
        for (int col = 0; col < TERM_COLS; col++)
        {
            termBuffer[row][col] = ' ';
        }
    }
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

    terminal.setTextColor(
        GREEN,
        BLACK
    );

    terminal.setTextSize(1);
    terminal.setTextWrap(false);

    cursorX = 0;
    cursorY = 0;

    ansiState = ANSI_NORMAL;

    terminalClearBuffer();
    terminalRenderAll();
}


/*
 * ====================================================
 * SCROLLING
 * ====================================================
 */

static void terminalScroll()
{
    for (int row = 0; row < TERM_ROWS - 1; row++)
    {
        memcpy(
            termBuffer[row],
            termBuffer[row + 1],
            TERM_COLS
        );
    }

    for (int col = 0; col < TERM_COLS; col++)
    {
        termBuffer[TERM_ROWS - 1][col] = ' ';
    }

    cursorY = TERM_ROWS - 1;

    terminalRenderAll();
}


static void terminalCheckCursor()
{
    if (cursorX < 0)
        cursorX = 0;

    if (cursorX >= TERM_COLS)
    {
        cursorX = 0;
        cursorY++;
    }

    if (cursorY < 0)
        cursorY = 0;

    if (cursorY >= TERM_ROWS)
    {
        terminalScroll();
    }
}


/*
 * ====================================================
 * CHARACTER OPERATIONS
 * ====================================================
 */

static void terminalPutPrintable(uint8_t ch)
{
    terminalCheckCursor();

    termBuffer[cursorY][cursorX] = (char)ch;

    terminalRenderCell(
        cursorX,
        cursorY
    );

    cursorX++;

    terminalCheckCursor();
}


static void terminalCR()
{
    cursorX = 0;
}


static void terminalLF()
{
    cursorY++;

    terminalCheckCursor();
}


static void terminalBackspace()
{
    if (cursorX > 0)
    {
        cursorX--;
    }
    else if (cursorY > 0)
    {
        cursorY--;
        cursorX = TERM_COLS - 1;
    }
}


static void terminalTab()
{
    int next =
        ((cursorX / 8) + 1) * 8;

    if (next >= TERM_COLS)
    {
        cursorX = 0;
        cursorY++;
    }
    else
    {
        cursorX = next;
    }

    terminalCheckCursor();
}


/*
 * ====================================================
 * ANSI / VT100
 * ====================================================
 */

static int ansiGetParam(
    int index,
    int defaultValue
)
{
    if (index > ansiParamIndex)
        return defaultValue;

    if (ansiParam[index] == 0)
        return defaultValue;

    return ansiParam[index];
}


static void terminalClearToEnd()
{
    for (int row = cursorY; row < TERM_ROWS; row++)
    {
        int start =
            (row == cursorY)
                ? cursorX
                : 0;

        for (int col = start; col < TERM_COLS; col++)
        {
            termBuffer[row][col] = ' ';
        }
    }

    terminalRenderAll();
}


static void terminalClearFromStart()
{
    for (int row = 0; row <= cursorY; row++)
    {
        int end =
            (row == cursorY)
                ? cursorX
                : TERM_COLS - 1;

        for (int col = 0; col <= end; col++)
        {
            termBuffer[row][col] = ' ';
        }
    }

    terminalRenderAll();
}


static void terminalEraseLine(int mode)
{
    if (mode == 0)
    {
        for (
            int col = cursorX;
            col < TERM_COLS;
            col++
        )
        {
            termBuffer[cursorY][col] = ' ';
        }
    }
    else if (mode == 1)
    {
        for (
            int col = 0;
            col <= cursorX &&
            col < TERM_COLS;
            col++
        )
        {
            termBuffer[cursorY][col] = ' ';
        }
    }
    else if (mode == 2)
    {
        for (int col = 0; col < TERM_COLS; col++)
        {
            termBuffer[cursorY][col] = ' ';
        }
    }

    terminalRenderAll();
}


static void ansiExecute(uint8_t command)
{
    int amount;

    switch (command)
    {
        case 'A':

            amount = ansiGetParam(0, 1);

            cursorY -= amount;

            if (cursorY < 0)
                cursorY = 0;

            break;


        case 'B':

            amount = ansiGetParam(0, 1);

            cursorY += amount;

            if (cursorY >= TERM_ROWS)
                cursorY = TERM_ROWS - 1;

            break;


        case 'C':

            amount = ansiGetParam(0, 1);

            cursorX += amount;

            if (cursorX >= TERM_COLS)
                cursorX = TERM_COLS - 1;

            break;


        case 'D':

            amount = ansiGetParam(0, 1);

            cursorX -= amount;

            if (cursorX < 0)
                cursorX = 0;

            break;


        case 'H':
        case 'f':
        {
            int row =
                ansiGetParam(0, 1);

            int col =
                ansiGetParam(1, 1);

            cursorY = row - 1;
            cursorX = col - 1;

            if (cursorY < 0)
                cursorY = 0;

            if (cursorY >= TERM_ROWS)
                cursorY = TERM_ROWS - 1;

            if (cursorX < 0)
                cursorX = 0;

            if (cursorX >= TERM_COLS)
                cursorX = TERM_COLS - 1;

            break;
        }


        case 'J':
        {
            int mode = ansiParam[0];

            if (mode == 2)
            {
                terminalClearBuffer();

                cursorX = 0;
                cursorY = 0;

                terminalRenderAll();
            }
            else if (mode == 1)
            {
                terminalClearFromStart();
            }
            else
            {
                terminalClearToEnd();
            }

            break;
        }


        case 'K':

            terminalEraseLine(
                ansiParam[0]
            );

            break;


        case 's':

            savedCursorX = cursorX;
            savedCursorY = cursorY;

            break;


        case 'u':

            cursorX = savedCursorX;
            cursorY = savedCursorY;

            terminalCheckCursor();

            break;


        /*
         * Colour/attribute selection is accepted
         * but ignored locally for now.
         */
        case 'm':

            break;


        default:

            break;
    }
}


static void ansiBeginCSI()
{
    memset(
        ansiParam,
        0,
        sizeof(ansiParam)
    );

    ansiParamIndex = 0;
    ansiState = ANSI_CSI;
}


static void terminalProcessCharacter(uint8_t ch)
{
    if (ansiState == ANSI_ESC)
    {
        ansiState = ANSI_NORMAL;

        if (ch == '[')
        {
            ansiBeginCSI();
            return;
        }

        if (ch == '7')
        {
            savedCursorX = cursorX;
            savedCursorY = cursorY;
            return;
        }

        if (ch == '8')
        {
            cursorX = savedCursorX;
            cursorY = savedCursorY;

            terminalCheckCursor();
            return;
        }

        if (ch == 'c')
        {
            _clrscr();
            return;
        }

        return;
    }


    if (ansiState == ANSI_CSI)
    {
        if (ch >= '0' && ch <= '9')
        {
            ansiParam[ansiParamIndex] =
                (ansiParam[ansiParamIndex] * 10)
                + (ch - '0');

            return;
        }

        if (ch == ';')
        {
            if (ansiParamIndex < 3)
            {
                ansiParamIndex++;
            }

            return;
        }

        if (ch == '?')
        {
            return;
        }

        ansiExecute(ch);

        ansiState = ANSI_NORMAL;

        return;
    }


    if (ch == 0x1B)
    {
        ansiState = ANSI_ESC;
        return;
    }


    if (ch == 0x0C)
    {
        _clrscr();
        return;
    }


    if (ch == 0x0D)
    {
        terminalCR();
        return;
    }


    if (ch == 0x0A)
    {
        terminalLF();
        return;
    }


    if (ch == 0x08)
    {
        terminalBackspace();
        return;
    }


    if (ch == 0x09)
    {
        terminalTab();
        return;
    }


    if (ch == 0x07)
    {
        return;
    }


    if (ch >= 0x20)
    {
        terminalPutPrintable(ch);
    }
}


/*
 * ====================================================
 * INPUT BUFFER
 * ====================================================
 */

static void queueKey(uint8_t ch)
{
    uint8_t next =
        (keyHead + 1) %
        sizeof(keyBuffer);

    if (next == keyTail)
    {
        return;
    }

    keyBuffer[keyHead] = ch;

    keyHead = next;
}


/*
 * ====================================================
 * CARDPUTER KEYBOARD INPUT
 * ====================================================
 */

static void pollCardputerKeyboard()
{
    M5Cardputer.update();

    if (!M5Cardputer.Keyboard.isChange())
        return;

    if (!M5Cardputer.Keyboard.isPressed())
        return;


    Keyboard_Class::KeysState status =
        M5Cardputer.Keyboard.keysState();


    for (auto c : status.word)
    {
        uint8_t ch = (uint8_t)c;


        if (status.ctrl)
        {
            if (ch >= 'a' && ch <= 'z')
            {
                ch =
                    (ch - 'a') + 1;
            }
            else if (
                ch >= 'A' &&
                ch <= 'Z'
            )
            {
                ch =
                    (ch - 'A') + 1;
            }
            else if (
                ch >= '@' &&
                ch <= '_'
            )
            {
                ch &= 0x1F;
            }
        }

        queueKey(ch);
    }


    if (status.enter)
    {
        queueKey(0x0D);
    }


    if (
        status.backspace ||
        status.del
    )
    {
        queueKey(0x08);
    }


    if (status.tab)
    {
        queueKey(0x09);
    }


    if (status.esc)
    {
        queueKey(0x1B);
    }
}


/*
 * ====================================================
 * USB CDC INPUT
 * ====================================================
 */

static void pollUSBKeyboard()
{
    while (Serial.available())
    {
        int value = Serial.read();

        if (value < 0)
            break;


        uint8_t ch =
            (uint8_t)value;


        /*
         * DEL from PC terminals becomes CP/M BS.
         */
        if (ch == 0x7F)
        {
            ch = 0x08;
        }


        /*
         * Normalise terminal Enter handling.
         *
         * CR       -> CR
         * LF       -> CR
         * CR + LF  -> one CR only
         */
        if (ch == 0x0D)
        {
            queueKey(0x0D);

            lastUSBWasCR = true;

            continue;
        }


        if (ch == 0x0A)
        {
            if (!lastUSBWasCR)
            {
                queueKey(0x0D);
            }

            lastUSBWasCR = false;

            continue;
        }


        lastUSBWasCR = false;


        /*
         * Everything else passes directly to CP/M,
         * including Ctrl+C, ESC, TAB etc.
         */
        queueKey(ch);
    }
}


/*
 * Poll every available console input source.
 */
static void pollInputs()
{
    pollCardputerKeyboard();
    pollUSBKeyboard();
}


/*
 * ====================================================
 * RunCPM CONSOLE
 * ====================================================
 */

int _kbhit(void)
{
    pollInputs();

    return (
        keyHead != keyTail
    );
}


uint8 _getch(void)
{
    while (
        keyHead == keyTail
    )
    {
        pollInputs();

        delay(1);
    }


    uint8 ch =
        keyBuffer[keyTail];


    keyTail =
        (keyTail + 1) %
        sizeof(keyBuffer);


    return ch;
}


uint8 _getche(void)
{
    uint8 ch =
        _getch();

    _putch(ch);

    return ch;
}


void _putch(uint8 ch)
{
    /*
     * Native Cardputer console.
     */
    terminalProcessCharacter(ch);


    /*
     * Exact same CP/M output also goes to USB.
     */
    Serial.write(ch);
}


void _clrscr(void)
{
    terminalClearBuffer();

    cursorX = 0;
    cursorY = 0;

    ansiState = ANSI_NORMAL;

    terminalRenderAll();
}


/*
 * ====================================================
 * PUN: AND LST:
 * ====================================================
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
 * ====================================================
 * RunCPM CORE
 * ====================================================
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
 * ====================================================
 * START CP/M
 * ====================================================
 */

void setup()
{
    /*
     * ESP32-S3 native USB CDC.
     *
     * Do not wait for a PC.
     * CP/M remains a standalone Cardputer system.
     */
    Serial.begin();


    /*
     * Cardputer hardware.
     */
    auto cfg =
        M5.config();

    M5Cardputer.begin(
        cfg,
        true
    );


    /*
     * Native local terminal.
     */
    terminalInit();


    _puts(
        "CARDPUTER CP/M\r\n"
    );

    _puts(
        "--------------\r\n"
    );

    _puts(
        "\r\n"
    );


    /*
     * SD card.
     */
    _puts(
        "Initializing SD...\r\n"
    );


    SPI.begin(
        SPIINIT
    );


    if (!SD.begin(SDINIT))
    {
        _puts(
            "\r\n"
            "SD CARD FAILED\r\n"
        );

        return;
    }


    _puts(
        "SD card OK\r\n"
    );


    _puts(
        "RunCPM "
    );

    _puts(
        VERSION
    );

    _puts(
        "\r\n"
    );


    _puts(
        "Board: "
    );

    _puts(
        BOARD
    );

    _puts(
        "\r\n"
    );


    _puts(
        "CPU: "
    );

    _puts(
        CPU_IS
    );

    _puts(
        "\r\n\r\n"
    );


#ifndef DEBUG

    Z80estimateClock();

#endif


#ifdef DEBUGLOG

    _sys_deletefile(
        (uint8 *)LogName
    );

#endif


    if (!(
        VersionCCP >= 0x10 ||
        SD.exists(CCPname)
    ))
    {
        _puts(
            "Unable to load CCP.\r\n"
        );

        return;
    }


#ifdef ABDOS

    _PatchBIOS();

#endif


    /*
     * Main CP/M loop.
     */
    while (true)
    {
        _puts(
            CCPHEAD
        );


        _PatchCPM();


        Status =
            STATUS_RUNNING;


#ifdef CCP_INTERNAL

        _ccp();

#else

        if (!_RamLoad(
            (uint8 *)CCPname,
            CCPaddr,
            0
        ))
        {
            _puts(
                "Unable to load CCP.\r\n"
            );

            break;
        }


        if (firstBoot)
        {
            if (_sys_exists(
                (uint8 *)AUTOEXEC
            ))
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
                firstBoot =
                    FALSE;
            }
        }


        Z80reset();


        SET_LOW_REGISTER(
            BC,
            _RamRead(
                DSKByte
            )
        );


        PC =
            CCPaddr;


        Z80run(
            cpuDelayInstructions
        );

#endif


        if (
            Status ==
            STATUS_EXIT
        )
        {
            break;
        }


#ifdef USE_PUN

        if (pun_dev)
        {
            _sys_fflush(
                pun_dev
            );
        }

#endif


#ifdef USE_LST

        if (lst_dev)
        {
            _sys_fflush(
                lst_dev
            );
        }

#endif
    }


    _puts(
        "\r\nCP/M halted.\r\n"
    );
}


void loop()
{
    pollInputs();

    delay(5);
}