# windows_udp

本目录是用 Visual Studio 2022 构建的 C++ Windows UDP 示例。解决方案由 `debug_build.bat` 编译。

## 改完要写进 readme.md

实现或修改行为时，在 `readme.md` 里增加详细说明，不要另建文档。

说明用中文，沿用文件里已有的讲解方式，写清：

- 这一步在解决什么问题
- 报文、端口、超时、重试等约定
- 接收方和发送方各自怎么处理
- 怎么启动对应的 `x64\Debug\*.exe` 做测试

新工程要加入 `windows_udp.sln`。否则 `debug_build.bat` 不会编译它。

## 用 debug_build.bat 编译整个项目

改完代码后，在本目录运行 `debug_build.bat`，编译整个解决方案的 Debug|x64。

脚本最后有 `pause`。在不能按键的环境里这样跑，避免一直等输入：

```
cmd /c "echo.| debug_build.bat"
```

不要只编译单个 `.vcxproj`。单独编译时输出会落到工程子目录，和解决方案的 `x64\Debug` 不一致。程序路径以解决方案目录下的 `x64\Debug\` 为准。

编译失败就先改到通过，再告诉用户结果。
