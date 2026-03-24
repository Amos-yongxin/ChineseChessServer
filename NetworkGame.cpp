// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2019-2026 XMuli & Contributors
// SPDX-GitHub: https://github.com/XMuli/ChineseChess
// SPDX-Author: XMuli <xmulitech@gmail.com>

#include "NetworkGame.h"
#include <mutex>
#include "ui_ChessBoard.h"
#include <QCoreApplication>
#include <QNetworkInterface>
#include <QDebug>
#include <QNetworkProxyFactory>
#include <QHostAddress>
#include <QStringList>
#include <QSignalBlocker>
#include <QComboBox>
#include <QSpinBox>
#include <algorithm>
#include <iterator>

namespace {
constexpr quint8 kGameOverPacketType = 0xFD;
constexpr quint8 kTestCommandPacketType = 0xFC;
constexpr quint8 kTestCommandLoadPreset = 0x01;
constexpr quint8 kRolePacketType = 0xFB;
constexpr quint8 kRoleStatusInvalid = 0xFF;
constexpr quint8 kRoleStatusOccupied = 0xFE;
constexpr quint8 kRoleStatusRepeated = 0xFD;

struct IpCandidate {
    QString ip;
    QString label;
    int priority = 2;
};

struct TestPiecePlacement {
    int id;
    int row;
    int col;
};

struct TestPreset {
    quint8 id;
    bool redFirst;
    QVector<TestPiecePlacement> pieces;
};

const QVector<TestPreset>& testPresets()
{
    static const QVector<TestPreset> presets = {
        {
            1,
            true,
            {
                {4, 0, 3},
                {20, 9, 5},
                {24, 2, 4},
            }
        },
        {
            2,
            true,
            {
                {4, 0, 4},
                {3, 1, 4},
                {16, 1, 3},
                {24, 2, 5},
                {20, 9, 4},
            }
        },
    };
    return presets;
}

const TestPreset* findTestPreset(quint8 id)
{
    const auto& presets = testPresets();
    for (const TestPreset& preset : presets) {
        if (preset.id == id)
            return &preset;
    }
    return nullptr;
}

bool isInterfaceUsable(const QNetworkInterface& iface)
{
    if (!iface.isValid())
        return false;
    const auto flags = iface.flags();
    if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning))
        return false;
    if (flags & QNetworkInterface::IsLoopBack)
        return false;
    return true;
}

int interfacePriority(const QNetworkInterface& iface)
{
    const QString human = iface.humanReadableName().toLower();
    const QString raw = iface.name().toLower();
    auto containsAny = [&](const QStringList& keys) {
        for (const QString& key : keys) {
            if (human.contains(key) || raw.contains(key))
                return true;
        }
        return false;
    };

    if (containsAny({"wlan", "wifi", "wi-fi", "wireless", "wl"}))
        return 0; // Wi-Fi
    if (containsAny({"eth", "en", "lan"}))
        return 1; // Ethernet
    return 2;      // Others
}

QString interfaceLabel(int priority)
{
    switch (priority) {
    case 0: return QStringLiteral("Wi-Fi");
    case 1: return QStringLiteral("Ethernet");
    default: return QStringLiteral("Other");
    }
}

QList<IpCandidate> collectIpCandidates()
{
    QList<IpCandidate> result;
    const auto interfaces = QNetworkInterface::allInterfaces();

    for (const QNetworkInterface& iface : interfaces) {
        if (!isInterfaceUsable(iface))
            continue;

        const int priority = interfacePriority(iface);
        const QString tag = interfaceLabel(priority);
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol)
                continue;

            const QString ip = addr.toString();
            const bool duplicate = std::any_of(result.cbegin(), result.cend(), [&](const IpCandidate& candidate) {
                return candidate.ip == ip;
            });
            if (duplicate)
                continue;

            IpCandidate candidate;
            candidate.ip = ip;
            candidate.priority = priority;
            candidate.label = QStringLiteral("%1  (%2 · %3)").arg(ip, tag, iface.humanReadableName());
            result.append(candidate);
        }
    }

    std::sort(result.begin(), result.end(), [](const IpCandidate& lhs, const IpCandidate& rhs) {
        if (lhs.priority != rhs.priority)
            return lhs.priority < rhs.priority;
        return lhs.ip < rhs.ip;
    });

    return result;
}

