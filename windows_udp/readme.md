# 项目介绍
BroadcastReceiver 广播接收方
BroadcastSender 广播发送方
BroadcastReceiver2 广播的回包，广播接收方
BroadcastSender2 广播的回包，广播发送方

# 什么是“广播的回包”
“广播的回包”是指：一方先发 UDP 广播去找人，收到广播的一方再给发送方回一个应答包。现在这两个程序BroadcastSender和BroadcastReceiver只做了前半段，发送方发出去就关套接字，接收方只打印、不回答。

## 现在缺的是什么

`BroadcastSender` 把报文发到 `255.255.255.255:9000`，然后立刻 `closesocket`。`BroadcastReceiver` 在 9000 上 `recvfrom`，已经拿到了发送方的地址和端口，但没有用这个地址回包。

UDP 广播本身没有“连接”。回包靠的是 `recvfrom` 填出来的对方地址：接收方用同一个套接字 `sendto` 回去，发送方在原来的套接字上继续 `recvfrom`。

```
发送方                          局域网里每个接收方
  |  广播: "谁在？"  ----------------->  收到
  |  <-----------------  单播回包: "我是设备 A，IP=..."
  |  <-----------------  单播回包: "我是设备 B，IP=..."
```

回包一般是单播，发给广播发送方的 IP 和源端口，不是再广播一次。多台机器都会回，所以发送方要循环收，直到超时。

## 接收方怎么回

`recvfrom` 已经把对方地址放进 `senderAddr`（IP 和源端口都在里面）。收到请求后直接回这个地址：

```cpp
const std::string reply = "I am here";
int sent = sendto(
    sock,
    reply.c_str(),
    static_cast<int>(reply.size()),
    0,
    reinterpret_cast<const sockaddr*>(&senderAddr),
    senderLen);
```

回包不需要 `SO_BROADCAST`。源端口会是接收方绑定的 9000，目的地址就是刚才那个广播发送方。

## 发送方怎么收

套接字在第一次 `sendto` 之后，系统会分配一个临时本地端口。接收方看到的就是这个端口，回包也会回到这个端口。所以必须用发广播的那个套接字来收，不能发完就关。

```cpp
DWORD timeoutMs = 2000;
setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
    reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

sendto(...);  // 现有广播

while (true) {
    sockaddr_in from{};
    int fromLen = sizeof(from);
    int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
        reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (len == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAETIMEDOUT)
            break;   // 2 秒内没有新回包，收集结束
        // 其它错误再处理
        break;
    }
    // 打印来自谁、回了什么
}
```

`WSAETIMEDOUT` 在这里是正常结束，不是失败。一台机器回一次，多台就多收几次。

## 实际做的时候注意这几件事

1. **请求和应答要能区分。** 例如请求以 `DISCOVER` 开头，应答以 `HERE` 开头。否则有人把应答也广播出去时，接收方会把应答再当成请求，来回对打。
2. **同一台电脑上互测会收到自己的广播。** 发送方如果也绑了 9000，就会收到自己发出的包。可以比对源 IP，或者只对 `DISCOVER` 回包。
3. **防火墙要放行 UDP 9000 入站。** 广播发出去成功，不代表回包能进来。
4. **广播过不了路由器。** `255.255.255.255` 只在当前子网有效。公司若指定了定向广播（例如 `192.168.1.255`）或固定回包端口，以协议文档为准；没写的话，就回 `recvfrom` 给出的那个地址和端口。


# 测试方法
约定很简单：发送方把 DISCOVER 广播到 255.255.255.255:9000，接收方只对这个前缀应答，用 recvfrom 得到的地址单播回 HERE 计算机名。回包回到发送方 sendto 时系统分配的临时端口，所以发送方发完不能关套接字，而是在同一套接字上收，2 秒内没有新包就结束。

先启动 x64\Debug\BroadcastReceiver2.exe，再启动 x64\Debug\BroadcastSender2.exe。不要同时运行原来的 BroadcastReceiver，它也占用 9000 端口。