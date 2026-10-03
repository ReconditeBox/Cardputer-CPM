#ifndef CARDPUTER_FTP_CLIENT_H
#define CARDPUTER_FTP_CLIENT_H

/*
 * Interactive outbound FTP client for Cardputer-CPM.
 *
 * FTP.COM host [port]
 *
 * The client deliberately uses passive-mode FTP only.  That fits the
 * Cardputer's normal NAT/WiFi environment and mirrors FTPD's passive-only
 * design.  Transfers are binary by default.
 */

#define FTPCLIENT_LINE_MAX 255
#define FTPCLIENT_TIMEOUT_MS 15000
#define FTPCLIENT_IO_BUFFER 1024

static bool cardputerNetworkReadLine(
    const char *prompt,
    char *buffer,
    size_t bufferSize,
    bool echo = true
)
{
    if (
        !buffer ||
        bufferSize < 2
    )
    {
        return false;
    }

    if (prompt)
    {
        _puts(prompt);
    }

    size_t length = 0;

    while (true)
    {
        uint8_t ch =
            _getcon();

        if (
            ch == 0x03
        )
        {
            _puts("^C\r\n");
            buffer[0] = 0;
            return false;
        }

        if (
            ch == '\r' ||
            ch == '\n'
        )
        {
            _puts("\r\n");
            buffer[length] = 0;
            return true;
        }

        if (
            ch == 0x08 ||
            ch == 0x7F
        )
        {
            if (length)
            {
                length--;

                if (echo)
                {
                    _puts("\b \b");
                }
            }

            continue;
        }

        if (
            ch < 0x20 ||
            ch > 0x7E
        )
        {
            continue;
        }

        if (
            length + 1 <
            bufferSize
        )
        {
            buffer[length++] =
                (char)ch;

            if (echo)
            {
                _putcon(ch);
            }
        }
    }
}


static bool ftpClientReadLine(
    WiFiClient &client,
    char *line,
    size_t lineSize
)
{
    if (
        !line ||
        lineSize < 2
    )
    {
        return false;
    }

    size_t length = 0;
    uint32_t lastData =
        millis();

    while (true)
    {
        while (client.available())
        {
            int value =
                client.read();

            if (value < 0)
            {
                break;
            }

            lastData =
                millis();

            char ch =
                (char)value;

            if (ch == '\r')
            {
                continue;
            }

            if (ch == '\n')
            {
                line[length] = 0;
                return true;
            }

            if (
                length + 1 <
                lineSize
            )
            {
                line[length++] =
                    ch;
            }
        }

        if (
            !client.connected() &&
            !client.available()
        )
        {
            if (length)
            {
                line[length] = 0;
                return true;
            }

            return false;
        }

        if (
            (uint32_t)(
                millis() -
                lastData
            ) >
                FTPCLIENT_TIMEOUT_MS
        )
        {
            return false;
        }

        delay(1);
    }
}


static int ftpClientReadReply(
    WiFiClient &client,
    char *lastLine = NULL,
    size_t lastLineSize = 0
)
{
    char line[
        FTPCLIENT_LINE_MAX + 1
    ];

    int code =
        -1;

    bool multiline =
        false;

    char expected[4] =
    {
        0,
        0,
        0,
        0
    };

    while (ftpClientReadLine(
        client,
        line,
        sizeof(line)
    ))
    {
        _puts(line);
        _puts("\r\n");

        if (
            lastLine &&
            lastLineSize
        )
        {
            strncpy(
                lastLine,
                line,
                lastLineSize - 1
            );

            lastLine[
                lastLineSize - 1
            ] = 0;
        }

        if (
            strlen(line) >= 3 &&
            isdigit(
                (unsigned char)line[0]
            ) &&
            isdigit(
                (unsigned char)line[1]
            ) &&
            isdigit(
                (unsigned char)line[2]
            )
        )
        {
            int thisCode =
                (
                    line[0] - '0'
                ) * 100 +
                (
                    line[1] - '0'
                ) * 10 +
                (
                    line[2] - '0'
                );

            if (code < 0)
            {
                code =
                    thisCode;

                expected[0] =
                    line[0];

                expected[1] =
                    line[1];

                expected[2] =
                    line[2];

                if (
                    line[3] ==
                    '-'
                )
                {
                    multiline =
                        true;

                    continue;
                }

                return code;
            }

            if (
                multiline &&
                line[0] ==
                    expected[0] &&
                line[1] ==
                    expected[1] &&
                line[2] ==
                    expected[2] &&
                line[3] ==
                    ' '
            )
            {
                return code;
            }
        }
    }

    return code;
}


