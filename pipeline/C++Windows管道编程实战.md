# C++ Windows 管道编程实战

## 一、管道是什么？

管道（Pipe）可以理解成一个特殊的"内存文件"：一端往里写，另一端往外读，数据先进先出（FIFO）。它是 Windows 上最经典的进程间通信（IPC）手段之一，读写操作和文件一样用 `ReadFile` / `WriteFile`，学习成本极低。

Windows 提供两类管道：

| | 命名管道 Named Pipe | 匿名管道 Anonymous Pipe |
|---|---|---|
| 有没有名字 | 有，`\\.\pipe\xxx` | 没有 |
| 谁能用 | 任意进程，甚至跨机器 | 只能父子进程（靠句柄继承） |
| 方向 | 双向（也可配置成单向） | 单向，双向要两根 |
| 数据模式 | 字节模式 / 消息模式可选 | 只有字节模式 |
| 创建 API | `CreateNamedPipe` + `ConnectNamedPipe` | `CreatePipe` |

这篇文章的主角是命名管道。

---

## 二、最小可用示例：一对一回声服务

先看最经典的入门例子：服务端创建管道等待连接，客户端连上后发消息，服务端收到后回复（回声）。为了中文控制台输出不乱码，示例里有两行 `_setmode(_fileno(...), _O_U16TEXT)` 设置，后面代码沿用不再解释；所有示例默认在 VS 的 UNICODE 配置下编译（新建项目的默认值）。

### 服务端（精简版）

```cpp
// server.cpp — 最简命名管道服务端
#include <windows.h>
#include <iostream>
#include <string>
#include <io.h>      // _setmode
#include <fcntl.h>   // _O_U16TEXT

const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\MyLearningPipe";

int main()
{
    _setmode(_fileno(stdout), _O_U16TEXT);   // 控制台中文设置, 见上文说明

    // 1. 创建管道: 双向 + 消息模式
    HANDLE hPipe = CreateNamedPipe(
        PIPE_NAME,
        PIPE_ACCESS_DUPLEX,                          // 双向读写
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE,   // 消息模式(见第五节)
        1,                                           // 最多 1 个客户端
        4096, 4096, 0, NULL);

    // 2. 阻塞等待客户端连接
    ConnectNamedPipe(hPipe, NULL);

    // 3. 收发循环: 读一条 -> 回一条, 直到收到 "quit"
    wchar_t buf[1024];
    DWORD n;
    while (ReadFile(hPipe, buf, sizeof(buf) - sizeof(wchar_t), &n, NULL) && n > 0)
    {
        buf[n / sizeof(wchar_t)] = L'\0';
        std::wstring msg(buf);
        std::wcout << L"[收到] " << msg << std::endl;
        if (msg == L"quit") break;

        std::wstring reply = L"服务端已收到: " + msg;
        WriteFile(hPipe, reply.c_str(),
                  (DWORD)((reply.size() + 1) * sizeof(wchar_t)), &n, NULL);
    }

    // 4. 收尾
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
}
```

### 客户端（精简版）

```cpp
// client.cpp — 最简命名管道客户端
#include <windows.h>
#include <iostream>
#include <string>
#include <io.h>      // _setmode
#include <fcntl.h>   // _O_U16TEXT

const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\MyLearningPipe";

int main()
{
    _setmode(_fileno(stdout), _O_U16TEXT);   // 控制台中文设置, 见上文说明
    _setmode(_fileno(stdin), _O_U16TEXT);

    // 1. 等服务端的管道可用(最多 5 秒), 否则 CreateFile 会直接失败
    if (!WaitNamedPipe(PIPE_NAME, 5000))
    {
        std::wcout << L"连接失败: 请先启动服务端 (错误码 " << GetLastError() << L")" << std::endl;
        return 1;
    }

    // 2. 打开管道 = 建立连接(和打开文件写法一模一样)
    HANDLE hPipe = CreateFile(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                              0, NULL, OPEN_EXISTING, 0, NULL);

    // 3. 客户端读模式也要改成消息模式, 和服务端保持一致
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

    // 4. 发一条 -> 收一条回复
    std::wstring line = L"你好, 管道";
    DWORD n;
    WriteFile(hPipe, line.c_str(), (DWORD)((line.size() + 1) * sizeof(wchar_t)), &n, NULL);

    wchar_t buf[1024];
    ReadFile(hPipe, buf, sizeof(buf) - sizeof(wchar_t), &n, NULL);
    buf[n / sizeof(wchar_t)] = L'\0';
    std::wcout << L"[回复] " << buf << std::endl;

    CloseHandle(hPipe);
}
```