QString roleStatusText(quint8 status)
{
    switch (status) {
    case 1:
        return QStringLiteral("角色握手成功：red");
    case 2:
        return QStringLiteral("角色握手成功：black");
    case kRoleStatusInvalid:
        return QStringLiteral("角色握手失败：非法角色值");
    case kRoleStatusOccupied:
        return QStringLiteral("角色握手失败：该角色已被占用");
    case kRoleStatusRepeated:
        return QStringLiteral("角色握手失败：重复声明角色");
    default:
        return QStringLiteral("角色握手失败：未知状态 %1").arg(status);
    }
}
} // namespace

NetworkGame::NetworkGame(bool isServer, bool localIsRed)
{
    m_isServerHost = isServer;
    m_localRole = isServer ? Role::None : (localIsRed ? Role::Red : Role::Black);
    m_clientHandshakeDone = false;
    m_waitingForAck = false;
    m_expectOptionalGameOver = false;
    m_bIsTcpServer = isServer;
    setPerspectiveFlipped(!isServer && !localIsRed);
    m_tcpServer = nullptr;
    redTcpSocket = nullptr;
    blackTcpSocket = nullptr;
    m_tcpSocket = nullptr;

    QNetworkProxyFactory::setUseSystemConfiguration(false);

    initUI();
    if (m_isServerHost) {
        m_tcpServer = new QTcpServer(this);
        onBtnTryConnect();
        connect(m_tcpServer, &QTcpServer::newConnection, this, &NetworkGame::slotNewConnection);
    } else {
        m_tcpSocket = new QTcpSocket(this);
        connect(m_tcpSocket, &QTcpSocket::readyRead, this, &NetworkGame::slotClientReadyRead);
        connect(m_tcpSocket, &QTcpSocket::disconnected, this, [this]() {
            m_clientHandshakeDone = false;
            m_waitingForAck = false;
            m_expectOptionalGameOver = false;
            m_clientBuffer.clear();
            updateConnectionStatus(QStringLiteral("与服务器的连接已断开"));
        });
    }
    connect(ChessBoard::ui->btnTcpConnect, &QPushButton::released, this, &NetworkGame::onBtnTryConnect);
    connect(ChessBoard::ui->comboIp, &QComboBox::currentTextChanged, this, &NetworkGame::handleServerEndpointChange);
    connect(ChessBoard::ui->sbPort, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, &NetworkGame::handleServerEndpointChange);
    updateConnectionStatus();
}

void NetworkGame::initUI()
{
    auto& ui = ChessBoard::ui;
    const QString preservedIp = currentIpText();
    const int preservedPort = ui->sbPort->value();

    if (m_isServerHost) {
        ui->networkGroup->setTitle("服务器监听的IP和Port");
        ui->btnTcpConnect->setText("监听");
        const QList<IpCandidate> candidates = collectIpCandidates();
        QStringList ipList;
        std::transform(candidates.cbegin(), candidates.cend(), std::back_inserter(ipList), [](const IpCandidate& c){ return c.ip; });
        populateLocalIpChoices(ipList, ipList.isEmpty() ? QStringLiteral("127.0.0.1") : ipList.first());
        ui->comboIp->setEditable(false);
        ui->comboIp->setEnabled(true);
        ui->comboIp->setStyleSheet(QString());
        {
            QSignalBlocker blocker(ui->sbPort);
            ui->sbPort->setValue(preservedPort);
        }
    } else {
        ui->networkGroup->setTitle(QString("请输入[服务器]的IP和Port [本端%1]").arg(roleLabel(m_localRole)));
        ui->btnTcpConnect->setText("连接");
        ui->btnTcpConnect->show();
        ui->comboIp->clear();
        ui->comboIp->setEditable(true);
        ui->comboIp->setEnabled(true);
        ui->comboIp->setStyleSheet(QStringLiteral(
            "QComboBox::drop-down { width: 0px; border: none; }\n"
            "QComboBox::down-arrow { image: none; }"
        ));
        ui->comboIp->setEditText(preservedIp);
        {
            QSignalBlocker blocker(ui->sbPort);
            ui->sbPort->setValue(preservedPort);
        }
    }
}

