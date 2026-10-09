#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <iostream>
#include <string>

#pragma comment(lib, "Ws2_32.lib")

// 与 BroadcastSender3 约定的端口。本程序绑定它来收发现广播和后续 DATA。
constexpr unsigned short kPort = 9001;

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

// DISCOVER <session>
bool parseDiscover(const char* buf, int len, unsigned& session)
{
    const char prefix[] = "DISCOVER ";
    const int prefixLen = static_cast<int>(sizeof(prefix) - 1);
    if (!startsWith(buf, len, prefix)) {
        return false;
    }

    int index = prefixLen;
    return readUint(buf, len, index, session) && index == len;
}

// DATA <session> <seq> <payload>
bool parseData(
    const char* buf,
    int len,
    unsigned& session,
    unsigned& seq,
    std::string& payload)
{
    const char prefix[] = "DATA ";
    const int prefixLen = static_cast<int>(sizeof(prefix) - 1);
    if (!startsWith(buf, len, prefix)) {
        return false;
    }

    int index = prefixLen;
    if (!readUint(buf, len, index, session) || !readSpace(buf, len, index)) {
        return false;
    }

    if (!readUint(buf, len, index, seq) || !readSpace(buf, len, index)) {
        return false;
    }

    payload.assign(buf + index, buf + len);
    return true;
}

std::string endpointText(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN]{};
    InetNtopA(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

bool sendText(SOCKET sock, const std::string& text, const sockaddr_in& dest)
{
    const int sent = sendto(
        sock,
        text.c_str(),
        static_cast<int>(text.size()),
        0,
        reinterpret_cast<const sockaddr*>(&dest),
        sizeof(dest));

    if (sent == SOCKET_ERROR) {
        std::cerr << "sendto failed: " << WSAGetLastError() << '\n';
        return false;
    }

    std::cout << "Sent " << text << " -> " << endpointText(dest)
        << ", bytes = " << sent << '\n';
    return true;
}

}  // namespace

int main(int argc, char* argv[])
{
    std::cout << std::unitbuf;

    // 健康网络上 ACK 往往第一次就到，重试分支不容易被看到。
    // --drop-first：每个新序号的第一份 DATA 直接丢掉，不交付、不回 ACK。
    // --drop-ack：第一份照常交付，但故意不回 ACK。重传会走“重复包只补 ACK”的分支。
    bool dropFirstCopy = false;
    bool dropAck = false;
    if (argc >= 2 && std::string(argv[1]) == "--drop-first") {
        dropFirstCopy = true;
    } else if (argc >= 2 && std::string(argv[1]) == "--drop-ack") {
        dropAck = true;
    }

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

    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(kPort);
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(
        sock,
        reinterpret_cast<sockaddr*>(&localAddr),
        sizeof(localAddr)) == SOCKET_ERROR) {
        std::cerr << "bind failed: " << WSAGetLastError() << '\n';
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    char computerName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD nameLen = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameA(computerName, &nameLen)) {
        strcpy_s(computerName, "unknown");
    }

    std::cout << "Waiting on UDP " << kPort << '\n'
        << "Computer name: " << computerName << '\n'
        << "Drop first DATA: " << (dropFirstCopy ? "on" : "off")
        << ", drop first ACK: " << (dropAck ? "on" : "off") << '\n';
    std::cout.flush();

    // session 0 表示还没接受过发现请求。DATA 在这之前直接忽略。
    unsigned session = 0;
    unsigned nextSeq = 1;
    // 模拟丢包只影响每个序号的第一份，否则重传也会被丢掉，发送方永远等不到 ACK。
    unsigned droppedSeq = 0;
    unsigned suppressedAckSeq = 0;

    char buffer[1500];

    while (true) {
        sockaddr_in senderAddr{};
        int senderLen = sizeof(senderAddr);

        const int len = recvfrom(
            sock,
            buffer,
            static_cast<int>(sizeof(buffer) - 1),
            0,
            reinterpret_cast<sockaddr*>(&senderAddr),
            &senderLen);

        if (len == SOCKET_ERROR) {
            std::cerr << "recvfrom failed: " << WSAGetLastError() << '\n';
            break;
        }

        buffer[len] = '\0';
        std::cout << "\nReceived: " << buffer
            << "\nFrom: " << endpointText(senderAddr) << '\n';

        unsigned discoverSession = 0;
        if (parseDiscover(buffer, len, discoverSession)) {
            // 新会话才重置序号。同一会话的 DISCOVER 重发不能把已经交付的进度清掉。
            if (discoverSession != session) {
                session = discoverSession;
                nextSeq = 1;
                droppedSeq = 0;
                suppressedAckSeq = 0;
                std::cout << "New session " << session
                    << ", next seq = 1\n";
            }

            const std::string reply =
                "HERE " + std::to_string(session) + " " + computerName;
            sendText(sock, reply, senderAddr);
            continue;
        }

        unsigned dataSession = 0;
        unsigned seq = 0;
        std::string payload;
        if (!parseData(buffer, len, dataSession, seq, payload)) {
            std::cout << "Not DISCOVER or DATA, ignore\n";
            continue;
        }

        if (session == 0 || dataSession != session) {
            std::cout << "DATA session " << dataSession
                << " does not match current session " << session
                << ", ignore\n";
            continue;
        }

        if (dropFirstCopy && seq == nextSeq && droppedSeq != seq) {
            droppedSeq = seq;
            std::cout << "[drop] seq=" << seq
                << " first copy discarded, no ACK\n";
            continue;
        }

        if (seq == nextSeq) {
            // 只在第一次见到这个序号时交付。打印代表“交给上层”。
            std::cout << "Deliver seq=" << seq
                << " payload=" << payload << '\n';
            ++nextSeq;

            if (dropAck && suppressedAckSeq != seq) {
                suppressedAckSeq = seq;
                std::cout << "[drop-ack] seq=" << seq
                    << " delivered, ACK suppressed\n";
                continue;
            }
        } else if (seq < nextSeq) {
            // ACK 丢了时发送方会重传。包其实已经交付过，这里只补 ACK。
            std::cout << "Duplicate seq=" << seq
                << ", resend ACK, do not deliver again\n";
        } else {
            // 停等协议下发送方不会跳号。更大的序号先不 ACK，避免把空洞当成已收到。
            std::cout << "Future seq=" << seq
                << ", expected " << nextSeq << ", no ACK\n";
            continue;
        }

        const std::string ack =
            "ACK " + std::to_string(session) + " " + std::to_string(seq);
        sendText(sock, ack, senderAddr);
        std::cout.flush();
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