### 三个最容易踩的坑

1. **管道名两端必须完全一致**，且服务端必须先启动（客户端的 `WaitNamedPipe` 就是干这个的）。
2. 消息模式下，客户端必须用 `SetNamedPipeHandleState` 把读模式设为 `PIPE_READMODE_MESSAGE`，否则默认按字节模式读，可能读到"半条消息"。
3. 服务端 `ConnectNamedPipe` 返回 FALSE 不一定是失败——如果客户端抢在调用之前连上了，错误码是 `ERROR_PIPE_CONNECTED`，照样算连接成功。

> **排错速查**：客户端 `CreateFile` 报错码 2（ERROR_FILE_NOT_FOUND）→ 服务端没启动或名字写错；报错码 231（ERROR_PIPE_BUSY）→ 管道实例全被占满，等一会重试。

---

## 三、深入一：循环接受多个客户端

上一节的服务器服务完一个客户端就退出了。真实场景（比如进度广播）里，服务器要能**一直接客**。关键就两行代码：客户端断开后调用 `DisconnectNamedPipe`，然后**用同一个句柄再次 `ConnectNamedPipe`**。

### 串行版：一个接一个地服务

```cpp
// MultiClientServer.cpp — 循环接受多个客户端(串行版)
#include <windows.h>
#include <iostream>
#include <string>
#include <io.h>      // _setmode
#include <fcntl.h>   // _O_U16TEXT

const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\MyLearningPipe";

int main()
{
    _setmode(_fileno(stdout), _O_U16TEXT);   // 控制台中文设置, 见上文说明

    // nMaxInstances 限制的是"同一时刻"最多有多少个客户端连着这个管道。
    // 串行版同一时刻只服务 1 个客户端, 填 1 也够用;
    // 这里先写成 PIPE_UNLIMITED_INSTANCES, 为后面的并发版做铺垫。
    HANDLE hPipe = CreateNamedPipe(
        PIPE_NAME,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        4096, 4096, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE)
        return 1;

    std::wcout << L"服务端启动, 循环等待客户端..." << std::endl;

    for (;;)
    {
        // ---- 接受下一个客户端 ----
        if (!ConnectNamedPipe(hPipe, NULL)
            && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            std::wcerr << L"连接出错: " << GetLastError() << std::endl;
            continue;
        }
        std::wcout << L"客户端已连接!" << std::endl;

        // ---- 服务当前客户端, 直到它断开 ----
        wchar_t buf[1024];
        DWORD n;
        for (;;)
        {
            BOOL ok = ReadFile(hPipe, buf, sizeof(buf) - sizeof(wchar_t), &n, NULL);
            if (!ok || n == 0)
                break;                          // 客户端断开连接

            buf[n / sizeof(wchar_t)] = L'\0';
            std::wstring msg(buf);
            std::wcout << L"[收到] " << msg << std::endl;

            if (msg == L"quit")
                break;

            std::wstring reply = L"服务端已收到: " + msg;
            WriteFile(hPipe, reply.c_str(),
                      (DWORD)((reply.size() + 1) * sizeof(wchar_t)), &n, NULL);
        }

        // ---- 关键: 断开当前客户端, 准备服务下一个 ----
        // DisconnectNamedPipe 不会销毁管道, 之后可以再次 ConnectNamedPipe
        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        std::wcout << L"客户端已断开, 继续等待下一个..." << std::endl;
    }

    CloseHandle(hPipe);   // 正常跑不到这里, 可自行加退出信号
}
```

串行版的局限很明显：服务 A 的时候，B 想连也连不上（要排队）。如果想让**多个客户端同时连着**（比如进度广播给多个 UI），需要多实例并发版。

### 并发版骨架：一个实例 + 一个线程服务一个客户端

