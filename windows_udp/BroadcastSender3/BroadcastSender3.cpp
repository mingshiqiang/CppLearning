#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <string>

#pragma comment(lib, "Ws2_32.lib")

// 与 BroadcastReceiver3 约定的端口。发现广播发到这个端口，
// 随后的 DATA / ACK 也走接收方绑定的这个端口。
constexpr unsigned short kPort = 9001;

// 发现：广播 DISCOVER，等 HERE。没人回答就整段重发。
constexpr int kDiscoverAttempts = 3;
constexpr DWORD kDiscoverWaitMs = 1000;

// 停等：一条 DATA 对应一个 ACK。超时未收到匹配 ACK 就重发同一条。
// 4 次 = 首次发送 + 3 次重试。同一时刻只有一条未确认数据。
constexpr int kMaxAttempts = 4;
constexpr DWORD kAckWaitMs = 500;

const char* kPayloads[] = {
    "reading-1",
    "reading-2",
    "reading-3",
};

enum class RecvResult {
    Packet,
    Timeout,
    Error
};

enum class WaitAck {
    Acked,
    TimedOut,
    Failed
};

namespace {

bool startsWith(const char* buf, int len, const char* prefix)
{
    const int prefixLen = static_cast<int>(std::char_traits<char>::length(prefix));
    return len >= prefixLen &&
        std::char_traits<char>::compare(buf, prefix, prefixLen) == 0;
}

bool readUint(const char* buf, int len, int& index, unsigned& value)
{
    if (index >= len || buf[index] < '0' || buf[index] > '9') {
        return false;
    }

    unsigned parsed = 0;
    while (index < len && buf[index] >= '0' && buf[index] <= '9') {
        parsed = parsed * 10u + static_cast<unsigned>(buf[index] - '0');
        ++index;
    }

    value = parsed;
    return true;
}

bool readSpace(const char* buf, int len, int& index)
{
    if (index >= len || buf[index] != ' ') {
        return false;
    }

    ++index;
    return true;
}

// HERE <session> <name>
bool parseHere(const char* buf, int len, unsigned& session, std::string& name)
{
    const char prefix[] = "HERE ";
    const int prefixLen = static_cast<int>(sizeof(prefix) - 1);
    if (!startsWith(buf, len, prefix)) {
        return false;
    }

    int index = prefixLen;
    if (!readUint(buf, len, index, session) || !readSpace(buf, len, index)) {
        return false;
    }

    name.assign(buf + index, buf + len);
    return true;
}

// ACK <session> <seq>
bool parseAck(const char* buf, int len, unsigned& session, unsigned& seq)
{
    const char prefix[] = "ACK ";
    const int prefixLen = static_cast<int>(sizeof(prefix) - 1);
    if (!startsWith(buf, len, prefix)) {
        return false;
    }

    int index = prefixLen;
    if (!readUint(buf, len, index, session) || !readSpace(buf, len, index)) {
        return false;
    }

    if (!readUint(buf, len, index, seq) || index != len) {
        return false;
    }

    return true;
}

bool samePeer(const sockaddr_in& left, const sockaddr_in& right)
{
    return left.sin_addr.s_addr == right.sin_addr.s_addr &&
        left.sin_port == right.sin_port;
}

std::string endpointText(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN]{};
    InetNtopA(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

// waitMs 为 0 时不要交给 SO_RCVTIMEO。Windows 上 0 表示一直阻塞。
RecvResult recvDatagram(
    SOCKET sock,
    DWORD waitMs,
    char* buffer,
    int bufferSize,
    int& outLen,
    sockaddr_in& from)
{
    outLen = 0;

    if (waitMs == 0) {
        return RecvResult::Timeout;
    }

    if (setsockopt(
        sock,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&waitMs),
        sizeof(waitMs)) == SOCKET_ERROR) {
        std::cerr << "setsockopt SO_RCVTIMEO failed: "
            << WSAGetLastError() << '\n';
        return RecvResult::Error;
    }

    int fromLen = sizeof(from);
    const int len = recvfrom(
        sock,
        buffer,
        bufferSize - 1,
        0,
        reinterpret_cast<sockaddr*>(&from),
        &fromLen);

    if (len == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) {
            return RecvResult::Timeout;
        }

        std::cerr << "recvfrom failed: " << err << '\n';
        return RecvResult::Error;
    }

    buffer[len] = '\0';
    outLen = len;
    return RecvResult::Packet;
}