void NetworkGame::clickPieces(int checkedID, int &row, int &col)
{
    if (m_isServerHost) {
        updateConnectionStatus(QStringLiteral("服务器模式仅负责裁判与转发，不直接执棋"));
        return;
    }
    if (!m_clientHandshakeDone) {
        updateConnectionStatus(QStringLiteral("角色握手未完成，等待另一方加入"));
        return;
    }
    if (!m_tcpSocket || m_tcpSocket->state() != QAbstractSocket::ConnectedState) {
        updateConnectionStatus(QStringLiteral("尚未连接到服务器"));
        return;
    }
    if (m_waitingForAck) {
        updateConnectionStatus(QStringLiteral("等待服务器确认当前走子"));
        return;
    }

    if (m_nSelectID == -1 && checkedID >= 0 && checkedID < 32) {
        if (m_ChessPieces[checkedID].m_bRed != localRoleIsRed())
            return;
    }

    whoWin();
    const int previousStepCount = m_ChessSteps.size();
    ChessBoard::clickPieces(checkedID, row, col);
    if (m_ChessSteps.size() != previousStepCount + 1)
        return;

    ChessStep* step = m_ChessSteps.last();
    const char packet[3] = {
        static_cast<char>(step->m_nMoveID),
        static_cast<char>(step->m_nRowTo),
        static_cast<char>(step->m_nnColTo),
    };

    const qint64 written = m_tcpSocket->write(packet, 3);
    m_tcpSocket->flush();
    if (written != 3) {
        backOne();
        update();
        updateConnectionStatus(QStringLiteral("走子发送失败，已回滚本地状态"));
        return;
    }

    m_waitingForAck = true;
    m_expectOptionalGameOver = false;
    updateConnectionStatus(QStringLiteral("已发送走子，等待服务器确认：%1 %2 %3")
                               .arg(step->m_nMoveID)
                               .arg(step->m_nRowTo)
                               .arg(step->m_nnColTo));
}

void NetworkGame::slotNewConnection()
{
    if (!m_tcpServer)
        return;

    while (m_tcpServer->hasPendingConnections()) {
        QTcpSocket* socket = m_tcpServer->nextPendingConnection();
        if (!socket)
            continue;

        m_serverBuffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            handleServerReadyRead(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            handleSocketDisconnected(socket);
        });
        updateConnectionStatus(QStringLiteral("客户端已连接，等待其发送角色声明"));
    }
}

void NetworkGame::slotClientReadyRead()
{
    if (!m_tcpSocket)
        return;

    m_clientBuffer.append(m_tcpSocket->readAll());
    processClientBuffer();
}

void NetworkGame::handleServerReadyRead(QTcpSocket* socket)
{
    if (!socket)
        return;

    QByteArray& buffer = m_serverBuffers[socket];
    buffer.append(socket->readAll());

    while (true) {
        if (!m_serverRoles.contains(socket)) {
            if (buffer.size() < 2)
                return;

            if (static_cast<quint8>(buffer[0]) != kRolePacketType) {
                disconnectSocketWithRoleStatus(socket, kRoleStatusInvalid, QStringLiteral("连接后的首包必须为角色声明"));
                return;
            }

            const quint8 roleCode = static_cast<quint8>(buffer[1]);
            buffer.remove(0, 2);
            const Role requestedRole = roleFromCode(roleCode);
            if (requestedRole == Role::None) {
                disconnectSocketWithRoleStatus(socket, kRoleStatusInvalid, QStringLiteral("收到非法角色值"));
                return;
            }
            if ((requestedRole == Role::Red && redTcpSocket && redTcpSocket != socket) ||
                (requestedRole == Role::Black && blackTcpSocket && blackTcpSocket != socket)) {
                disconnectSocketWithRoleStatus(socket, kRoleStatusOccupied, QStringLiteral("角色 %1 已被占用").arg(roleLabel(requestedRole)));
                return;
            }

            m_serverRoles.insert(socket, requestedRole);
            if (requestedRole == Role::Red)
                redTcpSocket = socket;
            else
                blackTcpSocket = socket;

            updateConnectionStatus();
            finalizeHandshakeIfReady();
            continue;
        }

        if (buffer.isEmpty())
            return;

        const quint8 packetType = static_cast<quint8>(buffer[0]);
        if (!bothRolesReady()) {
            if (packetType == kRolePacketType) {
                if (buffer.size() < 2)
                    return;
                buffer.remove(0, 2);
                disconnectSocketWithRoleStatus(socket, kRoleStatusRepeated, QStringLiteral("重复声明角色"));
            } else {
                disconnectSocketWithRoleStatus(socket, kRoleStatusInvalid, QStringLiteral("对局尚未就绪，不能发送普通数据包"));
            }
            return;
        }

        const int packetSize = (packetType == kRolePacketType) ? 2 : 3;
        if (buffer.size() < packetSize)
            return;

        if (packetType == kRolePacketType) {
            buffer.remove(0, 2);
            disconnectSocketWithRoleStatus(socket, kRoleStatusRepeated, QStringLiteral("重复声明角色"));
            return;
        }

        const QByteArray packet = buffer.left(3);
        buffer.remove(0, 3);
        processServerPacket(socket, packet);
    }
}

