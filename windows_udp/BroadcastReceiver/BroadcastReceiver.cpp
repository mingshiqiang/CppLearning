#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>

#pragma comment(lib, "Ws2_32.lib")

int main()
{
    // 1. 初始化 Winsock
    WSADATA wsaData{};
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);

    if (ret != 0) {
        std::cerr << "WSAStartup failed: " << ret << '\n';
        return 1;
    }

    // 2. 创建 UDP 套接字
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock == INVALID_SOCKET) {
        std::cerr << "socket failed: "
            << WSAGetLastError() << '\n';
        WSACleanup();
        return 1;
    }

    // 3. 绑定本地 9000 端口
    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(9000);
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock,
        reinterpret_cast<sockaddr*>(&localAddr),
        sizeof(localAddr)) == SOCKET_ERROR) {
        std::cerr << "bind failed: "
            << WSAGetLastError() << '\n';
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::cout << "Waiting for UDP broadcast on port 9000...\n";

    // 4. 持续接收数据
    char buffer[1500];

    while (true) {
        sockaddr_in senderAddr{};
        int senderLen = sizeof(senderAddr);

        int len = recvfrom(
            sock,
            buffer,
            sizeof(buffer) - 1,
            0,
            reinterpret_cast<sockaddr*>(&senderAddr),
            &senderLen
        );

        if (len == SOCKET_ERROR) {
            std::cerr << "recvfrom failed: "
                << WSAGetLastError() << '\n';
            break;
        }

        buffer[len] = '\0';

        char senderIp[INET_ADDRSTRLEN]{};
        InetNtopA(AF_INET,
            &senderAddr.sin_addr,
            senderIp,
            sizeof(senderIp));

        std::cout << "\nReceived message: " << buffer
            << "\nSender IP: " << senderIp
            << "\nSender port: "
            << ntohs(senderAddr.sin_port)
            << "\n";
    }

    // 5. 清理资源
    closesocket(sock);
    WSACleanup();

    return 0;
}