static int ftpClientCommand(
    WiFiClient &client,
    const char *command,
    char *lastLine = NULL,
    size_t lastLineSize = 0
)
{
    if (
        !client.connected() ||
        !command
    )
    {
        return -1;
    }

    client.print(
        command
    );

    client.print(
        "\r\n"
    );

    return ftpClientReadReply(
        client,
        lastLine,
        lastLineSize
    );
}


static bool ftpClientOpenPassive(
    WiFiClient &control,
    WiFiClient &data
)
{
    char reply[
        FTPCLIENT_LINE_MAX + 1
    ];

    int code =
        ftpClientCommand(
            control,
            "PASV",
            reply,
            sizeof(reply)
        );

    if (code != 227)
    {
        _puts(
            "FTP: PASV failed\r\n"
        );

        return false;
    }

    const char *open =
        strchr(
            reply,
            '('
        );

    if (!open)
    {
        _puts(
            "FTP: invalid PASV response\r\n"
        );

        return false;
    }

    int h1;
    int h2;
    int h3;
    int h4;
    int p1;
    int p2;

    if (
        sscanf(
            open + 1,
            "%d,%d,%d,%d,%d,%d",
            &h1,
            &h2,
            &h3,
            &h4,
            &p1,
            &p2
        ) != 6
    )
    {
        _puts(
            "FTP: invalid PASV address\r\n"
        );

        return false;
    }

    if (
        h1 < 0 || h1 > 255 ||
        h2 < 0 || h2 > 255 ||
        h3 < 0 || h3 > 255 ||
        h4 < 0 || h4 > 255 ||
        p1 < 0 || p1 > 255 ||
        p2 < 0 || p2 > 255
    )
    {
        _puts(
            "FTP: invalid PASV address\r\n"
        );

        return false;
    }

    IPAddress address(
        h1,
        h2,
        h3,
        h4
    );

    uint16_t port =
        (uint16_t)(
            p1 * 256 +
            p2
        );

    data.setNoDelay(
        true
    );

    if (!data.connect(
        address,
        port
    ))
    {
        _puts(
            "FTP: passive data connection failed\r\n"
        );

        return false;
    }

    return true;
}


static bool ftpClientPrepareTransfer(
    WiFiClient &control,
    WiFiClient &data,
    const char *command
)
{
    if (!ftpClientOpenPassive(
        control,
        data
    ))
    {
        return false;
    }

    int code =
        ftpClientCommand(
            control,
            command
        );

    if (
        code != 125 &&
        code != 150
    )
    {
        data.stop();

        return false;
    }

    return true;
}


static bool ftpClientDrainListing(
    WiFiClient &data
)
{
    uint32_t lastData =
        millis();

    while (
        data.connected() ||
        data.available()
    )
    {
        while (data.available())
        {
            int value =
                data.read();

            if (value < 0)
            {
                break;
            }

            _putcon(
                (uint8_t)value
            );

            lastData =
                millis();
        }

        if (
            (uint32_t)(
                millis() -
                lastData
            ) >
                FTPCLIENT_TIMEOUT_MS
        )
        {
            data.stop();
            return false;
        }

        delay(1);
    }

    data.stop();
    return true;
}


static const char *ftpClientRemoteBaseName(
    const char *path
)
{
    if (!path)
    {
        return path;
    }

    const char *base =
        path;

    while (*path)
    {
        if (
            *path == '/' ||
            *path == '\\'
        )
        {
            base =
                path + 1;
        }

        path++;
    }

    return base;
}