```cpp
// 并发版骨架: 每接受一个客户端, 就立刻再创建一个实例等下一个
DWORD WINAPI ClientThread(LPVOID param)
{
    HANDLE hPipe = (HANDLE)param;
    // ... 收发消息, 和串行版内层循环一样 ...

    FlushFileBuffers(hPipe);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
    return 0;
}

int main()
{
    bool first = true;   // 第一个实例必须带 FILE_FLAG_FIRST_PIPE_INSTANCE
    for (;;)
    {
        HANDLE hPipe = CreateNamedPipe(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,     // 允许多个客户端同时在线
            4096, 4096, 0, NULL);
        first = false;
        if (hPipe == INVALID_HANDLE_VALUE) break;

        if (!ConnectNamedPipe(hPipe, NULL)
            && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            CloseHandle(hPipe);
            continue;
        }

        HANDLE hThread = CreateThread(NULL, 0, ClientThread, hPipe, 0, NULL);
        CloseHandle(hThread);   // 不等待线程结束, 立刻回去接下一个客户
    }
}
```

两个必须知道的细节：

1. **`FILE_FLAG_FIRST_PIPE_INSTANCE`**：同名管道创建多个实例时，第一个实例必须带这个标志；后续实例再带它反而会失败（ERROR_ACCESS_DENIED）。上面的 `first` 布尔量就是这个用途。
2. **`nMaxInstances`（即 `PIPE_UNLIMITED_INSTANCES` 那个参数）限制的是同名管道实例总数**——每个实例同一时刻服务一个客户端，所以它也就是"同时在线客户端数"的上限。串行版只有一个实例，填 1 足够；并发版按需填，`PIPE_UNLIMITED_INSTANCES` 表示不限（上限 255）。

第七节的进度广播服务器就是并发版的完整落地。

---

## 四、深入二：字节模式 vs 消息模式

这是命名管道最容易让人糊涂的地方。管道在创建时由服务端指定类型，客户端必须配合。

### 现象对比

```cpp
// ============ 消息模式 (PIPE_TYPE_MESSAGE) ============
// 客户端写两条消息:
WriteFile(hPipe, L"hello", 6 * 2, &n, NULL);   // 第 1 条消息
WriteFile(hPipe, L"world", 6 * 2, &n, NULL);   // 第 2 条消息

// 服务端读两次, 每次恰好读回一条完整消息:
ReadFile(hPipe, buf1, 1024, &n, NULL);   // -> "hello"
ReadFile(hPipe, buf2, 1024, &n, NULL);   // -> "world"

// ============ 字节模式 (PIPE_TYPE_BYTE) ============
// 同样的两次写, 服务端读一次的结果是"不确定"的:
ReadFile(hPipe, buf, 1024, &n, NULL);
// buf 可能是 "helloworld"(全读走了), 也可能是 "hello"(只读到一半),
// 甚至更少 —— 字节流没有消息边界, 一切都看缓冲区的心情
```

### 区别总结

| | 字节模式 `PIPE_TYPE_BYTE` | 消息模式 `PIPE_TYPE_MESSAGE` |
|---|---|---|
| 数据边界 | 无，连续字节流 | 有，每次 `WriteFile` 是一条消息 |
| 一次 `ReadFile` 读到 | 任意数量字节，可能跨消息 | 恰好一条完整消息 |
| 客户端读模式设置 | 默认就是字节模式，不用设 | 必须 `SetNamedPipeHandleState` |
| 大消息 | 天然支持，循环读即可 | 缓冲区不够时报 `ERROR_MORE_DATA`，剩余部分要接着读 |
| 适合场景 | **文件内容、大数据流** | **控制指令、状态/进度上报** |

### 怎么选

- **文件内容**：用字节模式。文件本来就是一串字节，没有"消息"概念，循环读满一块写一块，天然匹配。
- **指令/进度**：用消息模式。一条进度更新就是一条短消息，读一次得一条，不用自己拆包，省心。
- **字节模式传多段数据**：必须自带协议，最常见的是"4 字节长度头 + 内容"——先读满 4 字节知道长度，再按长度读内容，两次数据就不会"串台"（TCP 编程里也是同样的套路）。

> 顺带一提：匿名管道**只有字节模式**，没有消息模式。

---

## 五、深入三：匿名管道 CreatePipe

