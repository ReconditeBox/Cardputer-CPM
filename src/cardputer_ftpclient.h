#ifndef CARDPUTER_FTPCLIENT_H
#define CARDPUTER_FTPCLIENT_H

#define FTPCLIENT_LINE_SIZE 256
#define FTPCLIENT_IO_SIZE   1024
#define FTPCLIENT_TIMEOUT   15000

static bool ftpClientReadControlLine(
    WiFiClient &client,
    char *buffer,
    size_t bufferSize
)
{
    if (!buffer || bufferSize < 2)
        return false;

    size_t length = 0;
    uint32_t lastData = millis();

    while (true)
    {
        while (client.available())
        {
            int value = client.read();

            if (value < 0)
                break;

            char ch = (char)value;
            lastData = millis();

            if (ch == '\r')
                continue;

            if (ch == '\n')
            {
                buffer[length] = 0;
                return true;
            }

            if (length + 1 < bufferSize)
                buffer[length++] = ch;
        }

        if (!client.connected() && !client.available())
        {
            if (length)
            {
                buffer[length] = 0;
                return true;
            }

            return false;
        }

        if ((uint32_t)(millis() - lastData) > FTPCLIENT_TIMEOUT)
            return false;

        delay(1);
    }
}


static bool ftpClientReadResponse(
    WiFiClient &client,
    int &code,
    char *capture = NULL,
    size_t captureSize = 0
)
{
    char line[FTPCLIENT_LINE_SIZE];

    if (!ftpClientReadControlLine(
        client,
        line,
        sizeof(line)
    ))
    {
        _puts("FTP: control response timeout\r\n");
        return false;
    }

    _puts(line);
    _puts("\r\n");

    if (capture && captureSize)
    {
        strncpy(capture, line, captureSize - 1);
        capture[captureSize - 1] = 0;
    }

    if (
        strlen(line) < 3 ||
        !isdigit((unsigned char)line[0]) ||
        !isdigit((unsigned char)line[1]) ||
        !isdigit((unsigned char)line[2])
    )
    {
        return false;
    }

    code =
        (line[0] - '0') * 100 +
        (line[1] - '0') * 10 +
        (line[2] - '0');

    bool multiline =
        line[3] == '-';

    while (multiline)
    {
        if (!ftpClientReadControlLine(
            client,
            line,
            sizeof(line)
        ))
        {
            return false;
        }

        _puts(line);
        _puts("\r\n");

        if (
            strlen(line) >= 4 &&
            isdigit((unsigned char)line[0]) &&
            isdigit((unsigned char)line[1]) &&
            isdigit((unsigned char)line[2]) &&
            line[3] == ' ' &&
            (
                (line[0] - '0') * 100 +
                (line[1] - '0') * 10 +
                (line[2] - '0')
            ) == code
        )
        {
            multiline = false;
        }
    }

    return true;
}


static bool ftpClientCommand(
    WiFiClient &client,
    const char *command,
    int &code,
    char *capture = NULL,
    size_t captureSize = 0
)
{
    if (!client.connected())
        return false;

    client.print(command);
    client.print("\r\n");

    return ftpClientReadResponse(
        client,
        code,
        capture,
        captureSize
    );
}


static bool ftpClientPassive(
    WiFiClient &control,
    WiFiClient &data
)
{
    int code = 0;
    char response[FTPCLIENT_LINE_SIZE];

    if (
        !ftpClientCommand(
            control,
            "PASV",
            code,
            response,
            sizeof(response)
        ) ||
        code != 227
    )
    {
        return false;
    }

    const char *open =
        strchr(
            response,
            '('
        );

    if (!open)
        return false;

    unsigned h1, h2, h3, h4, p1, p2;

    if (
        sscanf(
            open + 1,
            "%u,%u,%u,%u,%u,%u",
            &h1,
            &h2,
            &h3,
            &h4,
            &p1,
            &p2
        ) != 6 ||
        p1 > 255 ||
        p2 > 255
    )
    {
        return false;
    }

    uint16_t port =
        (uint16_t)(
            p1 * 256 +
            p2
        );

    /*
     * Use the control peer rather than PASV's advertised host. This avoids
     * the common NAT/private-address PASV problem while retaining the
     * server-selected passive port.
     */
    IPAddress peer =
        control.remoteIP();

    data.setNoDelay(true);

    if (!data.connect(
        peer,
        port
    ))
    {
        _puts("FTP: passive data connection failed\r\n");
        return false;
    }

    return true;
}


