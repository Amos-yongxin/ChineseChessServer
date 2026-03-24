#include <winsock2.h>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace {
constexpr unsigned char kRolePacketType = 0xFB;
constexpr unsigned char kGameOverPacketType = 0xFD;
constexpr unsigned char kRoleStatusInvalid = 0xFF;
constexpr unsigned char kRoleStatusOccupied = 0xFE;
constexpr unsigned char kRoleStatusRepeated = 0xFD;
constexpr unsigned short kDefaultPort = 12345;
constexpr int kMaxViolations = 3;
constexpr long kGameOverWaitMs = 300;

enum class AckStatus {
    Success,
    Rejected,
    Disconnected,
    Invalid,
};

enum class OptionalPacketStatus {
    None,
    GameOver,
    Disconnected,
    Invalid,
};

enum class OpponentPacketStatus {
    Sync,
    GameOver,
    Disconnected,
    Invalid,
};

struct Move {
    int id = 0;
    int row = 0;
    int col = 0;
};

struct ClientConfig {
    bool isRed = false;
    std::string roleName;
    std::string serverIp;
    unsigned short port = kDefaultPort;
};

enum class RoleHandshakeStatus {
    Accepted,
    Occupied,
    Invalid,
    Repeated,
    Disconnected,
    UnexpectedPacket,
};

bool recvExact(SOCKET sock, char* buf, int len)
{
    int received = 0;
    while (received < len) {
        const int ret = recv(sock, buf + received, len - received, 0);
        if (ret <= 0) {
            return false;
        }
        received += ret;
    }
    return true;
}

bool sendExact(SOCKET sock, const char* buf, int len)
{
    int sent = 0;
    while (sent < len) {
        const int ret = send(sock, buf + sent, len - sent, 0);
        if (ret <= 0) {
            return false;
        }
        sent += ret;
    }
    return true;
}

void printUsage(const char* programName)
{
    std::cout
        << "Usage:\n"
        << "  " << programName << " <r|b> <server_ip> [port]\n"
        << "  " << programName << " --help\n\n"
        << "Arguments:\n"
        << "  r|b        Client role. Use 'r' for red and 'b' for black.\n"
        << "  server_ip  IPv4 address of the server.\n"
        << "  port       Optional TCP port. Default is 12345.\n\n"
        << "Notes:\n"
        << "  - The role is declared to the server via a handshake packet.\n"
        << "  - red and black must be agreed before the match and cannot conflict.\n"
        << "  - Input moves as: id row col\n"
        << "  - Type q or quit to exit.\n";
}

bool parsePort(const char* text, unsigned short& port)
{
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (!end || *end != '\0' || value < 1 || value > 65535) {
        return false;
    }

    port = static_cast<unsigned short>(value);
    return true;
}

bool parseArguments(int argc, char* argv[], ClientConfig& config)
{
    if (argc == 2) {
        const std::string arg = argv[1];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return false;
        }
    }

    if (argc < 3 || argc > 4) {
        printUsage(argv[0]);
        return false;
    }

    const std::string role = argv[1];
    if (role != "r" && role != "b") {
        std::cout << "invalid role: " << role << "\n";
        printUsage(argv[0]);
        return false;
    }

    config.isRed = (role == "r");
    config.roleName = config.isRed ? "red" : "black";
    config.serverIp = argv[2];

    if (argc == 4 && !parsePort(argv[3], config.port)) {
        std::cout << "invalid port: " << argv[3] << "\n";
        printUsage(argv[0]);
        return false;
    }

    return true;
}

AckStatus receiveResult(SOCKET sock)
{
    char ack = 0;
    if (!recvExact(sock, &ack, 1)) {
        std::cout << "server disconnected while waiting for ack\n";
        return AckStatus::Disconnected;
    }

    const int result = static_cast<signed char>(ack);
    std::cout << "ack: " << result << std::endl;
    if (result == 1) {
        return AckStatus::Success;
    }
    if (result == -1) {
        return AckStatus::Rejected;
    }

    std::cout << "unexpected ack value: " << result << "\n";
    return AckStatus::Invalid;
}

bool sendRoleDeclaration(SOCKET sock, bool isRed)
{
    const char packet[2] = {
        static_cast<char>(kRolePacketType),
        static_cast<char>(isRed ? 1 : 2),
    };
    return sendExact(sock, packet, 2);
}

