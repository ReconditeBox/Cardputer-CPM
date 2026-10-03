#ifndef CARDPUTER_FTPD_H
#define CARDPUTER_FTPD_H

/*
 * Minimal FTP server for the Cardputer CP/M environment.
 *
 * The FTP namespace is deliberately NOT a view of the SD card.
 * It exposes only CP/M drive F:, with one flat user area at a time.
 *
 * FTP CWD 0 .. CWD 15 selects the CP/M user number.
 * The corresponding host backing folders are:
 *
 *   F/0 ... F/9, F/A ... F/F
 *
 * No FTP directory creation/removal is supported and filenames are
 * constrained to CP/M-compatible 8.3 names.
 */

#define FTP_CONTROL_PORT 21
#define FTP_PASSIVE_PORT_FIRST 50000
#define FTP_PASSIVE_PORT_LAST  50199
#define FTP_CONTROL_LINE_MAX 255
#define FTP_FILE_PATH_MAX 64
#define FTP_FILE_NAME_MAX 13
#define FTP_DATA_BUFFER_SIZE 1024
#define FTP_DATA_CONNECT_TIMEOUT_MS 8000

static WiFiServer *ftpControlServer =
    NULL;

static WiFiServer *ftpPassiveServer =
    NULL;

static WiFiClient ftpControlClient;
static WiFiClient ftpDataClient;

static bool ftpActive =
    false;

static bool ftpLoggedIn =
    false;

static uint8_t ftpCurrentUser =
    0;

static uint16_t ftpPassivePort =
    0;

static uint16_t ftpNextPassivePort =
    FTP_PASSIVE_PORT_FIRST;

static IPAddress ftpRemoteIP;

static bool ftpRemoteIPValid =
    false;

static char ftpControlLine[
    FTP_CONTROL_LINE_MAX + 1
];

static size_t ftpControlLineLength =
    0;

static char ftpRenameFrom[
    FTP_FILE_PATH_MAX
];

static bool ftpRenamePending =
    false;

static char ftpLastEvent[96] =
    "Waiting for client...";

static uint8_t ftpDataBuffer[
    FTP_DATA_BUFFER_SIZE
];


static void ftpStop();


static bool ftpIsActive()
{
    return ftpActive;
}


static char ftpUserFolder(
    uint8_t user
)
{
    if (user < 10)
    {
        return (char)(
            '0' + user
        );
    }

    return (char)(
        'A' + (user - 10)
    );
}


static void ftpFormatRemoteIP(
    char *buffer,
    size_t bufferSize
)
{
    if (
        !buffer ||
        bufferSize == 0
    )
    {
        return;
    }

    if (!ftpRemoteIPValid)
    {
        snprintf(
            buffer,
            bufferSize,
            "unknown"
        );

        return;
    }

    snprintf(
        buffer,
        bufferSize,
        "%u.%u.%u.%u",
        ftpRemoteIP[0],
        ftpRemoteIP[1],
        ftpRemoteIP[2],
        ftpRemoteIP[3]
    );
}


static void ftpShowStatus()
{
    M5Cardputer.Display.fillScreen(
        BLACK
    );

    M5Cardputer.Display.setTextColor(
        GREEN,
        BLACK
    );

    M5Cardputer.Display.setTextSize(1);

    M5Cardputer.Display.setCursor(
        0,
        0
    );

    M5Cardputer.Display.println(
        "FTPD"
    );

    M5Cardputer.Display.println(
        "----"
    );

    M5Cardputer.Display.println();

    IPAddress ip =
        WiFi.localIP();

    M5Cardputer.Display.printf(
        "Listen: %u.%u.%u.%u:%u\n",
        ip[0],
        ip[1],
        ip[2],
        ip[3],
        FTP_CONTROL_PORT
    );

    M5Cardputer.Display.printf(
        "Drive:  F:   User: %u\n",
        ftpCurrentUser
    );

    if (
        ftpControlClient &&
        ftpControlClient.connected()
    )
    {
        M5Cardputer.Display.println(
            "State:  connected"
        );

        if (ftpRemoteIPValid)
        {
            M5Cardputer.Display.printf(
                "Client: %u.%u.%u.%u\n",
                ftpRemoteIP[0],
                ftpRemoteIP[1],
                ftpRemoteIP[2],
                ftpRemoteIP[3]
            );
        }
    }
    else
    {
        M5Cardputer.Display.println(
            "State:  waiting"
        );
    }

    M5Cardputer.Display.println();

    M5Cardputer.Display.println(
        ftpLastEvent
    );

    M5Cardputer.Display.println();

    M5Cardputer.Display.println(
        "Fn+=  LOCAL"
    );
}