匿名管道没有名字，所以**别的进程找不到它**——只能靠"句柄继承"传给子进程。典型用法：父进程创建管道，把写端塞给子进程当标准输出，从而捕获子进程的输出（命令行里 `cmd1 | cmd2` 的竖线就是这个原理）。

```cpp
// 父进程: 用匿名管道抓取子进程的输出
#include <windows.h>
#include <iostream>

int main()
{
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };  // TRUE = 句柄可被子进程继承
    CreatePipe(&hRead, &hWrite, &sa, 0);                  // 匿名管道: 单向, 字节模式

    STARTUPINFO si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;   // 子进程往 stdout 写, 实际是写进管道
    si.hStdError  = hWrite;
    PROCESS_INFORMATION pi;
    CreateProcess(NULL, (LPWSTR)L"child.exe", NULL, NULL,
                  TRUE /* 允许继承句柄 */, 0, NULL, NULL, &si, &pi);

    CloseHandle(hWrite);      // 关键! 父进程必须关掉自己这份写端,
                              // 否则子进程退出后, 读端永远等不到"文件结尾"

    char buf[256];
    DWORD n;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &n, NULL) && n > 0)
    {
        buf[n] = 0;
        std::cout << buf;     // 打印子进程的输出
    }
    CloseHandle(hRead);
    WaitForSingleObject(pi.hProcess, INFINITE);
}
```

### 匿名 vs 命名

| | 匿名管道 | 命名管道 |
|---|---|---|
| 创建 | `CreatePipe`，一对句柄（读端/写端） | `CreateNamedPipe`，一个双向句柄 |
| 谁能连 | 只有继承到句柄的父子进程 | 任何知道名字的进程，甚至跨机器 |
| 方向 | 单向，双向要两根管道 | 双向 |
| 数据模式 | 只有字节模式 | 字节 / 消息可选 |
| 典型用途 | 重定向子进程 stdout、父传子数据 | 任意两个独立进程通信、进度广播 |

**选型直觉**：父子进程之间传数据、抓子进程输出 → 匿名管道；两个独立进程、多客户端、跨机器 → 命名管道。

---

## 六、不同设备之间能用管道通信吗？

比如手机和电脑之间能用管道通信吗。

**能，但有条件。**

| 通信双方                   | 能否直接用命名管道 | 说明                                         |
| -------------------------- | ------------------ | -------------------------------------------- |
| 同一台电脑的两个进程       | ✅ 可以             | `\\.\pipe\xxx`，管道最经典的用法             |
| 局域网内 Windows ↔ Windows | ✅ 可以             | 管道名改成 `\\对方IP\pipe\xxx`，代码只改一行 |
| 电脑 ↔ 手机（Android/iOS） | ❌ 不行             | 手机不支持 SMB 命名管道协议                  |
| 手机 ↔ 手机                | ❌ 不行             | 同上，一般走 TCP 或系统级框架                |

原因在于：**Windows 命名管道的底层依赖 SMB 协议**（IPC$ 管道共享）。只要是支持 SMB 的设备（Windows 全系、部分 Linux/Samba 环境），跨机器用命名管道毫无压力；而手机系统不实现这套协议，所以跨平台传输的通用做法是 **TCP socket**。

实际项目里最常见的架构是分层组合：

```
 手机 (TCP 客户端)                     PC (接收方)
┌──────────────┐  TCP(9527端口)   ┌─────────────────────────────┐
│ 发送文件数据   │ ───────────────► │ 传输引擎(收文件、算进度)       │
│ 每块报告进度   │                  │    │ 命名管道广播进度          │
└──────────────┘                  │    ▼                        │
                                  │ UI进程1 / UI进程2 / 悬浮窗... │
                                  └─────────────────────────────┘
```

**设备之间走 TCP，本机进程之间走管道。** 

## 七、实战：文件传输进度怎么传？

回到第六节的问题。以"手机 ↔ 电脑传文件、电脑上显示进度"为例，分三种情况给代码。

### 7.1 情况一：局域网内两台 Windows 电脑（命名管道跨机器）

接收方的传输引擎在 `192.168.1.100` 上创建管道；另一台电脑（或接收方本机）的 UI 进程订阅进度。**代码和第三节几乎一样，只有管道名不同**：

