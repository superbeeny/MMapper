// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2002-2005 by Tomas Mecir - kmuddy@kmuddy.com
// Copyright (C) 2019 The MMapper Authors
// Author: Nils Schimmelmann <nschimme@gmail.com> (Jahara)

#include "AbstractTelnet.h"

#include <cassert>
#include <limits>
#include <QByteArray>
#include <QMessageLogContext>
#include <QObject>
#include <QString>

#include "../configuration/configuration.h"
#include "../global/utils.h"
#include "TextCodec.h"

static QString telnetCommandName(uint8_t cmd)
{
#define CASE(x) \
    case TN_##x: \
        return #x
    switch (cmd) {
        CASE(SE);
        CASE(NOP);
        CASE(DM);
        CASE(B);
        CASE(IP);
        CASE(AO);
        CASE(AYT);
        CASE(EC);
        CASE(EL);
        CASE(GA);
        CASE(SB);
        CASE(WILL);
        CASE(WONT);
        CASE(DO);
        CASE(DONT);
        CASE(IAC);
    }
    return QString::asprintf("%u", cmd);
#undef CASE
}

static QString telnetOptionName(uint8_t opt)
{
#define CASE(x) \
    case OPT_##x: \
        return #x
    switch (opt) {
        CASE(ECHO);
        CASE(SUPPRESS_GA);
        CASE(STATUS);
        CASE(TIMING_MARK);
        CASE(TERMINAL_TYPE);
        CASE(NAWS);
        CASE(CHARSET);
        CASE(COMPRESS2);
        CASE(GMCP);
    }
    return QString::asprintf("%u", opt);
#undef CASE
}
static QString telnetSubnegName(uint8_t opt)
{
#define CASE(x) \
    case TNSB_##x: \
        return #x
    switch (opt) {
        CASE(IS);
        CASE(SEND); // TODO: conflict between SEND and REQUEST
        CASE(ACCEPTED);
        CASE(REJECTED);
        CASE(TTABLE_IS);
        CASE(TTABLE_REJECTED);
        CASE(TTABLE_ACK);
        CASE(TTABLE_NAK);
    }
    return QString::asprintf("%u", opt);
#undef CASE
}

static bool containsIAC(const QByteArray &arr)
{
    for (const auto &c : arr) {
        if (static_cast<uint8_t>(c) == TN_IAC)
            return true;
    }
    return false;
}

struct TelnetFormatter final : public AppendBuffer
{
    void addRaw(const uint8_t byte)
    {
        auto &s = static_cast<AppendBuffer &>(*this);
        s += byte;
    }

    void addEscaped(const uint8_t byte)
    {
        addRaw(byte);
        if (byte == TN_IAC) {
            addRaw(byte);
        }
    }

    void addTwoByteEscaped(const uint16_t n)
    {
        // network order is big-endian
        addEscaped(static_cast<uint8_t>(n >> 8));
        addEscaped(static_cast<uint8_t>(n));
    }

    void addClampedTwoByteEscaped(const int n)
    {
        static constexpr const auto lo = static_cast<int>(std::numeric_limits<uint16_t>::min());
        static constexpr const auto hi = static_cast<int>(std::numeric_limits<uint16_t>::max());
        static_assert(lo == 0);
        static_assert(hi == 65535);
        addTwoByteEscaped(static_cast<uint16_t>(std::clamp(n, lo, hi)));
    }

    void addEscapedBytes(const QByteArray &s)
    {
        for (const auto &c : s) {
            addEscaped(static_cast<uint8_t>(c));
        }
    }

    void addCommand(uint8_t cmd)
    {
        addRaw(TN_IAC);
        addRaw(cmd);
    }

    void addSubnegBegin(uint8_t opt)
    {
        addCommand(TN_SB);
        addRaw(opt);
    }

    void addSubnegEnd() { addCommand(TN_SE); }
};

AbstractTelnet::AbstractTelnet(TextCodecStrategyEnum strategy,
                               const bool debug,
                               QObject *const parent,
                               const QByteArray &defaultTermType)
    : QObject(parent)
    , m_defaultTermType(defaultTermType)
    , textCodec{strategy}
    , debug{debug}
{
    reset();
}