bool discoverPeer(
    SOCKET sock,
    unsigned session,
    sockaddr_in& peer,
    std::string& peerName)
{
    sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(kPort);
    destAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const std::string message = "DISCOVER " + std::to_string(session);

    for (int attempt = 1; attempt <= kDiscoverAttempts; ++attempt) {
        const int sent = sendto(
            sock,
            message.c_str(),
            static_cast<int>(message.size()),
            0,
            reinterpret_cast<const sockaddr*>(&destAddr),
            sizeof(destAddr));

        if (sent == SOCKET_ERROR) {
            std::cerr << "Discover sendto failed: "
                << WSAGetLastError() << '\n';
            return false;
        }

        if (attempt == 1) {
            sockaddr_in localAddr{};
            int localLen = sizeof(localAddr);
            if (getsockname(
                sock,
                reinterpret_cast<sockaddr*>(&localAddr),
                &localLen) == 0) {
                std::cout << "Discover replies come back to local port "
                    << ntohs(localAddr.sin_port) << '\n';
            }
        }

        std::cout << "Discover attempt " << attempt << "/"
            << kDiscoverAttempts << ": " << message << '\n';
        std::cout.flush();

        // 截止时间固定在本次广播之后。中途收到无关数据报时继续等，
        // 不把 SO_RCVTIMEO 重新拉满，否则噪声包会把等待拖得很长。
        const ULONGLONG deadline = GetTickCount64() + kDiscoverWaitMs;
        char buffer[1500];

        while (true) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                break;
            }

            sockaddr_in from{};
            int len = 0;
            const RecvResult result = recvDatagram(
                sock,
                static_cast<DWORD>(deadline - now),
                buffer,
                static_cast<int>(sizeof(buffer)),
                len,
                from);

            if (result == RecvResult::Error) {
                return false;
            }

            if (result == RecvResult::Timeout) {
                break;
            }

            unsigned replySession = 0;
            std::string name;
            if (!parseHere(buffer, len, replySession, name) ||
                replySession != session) {
                std::cout << "Ignore during discover: " << buffer
                    << " from " << endpointText(from) << '\n';
                continue;
            }

            peer = from;
            peerName = name;
            std::cout << "Peer " << endpointText(peer)
                << " name=" << peerName << '\n';
            return true;
        }

        std::cout << "No HERE within " << kDiscoverWaitMs << " ms\n";
    }

    std::cerr << "Discover failed after " << kDiscoverAttempts
        << " attempts\n";
    return false;
}

WaitAck waitForAck(
    SOCKET sock,
    unsigned session,
    unsigned seq,
    const sockaddr_in& peer)
{
    const ULONGLONG deadline = GetTickCount64() + kAckWaitMs;
    char buffer[1500];

    while (true) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            return WaitAck::TimedOut;
        }

        sockaddr_in from{};
        int len = 0;
        const RecvResult result = recvDatagram(
            sock,
            static_cast<DWORD>(deadline - now),
            buffer,
            static_cast<int>(sizeof(buffer)),
            len,
            from);

        if (result == RecvResult::Error) {
            return WaitAck::Failed;
        }

        if (result == RecvResult::Timeout) {
            return WaitAck::TimedOut;
        }

        if (!samePeer(from, peer)) {
            std::cout << "Ignore packet from " << endpointText(from) << '\n';
            continue;
        }

        unsigned ackSession = 0;
        unsigned ackSeq = 0;
        if (!parseAck(buffer, len, ackSession, ackSeq)) {
            std::cout << "Ignore non-ACK: " << buffer << '\n';
            continue;
        }

        if (ackSession != session || ackSeq != seq) {
            std::cout << "Ignore ACK session=" << ackSession
                << " seq=" << ackSeq << '\n';
            continue;
        }

        return WaitAck::Acked;
    }
}

bool sendReliable(
    SOCKET sock,
    unsigned session,
    unsigned seq,
    const char* payload,
    const sockaddr_in& peer)
{
    const std::string message =
        "DATA " + std::to_string(session) + " " +
        std::to_string(seq) + " " + payload;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        std::cout << "Send seq=" << seq
            << " attempt " << attempt << "/" << kMaxAttempts
            << ": " << message << '\n';
        std::cout.flush();

        const int sent = sendto(
            sock,
            message.c_str(),
            static_cast<int>(message.size()),
            0,
            reinterpret_cast<const sockaddr*>(&peer),
            sizeof(peer));

        if (sent == SOCKET_ERROR) {
            std::cerr << "sendto failed: " << WSAGetLastError() << '\n';
            continue;
        }

        const WaitAck wait = waitForAck(sock, session, seq, peer);
        if (wait == WaitAck::Acked) {
            std::cout << "ACK seq=" << seq
                << " on attempt " << attempt << '\n';
            return true;
        }

        if (wait == WaitAck::Failed) {
            return false;
        }

        std::cout << "Timeout " << kAckWaitMs
            << " ms, no ACK for seq=" << seq << '\n';
    }

    std::cerr << "Give up seq=" << seq
        << " after " << kMaxAttempts << " attempts\n";
    return false;
}

}  // namespace

int main()
{
    std::cout << std::unitbuf;

    WSADATA wsaData{};
    const int startup = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startup != 0) {
        std::cerr << "WSAStartup failed: " << startup << '\n';
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "socket failed: " << WSAGetLastError() << '\n';
        WSACleanup();
        return 1;
    }

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

    // 同一次运行共用一个会话号。接收方见到新会话号就把序号期待重置为 1，
    // 因此发送方可以反复启动，而不会把新数据当成上一轮的重复包。
    unsigned session = static_cast<unsigned>(GetTickCount());
    if (session == 0) {
        session = 1;
    }

    std::cout << "Session " << session
        << "\nDiscover on UDP " << kPort
        << ", then stop-and-wait DATA/ACK"
        << "\nACK wait " << kAckWaitMs << " ms, up to "
        << kMaxAttempts << " sends per message\n";

    sockaddr_in peer{};
    std::string peerName;
    bool ok = discoverPeer(sock, session, peer, peerName);

    const size_t payloadCount = sizeof(kPayloads) / sizeof(kPayloads[0]);
    size_t delivered = 0;

    if (ok) {
        for (size_t i = 0; i < payloadCount; ++i) {
            const unsigned seq = static_cast<unsigned>(i + 1);
            if (!sendReliable(sock, session, seq, kPayloads[i], peer)) {
                ok = false;
                break;
            }
            ++delivered;
        }
    }

    std::cout << "Delivered " << delivered << "/" << payloadCount << '\n';

    closesocket(sock);
    WSACleanup();
    system("pause");
    return ok ? 0 : 1;
}