```cpp
// 订阅端(UI 进程): 管道名从 \\.\pipe\xxx 换成 \\主机名或IP\pipe\xxx
#include <windows.h>
#include <iostream>
#include <io.h>      // _setmode
#include <fcntl.h>   // _O_U16TEXT

const wchar_t* PIPE_NAME = L"\\\\192.168.1.100\\pipe\\FileTransferProgress";

int main()
{
    _setmode(_fileno(stdout), _O_U16TEXT);   // 控制台中文设置, 见第三节说明

    WaitNamedPipe(PIPE_NAME, 5000);
    HANDLE hPipe = CreateFile(PIPE_NAME, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

    wchar_t buf[256];
    DWORD n;
    while (ReadFile(hPipe, buf, sizeof(buf) - sizeof(wchar_t), &n, NULL) && n > 0)
    {
        buf[n / sizeof(wchar_t)] = L'\0';
        std::wcout << L"\r" << buf << std::flush;   // buf = "进度 40%" 之类
    }

    CloseHandle(hPipe);
}
```

跨机器用命名管道的前提（都默认满足，不行就查这三点）：

1. 两台机器在同一局域网，能互相 ping 通；
2. 发送方开启了"Server"服务（即"文件和打印机共享"，Windows 默认开）；
3. 防火墙放行 TCP 445 端口（首次启用共享时系统会引导放行）。

### 7.2 情况二：进度广播服务器（多客户端命名管道，完整代码）

这是"传输引擎 → 本机/局域网内多个 UI 进程"的标准做法，把第四节的并发骨架落到实处：一个引擎边收文件边算进度，**广播**给所有订阅的客户端。

```cpp
// ProgressServer.cpp — 文件传输进度广播服务器
// 场景: 传输引擎一边接收文件数据, 一边把进度广播给所有订阅的 UI 客户端
#include <windows.h>
#include <vector>
#include <string>
#include <algorithm>

const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\FileTransferProgress";

CRITICAL_SECTION g_cs;                 // 保护客户端列表
std::vector<HANDLE> g_clients;         // 所有已连接客户端的句柄

// 向所有订阅者广播一条消息(消息模式, 一次一条)
void Broadcast(const std::wstring& msg)
{
    // 先复制一份句柄列表, 在锁外发送, 避免阻塞 I/O 长时间占着锁
    EnterCriticalSection(&g_cs);
    std::vector<HANDLE> snapshot = g_clients;
    LeaveCriticalSection(&g_cs);

    for (HANDLE h : snapshot)
    {
        DWORD n = 0;
        if (!WriteFile(h, msg.c_str(), (DWORD)((msg.size() + 1) * sizeof(wchar_t)), &n, NULL))
        {
            // 写失败: 客户端已断开, 先从列表移除, 句柄交给 ClientThread 清理
            EnterCriticalSection(&g_cs);
            auto it = std::find(g_clients.begin(), g_clients.end(), h);
            if (it != g_clients.end())
                g_clients.erase(it);
            LeaveCriticalSection(&g_cs);
        }
    }
}

// 模拟传输引擎的进度产生线程(真实项目里数据来自 TCP 接收循环)
DWORD WINAPI ProgressThread(LPVOID)
{
    for (int pct = 0; pct <= 100; pct += 10)
    {
        Broadcast(L"进度 " + std::to_wstring(pct) + L"%");
        Sleep(500);
    }
    Broadcast(L"传输完成");
    return 0;
}

// 每个客户端一个线程: 用 PeekNamedPipe 轮询指令, 顺带检测客户端断开
DWORD WINAPI ClientThread(LPVOID param)
{
    HANDLE hPipe = (HANDLE)param;

    wchar_t buf[256];
    DWORD n;
    for (;;)
    {
        // PeekNamedPipe 只查看不取走数据, 从不阻塞 —— 这是重点!
        // 如果这里用阻塞的 ReadFile, 会卡住 Broadcast 线程对同一句柄的 WriteFile
        DWORD avail = 0;
        BOOL peeked = PeekNamedPipe(hPipe, buf, sizeof(buf), NULL, &avail, NULL);
        if (!peeked && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            break;   // 客户端断开(ERROR_INSUFFICIENT_BUFFER = 有数据但 peek 缓冲区装不下)

        if (peeked && avail > 0)
        {
            // 有客户端发来的指令, 读出来处理
            if (!ReadFile(hPipe, buf, sizeof(buf) - sizeof(wchar_t), &n, NULL) || n == 0)
                break;
            buf[n / sizeof(wchar_t)] = L'\0';
            // 处理 "pause" / "cancel" 等指令
        }
        else
        {
            Sleep(100);   // 没有指令, 歇一会儿再查
        }
    }

    // 客户端断开: 从订阅列表移除并清理
    EnterCriticalSection(&g_cs);
    auto it = std::find(g_clients.begin(), g_clients.end(), hPipe);
    if (it != g_clients.end())
        g_clients.erase(it);
    LeaveCriticalSection(&g_cs);

    FlushFileBuffers(hPipe);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
    return 0;
}

int main()
{
    InitializeCriticalSection(&g_cs);

    // 启动"传输引擎"的进度模拟
    CreateThread(NULL, 0, ProgressThread, NULL, 0, NULL);

    // 接受循环: 每连上一个客户端就开一个线程, 同时创建下一个实例等新客户
    bool first = true;
    for (;;)
    {
        HANDLE hPipe = CreateNamedPipe(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096, 0, NULL);
        first = false;
        if (hPipe == INVALID_HANDLE_VALUE) break;

        if (!ConnectNamedPipe(hPipe, NULL)
            && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            CloseHandle(hPipe);
            continue;
        }

        EnterCriticalSection(&g_cs);
        g_clients.push_back(hPipe);
        LeaveCriticalSection(&g_cs);

        HANDLE hThread = CreateThread(NULL, 0, ClientThread, hPipe, 0, NULL);
        CloseHandle(hThread);
    }
}
```

