#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>

#pragma comment(lib, "Ws2_32.lib")

// 与 BroadcastSender2 约定的发现端口。本程序绑定它来收广播。
constexpr unsigned short kDiscoverPort = 9000;

// 只对这个前缀回包。应答自己以 "HERE" 开头，不会再次触发回包。
const char kDiscover[] = "DISCOVER";

int main()
{
    // 1. 初始化 Winsock
    WSADATA wsaData{};
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);

    if (ret != 0) {
        std::cerr << "WSAStartup failed: " << ret << '\n';
        return 1;
    }

    // 2. 创建 UDP 套接字。收广播和发回包用同一个套接字即可。
    //    回包是发往对方单播地址，不需要 SO_BROADCAST。
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock == INVALID_SOCKET) {
        std::cerr << "socket failed: "
            << WSAGetLastError() << '\n';
        WSACleanup();
        return 1;
    }

    // 3. 绑定 0.0.0.0:9000，才能收到发往本机任意网卡、目的端口 9000 的广播。
    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(kDiscoverPort);
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(
        sock,
        reinterpret_cast<sockaddr*>(&localAddr),
        sizeof(localAddr)) == SOCKET_ERROR) {
        std::cerr << "bind failed: "
            << WSAGetLastError() << '\n';
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 回包正文里带上计算机名，方便发送方区分多台设备。
    char computerName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD nameLen = MAX_COMPUTERNAME_LENGTH + 1;

    if (!GetComputerNameA(computerName, &nameLen)) {
        strcpy_s(computerName, "unknown");
    }

    const std::string reply = std::string("HERE ") + computerName;

    std::cout << "Waiting for UDP broadcast on port "
        << kDiscoverPort << "...\n"
        << "Reply text: " << reply << "\n";

    // 4. 持续接收。每收到一次发现请求，就按 recvfrom 带回的地址单播应答。
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
        InetNtopA(
            AF_INET,
            &senderAddr.sin_addr,
            senderIp,
            sizeof(senderIp));

        std::cout << "\nReceived: " << buffer
            << "\nSender: " << senderIp
            << ":" << ntohs(senderAddr.sin_port)
            << "\n";

        // senderAddr 同时包含对方 IP 和对方本次 sendto 的源端口。
        // 直接 sendto 这个地址，应答就是单播，不会再广播到整个子网。
        // 源端口则是本套接字绑定的 9000。
        const bool isDiscover =
            len >= static_cast<int>(sizeof(kDiscover) - 1) &&
            std::char_traits<char>::compare(
                buffer,
                kDiscover,
                sizeof(kDiscover) - 1) == 0;

        if (!isDiscover) {
            std::cout << "Not a discover request, no reply.\n";
            continue;
        }

        int sent = sendto(
            sock,
            reply.c_str(),
            static_cast<int>(reply.size()),
            0,
            reinterpret_cast<const sockaddr*>(&senderAddr),
            senderLen
        );

        if (sent == SOCKET_ERROR) {
            std::cerr << "sendto reply failed: "
                << WSAGetLastError() << '\n';
            continue;
        }

        std::cout << "Unicast reply sent, bytes = " << sent << '\n';
    }

    closesocket(sock);
    WSACleanup();

    return 0;
}