static void ftpConsoleConnectionEvent(
    bool connected
)
{
    char peer[32];
    char message[96];

    ftpFormatRemoteIP(
        peer,
        sizeof(peer)
    );

    snprintf(
        message,
        sizeof(message),
        "\r\n[FTP client %s from %s]\r\n",
        connected
            ? "connected"
            : "disconnected",
        peer
    );

    strncpy(
        ftpLastEvent,
        message + 2,
        sizeof(ftpLastEvent) - 1
    );

    ftpLastEvent[
        sizeof(ftpLastEvent) - 1
    ] = 0;

    size_t length =
        strlen(
            ftpLastEvent
        );

    while (
        length &&
        (
            ftpLastEvent[length - 1] == '\r' ||
            ftpLastEvent[length - 1] == '\n'
        )
    )
    {
        ftpLastEvent[--length] =
            0;
    }

    ftpShowStatus();
}


static void ftpReply(
    int code,
    const char *text
)
{
    if (
        !ftpControlClient ||
        !ftpControlClient.connected()
    )
    {
        return;
    }

    char line[192];

    snprintf(
        line,
        sizeof(line),
        "%d %s\r\n",
        code,
        text
            ? text
            : ""
    );

    ftpControlClient.print(
        line
    );
}


static void ftpClosePassive()
{
    if (
        ftpDataClient &&
        ftpDataClient.connected()
    )
    {
        ftpDataClient.stop();
    }

    ftpDataClient =
        WiFiClient();

    if (ftpPassiveServer)
    {
        ftpPassiveServer->stop();
        delete ftpPassiveServer;

        ftpPassiveServer =
            NULL;
    }

    ftpPassivePort =
        0;
}


static void ftpResetSessionState()
{
    ftpLoggedIn =
        false;

    ftpCurrentUser =
        0;

    ftpControlLineLength =
        0;

    ftpRenamePending =
        false;

    ftpRenameFrom[0] =
        0;

    ftpClosePassive();
}


static void ftpCloseControlSession(
    bool announce
)
{
    bool hadClient =
        ftpControlClient &&
        ftpControlClient.connected();

    if (hadClient)
    {
        ftpControlClient.stop();
    }

    ftpControlClient =
        WiFiClient();

    ftpClosePassive();

    if (
        announce &&
        ftpRemoteIPValid
    )
    {
        ftpConsoleConnectionEvent(
            false
        );
    }

    ftpRemoteIPValid =
        false;

    ftpResetSessionState();
}


static bool ftpPhysicalStopRequested()
{
    M5Cardputer.update();

    Keyboard_Class::KeysState status =
        M5Cardputer.Keyboard.keysState();

    return (
        status.fn &&
        status.f12
    );
}


static bool ftpTransferCanContinue()
{
    if (!ftpActive)
    {
        return false;
    }

    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        ftpStop();

        return false;
    }

    if (ftpPhysicalStopRequested())
    {
        ftpStop();

        return false;
    }

    return true;
}


static bool ftpBuildUserPath(
    uint8_t user,
    char *path,
    size_t pathSize
)
{
    if (
        user > 15 ||
        !path ||
        pathSize < 4
    )
    {
        return false;
    }

    int written =
        snprintf(
            path,
            pathSize,
            "F/%c",
            ftpUserFolder(user)
        );

    return (
        written > 0 &&
        (size_t)written < pathSize
    );
}


static bool ftpDriveFExists()
{
    File root =
        SD.open(
            "F",
            O_READ
        );

    if (!root)
    {
        return false;
    }

    bool isDirectory =
        root.isDirectory();

    root.close();

    return isDirectory;
}


static bool ftpEnsureUserArea(
    uint8_t user
)
{
    char path[
        FTP_FILE_PATH_MAX
    ];

    if (!ftpBuildUserPath(
        user,
        path,
        sizeof(path)
    ))
    {
        return false;
    }

    File existing =
        SD.open(
            path,
            O_READ
        );

    if (existing)
    {
        bool isDirectory =
            existing.isDirectory();

        existing.close();

        return isDirectory;
    }

    return SD.mkdir(
        path
    );
}


static bool ftpFilenameCharacterAllowed(
    char ch
)
{
    if (
        ch >= 'A' &&
        ch <= 'Z'
    )
    {
        return true;
    }

    if (
        ch >= '0' &&
        ch <= '9'
    )
    {
        return true;
    }

    const char *extras =
        "$#@!%&'()-^_{}~+";

    return strchr(
        extras,
        ch
    ) != NULL;
}