static bool ftpClientSetBinary(
    WiFiClient &control
)
{
    int code = 0;

    return (
        ftpClientCommand(
            control,
            "TYPE I",
            code
        ) &&
        code >= 200 &&
        code < 300
    );
}


static void ftpClientDrainListing(
    WiFiClient &data
)
{
    bool lastCR = false;
    uint32_t lastData = millis();

    while (
        data.connected() ||
        data.available()
    )
    {
        while (data.available())
        {
            int value = data.read();

            if (value < 0)
                break;

            uint8_t ch = (uint8_t)value;
            lastData = millis();

            if (ch == '\n' && !lastCR)
                _putcon('\r');

            _putcon(ch);
            lastCR = ch == '\r';
        }

        if (
            !data.connected() &&
            !data.available()
        )
        {
            break;
        }

        if (
            (uint32_t)(
                millis() -
                lastData
            ) >
                FTPCLIENT_TIMEOUT
        )
        {
            _puts("\r\nFTP: data timeout\r\n");
            break;
        }

        delay(1);
    }
}


static bool ftpClientLocalPath(
    const char *argument,
    uint8_t &drive,
    char *filename,
    size_t filenameSize,
    char *path,
    size_t pathSize
)
{
    if (!wgetParseDestination(
        argument,
        drive,
        filename,
        filenameSize
    ))
    {
        return false;
    }

    char temporary[WGET_PATH_SIZE];
    char backup[WGET_PATH_SIZE];

    return wgetBuildPaths(
        drive,
        filename,
        path,
        pathSize,
        temporary,
        sizeof(temporary),
        backup,
        sizeof(backup)
    );
}


