#include <winsock2.h>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

namespace {
constexpr unsigned char kGameOverPacketType = 0xFD;

bool recvExact(SOCKET sock, char* buf, int len) {
    int received = 0;
    while (received < len) {
        int ret = recv(sock, buf + received, len - received, 0);
        if (ret <= 0) {
            return false;
        }
        received += ret;
    }
    return true;
}

bool receiveResult(SOCKET sock) {
    char ack = 0;
    if (!recvExact(sock, &ack, 1)) {
        std::cout << "server disconnected\n";
        return false;
    }

    int result = static_cast<signed char>(ack);
    std::cout << "ack: " << result << std::endl;
    return true;
}

bool receiveSync(SOCKET sock) {
    char buf[3] = {0};
    if (!recvExact(sock, buf, 3)) {
        std::cout << "server disconnected\n";
        return false;
    }

    int id = static_cast<unsigned char>(buf[0]);
    int row = static_cast<unsigned char>(buf[1]);
    int col = static_cast<unsigned char>(buf[2]);
    std::cout << "sync: " << id << " " << row << " " << col << std::endl;
    return true;
}

void receiveOptionalGameOver(SOCKET sock) {
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);

    char buf[2] = {0};
    int len = recv(sock, buf, 2, 0);
    if (len == 2 && static_cast<unsigned char>(buf[0]) == kGameOverPacketType) {
        int code = static_cast<unsigned char>(buf[1]);
        std::cout << "game over: " << code << std::endl;
    }

    nonBlocking = 0;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
}

void sendMove(SOCKET sock) {
    int sid = 0;
    int srow = 0;
    int scol = 0;
    std::cout << "input move: ";
    std::cin >> sid >> srow >> scol;

    char sendbuf[3];
    sendbuf[0] = static_cast<char>(sid);
    sendbuf[1] = static_cast<char>(srow);
    sendbuf[2] = static_cast<char>(scol);
    send(sock, sendbuf, 3, 0);
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc < 3) {
        std::cout << "usage: client <r|b> <server_ip>\n";
        return 0;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(12345);
    server.sin_addr.s_addr = inet_addr(argv[2]);

    if(connect(sock, (sockaddr*)&server, sizeof(server)) < 0)
    {
        std::cout << "connect failed\n";
        closesocket(sock);
        WSACleanup();
        return 0;
    }

    std::cout << "connected to server\n";

    bool myTurn = (argv[1][0] == 'r');
    while(true)
    {
        if (myTurn) {
            sendMove(sock);
            if (!receiveResult(sock)) {
                break;
            }
            receiveOptionalGameOver(sock);
        } else {
            if (!receiveSync(sock)) {
                break;
            }
            receiveOptionalGameOver(sock);
        }
        myTurn = !myTurn;
    }

    closesocket(sock);
    WSACleanup();

    return 0;
}