static bool ftpCanonicalFilename(
    const char *argument,
    char *name,
    size_t nameSize
)
{
    if (
        !argument ||
        !name ||
        nameSize < FTP_FILE_NAME_MAX
    )
    {
        return false;
    }

    while (
        *argument == ' ' ||
        *argument == '\t'
    )
    {
        argument++;
    }

    /*
     * Some clients use the absolute path returned by PWD. Accept
     * /<current-user>/FILE, but never permit selecting another user
     * by embedding it in a file operation.
     */
    char absolutePrefix[8];

    snprintf(
        absolutePrefix,
        sizeof(absolutePrefix),
        "/%u/",
        ftpCurrentUser
    );

    size_t prefixLength =
        strlen(
            absolutePrefix
        );

    if (
        strncmp(
            argument,
            absolutePrefix,
            prefixLength
        ) == 0
    )
    {
        argument +=
            prefixLength;
    }
    else if (
        argument[0] == '.' &&
        argument[1] == '/'
    )
    {
        argument += 2;
    }
    else if (
        strchr(argument, '/') ||
        strchr(argument, '\\') ||
        strchr(argument, ':')
    )
    {
        return false;
    }

    if (
        !*argument ||
        strchr(argument, '/') ||
        strchr(argument, '\\') ||
        strchr(argument, ':')
    )
    {
        return false;
    }

    char upper[
        FTP_FILE_NAME_MAX
    ];

    size_t length =
        strlen(
            argument
        );

    if (
        length == 0 ||
        length >= sizeof(upper)
    )
    {
        return false;
    }

    for (
        size_t index = 0;
        index < length;
        index++
    )
    {
        char ch =
            argument[index];

        if (
            ch >= 'a' &&
            ch <= 'z'
        )
        {
            ch =
                (char)(
                    ch - 'a' + 'A'
                );
        }

        upper[index] =
            ch;
    }

    upper[length] =
        0;

    char *dot =
        strchr(
            upper,
            '.'
        );

    size_t baseLength =
        dot
            ? (size_t)(dot - upper)
            : length;

    size_t extensionLength =
        dot
            ? strlen(dot + 1)
            : 0;

    if (
        baseLength < 1 ||
        baseLength > 8 ||
        extensionLength > 3 ||
        (
            dot &&
            (
                extensionLength == 0 ||
                strchr(
                    dot + 1,
                    '.'
                )
            )
        )
    )
    {
        return false;
    }

    for (
        size_t index = 0;
        index < length;
        index++
    )
    {
        if (
            upper[index] ==
            '.'
        )
        {
            continue;
        }

        if (!ftpFilenameCharacterAllowed(
            upper[index]
        ))
        {
            return false;
        }
    }

    strncpy(
        name,
        upper,
        nameSize - 1
    );

    name[nameSize - 1] =
        0;

    return true;
}


static bool ftpBuildFilePath(
    const char *argument,
    char *path,
    size_t pathSize,
    char *canonicalName = NULL,
    size_t canonicalNameSize = 0
)
{
    char name[
        FTP_FILE_NAME_MAX
    ];

    if (!ftpCanonicalFilename(
        argument,
        name,
        sizeof(name)
    ))
    {
        return false;
    }

    char userPath[
        FTP_FILE_PATH_MAX
    ];

    if (!ftpBuildUserPath(
        ftpCurrentUser,
        userPath,
        sizeof(userPath)
    ))
    {
        return false;
    }

    int written =
        snprintf(
            path,
            pathSize,
            "%s/%s",
            userPath,
            name
        );

    if (
        written <= 0 ||
        (size_t)written >= pathSize
    )
    {
        return false;
    }

    if (
        canonicalName &&
        canonicalNameSize
    )
    {
        strncpy(
            canonicalName,
            name,
            canonicalNameSize - 1
        );

        canonicalName[
            canonicalNameSize - 1
        ] = 0;
    }

    return true;
}


static bool ftpParseUserArea(
    const char *argument,
    uint8_t &user
)
{
    if (!argument)
    {
        return false;
    }

    while (
        *argument == ' ' ||
        *argument == '\t'
    )
    {
        argument++;
    }

    if (*argument == '/')
    {
        argument++;
    }

    if (!*argument)
    {
        return false;
    }

    uint32_t value = 0;

    while (*argument)
    {
        if (
            *argument < '0' ||
            *argument > '9'
        )
        {
            return false;
        }

        value =
            value * 10 +
            (uint32_t)(
                *argument - '0'
            );

        if (value > 15)
        {
            return false;
        }

        argument++;
    }

    user =
        (uint8_t)value;

    return true;
}


static void ftpBeginPassive()
{
    ftpClosePassive();

    if (
        ftpNextPassivePort <
            FTP_PASSIVE_PORT_FIRST ||
        ftpNextPassivePort >
            FTP_PASSIVE_PORT_LAST
    )
    {
        ftpNextPassivePort =
            FTP_PASSIVE_PORT_FIRST;
    }

    ftpPassivePort =
        ftpNextPassivePort++;

    ftpPassiveServer =
        new WiFiServer(
            ftpPassivePort
        );

    if (!ftpPassiveServer)
    {
        ftpPassivePort =
            0;

        ftpReply(
            425,
            "Unable to create passive data socket."
        );

        return;
    }

    ftpPassiveServer->begin();
}


static bool ftpWaitForDataClient()
{
    if (
        !ftpPassiveServer ||
        !ftpPassivePort
    )
    {
        ftpReply(
            425,
            "Use PASV or EPSV first."
        );

        return false;
    }

    uint32_t started =
        millis();

    while (
        ftpActive &&
        (
            uint32_t
        )(
            millis() -
            started
        ) <
            FTP_DATA_CONNECT_TIMEOUT_MS
    )
    {
        WiFiClient incoming =
            ftpPassiveServer->available();

        if (incoming)
        {
            ftpDataClient =
                incoming;

            ftpDataClient.setNoDelay(
                true
            );

            return true;
        }

        if (!ftpTransferCanContinue())
        {
            return false;
        }

        delay(5);
    }

    ftpReply(
        425,
        "Data connection timed out."
    );

    ftpClosePassive();

    return false;
}