void AbstractTelnet::reset()
{
    myOptionState.fill(false);
    hisOptionState.fill(false);
    announcedState.fill(false);
    heAnnouncedState.fill(false);

    // reset telnet status
    termType = m_defaultTermType;
    state = TelnetStateEnum::NORMAL;
    commandBuffer.clear();
    resetGmcpModules();
    subnegBuffer.clear();
    sentBytes = 0;
    recvdGA = false;
    resetCompress();
}

void AbstractTelnet::resetGmcpModules()
{
    if (debug)
        qDebug() << "Clearing GMCP modules";
#define X_CASE(UPPER_CASE, CamelCase, normalized, friendly) \
    gmcp.supported[GmcpModuleTypeEnum::UPPER_CASE] = DEFAULT_GMCP_MODULE_VERSION;
    X_FOREACH_GMCP_MODULE_TYPE(X_CASE)
#undef X_CASE
    gmcp.modules.clear();
}

void AbstractTelnet::receiveGmcpModule(const GmcpModule &module, const bool enabled)
{
    if (enabled) {
        if (!module.hasVersion())
            throw std::runtime_error("missing version");
        if (debug)
            qDebug() << "Adding GMCP module" << ::toQByteArrayLatin1(module.toStdString());
        gmcp.modules.insert(module);
        if (module.isSupported())
            gmcp.supported[module.getType()] = module.getVersion();

    } else {
        if (debug)
            qDebug() << "Removing GMCP module" << ::toQByteArrayLatin1(module.toStdString());
        gmcp.modules.erase(module);
        if (module.isSupported())
            gmcp.supported[module.getType()] = DEFAULT_GMCP_MODULE_VERSION;
    }
}

void AbstractTelnet::submitOverTelnet(const QByteArray &data, const bool goAhead)
{
    AppendBuffer outdata = data; /* copy */

    // IAC byte must be doubled
    if (containsIAC(outdata)) {
        TelnetFormatter d;
        d.addEscapedBytes(outdata);
        outdata = d;
    }

    // Add IAC GA unless they are suppressed
    if (goAhead && !hisOptionState[OPT_SUPPRESS_GA]) {
        outdata += TN_IAC;
        outdata += TN_GA;
    }

    // data ready, send it
    sendRawData(outdata);
}

void AbstractTelnet::sendWindowSizeChanged(const int x, const int y)
{
    if (debug)
        qDebug() << "Sending NAWS" << x << y;
    // RFC 1073 specifies IAC SB NAWS WIDTH[1] WIDTH[0] HEIGHT[1] HEIGHT[0] IAC SE
    TelnetFormatter s;
    s.addSubnegBegin(OPT_NAWS);
    // RFC855 specifies that option parameters with a byte value of 255 must be doubled
    s.addClampedTwoByteEscaped(x);
    s.addClampedTwoByteEscaped(y);
    s.addSubnegEnd();
    sendRawData(s);
}

void AbstractTelnet::sendTelnetOption(unsigned char type, unsigned char option)
{
    if (debug)
        qDebug() << "* Sending Telnet Command: " << telnetCommandName(type)
                 << telnetOptionName(option);
    AppendBuffer s;
    s += TN_IAC;
    s += type;
    s += option;
    sendRawData(s);
}

void AbstractTelnet::requestTelnetOption(unsigned char type, unsigned char option)
{
    myOptionState[option] = true;
    announcedState[option] = true;
    sendTelnetOption(type, option);
}

void AbstractTelnet::sendCharsetRequest(const QStringList &myCharacterSets)
{
    if (debug)
        qDebug() << "Requesting charsets" << myCharacterSets;
    static const auto delimeter = ";";
    TelnetFormatter s;
    s.addSubnegBegin(OPT_CHARSET);
    s.addRaw(TNSB_REQUEST);
    for (QString characterSet : myCharacterSets) {
        s.addEscapedBytes(delimeter);
        s.addEscapedBytes(characterSet.toLocal8Bit());
    }
    s.addSubnegEnd();
    sendRawData(s);
}

bool AbstractTelnet::isGmcpModuleEnabled(const GmcpModuleTypeEnum &name)
{
    if (!myOptionState[OPT_GMCP])
        return false;

    return gmcp.supported[name] != DEFAULT_GMCP_MODULE_VERSION;
}

void AbstractTelnet::sendGmcpMessage(const GmcpMessage &msg)
{
    auto payload = msg.toRawBytes();
    if (debug)
        qDebug() << "Sending GMCP:" << payload;
    TelnetFormatter s;
    s.addSubnegBegin(OPT_GMCP);
    s.addEscapedBytes(payload);
    s.addSubnegEnd();
    sendRawData(s);
}