void NetworkGame::processServerPacket(QTcpSocket* fromTcpSocket, const QByteArray& packet)
{
    if (!fromTcpSocket || packet.size() < 3)
        return;

    const Role actorRole = m_serverRoles.value(fromTcpSocket, Role::None);
    if (actorRole == Role::None)
        return;

    QTcpSocket* const toTcpSocket = (actorRole == Role::Red) ? blackTcpSocket : redTcpSocket;
    const bool isRed = (actorRole == Role::Red);
    const quint8 packetType = static_cast<quint8>(packet[0]);
    if (packetType == kTestCommandPacketType) {
        const quint8 command = static_cast<quint8>(packet[1]);
        const quint8 presetId = static_cast<quint8>(packet[2]);
        char res = -1;
        if (command == kTestCommandLoadPreset && loadTestPreset(presetId)) {
            res = 1;
        }
        fromTcpSocket->write(&res, 1);
        fromTcpSocket->flush();
        return;
    }

    const int nCheckedID = static_cast<quint8>(packet[0]);
    const int nRow = static_cast<quint8>(packet[1]);
    const int nCol = static_cast<quint8>(packet[2]);
    const int killid = findOccupantId(nRow, nCol);
    char res = 1;
    if (!ChessBoard::tryMoveStone(nCheckedID, killid, nRow, nCol, isRed)) {
        res = -1;
        errCnt[isRed]++;
        updateViolationDisplay();
        QString message;
        detectGameResult(false, message);
        // 返回1，执行成功，返回-1，执行失败
        fromTcpSocket->write(&res, 1);
        fromTcpSocket->flush();
        sendGameOverPacket();
        presentLastGameResult();
        return;
    }
    fromTcpSocket->write(&res, 1);
    if (toTcpSocket) {
        toTcpSocket->write(packet, 3);
        toTcpSocket->flush();
    }
    fromTcpSocket->flush();
    sendGameOverPacket();
    presentLastGameResult();
}

void NetworkGame::back()
{
    updateConnectionStatus(QStringLiteral("当前网络对战协议暂不支持悔棋"));
}

NetworkGame::Role NetworkGame::roleFromCode(quint8 code)
{
    switch (code) {
    case 1:
        return Role::Red;
    case 2:
        return Role::Black;
    default:
        return Role::None;
    }
}

quint8 NetworkGame::roleToCode(Role role)
{
    return static_cast<quint8>(role);
}

QString NetworkGame::roleLabel(Role role)
{
    switch (role) {
    case Role::Red:
        return QStringLiteral("red");
    case Role::Black:
        return QStringLiteral("black");
    default:
        return QStringLiteral("none");
    }
}

bool NetworkGame::localRoleIsRed() const
{
    return m_localRole == Role::Red;
}

bool NetworkGame::remoteRoleIsRed() const
{
    return m_localRole == Role::Black;
}

bool NetworkGame::bothRolesReady() const
{
    return redTcpSocket && blackTcpSocket;
}

int NetworkGame::findOccupantId(int row, int col) const
{
    for (int i = 0; i < 32; ++i) {
        if (!m_ChessPieces[i].m_bDead && m_ChessPieces[i].m_nRow == row && m_ChessPieces[i].m_nCol == col)
            return i;
    }
    return -1;
}

