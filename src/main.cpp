#include <Arduino.h>
#include <M5Cardputer.h>
#include <string.h>

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
 * ----------------------------------------------------
 * Rename RunCPM's Serial console functions.
 * We provide Cardputer-native replacements below.
 * ----------------------------------------------------
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
 * Cardputer local terminal
 * ====================================================
 *
 * Native display: 240 x 135
 *
 * Default 6 x 8 font:
 *
 *     40 columns
 *     16 rows
 *
 * 40 * 6 = 240
 * 16 * 8 = 128
 *
 * The remaining pixels at the bottom are unused.
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
 * ANSI parser states.
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
 * Keyboard buffer.
 */
static uint8_t keyBuffer[64];
static uint8_t keyHead = 0;
static uint8_t keyTail = 0;


/*
 * Forward declarations required by C++.
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
    if (col < 0 ||
        col >= TERM_COLS ||
        row < 0 ||
        row >= TERM_ROWS)
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

    /*
     * We do our own wrapping.
     */
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
 * TERMINAL CHARACTER OPERATIONS
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
 * ANSI / VT100 SUPPORT
 * ====================================================
 *
 * Implemented:
 *
 * ESC [ A     cursor up
 * ESC [ B     cursor down
 * ESC [ C     cursor right
 * ESC [ D     cursor left
 *
 * ESC [ H     cursor position
 * ESC [ f     cursor position
 *
 * ESC [ J     erase display
 * ESC [ K     erase line
 *
 * ESC [ s     save cursor
 * ESC [ u     restore cursor
 *
 * SGR (ESC [ ... m) is accepted and ignored for now.
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
        /*
         * Cursor to end.
         */
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
        /*
         * Start to cursor.
         */
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
        /*
         * Entire line.
         */
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
        /*
         * Cursor up.
         */
        case 'A':

            amount = ansiGetParam(0, 1);

            cursorY -= amount;

            if (cursorY < 0)
                cursorY = 0;

            break;


        /*
         * Cursor down.
         */
        case 'B':

            amount = ansiGetParam(0, 1);

            cursorY += amount;

            if (cursorY >= TERM_ROWS)
                cursorY = TERM_ROWS - 1;

            break;


        /*
         * Cursor right.
         */
        case 'C':

            amount = ansiGetParam(0, 1);

            cursorX += amount;

            if (cursorX >= TERM_COLS)
                cursorX = TERM_COLS - 1;

            break;


        /*
         * Cursor left.
         */
        case 'D':

            amount = ansiGetParam(0, 1);

            cursorX -= amount;

            if (cursorX < 0)
                cursorX = 0;

            break;


        /*
         * Cursor position.
         *
         * ANSI positions are 1 based:
         *
         * ESC [ row ; column H
         */
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


        /*
         * Erase display.
         */
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


        /*
         * Erase line.
         */
        case 'K':

            terminalEraseLine(
                ansiParam[0]
            );

            break;


        /*
         * Save cursor.
         */
        case 's':

            savedCursorX = cursorX;
            savedCursorY = cursorY;

            break;


        /*
         * Restore cursor.
         */
        case 'u':

            cursorX = savedCursorX;
            cursorY = savedCursorY;

            terminalCheckCursor();

            break;


        /*
         * Select Graphic Rendition.
         *
         * At present we deliberately ignore colours
         * and attributes while accepting the sequence.
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
    /*
     * ---------------------------
     * ESCAPE STATE
     * ---------------------------
     */

    if (ansiState == ANSI_ESC)
    {
        ansiState = ANSI_NORMAL;

        if (ch == '[')
        {
            ansiBeginCSI();
            return;
        }

        /*
         * ANSI save/restore cursor.
         */
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

        /*
         * ESC c = reset terminal.
         */
        if (ch == 'c')
        {
            _clrscr();
            return;
        }

        return;
    }


    /*
     * ---------------------------
     * CSI STATE
     * ---------------------------
     */

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

        /*
         * Ignore ANSI private-mode markers such as '?'.
         */
        if (ch == '?')
        {
            return;
        }

        ansiExecute(ch);

        ansiState = ANSI_NORMAL;

        return;
    }


    /*
     * ---------------------------
     * NORMAL STATE
     * ---------------------------
     */

    if (ch == 0x1B)
    {
        ansiState = ANSI_ESC;
        return;
    }


    /*
     * Form feed.
     */
    if (ch == 0x0C)
    {
        _clrscr();
        return;
    }


    /*
     * Carriage return.
     */
    if (ch == 0x0D)
    {
        terminalCR();
        return;
    }


    /*
     * Line feed.
     */
    if (ch == 0x0A)
    {
        terminalLF();
        return;
    }


    /*
     * Backspace.
     */
    if (ch == 0x08)
    {
        terminalBackspace();
        return;
    }


    /*
     * Tab.
     */
    if (ch == 0x09)
    {
        terminalTab();
        return;
    }


    /*
     * Bell.
     *
     * Ignore for the moment.
     */
    if (ch == 0x07)
    {
        return;
    }


    /*
     * Printable characters.
     */
    if (ch >= 0x20)
    {
        terminalPutPrintable(ch);
    }
}


/*
 * ====================================================
 * KEYBOARD
 * ====================================================
 */

static void queueKey(uint8_t ch)
{
    uint8_t next =
        (keyHead + 1) %
        sizeof(keyBuffer);

    if (next == keyTail)
    {
        /*
         * Buffer full.
         */
        return;
    }

    keyBuffer[keyHead] = ch;

    keyHead = next;
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
     * Normal printable keyboard input.
     */
    for (auto c : status.word)
    {
        uint8_t ch = (uint8_t)c;


        /*
         * Ctrl+A through Ctrl+Z.
         *
         * ASCII control characters are:
         *
         * Ctrl+A = 01h
         * ...
         * Ctrl+Z = 1Ah
         */
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


    /*
     * CP/M expects carriage return from Enter.
     */
    if (status.enter)
    {
        queueKey(0x0D);
    }


    /*
     * CP/M conventionally uses BS for erase.
     */
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
 * RunCPM CONSOLE FUNCTIONS
 * ====================================================
 */

int _kbhit(void)
{
    pollKeyboard();

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
        pollKeyboard();

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
    terminalProcessCharacter(ch);
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
 * RunCPM PUN: and LST:
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
     * Start the Cardputer.
     */
    auto cfg =
        M5.config();

    M5Cardputer.begin(
        cfg,
        true
    );


    /*
     * Start the local terminal.
     */
    terminalInit();


    _puts(
        "CARDPUTER CP/M\r\n"
    );

    _puts(
        "--------------\r\n"
    );

    _puts("\r\n");


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


    /*
     * Check CCP.
     */
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


/*
 * Usually CP/M remains inside setup().
 *
 * If it exits, continue servicing the
 * Cardputer keyboard.
 */
void loop()
{
    pollKeyboard();

    delay(5);
}