void AbstractTelnet::sendTerminalType(const QByteArray &terminalType)
{
    if (debug)
        qDebug() << "Sending Terminal Type:" << terminalType;
    TelnetFormatter s;
    s.addSubnegBegin(OPT_TERMINAL_TYPE);
    // RFC855 specifies that option parameters with a byte value of 255 must be doubled
    s.addEscaped(TNSB_IS); /* NOTE: "IS" will never actually be escaped */
    s.addEscapedBytes(terminalType);
    s.addSubnegEnd();
    sendRawData(s);
}

void AbstractTelnet::sendCharsetRejected()
{
    TelnetFormatter s;
    s.addSubnegBegin(OPT_CHARSET);
    s.addRaw(TNSB_REJECTED);
    s.addSubnegEnd();
    sendRawData(s);
}

void AbstractTelnet::sendCharsetAccepted(const QByteArray &characterSet)
{
    if (debug)
        qDebug() << "Accepted Charset" << characterSet;
    TelnetFormatter s;
    s.addSubnegBegin(OPT_CHARSET);
    s.addRaw(TNSB_ACCEPTED);
    s.addEscapedBytes(characterSet);
    s.addSubnegEnd();
    sendRawData(s);
}

void AbstractTelnet::sendOptionStatus()
{
    AppendBuffer s;
    s += TN_IAC;
    s += TN_SB;
    s += OPT_STATUS;
    s += TNSB_IS;
    for (size_t i = 0; i < NUM_OPTS; ++i) {
        if (myOptionState[i]) {
            s += TN_WILL;
            s += static_cast<unsigned char>(i);
        }
        if (hisOptionState[i]) {
            s += TN_DO;
            s += static_cast<unsigned char>(i);
        }
    }
    s += TN_IAC;
    s += TN_SE;
    sendRawData(s);
}

void AbstractTelnet::sendAreYouThere()
{
    sendRawData("I'm here! Please be more patient!\r\n");
    // well, this should never be executed, as the response would probably
    // be treated as a command. But that's server's problem, not ours...
    // If the server wasn't capable of handling this, it wouldn't have
    // sent us the AYT command, would it? Impatient server = bad server.
    // Let it suffer! ;-)
}

void AbstractTelnet::sendTerminalTypeRequest()
{
    TelnetFormatter s;
    s.addSubnegBegin(OPT_TERMINAL_TYPE);
    s.addEscaped(TNSB_SEND);
    s.addSubnegEnd();
    sendRawData(s);
}