static bool ftpClientGet(
    WiFiClient &control,
    const char *remoteName,
    const char *localArgument
)
{
    if (
        !remoteName ||
        !remoteName[0]
    )
    {
        _puts(
            "Usage: get remote [local]\r\n"
        );

        return false;
    }

    uint8_t drive =
        cDrive;

    char localName[
        WGET_NAME_SIZE
    ];

    if (
        localArgument &&
        localArgument[0]
    )
    {
        if (!wgetParseDestination(
            localArgument,
            drive,
            localName,
            sizeof(localName)
        ))
        {
            _puts(
                "FTP: invalid local CP/M 8.3 name\r\n"
            );

            return false;
        }
    }
    else
    {
        if (!wgetCanonicalFilename(
            ftpClientRemoteBaseName(
                remoteName
            ),
            localName,
            sizeof(localName)
        ))
        {
            _puts(
                "FTP: remote name is not CP/M 8.3; specify a local name\r\n"
            );

            return false;
        }
    }

    char destination[
        WGET_PATH_SIZE
    ];

    char temporary[
        WGET_PATH_SIZE
    ];

    char backup[
        WGET_PATH_SIZE
    ];

    if (!wgetBuildPaths(
        drive,
        localName,
        destination,
        sizeof(destination),
        temporary,
        sizeof(temporary),
        backup,
        sizeof(backup)
    ))
    {
        _puts(
            "FTP: local drive/user unavailable\r\n"
        );

        return false;
    }

    if (
        SD.exists(
            destination
        ) &&
        _sys_isreadonly(
            (uint8 *)destination
        )
    )
    {
        _puts(
            "FTP: local destination is read-only\r\n"
        );

        return false;
    }

    SD.remove(
        temporary
    );

    File output =
        SD.open(
            temporary,
            O_CREAT |
            O_WRITE |
            O_TRUNC
        );

    if (!output)
    {
        _puts(
            "FTP: cannot create local temporary file\r\n"
        );

        return false;
    }

    WiFiClient data;

    char command[
        FTPCLIENT_LINE_MAX + 1
    ];

    snprintf(
        command,
        sizeof(command),
        "RETR %s",
        remoteName
    );

    if (!ftpClientPrepareTransfer(
        control,
        data,
        command
    ))
    {
        output.close();
        SD.remove(
            temporary
        );

        return false;
    }

    uint8_t buffer[
        FTPCLIENT_IO_BUFFER
    ];

    uint32_t byteCount =
        0;

    uint32_t lastData =
        millis();

    bool okay =
        true;

    while (
        data.connected() ||
        data.available()
    )
    {
        int available =
            data.available();

        if (available > 0)
        {
            size_t wanted =
                (size_t)available;

            if (
                wanted >
                sizeof(buffer)
            )
            {
                wanted =
                    sizeof(buffer);
            }

            int got =
                data.readBytes(
                    buffer,
                    wanted
                );

            if (got <= 0)
            {
                okay =
                    false;

                break;
            }

            size_t written =
                output.write(
                    buffer,
                    (size_t)got
                );

            if (
                written !=
                (size_t)got
            )
            {
                _puts(
                    "FTP: local SD write failed\r\n"
                );

                okay =
                    false;

                break;
            }

            byteCount +=
                (uint32_t)got;

            lastData =
                millis();

            continue;
        }

        if (
            (uint32_t)(
                millis() -
                lastData
            ) >
                FTPCLIENT_TIMEOUT_MS
        )
        {
            _puts(
                "FTP: data receive timeout\r\n"
            );

            okay =
                false;

            break;
        }

        delay(1);
    }

    data.stop();

    output.flush();
    output.close();

    int finalCode =
        ftpClientReadReply(
            control
        );

    if (
        !okay ||
        (
            finalCode < 200 ||
            finalCode >= 300
        )
    )
    {
        SD.remove(
            temporary
        );

        _puts(
            "FTP: download failed\r\n"
        );

        return false;
    }

    if (!wgetCommitTemporaryFile(
        temporary,
        destination,
        backup
    ))
    {
        SD.remove(
            temporary
        );

        _puts(
            "FTP: unable to install local file\r\n"
        );

        return false;
    }

    char message[128];

    snprintf(
        message,
        sizeof(message),
        "FTP: %lu bytes saved as %c%u:%s\r\n",
        (unsigned long)byteCount,
        'A' + drive,
        userCode,
        localName
    );

    _puts(
        message
    );

    return true;
}


