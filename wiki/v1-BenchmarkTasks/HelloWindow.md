# HelloWindow

v1 中最简单的图形编程任务：从零搭建一个可运行、可关闭的 OpenGL 窗口示例，考察模型对窗口/上下文初始化与资源清理的基本功。

## 题目介绍

使用 C++、GLFW 和 GLAD，以 OpenGL 4.6 为目标创建一个可运行的 Visual Studio C++ OpenGL 项目，打开一个窗口并持续渲染，直到用户关闭它。

- 窗口大小 `800 x 600`，标题 `OpenGL HelloWindow Test`
- 清除颜色 `(0.2, 0.3, 0.8, 1.0)`，每帧清空窗口
- 按 `Esc` 可关闭窗口
- 可在 MSBuild 的 `Debug|x64` 配置下构建

完整任务定义：[`HelloWindow_TASK.md`](https://github.com/yhcedpn/GraphicsCodingBenchmark/blob/main/OpenGL/HelloWindow_Codex+gpt-5.4@xhigh/HelloWindow_TASK.md)

## 相关评审 issue

- [gpt-5.4 执行 HelloWindow 任务（#5）](https://github.com/yhcedpn/GraphicsCodingBenchmark/issues/5)
- [gpt-5.5 执行 HelloWindow 任务（#4）](https://github.com/yhcedpn/GraphicsCodingBenchmark/issues/4)