> **这里有一个很容易踩的坑**：不能像第四节那样在 ClientThread 里用阻塞的 ReadFile 等待客户端指令。对同一个同步句柄，一个线程的阻塞读会卡住另一个线程对同一句柄的写——Broadcast 的 WriteFile 会一直挂起，整个广播瘫痪（本文代码实测踩过）。所以这里改用 PeekNamedPipe 轮询，它只查看不取走数据、从不阻塞。同理，Broadcast 里也不要在临界区内做阻塞 I/O，先复制句柄列表再在锁外发送。

订阅端直接用 7.1 的代码（本机订阅用 `\\.\pipe\FileTransferProgress`，远程机器订阅用 `\\192.168.1.100\pipe\FileTransferProgress`）。开两三个订阅端，就能看到进度同时推给所有窗口——这就是"一个服务器多客户端"的实际价值。

### 7.3 情况三：手机 ↔ 电脑（TCP socket）

手机不支持 SMB 命名管道，所以**设备之间这段换成 TCP**：手机每发完一块数据，就向电脑上报一行进度。PC 端接收：

```cpp
// PC 端: TCP 进度接收服务(手机端用 Java/Kotlin/Swift 开 socket 连过来)
// 协议约定: 每发送完一块文件数据, 发一行 "百分比\n", 如 "45\n"
#include <winsock2.h>
#include <iostream>
#include <cstdlib>   // atoi
#pragma comment(lib, "ws2_32.lib")

int main()
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;      // 监听所有网卡
    addr.sin_port = htons(9527);
    bind(s, (sockaddr*)&addr, sizeof(addr));
    listen(s, 5);

    SOCKET c = accept(s, NULL, NULL);       // 手机连上了
    char line[64];
    while (recv(c, line, sizeof(line) - 1, 0) > 0)
    {
        int pct = atoi(line);               // 简单起见, 一行一个百分比
        std::cout << "进度: " << pct << "%\n";
    }
}
```

手机端任何语言都能写：`Socket("192.168.1.100", 9527)`，连上后按协议发进度行即可。注意 TCP 也是字节流，和第五节说的一样：**工程代码要处理粘包/分包**（按行读或加长度头），上面的例子只是演示思路。

### 7.4 选型小结

| 通信需求 | 推荐方式 |
|---|---|
| 本机两个独立进程传指令/进度 | 命名管道（消息模式） |
| 本机父子进程、抓子进程输出 | 匿名管道 |
| 局域网内 Windows ↔ Windows | 命名管道（远程管道名） |
| 手机 ↔ 电脑 / 跨平台 | TCP socket |
| 传输文件内容本身 | 字节流（管道字节模式或 TCP） |