void AbstractTelnet::processTelnetCommand(const AppendBuffer &command)
{
    assert(!command.isEmpty());

    const unsigned char ch = command.unsigned_at(1);
    unsigned char option;

    switch (command.length()) {
    case 1:
        break;
    case 2:
        if (ch != TN_GA && debug)
            qDebug() << "* Processing Telnet Command:" << telnetCommandName(ch);

        switch (ch) {
        case TN_AYT:
            sendAreYouThere();
            break;
        case TN_GA:
            recvdGA = true; // signal will be emitted later
            break;
        }
        break;

    case 3:
        if (debug)
            qDebug() << "* Processing Telnet Command:" << telnetCommandName(ch)
                     << telnetOptionName(command.unsigned_at(2));

        switch (ch) {
        case TN_WILL:
            // server wants to enable some option (or he sends a timing-mark)...
            option = command.unsigned_at(2);

            heAnnouncedState[option] = true;
            if (!hisOptionState[option])
            // only if this is not set; if it's set, something's wrong wth the server
            // (according to telnet specification, option announcement may not be
            // unless explicitly requested)
            {
                if (!myOptionState[option])
                // only if the option is currently disabled
                {
                    if ((option == OPT_SUPPRESS_GA) || (option == OPT_STATUS)
                        || (option == OPT_TERMINAL_TYPE) || (option == OPT_NAWS)
                        || (option == OPT_ECHO) || (option == OPT_CHARSET)
                        || (option == OPT_COMPRESS2 && !NO_ZLIB) || (option == OPT_GMCP))
                    // these options are supported
                    {
                        sendTelnetOption(TN_DO, option);
                        hisOptionState[option] = true;
                        if (option == OPT_ECHO) {
                            receiveEchoMode(false);
                        }
                    } else {
                        sendTelnetOption(TN_DONT, option);
                        hisOptionState[option] = false;
                    }
                } else if (myOptionState[option] && option == OPT_TERMINAL_TYPE) {
                    sendTerminalTypeRequest();
                }
            } else if (debug) {
                qDebug() << "His option" << telnetOptionName(option) << "was already enabled";
            }
            break;
        case TN_WONT:
            // server refuses to enable some option...
            option = command.unsigned_at(2);
            if (!myOptionState[option])
            // only if the option is currently disabled
            {
                // send DONT if needed (see RFC 854 for details)
                if (hisOptionState[option] || (!heAnnouncedState[option])) {
                    sendTelnetOption(TN_DONT, option);
                    hisOptionState[option] = false;
                    if (option == OPT_ECHO) {
                        receiveEchoMode(true);
                    }
                }
            }
            heAnnouncedState[option] = true;
            break;
        case TN_DO:
            // server wants us to enable some option
            option = command.unsigned_at(2);
            if (option == OPT_TIMING_MARK) {
                // send WILL TIMING_MARK
                sendTelnetOption(TN_WILL, option);
            } else if (!myOptionState[option])
            // only if the option is currently disabled
            {
                if ((option == OPT_SUPPRESS_GA) || (option == OPT_STATUS)
                    || (option == OPT_TERMINAL_TYPE) || (option == OPT_NAWS) || (option == OPT_ECHO)
                    || (option == OPT_CHARSET) || (option == OPT_GMCP)) {
                    sendTelnetOption(TN_WILL, option);
                    myOptionState[option] = true;
                    announcedState[option] = true;
                } else {
                    sendTelnetOption(TN_WONT, option);
                    myOptionState[option] = false;
                    announcedState[option] = true;
                }
            } else if (debug) {
                qDebug() << "My option" << telnetOptionName(option) << "was already enabled";
            }
            if (myOptionState[OPT_NAWS] && option == OPT_NAWS) {
                // NAWS here - window size info must be sent
                // REVISIT: Should we attempt to rate-limit this to avoid spamming dozens of NAWS
                // messages per second when the user adjusts the window size?
                sendWindowSizeChanged(current.x, current.y);

            } else if (myOptionState[OPT_CHARSET] && option == OPT_CHARSET) {
                sendCharsetRequest(textCodec.supportedEncodings());
                // REVISIT: RFC 2066 states to queue all subsequent data until ACCEPTED / REJECTED
            } else if (myOptionState[OPT_COMPRESS2] && option == OPT_COMPRESS2 && !NO_ZLIB) {
                // REVISIT: Start deflating after sending IAC SB COMPRESS2 IAC SE

            } else if (myOptionState[OPT_GMCP] && option == OPT_GMCP) {
                onGmcpEnabled();
            }
            break;
        case TN_DONT:
            // only respond if value changed or if this option has not been announced yet
            option = command.unsigned_at(2);
            if (myOptionState[option] || (!announcedState[option])) {
                sendTelnetOption(TN_WONT, option);
                announcedState[option] = true;
            }
            myOptionState[option] = false;
            break;
        }
        break;

    default:
        // other cmds should not arrive, as they were not negotiated.
        // if they do, they are merely ignored
        break;
    }
    // other commands are simply ignored (NOP and such, see .h file for list)
}