RoleHandshakeStatus receiveRoleStatus(SOCKET sock, bool requestedRed)
{
    char packet[2] = {0};
    if (!recvExact(sock, packet, 2)) {
        std::cout << "server disconnected while waiting for role handshake\n";
        return RoleHandshakeStatus::Disconnected;
    }

    if (static_cast<unsigned char>(packet[0]) != kRolePacketType) {
        std::cout << "unexpected handshake packet type: " << static_cast<int>(static_cast<unsigned char>(packet[0])) << "\n";
        return RoleHandshakeStatus::UnexpectedPacket;
    }

    const unsigned char status = static_cast<unsigned char>(packet[1]);
    if ((requestedRed && status == 1) || (!requestedRed && status == 2)) {
        std::cout << "role handshake accepted: " << (requestedRed ? "red" : "black") << std::endl;
        return RoleHandshakeStatus::Accepted;
    }

    if (status == kRoleStatusOccupied) {
        std::cout << "role handshake failed: requested role is already occupied\n";
        return RoleHandshakeStatus::Occupied;
    }
    if (status == kRoleStatusInvalid) {
        std::cout << "role handshake failed: invalid role declaration\n";
        return RoleHandshakeStatus::Invalid;
    }
    if (status == kRoleStatusRepeated) {
        std::cout << "role handshake failed: repeated role declaration\n";
        return RoleHandshakeStatus::Repeated;
    }

    std::cout << "role handshake failed: unexpected status " << static_cast<int>(status) << "\n";
    return RoleHandshakeStatus::UnexpectedPacket;
}

bool receiveSync(SOCKET sock, Move& move)
{
    char buf[3] = {0};
    if (!recvExact(sock, buf, 3)) {
        std::cout << "server disconnected while waiting for sync\n";
        return false;
    }

    move.id = static_cast<unsigned char>(buf[0]);
    move.row = static_cast<unsigned char>(buf[1]);
    move.col = static_cast<unsigned char>(buf[2]);
    std::cout << "sync: " << move.id << " " << move.row << " " << move.col << std::endl;
    return true;
}

bool waitForReadable(SOCKET sock, long timeoutMs)
{
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);

    timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    const int ret = select(0, &readSet, nullptr, nullptr, &timeout);
    return ret > 0 && FD_ISSET(sock, &readSet);
}

const char* gameOverLabel(unsigned char code)
{
    switch (code) {
    case 1:
        return "red_win";
    case 2:
        return "black_win";
    case 3:
        return "draw";
    default:
        return "unknown";
    }
}

bool printGameOver(unsigned char code)
{
    if (code < 1 || code > 3) {
        std::cout << "unexpected game-over code: " << static_cast<int>(code) << "\n";
        return false;
    }

    std::cout << "game over: " << gameOverLabel(code)
              << " (" << static_cast<int>(code) << ")\n";
    return true;
}

OptionalPacketStatus receiveOptionalGameOver(SOCKET sock)
{
    if (!waitForReadable(sock, kGameOverWaitMs)) {
        return OptionalPacketStatus::None;
    }

    char buf[2] = {0};
    if (!recvExact(sock, buf, 2)) {
        std::cout << "server disconnected while waiting for game-over packet\n";
        return OptionalPacketStatus::Disconnected;
    }

    if (static_cast<unsigned char>(buf[0]) != kGameOverPacketType) {
        std::cout << "unexpected packet after move: ["
                  << static_cast<int>(static_cast<unsigned char>(buf[0])) << ", "
                  << static_cast<int>(static_cast<unsigned char>(buf[1])) << "]\n";
        return OptionalPacketStatus::Invalid;
    }

    const unsigned char code = static_cast<unsigned char>(buf[1]);
    return printGameOver(code) ? OptionalPacketStatus::GameOver : OptionalPacketStatus::Invalid;
}

OpponentPacketStatus receiveOpponentPacket(SOCKET sock, Move& move)
{
    char first = 0;
    if (!recvExact(sock, &first, 1)) {
        std::cout << "server disconnected while waiting for opponent packet\n";
        return OpponentPacketStatus::Disconnected;
    }

    const unsigned char packetType = static_cast<unsigned char>(first);
    if (packetType == kGameOverPacketType) {
        char code = 0;
        if (!recvExact(sock, &code, 1)) {
            std::cout << "server disconnected while waiting for game-over packet\n";
            return OpponentPacketStatus::Disconnected;
        }

        return printGameOver(static_cast<unsigned char>(code))
                   ? OpponentPacketStatus::GameOver
                   : OpponentPacketStatus::Invalid;
    }

    if (packetType > 31) {
        std::cout << "unexpected opponent packet type: " << static_cast<int>(packetType) << "\n";
        return OpponentPacketStatus::Invalid;
    }

    char tail[2] = {0};
    if (!recvExact(sock, tail, 2)) {
        std::cout << "server disconnected while waiting for sync tail\n";
        return OpponentPacketStatus::Disconnected;
    }

    move.id = packetType;
    move.row = static_cast<unsigned char>(tail[0]);
    move.col = static_cast<unsigned char>(tail[1]);
    std::cout << "sync: " << move.id << " " << move.row << " " << move.col << std::endl;
    return OpponentPacketStatus::Sync;
}

