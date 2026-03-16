#include <winsock2.h>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

int receive(SOCKET sock) {
    // 1 接收服务器数据（阻塞）
    char buf[3];
    int len = recv(sock, buf, 3, 0);

    if(len <= 0)
    {
        cout << "server disconnected\n";
        return -1;
    }

    int id  = (unsigned char)buf[0];
    int row = (unsigned char)buf[1];
    int col = (unsigned char)buf[2];

    cout << "recv: "
            << id << " "
            << row << " "
            << col << endl;
    return id;
}

void senddata(SOCKET sock) {
    // 2 输入棋步
    int sid, srow, scol;
    do {
        cout << "input move: ";
        cin >> sid >> srow >> scol;

        char sendbuf[3];
        sendbuf[0] = sid;
        sendbuf[1] = srow;
        sendbuf[2] = scol;

        // 3 发送
        send(sock, sendbuf, 3, 0);
    } while(receive(sock)==255);
}

int main(int argc, char* argv[])
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(12345);
    server.sin_addr.s_addr = inet_addr("172.27.32.1");

    if(connect(sock, (sockaddr*)&server, sizeof(server)) < 0)
    {
        cout << "connect failed\n";
        return 0;
    }

    cout << "connected to server\n";

    while(true)
    {
        if(argv[1][0] == 'r') {
            senddata(sock);
            receive(sock);
        } else {
            receive(sock);
            senddata(sock);
        }

    }

    closesocket(sock);
    WSACleanup();

    return 0;
}