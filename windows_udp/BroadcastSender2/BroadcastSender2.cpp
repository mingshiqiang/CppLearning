#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>

#pragma comment(lib, "Ws2_32.lib")

// 接收方监听的端口。回包不会发到这个端口，而是发回本程序 sendto 时使用的源端口。
constexpr unsigned short kDiscoverPort = 9000;

// 请求与应答用不同前缀，避免把应答再当成新的发现请求。
const char kDiscover[] = "DISCOVER";

// 发出广播后，等待回包的最长时间。每收到一包都会重新计时。
constexpr DWORD kReplyTimeoutMs = 2000;

int main()
{
    // 1. 初始化 Winsock
    WSADATA wsaData{};
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);

    if (ret != 0) {
        std::cerr << "WSAStartup failed: " << ret << '\n';
        return 1;
    }

    // 2. 创建 UDP 套接字。广播和接收回包用同一个套接字：
    //    第一次 sendto 之后，系统会给它分配一个临时本地端口，回包就回到这个端口。
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock == INVALID_SOCKET) {
        std::cerr << "socket failed: "
            << WSAGetLastError() << '\n';
        WSACleanup();
        return 1;
    }

    // 3. 允许向 255.255.255.255 发送。只影响发送广播，接收单播回包不需要它。
    BOOL enableBroadcast = TRUE;

    if (setsockopt(
        sock,
        SOL_SOCKET,
        SO_BROADCAST,
        reinterpret_cast<const char*>(&enableBroadcast),
        sizeof(enableBroadcast)) == SOCKET_ERROR) {
        std::cerr << "setsockopt SO_BROADCAST failed: "
            << WSAGetLastError() << '\n';
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 4. 设置接收超时。收不到回包时 recvfrom 返回 SOCKET_ERROR，
    //    WSAGetLastError() 为 WSAETIMEDOUT。这是正常结束，不是故障。
    if (setsockopt(
        sock,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&kReplyTimeoutMs),
        sizeof(kReplyTimeoutMs)) == SOCKET_ERROR) {
        std::cerr << "setsockopt SO_RCVTIMEO failed: "
            << WSAGetLastError() << '\n';
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 5. 发现请求发往本子网的受限广播地址。路由器不会转发这个地址。
    sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(kDiscoverPort);
    destAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const std::string message = kDiscover;

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
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 6. sendto 成功后套接字才真正绑定本地端口。接收方 recvfrom 看到的
    //    “发送方端口”就是这里的端口，单播回包也必须回到这个套接字。
    //    所以这里不能 closesocket，否则回包没有进程接收。
    sockaddr_in localAddr{};
    int localLen = sizeof(localAddr);

    if (getsockname(
        sock,
        reinterpret_cast<sockaddr*>(&localAddr),
        &localLen) == SOCKET_ERROR) {
        std::cerr << "getsockname failed: "
            << WSAGetLastError() << '\n';
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::cout << "Broadcast sent, bytes = " << sent
        << "\nWaiting for unicast replies on local port "
        << ntohs(localAddr.sin_port)
        << " (timeout " << kReplyTimeoutMs << " ms)...\n";

    // 7. 一台接收方回一包，多台就收多包。两秒内没有新包就结束。
    char buffer[1500];
    int replyCount = 0;

    while (true) {
        sockaddr_in fromAddr{};
        int fromLen = sizeof(fromAddr);

        int len = recvfrom(
            sock,
            buffer,
            sizeof(buffer) - 1,
            0,
            reinterpret_cast<sockaddr*>(&fromAddr),
            &fromLen
        );

        if (len == SOCKET_ERROR) {
            int err = WSAGetLastError();

            if (err == WSAETIMEDOUT) {
                break;
            }

            std::cerr << "recvfrom failed: " << err << '\n';
            closesocket(sock);
            WSACleanup();
            return 1;
        }

        buffer[len] = '\0';
        ++replyCount;

        char fromIp[INET_ADDRSTRLEN]{};
        InetNtopA(
            AF_INET,
            &fromAddr.sin_addr,
            fromIp,
            sizeof(fromIp));

        std::cout << "\nReply: " << buffer
            << "\nFrom: " << fromIp
            << ":" << ntohs(fromAddr.sin_port)
            << "\n";
    }

    std::cout << "\nDone. Reply count = " << replyCount << '\n';

    closesocket(sock);
    WSACleanup();

    system("pause");

    return 0;
}
