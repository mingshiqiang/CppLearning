
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>

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

    // 3. 允许套接字发送广播
    BOOL enableBroadcast = TRUE;

    if (setsockopt(
        sock,
        SOL_SOCKET,
        SO_BROADCAST,
        reinterpret_cast<const char*>(&enableBroadcast),
        sizeof(enableBroadcast)) == SOCKET_ERROR) {
        std::cerr << "setsockopt failed: "
            << WSAGetLastError() << '\n';
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 4. 设置目标广播地址和端口
    sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(9000);
    destAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    // 5. 发送广播消息
    const std::string message =
        "Hello! I am a broadcast message.";

    int sent = sendto(
        sock,
        message.c_str(),
        static_cast<int>(message.size()),
        0,
        reinterpret_cast<const sockaddr*>(&destAddr),
        sizeof(destAddr)
    );

    if (sent == SOCKET_ERROR) {
        std::cerr << "sendto failed: "
            << WSAGetLastError() << '\n';
    }
    else {
        std::cout << "Broadcast sent, bytes = "
            << sent << '\n';
    }

    // 6. 清理资源
    closesocket(sock);
    WSACleanup();

    system("pause");

    return 0;
}