void NetworkGame::updateConnectionStatus(const QString& overrideText)
{
    if (!ui || !ui->labConnectStatus)
        return;

    if (!overrideText.isEmpty()) {
        ui->labConnectStatus->setText(overrideText);
        return;
    }

    QString text;
    if (m_isServerHost) {
        if (redTcpSocket && blackTcpSocket) {
            text = QStringLiteral("角色握手完成：red 与 black 已就绪");
        } else if (redTcpSocket) {
            text = QStringLiteral("red 已声明角色，等待 black 加入");
        } else if (blackTcpSocket) {
            text = QStringLiteral("black 已声明角色，等待 red 加入");
        } else if (m_tcpServer && m_tcpServer->isListening()) {
            text = QStringLiteral("服务器已监听，等待客户端连接并声明角色");
        } else {
            text = QStringLiteral("服务器未监听");
        }
    } else {
        if (!m_tcpSocket || m_tcpSocket->state() != QAbstractSocket::ConnectedState) {
            text = QStringLiteral("客户端未连接到服务器");
        } else if (!m_clientHandshakeDone) {
            text = QStringLiteral("已连接服务器，已声明本端角色 %1，等待另一方角色加入").arg(roleLabel(m_localRole));
        } else if (m_waitingForAck) {
            text = QStringLiteral("角色握手完成：本端 %1，等待服务器确认当前走子").arg(roleLabel(m_localRole));
        } else {
            text = QStringLiteral("角色握手完成：本端 %1").arg(roleLabel(m_localRole));
        }
    }

    ui->labConnectStatus->setText(text);
}

void NetworkGame::sendRoleStatus(QTcpSocket* socket, quint8 status)
{
    if (!socket)
        return;

    const char packet[2] = {
        static_cast<char>(kRolePacketType),
        static_cast<char>(status),
    };
    socket->write(packet, 2);
    socket->flush();
}

void NetworkGame::disconnectSocketWithRoleStatus(QTcpSocket* socket, quint8 status, const QString& text)
{
    if (!socket)
        return;

    sendRoleStatus(socket, status);
    updateConnectionStatus(text);
    socket->disconnectFromHost();
}

void NetworkGame::finalizeHandshakeIfReady()
{
    if (!bothRolesReady())
        return;

    sendRoleStatus(redTcpSocket, roleToCode(Role::Red));
    sendRoleStatus(blackTcpSocket, roleToCode(Role::Black));
    updateConnectionStatus(QStringLiteral("角色握手完成：red 与 black 已就绪，可以开始对局"));
}

void NetworkGame::handleSocketDisconnected(QTcpSocket* socket)
{
    if (!socket)
        return;

    m_serverBuffers.remove(socket);
    const Role role = m_serverRoles.take(socket);
    if (role == Role::Red && redTcpSocket == socket)
        redTcpSocket = nullptr;
    if (role == Role::Black && blackTcpSocket == socket)
        blackTcpSocket = nullptr;

    updateConnectionStatus(role == Role::None
                               ? QStringLiteral("有客户端断开连接")
                               : QStringLiteral("%1 已断开连接").arg(roleLabel(role)));
    socket->deleteLater();
}