static void ftpHandlePasv()
{
    ftpBeginPassive();

    if (!ftpPassiveServer)
    {
        return;
    }

    IPAddress ip =
        WiFi.localIP();

    char reply[96];

    snprintf(
        reply,
        sizeof(reply),
        "Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
        ip[0],
        ip[1],
        ip[2],
        ip[3],
        ftpPassivePort >> 8,
        ftpPassivePort & 0xFF
    );

    ftpReply(
        227,
        reply
    );
}


static void ftpHandleEpsv()
{
    ftpBeginPassive();

    if (!ftpPassiveServer)
    {
        return;
    }

    char reply[80];

    snprintf(
        reply,
        sizeof(reply),
        "Entering Extended Passive Mode (|||%u|).",
        ftpPassivePort
    );

    ftpReply(
        229,
        reply
    );
}


static bool ftpRequireLogin()
{
    if (ftpLoggedIn)
    {
        return true;
    }

    ftpReply(
        530,
        "Please login with USER and PASS."
    );

    return false;
}


static void ftpSendDirectoryListing(
    bool namesOnly,
    bool machineFormat
)
{
    if (!ftpRequireLogin())
    {
        return;
    }

    char userPath[
        FTP_FILE_PATH_MAX
    ];

    if (!ftpBuildUserPath(
        ftpCurrentUser,
        userPath,
        sizeof(userPath)
    ))
    {
        ftpReply(
            451,
            "Unable to access user area."
        );

        ftpClosePassive();

        return;
    }

    File directory =
        SD.open(
            userPath,
            O_READ
        );

    if (!directory)
    {
        ftpReply(
            550,
            "User area is unavailable."
        );

        ftpClosePassive();

        return;
    }

    if (!ftpPassiveServer)
    {
        directory.close();

        ftpReply(
            425,
            "Use PASV or EPSV first."
        );

        return;
    }

    ftpReply(
        150,
        "Opening data connection."
    );

    if (!ftpWaitForDataClient())
    {
        directory.close();

        return;
    }

    File entry;

    while (
        ftpActive &&
        (
            entry =
                directory.openNextFile()
        )
    )
    {
        if (entry.isDirectory())
        {
            entry.close();
            continue;
        }

        char hostName[40];

        entry.getName(
            hostName,
            sizeof(hostName)
        );

        char canonical[
            FTP_FILE_NAME_MAX
        ];

        /*
         * Hide host-side files that CP/M cannot address as 8.3 names.
         */
        if (!ftpCanonicalFilename(
            hostName,
            canonical,
            sizeof(canonical)
        ))
        {
            entry.close();
            continue;
        }

        uint32_t size =
            entry.size();

        entry.close();

        char line[160];

        if (namesOnly)
        {
            snprintf(
                line,
                sizeof(line),
                "%s\r\n",
                canonical
            );
        }
        else if (machineFormat)
        {
            snprintf(
                line,
                sizeof(line),
                "type=file;size=%lu; %s\r\n",
                (unsigned long)size,
                canonical
            );
        }
        else
        {
            snprintf(
                line,
                sizeof(line),
                "-rw-rw-rw- 1 ftp ftp %10lu Jan 01  1980 %s\r\n",
                (unsigned long)size,
                canonical
            );
        }

        ftpDataClient.print(
            line
        );

        if (!ftpTransferCanContinue())
        {
            directory.close();

            return;
        }
    }

    directory.close();

    if (
        ftpDataClient &&
        ftpDataClient.connected()
    )
    {
        ftpDataClient.flush();
    }

    ftpClosePassive();

    ftpReply(
        226,
        "Transfer complete."
    );
}


static void ftpHandleRetr(
    const char *argument
)
{
    if (!ftpRequireLogin())
    {
        return;
    }

    char path[
        FTP_FILE_PATH_MAX
    ];

    if (!ftpBuildFilePath(
        argument,
        path,
        sizeof(path)
    ))
    {
        ftpReply(
            550,
            "Invalid CP/M filename."
        );

        return;
    }

    File file =
        SD.open(
            path,
            O_READ
        );

    if (
        !file ||
        file.isDirectory()
    )
    {
        if (file)
        {
            file.close();
        }

        ftpReply(
            550,
            "File not found."
        );

        return;
    }

    if (!ftpPassiveServer)
    {
        file.close();

        ftpReply(
            425,
            "Use PASV or EPSV first."
        );

        return;
    }

    ftpReply(
        150,
        "Opening binary data connection."
    );

    if (!ftpWaitForDataClient())
    {
        file.close();

        return;
    }

    bool okay =
        true;

    while (
        ftpActive &&
        file.available()
    )
    {
        int count =
            file.read(
                ftpDataBuffer,
                sizeof(ftpDataBuffer)
            );

        if (count <= 0)
        {
            break;
        }

        size_t written =
            ftpDataClient.write(
                ftpDataBuffer,
                (size_t)count
            );

        if (
            written !=
            (size_t)count
        )
        {
            okay =
                false;

            break;
        }

        if (!ftpTransferCanContinue())
        {
            okay =
                false;

            break;
        }

        delay(0);
    }

    file.close();

    if (!ftpActive)
    {
        return;
    }

    ftpClosePassive();

    ftpReply(
        okay
            ? 226
            : 426,
        okay
            ? "Transfer complete."
            : "Transfer aborted."
    );
}