bool parseMoveLine(const std::string& line, Move& move)
{
    std::istringstream iss(line);
    int id = 0;
    int row = 0;
    int col = 0;
    std::string extra;

    if (!(iss >> id >> row >> col) || (iss >> extra)) {
        return false;
    }

    if (id < 0 || id > 31 || row < 0 || row > 9 || col < 0 || col > 8) {
        return false;
    }

    move.id = id;
    move.row = row;
    move.col = col;
    return true;
}

bool promptForMove(Move& move)
{
    while (true) {
        std::cout << "input move (id row col, q quit): ";

        std::string line;
        if (!std::getline(std::cin, line)) {
            return false;
        }

        if (line == "q" || line == "quit") {
            return false;
        }

        if (line.empty()) {
            std::cout << "empty input, please enter: id row col\n";
            continue;
        }

        if (!parseMoveLine(line, move)) {
            std::cout << "invalid input. expected: id row col, with id 0..31, row 0..9, col 0..8\n";
            continue;
        }

        return true;
    }
}

bool sendMove(SOCKET sock, const Move& move)
{
    const char sendbuf[3] = {
        static_cast<char>(move.id),
        static_cast<char>(move.row),
        static_cast<char>(move.col),
    };

    std::cout << "send: " << move.id << " " << move.row << " " << move.col << std::endl;
    if (!sendExact(sock, sendbuf, 3)) {
        std::cout << "failed to send move\n";
        return false;
    }

    return true;
}
} // namespace

int main(int argc, char* argv[])
{
    ClientConfig config;
    if (!parseArguments(argc, argv, config)) {
        return 0;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cout << "WSAStartup failed\n";
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cout << "socket creation failed\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(config.port);
    const unsigned long ip = inet_addr(config.serverIp.c_str());
    if (ip == INADDR_NONE && config.serverIp != "255.255.255.255") {
        std::cout << "invalid IPv4 address: " << config.serverIp << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    server.sin_addr.s_addr = ip;

    std::cout << "role: " << config.roleName << "\n"
              << "server: " << config.serverIp << ":" << config.port << "\n"
              << "note: red/black must be agreed before the match and declared to the server\n";

    if (connect(sock, reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0) {
        std::cout << "connect failed\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::cout << "connected to server\n";
    if (!sendRoleDeclaration(sock, config.isRed)) {
        std::cout << "failed to send role declaration\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    std::cout << "role declaration sent, waiting for handshake confirmation...\n";

    const RoleHandshakeStatus handshake = receiveRoleStatus(sock, config.isRed);
    if (handshake != RoleHandshakeStatus::Accepted) {
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    bool myTurn = config.isRed;
    int violations = 0;
    bool waitingNoticePrinted = false;

    while (true) {
        if (myTurn) {
            waitingNoticePrinted = false;
            std::cout << "your turn (" << config.roleName << ")\n";

            Move move;
            if (!promptForMove(move)) {
                std::cout << "client exit requested\n";
                break;
            }

            if (!sendMove(sock, move)) {
                break;
            }

            const AckStatus ack = receiveResult(sock);
            if (ack == AckStatus::Disconnected || ack == AckStatus::Invalid) {
                break;
            }

            const OptionalPacketStatus gameOver = receiveOptionalGameOver(sock);
            if (gameOver == OptionalPacketStatus::GameOver) {
                break;
            }
            if (gameOver == OptionalPacketStatus::Disconnected || gameOver == OptionalPacketStatus::Invalid) {
                break;
            }

            if (ack == AckStatus::Rejected) {
                ++violations;
                std::cout << "violations: " << violations << "/" << kMaxViolations << "\n";
                std::cout << "move rejected, still your turn\n";
                continue;
            }

            myTurn = false;
            continue;
        }

        if (!waitingNoticePrinted) {
            std::cout << "waiting for opponent move...\n";
            waitingNoticePrinted = true;
        }

        Move syncMove;
        const OpponentPacketStatus packetStatus = receiveOpponentPacket(sock, syncMove);
        if (packetStatus == OpponentPacketStatus::GameOver) {
            break;
        }
        if (packetStatus == OpponentPacketStatus::Disconnected || packetStatus == OpponentPacketStatus::Invalid) {
            break;
        }

        const OptionalPacketStatus gameOver = receiveOptionalGameOver(sock);
        if (gameOver == OptionalPacketStatus::GameOver) {
            break;
        }
        if (gameOver == OptionalPacketStatus::Disconnected || gameOver == OptionalPacketStatus::Invalid) {
            break;
        }

        myTurn = true;
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