void AbstractTelnet::processTelnetSubnegotiation(const AppendBuffer &payload)
{
    if (debug) {
        if (payload.length() == 1)
            qDebug() << "* Processing Telnet Subnegotiation:"
                     << telnetOptionName(payload.unsigned_at(0));
        else if (payload.length() >= 2)
            qDebug() << "* Processing Telnet Subnegotiation:"
                     << telnetOptionName(payload.unsigned_at(0))
                     << telnetSubnegName(payload.unsigned_at(1));
    }

    // subnegotiation - we analyze and respond...
    const uint8_t option = static_cast<uint8_t>(payload[0]);
    switch (option) {
    case OPT_STATUS:
        // see OPT_TERMINAL_TYPE for explanation why I'm doing this
        if (true /*myOptionState[OPT_STATUS]*/) {
            if (payload[1] == TNSB_SEND)
            // request to send all enabled commands; if server sends his
            // own list of commands, we just ignore it (well, he shouldn't
            // send anything, as we do not request anything, but there are
            // so many servers out there, that you can never be sure...)
            {
                sendOptionStatus();
            }
        }
        break;

    case OPT_TERMINAL_TYPE:
        if (myOptionState[OPT_TERMINAL_TYPE]) {
            switch (payload[1]) {
            case TNSB_SEND:
                // server wants us to send terminal type
                sendTerminalType(termType);
                break;
            case TNSB_IS:
                // Extract sender's terminal type
                // TERMINAL_TYPE IS <...>
                receiveTerminalType(payload.mid(2));
                break;
            }
        }
        break;

    case OPT_CHARSET:
        if (myOptionState[OPT_CHARSET]) {
            switch (payload[1]) {
            case TNSB_REQUEST:
                // MMapper does not support [TTABLE]
                if (payload.length() >= 4 && payload[2] != '[') {
                    bool accepted = false;
                    // Split remainder into delim and IAC
                    // CHARSET REQUEST <sep> <charsets>
                    const auto sep = payload[2];
                    const auto characterSets = payload.mid(3).split(sep);
                    for (const auto &characterSet : characterSets) {
                        if (textCodec.supports(characterSet)) {
                            accepted = true;
                            textCodec.setEncodingForName(characterSet);

                            // Reply to server that we accepted this encoding
                            sendCharsetAccepted(characterSet);
                            break;
                        }
                    }
                    if (accepted) {
                        break;
                    } else if (debug) {
                        qDebug() << "Rejected encoding" << characterSets;
                    }
                }
                // Reject invalid requests or if we did not find any supported codecs
                sendCharsetRejected();
                break;
            case TNSB_ACCEPTED:
                if (payload.length() > 3) {
                    // CHARSET ACCEPTED <charset>
                    auto characterSet = payload.mid(2);
                    textCodec.setEncodingForName(characterSet);
                    // TODO: RFC 2066 states to stop queueing data
                }
                break;
            case TNSB_REJECTED:
                // TODO: RFC 2066 states to stop queueing data
                break;
            case TNSB_TTABLE_IS:
                // We never request a [TTABLE] so this should not happen
                abort();
                /*NOTREACHED*/
                break;
            }
        }
        break;

    case OPT_COMPRESS2:
        assert(!NO_ZLIB);
        if (hisOptionState[OPT_COMPRESS2]) {
            if (debug) {
                if (inflateTelnet) {
                    qDebug() << "Compression was already enabled";
                    break;
                }
                qDebug() << "Starting compression";
            }
            recvdCompress = true;
        }
        break;

    case OPT_GMCP:
        if (myOptionState[OPT_GMCP]) {
            // Package[.SubPackages].Message <data>
            if (payload.length() <= 1) {
                qWarning() << "Invalid GMCP received" << payload;
                break;
            }
            try {
                GmcpMessage msg = GmcpMessage::fromRawBytes(payload.mid(1));
                if (debug) {
                    qDebug() << "Received GMCP message" << msg.getName().toQString()
                             << (msg.getJson() ? msg.getJson()->toQString() : "");
                }
                receiveGmcpMessage(msg);

            } catch (const std::exception &e) {
                qWarning() << "Corrupted GMCP received" << payload << e.what();
            }
        }
        break;

    case OPT_NAWS:
        if (myOptionState[OPT_NAWS]) {
            // NAWS <16-bit value> <16-bit value>
            if (payload.length() == 5) {
                const auto x1 = static_cast<uint8_t>(payload[1]);
                const auto x2 = static_cast<uint8_t>(payload[2]);
                const auto y1 = static_cast<uint8_t>(payload[3]);
                const auto y2 = static_cast<uint8_t>(payload[4]);
                const auto x = static_cast<uint16_t>((x1 << 8) + x2);
                const auto y = static_cast<uint16_t>((y1 << 8) + y2);
                receiveWindowSize(x, y);
                break;
            }
            qWarning() << "Corrupted NAWS received" << payload;
        }
        break;

    default:
        // other subnegs should not arrive and if they do, they are merely ignored
        break;
    }
}