static void ftpHandleStore(
    const char *argument,
    bool append
)
{
    if (!ftpRequireLogin())
    {
        return;
    }

    char path[
        FTP_FILE_PATH_MAX
    ];

    if (!ftpBuildFilePath(
        argument,
        path,
        sizeof(path)
    ))
    {
        ftpReply(
            550,
            "Invalid CP/M filename."
        );

        return;
    }

    if (!ftpEnsureUserArea(
        ftpCurrentUser
    ))
    {
        ftpReply(
            550,
            "Unable to access user area."
        );

        return;
    }

    if (!ftpPassiveServer)
    {
        ftpReply(
            425,
            "Use PASV or EPSV first."
        );

        return;
    }

    ftpReply(
        150,
        "Opening binary data connection."
    );

    if (!ftpWaitForDataClient())
    {
        return;
    }

    File file =
        SD.open(
            path,
            append
                ? (O_CREAT | O_WRITE | O_APPEND)
                : (O_CREAT | O_WRITE | O_TRUNC)
        );

    if (!file)
    {
        ftpReply(
            550,
            "Unable to open destination file."
        );

        ftpClosePassive();

        return;
    }

    bool okay =
        true;

    while (
        ftpActive &&
        ftpDataClient &&
        ftpDataClient.connected()
    )
    {
        int available =
            ftpDataClient.available();

        if (available <= 0)
        {
            if (!ftpTransferCanContinue())
            {
                okay =
                    false;

                break;
            }

            delay(2);
            continue;
        }

        int wanted =
            available;

        if (
            wanted >
            (int)sizeof(ftpDataBuffer)
        )
        {
            wanted =
                sizeof(ftpDataBuffer);
        }

        int count =
            ftpDataClient.read(
                ftpDataBuffer,
                wanted
            );

        if (count < 0)
        {
            okay =
                false;

            break;
        }

        if (count == 0)
        {
            continue;
        }

        size_t written =
            file.write(
                ftpDataBuffer,
                (size_t)count
            );

        if (
            written !=
            (size_t)count
        )
        {
            okay =
                false;

            break;
        }

        delay(0);
    }

    file.flush();
    file.close();

    if (!ftpActive)
    {
        return;
    }

    ftpClosePassive();

    ftpReply(
        okay
            ? 226
            : 426,
        okay
            ? "Transfer complete."
            : "Transfer aborted."
    );
}


static void ftpHandleDelete(
    const char *argument
)
{
    if (!ftpRequireLogin())
    {
        return;
    }

    char path[
        FTP_FILE_PATH_MAX
    ];

    if (!ftpBuildFilePath(
        argument,
        path,
        sizeof(path)
    ))
    {
        ftpReply(
            550,
            "Invalid CP/M filename."
        );

        return;
    }

    if (SD.remove(path))
    {
        ftpReply(
            250,
            "File deleted."
        );
    }
    else
    {
        ftpReply(
            550,
            "Delete failed."
        );
    }
}


static void ftpHandleSize(
    const char *argument
)
{
    if (!ftpRequireLogin())
    {
        return;
    }

    char path[
        FTP_FILE_PATH_MAX
    ];

    if (!ftpBuildFilePath(
        argument,
        path,
        sizeof(path)
    ))
    {
        ftpReply(
            550,
            "Invalid CP/M filename."
        );

        return;
    }

    File file =
        SD.open(
            path,
            O_READ
        );

    if (
        !file ||
        file.isDirectory()
    )
    {
        if (file)
        {
            file.close();
        }

        ftpReply(
            550,
            "File not found."
        );

        return;
    }

    char sizeText[40];

    snprintf(
        sizeText,
        sizeof(sizeText),
        "%lu",
        (unsigned long)file.size()
    );

    file.close();

    ftpReply(
        213,
        sizeText
    );
}


static void ftpHandleRenameFrom(
    const char *argument
)
{
    if (!ftpRequireLogin())
    {
        return;
    }

    if (!ftpBuildFilePath(
        argument,
        ftpRenameFrom,
        sizeof(ftpRenameFrom)
    ))
    {
        ftpReply(
            550,
            "Invalid CP/M filename."
        );

        return;
    }

    File file =
        SD.open(
            ftpRenameFrom,
            O_READ
        );

    if (
        !file ||
        file.isDirectory()
    )
    {
        if (file)
        {
            file.close();
        }

        ftpRenamePending =
            false;

        ftpReply(
            550,
            "File not found."
        );

        return;
    }

    file.close();

    ftpRenamePending =
        true;

    ftpReply(
        350,
        "RNFR accepted; send RNTO."
    );
}


