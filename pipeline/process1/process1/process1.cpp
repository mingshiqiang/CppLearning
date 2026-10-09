// process1.cpp — 命名管道 服务端(Server) 示例
//
// 作用: 创建一个命名管道, 等待客户端(process2)连接, 然后循环接收
//       客户端发来的消息并回复, 直到收到 "quit" 为止。
// 注意: 服务端必须先于客户端启动, 否则客户端连不上管道。
//
// 命名管道(Named Pipe)的特点:
//   - 有一个名字(本机为 \\.\pipe\xxx), 任意进程都能通过名字找到它
//   - 支持双向通信, 通信双方可以是两个完全独立的进程

#include <windows.h>
#include <iostream>
#include <string>
#include <io.h>      // _setmode
#include <fcntl.h>   // _O_U16TEXT

// 管道名必须以 \\.\pipe\ 开头; 客户端必须用完全相同的名字才能连上
const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\MyLearningPipe";

// 打印最近一次 Win32 API 调用的错误码
void PrintError(const wchar_t* what)
{
    std::wcerr << L"[错误] " << what << L", 错误码 = " << GetLastError() << std::endl;
}

int main()
{
    // 将标准输出切换到 UTF-16 模式, 保证中文能正确显示
    _setmode(_fileno(stdout), _O_U16TEXT);

    std::wcout << L"===== 进程1: 命名管道服务端 =====\n" << std::endl;

    // 1. 创建命名管道
    //    参数说明:
    //    - PIPE_ACCESS_DUPLEX    : 双向管道, 服务端既可读也可写
    //    - PIPE_TYPE_MESSAGE     : 消息模式, 每次 WriteFile 的内容是一条完整消息
    //    - PIPE_READMODE_MESSAGE : 服务端句柄按"消息"为单位读取
    //    - 1                     : 最多同时允许 1 个客户端连接
    //    - 4096 / 4096           : 输出/输入缓冲区大小(字节)
    HANDLE hPipe = CreateNamedPipe(
        PIPE_NAME,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        4096, 4096,
        0,     // 默认超时
        NULL); // 默认安全属性

    if (hPipe == INVALID_HANDLE_VALUE)
    {
        PrintError(L"CreateNamedPipe 失败");
        return 1;
    }
    std::wcout << L"管道已创建: " << PIPE_NAME << std::endl;

    // 2. 等待客户端连接 (阻塞, 直到客户端 CreateFile 打开该管道)
    std::wcout << L"等待客户端连接..." << std::endl;
    if (!ConnectNamedPipe(hPipe, NULL))
    {
        // 若客户端在 ConnectNamedPipe 调用之前就连上了, 返回此错误, 仍视为已连接
        if (GetLastError() != ERROR_PIPE_CONNECTED)
        {
            PrintError(L"ConnectNamedPipe 失败");
            CloseHandle(hPipe);
            return 1;
        }
    }
    std::wcout << L"客户端已连接!\n" << std::endl;

    // 3. 循环收发消息
    wchar_t buf[1024];
    DWORD bytesRead = 0;

    for (;;)
    {
        // 3.1 读取一条消息
        //     消息模式下 ReadFile 会阻塞, 直到读完整条消息才返回
        //     (注意: 若消息超过缓冲区大小会被截断, 学习示例这里简化处理)
        BOOL ok = ReadFile(hPipe, buf, sizeof(buf) - sizeof(wchar_t), &bytesRead, NULL);
        if (!ok || bytesRead == 0)
        {
            // 客户端关闭了管道
            std::wcout << L"客户端已断开连接。" << std::endl;
            break;
        }
        buf[bytesRead / sizeof(wchar_t)] = L'\0';   // 确保字符串有结尾

        std::wstring msg(buf);
        std::wcout << L"[收到客户端] " << msg << std::endl;

        if (msg == L"quit")
            break;   // 约定: quit 表示会话结束

        // 3.2 回复客户端 (连同结尾的 '\0' 一起作为一条消息发过去)
        std::wstring reply = L"服务端已收到: " + msg;
        DWORD bytesWritten = 0;
        ok = WriteFile(hPipe, reply.c_str(),
                       (DWORD)((reply.size() + 1) * sizeof(wchar_t)),
                       &bytesWritten, NULL);
        if (!ok)
        {
            PrintError(L"WriteFile 失败");
            break;
        }
    }

    // 4. 收尾: 冲刷缓冲 -> 断开连接 -> 关闭管道
    FlushFileBuffers(hPipe);      // 把缓冲数据全部发给客户端
    DisconnectNamedPipe(hPipe);   // 断开当前客户端(之后可再次 ConnectNamedPipe 接受新连接)
    CloseHandle(hPipe);           // 释放管道资源
    std::wcout << L"服务端已退出。" << std::endl;
    return 0;
}