void AbstractTelnet::onReadInternal(const QByteArray &data)
{
    if (data.isEmpty())
        return;

    // now we have the data, but we cannot forward it to next stage of processing,
    // because the data contains telnet commands
    // so we parse the text and process all telnet commands:

    AppendBuffer cleanData;
    cleanData.reserve(data.size());

    int pos = 0;
    while (pos < data.size()) {
        if (inflateTelnet) {
            int remaining = onReadInternalInflate(data.data() + pos, data.size() - pos, cleanData);
            pos = data.length() - remaining;
            // Continue because there might be additional chunks left to inflate
            continue;
        }

        // Process character by character
        const uint8_t c = static_cast<unsigned char>(data.at(pos));
        onReadInternal2(cleanData, c);
        pos++;

        if (recvdCompress) {
            initCompress();
            recvdCompress = false;
            // Start inflating at the next position
            continue;
        }

        if (recvdGA) {
            sendToMapper(cleanData, recvdGA); // with GO-AHEAD
            cleanData.clear();
            recvdGA = false;
        }
    }

    // some data left to send - do it now!
    if (!cleanData.isEmpty()) {
        sendToMapper(cleanData, recvdGA); // without GO-AHEAD
        cleanData.clear();
    }
}

/*
 * normal telnet state
 * -------------------
 * x                                # forward 0-254
 * IAC IAC                          # forward 255
 * IAC (WILL | WONT | DO | DONT) x  # negotiate 0-255 (w/ 255 = EXOPL)
 * IAC SB                           # begins subnegotiation
 * IAC SE                           # (error)
 * IAC x                            # exec command
 *
 * within a subnegotiation
 * -----------------------
 * x                                # appends 0-254 to option payload
 * IAC IAC                          # appends 255 to option payload
 * IAC (WILL | WONT | DO | DONT) x  # negotiate 0-255 (w/ 255 = EXOPL)
 * IAC SB                           # (error)
 * IAC SE                           # ends subnegotiation
 * IAC x                            # exec command
 *
 * NOTE: RFC 855 refers to IAC SE as a command rather than a delimiter,
 * so that implies you're still supposed to process "commands" (e.g. IAC GA)!
 *
 * So if you receive "IAC SB IAC WILL ECHO f o o IAC IAC b a r IAC SE"
 * then you process will(ECHO) followed by the subnegotiation(f o o 255 b a r).
 */
void AbstractTelnet::onReadInternal2(AppendBuffer &cleanData, const uint8_t c)
{
    switch (state) {
    case TelnetStateEnum::NORMAL:
        if (c == TN_IAC) {
            // this is IAC, previous character was regular data
            state = TelnetStateEnum::IAC;
            commandBuffer.append(c);
        } else {
            // plaintext
            cleanData.append(c);
        }
        break;
    case TelnetStateEnum::IAC:
        // seq. of two IACs
        if (c == TN_IAC) {
            state = TelnetStateEnum::NORMAL;
            cleanData.append(c);
            commandBuffer.clear();
        }
        // IAC DO/DONT/WILL/WONT
        else if ((c == TN_WILL) || (c == TN_WONT) || (c == TN_DO) || (c == TN_DONT)) {
            state = TelnetStateEnum::COMMAND;
            commandBuffer.append(c);
        }
        // IAC SB
        else if (c == TN_SB) {
            state = TelnetStateEnum::SUBNEG;
            commandBuffer.clear();
        }
        // IAC SE without IAC SB - error - ignored
        else if (c == TN_SE) {
            state = TelnetStateEnum::NORMAL;
            commandBuffer.clear();
        }
        // IAC fol. by something else than IAC, SB, SE, DO, DONT, WILL, WONT
        else {
            state = TelnetStateEnum::NORMAL;
            commandBuffer.append(c);
            processTelnetCommand(commandBuffer);
            // this could have set receivedGA to true; we'll handle that later
            // (at the end of this function)
            commandBuffer.clear();
        }
        break;
    case TelnetStateEnum::COMMAND:
        // IAC DO/DONT/WILL/WONT <command code>
        state = TelnetStateEnum::NORMAL;
        commandBuffer.append(c);
        processTelnetCommand(commandBuffer);
        commandBuffer.clear();
        break;

    case TelnetStateEnum::SUBNEG:
        if (c == TN_IAC) {
            // this is IAC, previous character was option payload
            state = TelnetStateEnum::SUBNEG_IAC;
            commandBuffer.append(c);
        } else {
            // option payload
            subnegBuffer.append(c);
        }
        break;
    case TelnetStateEnum::SUBNEG_IAC:
        // seq. of two IACs
        if (c == TN_IAC) {
            state = TelnetStateEnum::SUBNEG;
            subnegBuffer.append(c);
            commandBuffer.clear();
        }
        // IAC DO/DONT/WILL/WONT
        else if ((c == TN_WILL) || (c == TN_WONT) || (c == TN_DO) || (c == TN_DONT)) {
            state = TelnetStateEnum::SUBNEG_COMMAND;
            commandBuffer.append(c);
        }
        // IAC SE - end of subcommand
        else if (c == TN_SE) {
            state = TelnetStateEnum::NORMAL;
            processTelnetSubnegotiation(subnegBuffer);
            commandBuffer.clear();
            subnegBuffer.clear();
        }
        // IAC SB within IAC SB - error - ignored
        else if (c == TN_SB) {
            state = TelnetStateEnum::NORMAL;
            commandBuffer.clear();
            subnegBuffer.clear();
        }
        // IAC fol. by something else than IAC, SB, SE, DO, DONT, WILL, WONT
        else {
            state = TelnetStateEnum::SUBNEG;
            commandBuffer.append(c);
            processTelnetCommand(commandBuffer);
            // this could have set receivedGA to true; we'll handle that later
            // (at the end of this function)
            commandBuffer.clear();
        }
        break;
    case TelnetStateEnum::SUBNEG_COMMAND:
        // IAC DO/DONT/WILL/WONT <command code>
        state = TelnetStateEnum::SUBNEG;
        commandBuffer.append(c);
        processTelnetCommand(commandBuffer);
        commandBuffer.clear();
        break;
    }
}