static void ftpHandleRenameTo(
    const char *argument
)
{
    if (!ftpRequireLogin())
    {
        return;
    }

    if (!ftpRenamePending)
    {
        ftpReply(
            503,
            "RNFR required first."
        );

        return;
    }

    char newPath[
        FTP_FILE_PATH_MAX
    ];

    if (!ftpBuildFilePath(
        argument,
        newPath,
        sizeof(newPath)
    ))
    {
        ftpReply(
            550,
            "Invalid CP/M filename."
        );

        ftpRenamePending =
            false;

        return;
    }

    File file =
        SD.open(
            ftpRenameFrom,
            O_WRITE | O_APPEND
        );

    bool success =
        false;

    if (file)
    {
        success =
            file.rename(
                newPath
            );

        file.close();
    }

    ftpRenamePending =
        false;

    ftpReply(
        success
            ? 250
            : 550,
        success
            ? "Rename successful."
            : "Rename failed."
    );
}


static void ftpHandleCwd(
    const char *argument
)
{
    if (!ftpRequireLogin())
    {
        return;
    }

    uint8_t user;

    if (!ftpParseUserArea(
        argument,
        user
    ))
    {
        ftpReply(
            550,
            "CWD is restricted to CP/M user areas 0-15."
        );

        return;
    }

    if (!ftpEnsureUserArea(
        user
    ))
    {
        ftpReply(
            550,
            "Unable to select user area."
        );

        return;
    }

    ftpCurrentUser =
        user;

    ftpRenamePending =
        false;

    ftpShowStatus();

    ftpReply(
        250,
        "CP/M user area changed."
    );
}


