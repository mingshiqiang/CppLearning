// process2.cpp — 命名管道 客户端(Client) 示例
//
// 作用: 连接 process1(服务端)创建的命名管道, 从键盘输入消息发给服务端,
//       并打印服务端的回复。输入 "quit" 结束会话。
// 注意: 请先运行 process1(服务端), 再运行本程序。

#include <windows.h>
#include <iostream>
#include <string>
#include <io.h>      // _setmode
#include <fcntl.h>   // _O_U16TEXT

// 必须与服务端使用的管道名完全一致
const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\MyLearningPipe";

// 打印最近一次 Win32 API 调用的错误码
void PrintError(const wchar_t* what)
{
    std::wcerr << L"[错误] " << what << L", 错误码 = " << GetLastError() << std::endl;
}

int main()
{
    // 将标准输入/输出切换到 UTF-16 模式, 保证中文能正确输入和显示
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);

    std::wcout << L"===== 进程2: 命名管道客户端 =====\n" << std::endl;

    // 1. 等待服务端的管道可用 (最多等 5 秒)
    std::wcout << L"正在连接服务端..." << std::endl;
    if (!WaitNamedPipe(PIPE_NAME, 5000))
    {
        std::wcout << L"连接失败: 请确认 process1(服务端)已启动。" << std::endl;
        PrintError(L"WaitNamedPipe");
        return 1;
    }

    // 2. 打开管道 — 对命名管道执行 CreateFile 就等于"建立连接"
    //    GENERIC_READ | GENERIC_WRITE : 申请读写权限
    //    0                            : 该句柄不与子进程共享
    //    OPEN_EXISTING                : 管道必须由服务端先创建好
    HANDLE hPipe = CreateFile(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (hPipe == INVALID_HANDLE_VALUE)
    {
        PrintError(L"CreateFile 打开管道失败");
        return 1;
    }

    // 3. 把客户端句柄的读模式改为"消息模式", 与服务端保持一致
    //    (不设置的话默认是字节模式, 一次 ReadFile 可能只读到半条消息)
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

    std::wcout << L"连接成功! 输入消息发给服务端, 输入 quit 退出。" << std::endl;

    // 4. 循环: 读键盘输入 -> 发送 -> 接收回复
    std::wstring line;
    for (;;)
    {
        std::wcout << L"> ";
        if (!std::getline(std::wcin, line))
            break;   // 输入流结束(如按 Ctrl+Z 回车)

        if (line.empty())
            continue;

        // 4.1 把整行(连同结尾 '\0')作为一条消息发出去
        DWORD bytesWritten = 0;
        if (!WriteFile(hPipe, line.c_str(),
                       (DWORD)((line.size() + 1) * sizeof(wchar_t)),
                       &bytesWritten, NULL))
        {
            PrintError(L"WriteFile 失败");
            break;
        }

        if (line == L"quit")
            break;

        // 4.2 阻塞等待服务端回复一条消息
        wchar_t buf[1024];
        DWORD bytesRead = 0;
        if (!ReadFile(hPipe, buf, sizeof(buf) - sizeof(wchar_t), &bytesRead, NULL)
            || bytesRead == 0)
        {
            std::wcout << L"服务端已断开连接。" << std::endl;
            break;
        }
        buf[bytesRead / sizeof(wchar_t)] = L'\0';
        std::wcout << L"[服务端回复] " << buf << std::endl;
    }

    // 5. 关闭管道句柄
    CloseHandle(hPipe);
    std::wcout << L"客户端已退出。" << std::endl;
    return 0;
}