static bool ftpClientPut(
    WiFiClient &control,
    const char *localArgument,
    const char *remoteArgument
)
{
    if (
        !localArgument ||
        !localArgument[0]
    )
    {
        _puts(
            "Usage: put local [remote]\r\n"
        );

        return false;
    }

    uint8_t drive =
        cDrive;

    char localName[
        WGET_NAME_SIZE
    ];

    if (!wgetParseDestination(
        localArgument,
        drive,
        localName,
        sizeof(localName)
    ))
    {
        _puts(
            "FTP: invalid local CP/M 8.3 name\r\n"
        );

        return false;
    }

    char source[
        WGET_PATH_SIZE
    ];

    char unusedTemporary[
        WGET_PATH_SIZE
    ];

    char unusedBackup[
        WGET_PATH_SIZE
    ];

    if (!wgetBuildPaths(
        drive,
        localName,
        source,
        sizeof(source),
        unusedTemporary,
        sizeof(unusedTemporary),
        unusedBackup,
        sizeof(unusedBackup)
    ))
    {
        _puts(
            "FTP: local drive/user unavailable\r\n"
        );

        return false;
    }

    File input =
        SD.open(
            source,
            O_READ
        );

    if (
        !input ||
        input.isDirectory()
    )
    {
        if (input)
        {
            input.close();
        }

        _puts(
            "FTP: local file not found\r\n"
        );

        return false;
    }

    const char *remoteName =
        (
            remoteArgument &&
            remoteArgument[0]
        )
            ? remoteArgument
            : localName;

    WiFiClient data;

    char command[
        FTPCLIENT_LINE_MAX + 1
    ];

    snprintf(
        command,
        sizeof(command),
        "STOR %s",
        remoteName
    );

    if (!ftpClientPrepareTransfer(
        control,
        data,
        command
    ))
    {
        input.close();
        return false;
    }

    uint8_t buffer[
        FTPCLIENT_IO_BUFFER
    ];

    uint32_t byteCount =
        0;

    bool okay =
        true;

    while (input.available())
    {
        int got =
            input.read(
                buffer,
                sizeof(buffer)
            );

        if (got <= 0)
        {
            okay =
                false;

            break;
        }

        size_t sentTotal =
            0;

        while (
            sentTotal <
                (size_t)got
        )
        {
            size_t sent =
                data.write(
                    buffer +
                        sentTotal,
                    (size_t)got -
                        sentTotal
                );

            if (sent == 0)
            {
                _puts(
                    "FTP: data send failed\r\n"
                );

                okay =
                    false;

                break;
            }

            sentTotal +=
                sent;

            delay(0);
        }

        if (!okay)
        {
            break;
        }

        byteCount +=
            (uint32_t)got;

        delay(0);
    }

    input.close();

    data.flush();
    data.stop();

    int finalCode =
        ftpClientReadReply(
            control
        );

    if (
        !okay ||
        finalCode < 200 ||
        finalCode >= 300
    )
    {
        _puts(
            "FTP: upload failed\r\n"
        );

        return false;
    }

    char message[128];

    snprintf(
        message,
        sizeof(message),
        "FTP: %lu bytes uploaded as %s\r\n",
        (unsigned long)byteCount,
        remoteName
    );

    _puts(
        message
    );

    return true;
}


static void ftpClientHelp()
{
    _puts(
        "\r\n"
        "FTP commands\r\n"
        "------------\r\n"
        "ls [path]         directory listing\r\n"
        "pwd               remote directory\r\n"
        "cd path           change remote directory\r\n"
        "get remote [local]\r\n"
        "put local [remote]\r\n"
        "user name         log in as another user\r\n"
        "pass              enter password (hidden)\r\n"
        "binary            TYPE I\r\n"
        "ascii             TYPE A\r\n"
        "quote command     send raw FTP command\r\n"
        "help              this help\r\n"
        "quit              disconnect\r\n"
    );
}