static void ftpProcessCommand(
    char *line
)
{
    if (
        !line ||
        !*line
    )
    {
        return;
    }

    char *argument =
        strchr(
            line,
            ' '
        );

    if (argument)
    {
        *argument++ =
            0;

        while (
            *argument == ' ' ||
            *argument == '\t'
        )
        {
            argument++;
        }
    }
    else
    {
        argument =
            (char *)"";
    }

    for (
        char *cursor = line;
        *cursor;
        cursor++
    )
    {
        if (
            *cursor >= 'a' &&
            *cursor <= 'z'
        )
        {
            *cursor =
                (char)(
                    *cursor - 'a' + 'A'
                );
        }
    }

    if (
        strcmp(
            line,
            "USER"
        ) == 0
    )
    {
        ftpLoggedIn =
            false;

        ftpReply(
            331,
            "Anonymous password accepted; send PASS."
        );

        return;
    }

    if (
        strcmp(
            line,
            "PASS"
        ) == 0
    )
    {
        ftpLoggedIn =
            true;

        ftpReply(
            230,
            "Anonymous login successful."
        );

        return;
    }

    if (
        strcmp(
            line,
            "QUIT"
        ) == 0
    )
    {
        ftpReply(
            221,
            "Goodbye."
        );

        if (
            ftpControlClient &&
            ftpControlClient.connected()
        )
        {
            ftpControlClient.flush();
            delay(5);
        }

        ftpCloseControlSession(
            true
        );

        return;
    }

    if (
        strcmp(
            line,
            "NOOP"
        ) == 0
    )
    {
        ftpReply(
            200,
            "OK."
        );

        return;
    }

    if (
        strcmp(
            line,
            "SYST"
        ) == 0
    )
    {
        ftpReply(
            215,
            "UNIX Type: L8"
        );

        return;
    }

    if (
        strcmp(
            line,
            "FEAT"
        ) == 0
    )
    {
        ftpControlClient.print(
            "211-Features:\r\n"
            " EPSV\r\n"
            " PASV\r\n"
            " SIZE\r\n"
            " MLST type*;size*;\r\n"
            " UTF8\r\n"
            "211 End\r\n"
        );

        return;
    }

    if (
        strcmp(
            line,
            "OPTS"
        ) == 0
    )
    {
        ftpReply(
            200,
            "Option accepted."
        );

        return;
    }

    if (
        strcmp(
            line,
            "TYPE"
        ) == 0
    )
    {
        ftpReply(
            200,
            "Type set."
        );

        return;
    }

    if (
        strcmp(
            line,
            "MODE"
        ) == 0
    )
    {
        ftpReply(
            200,
            "Stream mode."
        );

        return;
    }

    if (
        strcmp(
            line,
            "STRU"
        ) == 0
    )
    {
        ftpReply(
            200,
            "File structure."
        );

        return;
    }

    if (!ftpRequireLogin())
    {
        return;
    }

    if (
        strcmp(
            line,
            "PWD"
        ) == 0 ||
        strcmp(
            line,
            "XPWD"
        ) == 0
    )
    {
        char reply[80];

        snprintf(
            reply,
            sizeof(reply),
            "\"/%u\" is the current CP/M user area.",
            ftpCurrentUser
        );

        ftpReply(
            257,
            reply
        );

        return;
    }

    if (
        strcmp(
            line,
            "CWD"
        ) == 0 ||
        strcmp(
            line,
            "XCWD"
        ) == 0
    )
    {
        ftpHandleCwd(
            argument
        );

        return;
    }

    if (
        strcmp(
            line,
            "CDUP"
        ) == 0 ||
        strcmp(
            line,
            "XCUP"
        ) == 0
    )
    {
        ftpReply(
            550,
            "No directory hierarchy; use CWD 0 through CWD 15."
        );

        return;
    }

    if (
        strcmp(
            line,
            "MKD"
        ) == 0 ||
        strcmp(
            line,
            "XMKD"
        ) == 0 ||
        strcmp(
            line,
            "RMD"
        ) == 0 ||
        strcmp(
            line,
            "XRMD"
        ) == 0
    )
    {
        ftpReply(
            550,
            "Directories are not supported."
        );

        return;
    }

    if (
        strcmp(
            line,
            "PASV"
        ) == 0
    )
    {
        ftpHandlePasv();

        return;
    }

    if (
        strcmp(
            line,
            "EPSV"
        ) == 0
    )
    {
        ftpHandleEpsv();

        return;
    }

    if (
        strcmp(
            line,
            "PORT"
        ) == 0 ||
        strcmp(
            line,
            "EPRT"
        ) == 0
    )
    {
        ftpReply(
            502,
            "Active data connections are not supported; use PASV or EPSV."
        );

        return;
    }

    if (
        strcmp(
            line,
            "LIST"
        ) == 0
    )
    {
        ftpSendDirectoryListing(
            false,
            false
        );

        return;
    }

    if (
        strcmp(
            line,
            "NLST"
        ) == 0
    )
    {
        ftpSendDirectoryListing(
            true,
            false
        );

        return;
    }

    if (
        strcmp(
            line,
            "MLSD"
        ) == 0
    )
    {
        ftpSendDirectoryListing(
            false,
            true
        );

        return;
    }

    if (
        strcmp(
            line,
            "RETR"
        ) == 0
    )
    {
        ftpHandleRetr(
            argument
        );

        return;
    }

    if (
        strcmp(
            line,
            "STOR"
        ) == 0
    )
    {
        ftpHandleStore(
            argument,
            false
        );

        return;
    }

    if (
        strcmp(
            line,
            "APPE"
        ) == 0
    )
    {
        ftpHandleStore(
            argument,
            true
        );

        return;
    }

    if (
        strcmp(
            line,
            "DELE"
        ) == 0
    )
    {
        ftpHandleDelete(
            argument
        );

        return;
    }

    if (
        strcmp(
            line,
            "RNFR"
        ) == 0
    )
    {
        ftpHandleRenameFrom(
            argument
        );

        return;
    }

    if (
        strcmp(
            line,
            "RNTO"
        ) == 0
    )
    {
        ftpHandleRenameTo(
            argument
        );

        return;
    }

    if (
        strcmp(
            line,
            "SIZE"
        ) == 0
    )
    {
        ftpHandleSize(
            argument
        );

        return;
    }

    if (
        strcmp(
            line,
            "ALLO"
        ) == 0
    )
    {
        ftpReply(
            202,
            "ALLO not required."
        );

        return;
    }

    if (
        strcmp(
            line,
            "CLNT"
        ) == 0
    )
    {
        ftpReply(
            200,
            "Client noted."
        );

        return;
    }

    if (
        strcmp(
            line,
            "ABOR"
        ) == 0
    )
    {
        ftpClosePassive();

        ftpReply(
            226,
            "Data connection closed."
        );

        return;
    }

    if (
        strcmp(
            line,
            "REST"
        ) == 0 ||
        strcmp(
            line,
            "STOU"
        ) == 0
    )
    {
        ftpReply(
            502,
            "Resume/unique-store is not supported."
        );

        return;
    }

    if (
        strcmp(
            line,
            "MDTM"
        ) == 0
    )
    {
        ftpReply(
            550,
            "File timestamps are unavailable."
        );

        return;
    }

    if (
        strcmp(
            line,
            "STAT"
        ) == 0
    )
    {
        char status[96];

        snprintf(
            status,
            sizeof(status),
            "FTPD active; F: user %u.",
            ftpCurrentUser
        );

        ftpReply(
            211,
            status
        );

        return;
    }

    if (
        strcmp(
            line,
            "HELP"
        ) == 0
    )
    {
        ftpReply(
            214,
            "Anonymous FTP for CP/M F:; use CWD 0-15 to select user area."
        );

        return;
    }

    if (
        strcmp(
            line,
            "AUTH"
        ) == 0 ||
        strcmp(
            line,
            "PBSZ"
        ) == 0 ||
        strcmp(
            line,
            "PROT"
        ) == 0
    )
    {
        ftpReply(
            502,
            "TLS is not supported."
        );

        return;
    }

    ftpReply(
        502,
        "Command not implemented."
    );
}