void NetworkGame::processClientBuffer()
{
    while (true) {
        if (!m_clientHandshakeDone) {
            if (m_clientBuffer.size() < 2)
                return;

            if (static_cast<quint8>(m_clientBuffer[0]) != kRolePacketType) {
                updateConnectionStatus(QStringLiteral("等待角色握手应答时收到非法数据"));
                if (m_tcpSocket)
                    m_tcpSocket->disconnectFromHost();
                return;
            }

            const quint8 status = static_cast<quint8>(m_clientBuffer[1]);
            m_clientBuffer.remove(0, 2);
            if (status == roleToCode(m_localRole)) {
                m_clientHandshakeDone = true;
                updateConnectionStatus(roleStatusText(status));
                continue;
            }

            updateConnectionStatus(roleStatusText(status));
            if (m_tcpSocket)
                m_tcpSocket->disconnectFromHost();
            return;
        }

        if (m_expectOptionalGameOver) {
            if (m_clientBuffer.size() >= 2 && static_cast<quint8>(m_clientBuffer[0]) == kGameOverPacketType) {
                const quint8 code = static_cast<quint8>(m_clientBuffer[1]);
                m_clientBuffer.remove(0, 2);
                m_expectOptionalGameOver = false;
                handleRemoteGameOver(code);
                continue;
            }
            if (m_clientBuffer.isEmpty())
                return;

            m_expectOptionalGameOver = false;
            continue;
        }

        if (m_waitingForAck) {
            if (m_clientBuffer.size() < 1)
                return;

            const qint8 ack = static_cast<qint8>(m_clientBuffer[0]);
            m_clientBuffer.remove(0, 1);
            if (ack != 1 && ack != -1) {
                updateConnectionStatus(QStringLiteral("收到非法 ACK：%1，可能协议状态错位").arg(ack));
                if (m_tcpSocket)
                    m_tcpSocket->disconnectFromHost();
                return;
            }

            m_waitingForAck = false;
            if (ack == -1) {
                backOne();
                m_nSelectID = -1;
                update();
                updateConnectionStatus(QStringLiteral("服务器拒绝本步，已回滚本地走子"));
            } else {
                updateConnectionStatus(QStringLiteral("服务器确认本步"));
            }
            m_expectOptionalGameOver = true;
            continue;
        }

        if (m_clientBuffer.size() >= 2 && static_cast<quint8>(m_clientBuffer[0]) == kGameOverPacketType) {
            const quint8 code = static_cast<quint8>(m_clientBuffer[1]);
            m_clientBuffer.remove(0, 2);
            handleRemoteGameOver(code);
            continue;
        }

        if (m_clientBuffer.size() < 3)
            return;

        const QByteArray packet = m_clientBuffer.left(3);
        m_clientBuffer.remove(0, 3);
        const int id = static_cast<quint8>(packet[0]);
        const int row = static_cast<quint8>(packet[1]);
        const int col = static_cast<quint8>(packet[2]);
        const int killid = findOccupantId(row, col);
        if (!ChessBoard::tryMoveStone(id, killid, row, col, remoteRoleIsRed())) {
            updateConnectionStatus(QStringLiteral("收到非法同步步，连接已断开"));
            if (m_tcpSocket)
                m_tcpSocket->disconnectFromHost();
            return;
        }

        update();
        updateConnectionStatus(QStringLiteral("已同步对手走子：%1 %2 %3").arg(id).arg(row).arg(col));
        m_expectOptionalGameOver = true;
    }
}

void NetworkGame::handleRemoteGameOver(quint8 code)
{
    QString message;
    switch (code) {
    case 1:
        m_lastGameResult = GameResult::RedWin;
        message = QStringLiteral("本局结束，红方胜利.");
        break;
    case 2:
        m_lastGameResult = GameResult::BlackWin;
        message = QStringLiteral("本局结束，黑方胜利.");
        break;
    case 3:
        m_lastGameResult = GameResult::Draw;
        message = QStringLiteral("本局结束，双方和棋.");
        break;
    default:
        updateConnectionStatus(QStringLiteral("收到未知终局结果码：%1").arg(code));
        return;
    }

    m_lastGameMessage = message;
    updateConnectionStatus(QStringLiteral("收到终局通知：%1").arg(message));
    presentLastGameResult();
}