static bool ftpClientGet(
    WiFiClient &control,
    const char *remoteName,
    const char *localArgument
)
{
    if (!remoteName || !remoteName[0])
        return false;

    const char *base =
        strrchr(
            remoteName,
            '/'
        );

    base =
        base
            ? base + 1
            : remoteName;

    uint8_t drive = cDrive;
    char filename[WGET_NAME_SIZE];

    if (localArgument && localArgument[0])
    {
        if (!wgetParseDestination(
            localArgument,
            drive,
            filename,
            sizeof(filename)
        ))
        {
            _puts("FTP: invalid local CP/M filename\r\n");
            return false;
        }
    }
    else
    {
        if (!wgetCanonicalFilename(
            base,
            filename,
            sizeof(filename)
        ))
        {
            _puts("FTP: specify a local CP/M 8.3 filename\r\n");
            return false;
        }
    }

    char destination[WGET_PATH_SIZE];
    char temporary[WGET_PATH_SIZE];
    char backup[WGET_PATH_SIZE];

    if (!wgetBuildPaths(
        drive,
        filename,
        destination,
        sizeof(destination),
        temporary,
        sizeof(temporary),
        backup,
        sizeof(backup)
    ))
    {
        _puts("FTP: local drive/user unavailable\r\n");
        return false;
    }

    if (
        SD.exists(destination) &&
        _sys_isreadonly((uint8 *)destination)
    )
    {
        _puts("FTP: local destination is read-only\r\n");
        return false;
    }

    if (!ftpClientSetBinary(control))
        return false;

    WiFiClient data;

    if (!ftpClientPassive(
        control,
        data
    ))
    {
        return false;
    }

    char command[FTPCLIENT_LINE_SIZE];

    snprintf(
        command,
        sizeof(command),
        "RETR %s",
        remoteName
    );

    int code = 0;

    control.print(command);
    control.print("\r\n");

    if (
        !ftpClientReadResponse(
            control,
            code
        ) ||
        (
            code != 125 &&
            code != 150
        )
    )
    {
        data.stop();
        return false;
    }

    SD.remove(temporary);

    File output =
        SD.open(
            temporary,
            O_CREAT |
            O_WRITE |
            O_TRUNC
        );

    if (!output)
    {
        data.stop();
        _puts("FTP: cannot create local temporary file\r\n");
        return false;
    }

    uint8_t buffer[FTPCLIENT_IO_SIZE];
    uint32_t total = 0;
    uint32_t lastData = millis();
    bool okay = true;

    while (
        data.connected() ||
        data.available()
    )
    {
        int available = data.available();

        if (available > 0)
        {
            size_t wanted =
                (size_t)available;

            if (wanted > sizeof(buffer))
                wanted = sizeof(buffer);

            int got =
                data.readBytes(
                    buffer,
                    wanted
                );

            if (got <= 0)
            {
                okay = false;
                break;
            }

            if (
                output.write(
                    buffer,
                    (size_t)got
                ) !=
                    (size_t)got
            )
            {
                okay = false;
                _puts("FTP: SD write failed\r\n");
                break;
            }

            total += (uint32_t)got;
            lastData = millis();
            continue;
        }

        if (
            !data.connected() &&
            !data.available()
        )
        {
            break;
        }

        if (
            (uint32_t)(
                millis() -
                lastData
            ) >
                FTPCLIENT_TIMEOUT
        )
        {
            okay = false;
            _puts("FTP: data timeout\r\n");
            break;
        }

        delay(1);
    }

    output.flush();
    output.close();
    data.stop();

    int finalCode = 0;

    if (
        !ftpClientReadResponse(
            control,
            finalCode
        ) ||
        finalCode < 200 ||
        finalCode >= 300
    )
    {
        okay = false;
    }

    if (
        !okay ||
        !wgetCommitTemporaryFile(
            temporary,
            destination,
            backup
        )
    )
    {
        SD.remove(temporary);
        _puts("FTP: download failed\r\n");
        return false;
    }

    char message[96];

    snprintf(
        message,
        sizeof(message),
        "FTP: %lu bytes saved as %c%u:%s\r\n",
        (unsigned long)total,
        'A' + drive,
        userCode,
        filename
    );

    _puts(message);

    return true;
}


static bool ftpClientPut(
    WiFiClient &control,
    const char *localArgument,
    const char *remoteName
)
{
    if (!localArgument || !localArgument[0])
        return false;

    uint8_t drive = cDrive;
    char filename[WGET_NAME_SIZE];
    char path[WGET_PATH_SIZE];

    if (!ftpClientLocalPath(
        localArgument,
        drive,
        filename,
        sizeof(filename),
        path,
        sizeof(path)
    ))
    {
        _puts("FTP: invalid local CP/M filename\r\n");
        return false;
    }

    File input =
        SD.open(
            path,
            O_READ
        );

    if (!input)
    {
        _puts("FTP: local file not found\r\n");
        return false;
    }

    if (!remoteName || !remoteName[0])
        remoteName = filename;

    if (!ftpClientSetBinary(control))
    {
        input.close();
        return false;
    }

    WiFiClient data;

    if (!ftpClientPassive(
        control,
        data
    ))
    {
        input.close();
        return false;
    }

    char command[FTPCLIENT_LINE_SIZE];

    snprintf(
        command,
        sizeof(command),
        "STOR %s",
        remoteName
    );

    control.print(command);
    control.print("\r\n");

    int code = 0;

    if (
        !ftpClientReadResponse(
            control,
            code
        ) ||
        (
            code != 125 &&
            code != 150
        )
    )
    {
        input.close();
        data.stop();
        return false;
    }

    uint8_t buffer[FTPCLIENT_IO_SIZE];
    uint32_t total = 0;
    bool okay = true;

    while (input.available())
    {
        int got =
            input.read(
                buffer,
                sizeof(buffer)
            );

        if (got <= 0)
        {
            okay = false;
            break;
        }

        size_t sent = 0;

        while (
            sent <
            (size_t)got
        )
        {
            size_t wrote =
                data.write(
                    buffer + sent,
                    (size_t)got - sent
                );

            if (!wrote)
            {
                okay = false;
                break;
            }

            sent += wrote;
        }

        if (!okay)
            break;

        total += (uint32_t)got;
    }

    input.close();

    data.flush();
    data.stop();

    int finalCode = 0;

    if (
        !ftpClientReadResponse(
            control,
            finalCode
        ) ||
        finalCode < 200 ||
        finalCode >= 300
    )
    {
        okay = false;
    }

    if (!okay)
    {
        _puts("FTP: upload failed\r\n");
        return false;
    }

    char message[80];

    snprintf(
        message,
        sizeof(message),
        "FTP: %lu bytes uploaded\r\n",
        (unsigned long)total
    );

    _puts(message);

    return true;
}