TextCodec &AbstractTelnet::getTextCodec()
{
    // Switch codec if RFC 2066 was not negotiated and the configuration was altered
    if (!hisOptionState[OPT_CHARSET]) {
        const CharacterEncodingEnum configEncoding = getConfig().general.characterEncoding;
        if (configEncoding != textCodec.getEncoding()) {
            textCodec.setEncoding(configEncoding);
        }
    }
    return textCodec;
}

int AbstractTelnet::onReadInternalInflate(const char *data,
                                          const int length,
                                          AppendBuffer &cleanData)
{
#ifdef MMAPPER_NO_ZLIB
    abort();
#else
    static constexpr const int CHUNK = 1024;
    char out[CHUNK];

    stream.avail_in = static_cast<uInt>(length);
    stream.next_in = reinterpret_cast<const Bytef *>(data);

    /* decompress until deflate stream ends */
    do {
        stream.avail_out = CHUNK;
        stream.next_out = reinterpret_cast<Bytef *>(out);
        int ret = inflate(&stream, Z_SYNC_FLUSH);
        assert(ret != Z_STREAM_ERROR); /* state not clobbered */
        switch (ret) {
        case Z_NEED_DICT:
        case Z_DATA_ERROR:
        case Z_MEM_ERROR:
        case Z_STREAM_END:
            /* clean up and return */
            inflateEnd(&stream);
            resetCompress();
            if (debug)
                qDebug() << "Ending compression";
            throw std::runtime_error(stream.msg);
        default:
            break;
        }

        const int outLen = CHUNK - static_cast<int>(stream.avail_out);
        for (auto i = 0; i < outLen; i++) {
            // Process character by character
            const uint8_t c = static_cast<unsigned char>(out[i]);
            onReadInternal2(cleanData, c);

            if (recvdGA) {
                sendToMapper(cleanData, recvdGA); // with GO-AHEAD
                cleanData.clear();
                recvdGA = false;
            }
        }

        if (debug && outLen > 0) {
            const double compressionRatio = static_cast<double>(outLen)
                                            / static_cast<double>(length);
            qDebug() << QString("zlib compression ratio of %1:1")
                            .arg(QString::number(compressionRatio, 'f', 1));
        }

    } while (stream.avail_out == 0);
    return static_cast<int>(stream.avail_in);
#endif
}

void AbstractTelnet::resetCompress()
{
    inflateTelnet = false;
    recvdCompress = false;
    hisOptionState[OPT_COMPRESS2] = false;
}

void AbstractTelnet::initCompress()
{
#ifdef MMAPPER_NO_ZLIB
    abort();
#else
    inflateTelnet = true;

    /* allocate inflate state */
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    stream.avail_in = 0;
    stream.next_in = Z_NULL;
    int ret = inflateInit(&stream);
    if (ret != Z_OK) {
        throw std::runtime_error("Unable to initialize zlib");
    }
#endif
}