static bool ftpStart()
{
    if (ftpActive)
    {
        _puts(
            "\r\nFTPD: already active\r\n"
        );

        return false;
    }

    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "\r\nFTPD: WiFi is offline\r\n"
        );

        return false;
    }

    if (!ftpDriveFExists())
    {
        _puts(
            "\r\nFTPD: F: drive directory is not available\r\n"
        );

        return false;
    }

    if (!ftpEnsureUserArea(0))
    {
        _puts(
            "\r\nFTPD: unable to access F: user 0\r\n"
        );

        return false;
    }

    ftpControlServer =
        new WiFiServer(
            FTP_CONTROL_PORT
        );

    if (!ftpControlServer)
    {
        _puts(
            "\r\nFTPD: unable to create server\r\n"
        );

        return false;
    }

    ftpControlServer->begin();

    ftpActive =
        true;

    ftpResetSessionState();

    strncpy(
        ftpLastEvent,
        "Waiting for client...",
        sizeof(ftpLastEvent) - 1
    );

    ftpLastEvent[
        sizeof(ftpLastEvent) - 1
    ] = 0;

    IPAddress ip =
        WiFi.localIP();

    char message[96];

    snprintf(
        message,
        sizeof(message),
        "\r\nFTPD: listening on %u.%u.%u.%u:%u (F: only)\r\n"
        "FTPD: Fn+= stops the server\r\n",
        ip[0],
        ip[1],
        ip[2],
        ip[3],
        FTP_CONTROL_PORT
    );

    _puts(
        message
    );

    ftpShowStatus();

    return true;
}


static void ftpStop()
{
    if (!ftpActive)
    {
        return;
    }

    bool hadClient =
        ftpControlClient &&
        ftpControlClient.connected();

    if (hadClient)
    {
        ftpReply(
            421,
            "FTPD stopped from the Cardputer console."
        );

        ftpControlClient.flush();

        delay(5);
    }

    ftpCloseControlSession(
        hadClient
    );

    if (ftpControlServer)
    {
        ftpControlServer->stop();
        delete ftpControlServer;

        ftpControlServer =
            NULL;
    }

    ftpActive =
        false;

    _puts(
        "\r\nFTPD: stopped\r\n"
    );
}


static void ftpAcceptControlClient()
{
    if (
        !ftpActive ||
        !ftpControlServer
    )
    {
        return;
    }

    WiFiClient incoming =
        ftpControlServer->available();

    if (!incoming)
    {
        return;
    }

    if (
        ftpControlClient &&
        ftpControlClient.connected()
    )
    {
        incoming.print(
            "421 FTPD already has an active client.\r\n"
        );

        incoming.flush();
        delay(2);
        incoming.stop();

        return;
    }

    ftpCloseControlSession(
        false
    );

    ftpControlClient =
        incoming;

    ftpControlClient.setNoDelay(
        true
    );

    ftpRemoteIP =
        ftpControlClient.remoteIP();

    ftpRemoteIPValid =
        true;

    ftpCurrentUser =
        0;

    ftpLoggedIn =
        false;

    ftpControlLineLength =
        0;

    ftpRenamePending =
        false;

    ftpReply(
        220,
        "Cardputer CP/M anonymous FTPD ready."
    );

    ftpConsoleConnectionEvent(
        true
    );
}


static void ftpService()
{
    if (!ftpActive)
    {
        return;
    }

    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "\r\nFTPD: WiFi connection lost\r\n"
        );

        ftpStop();

        return;
    }

    ftpAcceptControlClient();

    if (
        ftpControlClient &&
        !ftpControlClient.connected()
    )
    {
        ftpCloseControlSession(
            true
        );

        return;
    }

    if (
        !ftpControlClient ||
        !ftpControlClient.connected()
    )
    {
        return;
    }

    /*
     * Reject additional control clients while one session is active.
     */
    WiFiClient extra =
        ftpControlServer->available();

    if (extra)
    {
        extra.print(
            "421 FTPD already has an active client.\r\n"
        );

        extra.flush();
        delay(2);
        extra.stop();
    }

    while (
        ftpActive &&
        ftpControlClient &&
        ftpControlClient.connected() &&
        ftpControlClient.available()
    )
    {
        int value =
            ftpControlClient.read();

        if (value < 0)
        {
            break;
        }

        char ch =
            (char)value;

        if (ch == '\r')
        {
            continue;
        }

        if (ch == '\n')
        {
            ftpControlLine[
                ftpControlLineLength
            ] = 0;

            if (ftpControlLineLength)
            {
                ftpProcessCommand(
                    ftpControlLine
                );
            }

            ftpControlLineLength =
                0;

            if (
                !ftpControlClient ||
                !ftpControlClient.connected()
            )
            {
                break;
            }

            continue;
        }

        if (
            ftpControlLineLength <
            FTP_CONTROL_LINE_MAX
        )
        {
            ftpControlLine[
                ftpControlLineLength++
            ] = ch;
        }
        else
        {
            ftpControlLineLength =
                0;

            ftpReply(
                500,
                "Command line too long."
            );
        }
    }
}

#endif