static void ftpClientHelp()
{
    _puts(
        "Commands:\r\n"
        "  DIR [path]            list remote directory\r\n"
        "  CD path               change remote directory\r\n"
        "  PWD                   show remote directory\r\n"
        "  GET remote [local]    download to CP/M\r\n"
        "  PUT local [remote]    upload from CP/M\r\n"
        "  USER name             login as another user\r\n"
        "  PASS [password]       send password (prompted if omitted)\r\n"
        "  DELETE remote         delete remote file\r\n"
        "  HELP                  show commands\r\n"
        "  QUIT                  disconnect\r\n"
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
        _puts("\r\nFTP: WiFi is offline\r\n");
        return 0x00FF;
    }

    if (ftpIsActive())
    {
        _puts("\r\nFTP: unavailable while FTPD is active\r\n");
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
        networkConsoleTrim(
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

    uint32_t port = 21;

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
        _puts("\r\nFTP: invalid port\r\n");
        return 0x00FF;
    }

    IPAddress address;

    if (!networkResolve(
        arguments[0],
        address
    ))
    {
        _puts("\r\nFTP: host lookup failed\r\n");
        return 0x00FF;
    }

    char addressText[32];

    networkFormatAddress(
        address,
        addressText,
        sizeof(addressText)
    );

    char message[160];

    snprintf(
        message,
        sizeof(message),
        "\r\nFTP: connecting to %s (%s):%lu...\r\n",
        arguments[0],
        addressText,
        (unsigned long)port
    );

    _puts(message);

    WiFiClient control;
    control.setNoDelay(true);

    if (!control.connect(
        address,
        (uint16_t)port
    ))
    {
        _puts("FTP: connection failed\r\n");
        return 0x00FF;
    }

    int code = 0;

    if (
        !ftpClientReadResponse(
            control,
            code
        ) ||
        code >= 400
    )
    {
        control.stop();
        return 0x00FF;
    }

    /*
     * Start with anonymous login because that is the most useful CP/M
     * zero-configuration behaviour. USER can switch identity afterwards.
     */
    if (
        ftpClientCommand(
            control,
            "USER anonymous",
            code
        ) &&
        code == 331
    )
    {
        ftpClientCommand(
            control,
            "PASS cardputer@local",
            code
        );
    }

    ftpClientSetBinary(control);

    _puts(
        "FTP: passive binary client ready. HELP for commands.\r\n"
    );

    char line[FTPCLIENT_LINE_SIZE];

    while (control.connected())
    {
        _puts("ftp> ");

        if (!networkConsoleReadLine(
            line,
            sizeof(line),
            true
        ))
        {
            break;
        }

        char *commandLine =
            networkConsoleTrim(
                line
            );

        if (!commandLine[0])
            continue;

        char *args[4];

        int count =
            wifiTokenize(
                commandLine,
                args,
                4
            );

        if (count < 1)
            continue;

        networkConsoleUpperWord(
            args[0]
        );

        if (
            strcmp(args[0], "QUIT") == 0 ||
            strcmp(args[0], "BYE") == 0
        )
        {
            ftpClientCommand(
                control,
                "QUIT",
                code
            );

            break;
        }

        if (
            strcmp(args[0], "HELP") == 0 ||
            strcmp(args[0], "?") == 0
        )
        {
            ftpClientHelp();
            continue;
        }

        if (
            strcmp(args[0], "PWD") == 0
        )
        {
            ftpClientCommand(
                control,
                "PWD",
                code
            );

            continue;
        }

        if (
            strcmp(args[0], "CD") == 0 &&
            count >= 2
        )
        {
            char ftpCommand[FTPCLIENT_LINE_SIZE];

            snprintf(
                ftpCommand,
                sizeof(ftpCommand),
                "CWD %s",
                args[1]
            );

            ftpClientCommand(
                control,
                ftpCommand,
                code
            );

            continue;
        }

        if (
            (
                strcmp(args[0], "DIR") == 0 ||
                strcmp(args[0], "LS") == 0
            )
        )
        {
            WiFiClient data;

            if (!ftpClientPassive(
                control,
                data
            ))
            {
                continue;
            }

            char ftpCommand[FTPCLIENT_LINE_SIZE];

            if (count >= 2)
            {
                snprintf(
                    ftpCommand,
                    sizeof(ftpCommand),
                    "LIST %s",
                    args[1]
                );
            }
            else
            {
                strcpy(
                    ftpCommand,
                    "LIST"
                );
            }

            control.print(ftpCommand);
            control.print("\r\n");

            if (
                ftpClientReadResponse(
                    control,
                    code
                ) &&
                (
                    code == 125 ||
                    code == 150
                )
            )
            {
                ftpClientDrainListing(
                    data
                );

                data.stop();

                ftpClientReadResponse(
                    control,
                    code
                );
            }
            else
            {
                data.stop();
            }

            continue;
        }

        if (
            strcmp(args[0], "GET") == 0 &&
            count >= 2
        )
        {
            ftpClientGet(
                control,
                args[1],
                count >= 3
                    ? args[2]
                    : NULL
            );

            continue;
        }

        if (
            strcmp(args[0], "PUT") == 0 &&
            count >= 2
        )
        {
            ftpClientPut(
                control,
                args[1],
                count >= 3
                    ? args[2]
                    : NULL
            );

            continue;
        }

        if (
            strcmp(args[0], "USER") == 0 &&
            count >= 2
        )
        {
            char ftpCommand[FTPCLIENT_LINE_SIZE];

            snprintf(
                ftpCommand,
                sizeof(ftpCommand),
                "USER %s",
                args[1]
            );

            if (
                ftpClientCommand(
                    control,
                    ftpCommand,
                    code
                ) &&
                code == 331
            )
            {
                char password[96];

                _puts("Password: ");

                if (networkConsoleReadLine(
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
                        ftpCommand,
                        code
                    );
                }
            }

            continue;
        }

        if (
            strcmp(args[0], "PASS") == 0
        )
        {
            char password[96];

            if (count >= 2)
            {
                strncpy(
                    password,
                    args[1],
                    sizeof(password) - 1
                );

                password[
                    sizeof(password) - 1
                ] = 0;
            }
            else
            {
                _puts("Password: ");

                if (!networkConsoleReadLine(
                    password,
                    sizeof(password),
                    false
                ))
                {
                    continue;
                }
            }

            char ftpCommand[FTPCLIENT_LINE_SIZE];

            snprintf(
                ftpCommand,
                sizeof(ftpCommand),
                "PASS %s",
                password
            );

            ftpClientCommand(
                control,
                ftpCommand,
                code
            );

            continue;
        }

        if (
            strcmp(args[0], "DELETE") == 0 &&
            count >= 2
        )
        {
            char ftpCommand[FTPCLIENT_LINE_SIZE];

            snprintf(
                ftpCommand,
                sizeof(ftpCommand),
                "DELE %s",
                args[1]
            );

            ftpClientCommand(
                control,
                ftpCommand,
                code
            );

            continue;
        }

        _puts("FTP: unknown command. Type HELP.\r\n");
    }

    control.stop();

    _puts("FTP: disconnected\r\n");

    return 0;
}

#endif