uint16 cardputerFtpClientBdos(
    uint16 commandTail
)
{
    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "\r\nFTP: WiFi is offline\r\n"
        );

        return 0x00FF;
    }

    if (ftpIsActive())
    {
        _puts(
            "\r\nFTP: unavailable while FTPD is active\r\n"
        );

        return 0x00FF;
    }

    char tail[129];

    if (!networkReadCommandTail(
        commandTail,
        tail,
        sizeof(tail)
    ))
    {
        return 0x00FF;
    }

    char *text =
        wifiTrim(
            tail
        );

    char *arguments[3];

    int argumentCount =
        wifiTokenize(
            text,
            arguments,
            3
        );

    if (
        argumentCount < 1 ||
        argumentCount > 2
    )
    {
        _puts(
            "\r\nUsage: FTP host [port]\r\n"
        );

        return 0x00FF;
    }

    uint32_t port =
        21;

    if (
        argumentCount == 2 &&
        !networkParseNumber(
            arguments[1],
            1,
            65535,
            port
        )
    )
    {
        _puts(
            "\r\nFTP: invalid port\r\n"
        );

        return 0x00FF;
    }

    IPAddress address;

    if (!networkResolve(
        arguments[0],
        address
    ))
    {
        _puts(
            "\r\nFTP: host lookup failed\r\n"
        );

        return 0x00FF;
    }

    char addressText[32];

    networkFormatAddress(
        address,
        addressText,
        sizeof(addressText)
    );

    char message[192];

    snprintf(
        message,
        sizeof(message),
        "\r\nFTP: connecting to %s (%s):%lu...\r\n",
        arguments[0],
        addressText,
        (unsigned long)port
    );

    _puts(
        message
    );

    WiFiClient control;

    control.setNoDelay(
        true
    );

    if (!control.connect(
        address,
        (uint16_t)port
    ))
    {
        _puts(
            "FTP: connection failed\r\n"
        );

        return 0x00FF;
    }

    int greeting =
        ftpClientReadReply(
            control
        );

    if (
        greeting < 200 ||
        greeting >= 400
    )
    {
        control.stop();

        _puts(
            "FTP: server rejected connection\r\n"
        );

        return 0x00FF;
    }

    _puts(
        "FTP: attempting anonymous login\r\n"
    );

    int login =
        ftpClientCommand(
            control,
            "USER anonymous"
        );

    if (login == 331)
    {
        login =
            ftpClientCommand(
                control,
                "PASS cardputer@"
            );
    }

    if (
        login >= 200 &&
        login < 300
    )
    {
        ftpClientCommand(
            control,
            "TYPE I"
        );

        _puts(
            "FTP: anonymous login complete; binary mode\r\n"
        );
    }
    else
    {
        _puts(
            "FTP: anonymous login failed; use USER and PASS\r\n"
        );
    }

    ftpClientHelp();

    char line[
        FTPCLIENT_LINE_MAX + 1
    ];

    while (control.connected())
    {
        if (!cardputerNetworkReadLine(
            "ftp> ",
            line,
            sizeof(line)
        ))
        {
            break;
        }

        char *commandText =
            wifiTrim(
                line
            );

        if (!commandText[0])
        {
            continue;
        }

        char *parts[8];

        int partCount =
            wifiTokenize(
                commandText,
                parts,
                8
            );

        if (partCount <= 0)
        {
            continue;
        }

        if (partCount > 8)
        {
            _puts(
                "FTP: too many command arguments\r\n"
            );

            continue;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "quit"
            ) ||
            wifiEqualsIgnoreCase(
                parts[0],
                "bye"
            )
        )
        {
            ftpClientCommand(
                control,
                "QUIT"
            );

            break;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "help"
            ) ||
            strcmp(
                parts[0],
                "?"
            ) == 0
        )
        {
            ftpClientHelp();
            continue;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "pwd"
            )
        )
        {
            ftpClientCommand(
                control,
                "PWD"
            );

            continue;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "cd"
            )
        )
        {
            if (partCount != 2)
            {
                _puts(
                    "Usage: cd path\r\n"
                );

                continue;
            }

            char ftpCommand[
                FTPCLIENT_LINE_MAX + 1
            ];

            snprintf(
                ftpCommand,
                sizeof(ftpCommand),
                "CWD %s",
                parts[1]
            );

            ftpClientCommand(
                control,
                ftpCommand
            );

            continue;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "binary"
            )
        )
        {
            ftpClientCommand(
                control,
                "TYPE I"
            );

            continue;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "ascii"
            )
        )
        {
            ftpClientCommand(
                control,
                "TYPE A"
            );

            continue;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "user"
            )
        )
        {
            if (partCount != 2)
            {
                _puts(
                    "Usage: user name\r\n"
                );

                continue;
            }

            char ftpCommand[
                FTPCLIENT_LINE_MAX + 1
            ];

            snprintf(
                ftpCommand,
                sizeof(ftpCommand),
                "USER %s",
                parts[1]
            );

            int result =
                ftpClientCommand(
                    control,
                    ftpCommand
                );

            if (result == 331)
            {
                char password[96];

                if (cardputerNetworkReadLine(
                    "Password: ",
                    password,
                    sizeof(password),
                    false
                ))
                {
                    snprintf(
                        ftpCommand,
                        sizeof(ftpCommand),
                        "PASS %s",
                        password
                    );

                    ftpClientCommand(
                        control,
                        ftpCommand
                    );
                }
            }

            continue;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "pass"
            )
        )
        {
            char password[96];

            if (!cardputerNetworkReadLine(
                "Password: ",
                password,
                sizeof(password),
                false
            ))
            {
                continue;
            }

            char ftpCommand[
                FTPCLIENT_LINE_MAX + 1
            ];

            snprintf(
                ftpCommand,
                sizeof(ftpCommand),
                "PASS %s",
                password
            );

            ftpClientCommand(
                control,
                ftpCommand
            );

            continue;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "ls"
            ) ||
            wifiEqualsIgnoreCase(
                parts[0],
                "dir"
            )
        )
        {
            char ftpCommand[
                FTPCLIENT_LINE_MAX + 1
            ];

            if (
                partCount >= 2
            )
            {
                snprintf(
                    ftpCommand,
                    sizeof(ftpCommand),
                    "LIST %s",
                    parts[1]
                );
            }
            else
            {
                strcpy(
                    ftpCommand,
                    "LIST"
                );
            }

            WiFiClient data;

            if (ftpClientPrepareTransfer(
                control,
                data,
                ftpCommand
            ))
            {
                ftpClientDrainListing(
                    data
                );

                ftpClientReadReply(
                    control
                );
            }

            continue;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "get"
            )
        )
        {
            if (
                partCount < 2 ||
                partCount > 3
            )
            {
                _puts(
                    "Usage: get remote [local]\r\n"
                );

                continue;
            }

            ftpClientGet(
                control,
                parts[1],
                partCount == 3
                    ? parts[2]
                    : NULL
            );

            continue;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "put"
            )
        )
        {
            if (
                partCount < 2 ||
                partCount > 3
            )
            {
                _puts(
                    "Usage: put local [remote]\r\n"
                );

                continue;
            }

            ftpClientPut(
                control,
                parts[1],
                partCount == 3
                    ? parts[2]
                    : NULL
            );

            continue;
        }

        if (
            wifiEqualsIgnoreCase(
                parts[0],
                "quote"
            )
        )
        {
            if (partCount < 2)
            {
                _puts(
                    "Usage: quote command\r\n"
                );

                continue;
            }

            /*
             * Tokenisation inserted NULs, so reconstruct the common case
             * from the remaining parsed words.
             */
            char ftpCommand[
                FTPCLIENT_LINE_MAX + 1
            ];

            ftpCommand[0] = 0;

            for (
                int index = 1;
                index < partCount;
                index++
            )
            {
                if (index > 1)
                {
                    strncat(
                        ftpCommand,
                        " ",
                        sizeof(ftpCommand) -
                            strlen(ftpCommand) -
                            1
                    );
                }

                strncat(
                    ftpCommand,
                    parts[index],
                    sizeof(ftpCommand) -
                        strlen(ftpCommand) -
                        1
                );
            }

            ftpClientCommand(
                control,
                ftpCommand
            );

            continue;
        }

        _puts(
            "FTP: unknown command; type HELP\r\n"
        );
    }

    control.stop();

    _puts(
        "\r\nFTP: disconnected\r\n"
    );

    return 0;
}

#endif