void NetworkGame::onBtnTryConnect()
{
    auto& ui = ChessBoard::ui;
    QString text;
    const QString ipText = currentIpText();
    const QString portText = ui->sbPort->text();
    if ((m_isServerHost && ipText.isEmpty()) || portText.isEmpty()) {
        text = "IP或Port为空，请设置后重试";
        qDebug() << text;
        ui->labConnectStatus->setText(text);
        return;
    }

    if (m_isServerHost) {
        if (!m_tcpServer) return;
        if (m_tcpServer->isListening()) {
            qDebug() << "Stopping server...";
            m_tcpServer->close();
        }

        QHostAddress bindAddress;
        if (!bindAddress.setAddress(ipText)) {
            text = QString("Invalid server IP: %1").arg(ipText);
            ui->labConnectStatus->setText(text);
            qDebug() << text;
            return;
        }

        const quint16 portValue = static_cast<quint16>(ui->sbPort->value());
        if (m_tcpServer->listen(bindAddress, portValue)) {
            text = QString("Server is listening on \"%1\" port \"%2\" and waiting for red/black role declarations")
                       .arg(bindAddress.toString())
                       .arg(portValue);
            redTcpSocket = nullptr;
            blackTcpSocket = nullptr;
            m_serverBuffers.clear();
            m_serverRoles.clear();
        } else {
            text = QString("Server failed to start: %1").arg(m_tcpServer->errorString());
        }

    } else {
        if (!m_tcpSocket) return;
        const QString& ip = ipText;
        const QString& port = ui->sbPort->text();
        m_clientHandshakeDone = false;
        m_waitingForAck = false;
        m_expectOptionalGameOver = false;
        m_clientBuffer.clear();
        m_tcpSocket->connectToHost(QHostAddress(ip), port.toInt());
        if (m_tcpSocket->waitForConnected()) {
            const char rolePacket[2] = {
                static_cast<char>(kRolePacketType),
                static_cast<char>(roleToCode(m_localRole)),
            };
            m_tcpSocket->write(rolePacket, 2);
            m_tcpSocket->flush();
            text = QString("Server connection successful: %1:%2, role declared as %3, waiting for peer")
                       .arg(ip)
                       .arg(port)
                       .arg(roleLabel(m_localRole));
        } else {
            text = "Server connection failed: " + m_tcpSocket->errorString();
        }
    }

    qDebug() << text;
    ui->labConnectStatus->setText(text);

}

void NetworkGame::handleServerEndpointChange()
{
    if (m_isServerHost) {
        onBtnTryConnect();
    }
}

void NetworkGame::populateLocalIpChoices(const QStringList& candidates, const QString& preferredIp)
{
    auto combo = ChessBoard::ui->comboIp;
    if (!combo)
        return;

    const QString targetIp = preferredIp.trimmed();

    combo->blockSignals(true);
    combo->clear();
    for (const QString& ip : candidates) {
        combo->addItem(ip, ip);
    }
    if (!targetIp.isEmpty() && combo->findData(targetIp) == -1) {
        combo->addItem(targetIp, targetIp);
    }

    if (!targetIp.isEmpty()) {
        const int index = combo->findData(targetIp);
        if (index >= 0) {
            combo->setCurrentIndex(index);
        } else {
            combo->setEditText(targetIp);
        }
    } else if (combo->count() > 0) {
        combo->setCurrentIndex(0);
    } else {
        combo->setEditText(QStringLiteral("127.0.0.1"));
    }

    combo->blockSignals(false);
}

QString NetworkGame::currentIpText() const
{
    const auto combo = ChessBoard::ui->comboIp;
    if (!combo)
        return {};

    QString ip = combo->currentData().toString();
    if (ip.isEmpty())
        ip = combo->currentText();
    return ip.trimmed();
}

bool NetworkGame::loadTestPreset(quint8 presetId)
{
    const TestPreset* preset = findTestPreset(presetId);
    if (!preset)
        return false;

    init();
    for (int i = 0; i < 32; ++i) {
        m_ChessPieces[i].m_bDead = true;
    }

    for (const TestPiecePlacement& piece : preset->pieces) {
        if (piece.id < 0 || piece.id >= 32)
            return false;
        m_ChessPieces[piece.id].m_nRow = piece.row;
        m_ChessPieces[piece.id].m_nCol = piece.col;
        m_ChessPieces[piece.id].m_bDead = false;
    }

    m_bIsRed = preset->redFirst;
    textStepRecord.clear();
    drawTextStep();
    clearLastGameResult();
    resetRuleTracking();
    updateViolationDisplay();
    update();
    return true;
}

void NetworkGame::sendGameOverPacket()
{
    if (lastGameResult() == GameResult::None)
        return;

    const char packet[2] = {
        static_cast<char>(kGameOverPacketType),
        static_cast<char>(lastGameResult()),
    };

    if (redTcpSocket) {
        redTcpSocket->write(packet, 2);
        redTcpSocket->flush();
    }
    if (blackTcpSocket) {
        blackTcpSocket->write(packet, 2);
        blackTcpSocket->flush();
    }
}
