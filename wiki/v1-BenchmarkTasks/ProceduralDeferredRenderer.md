# ProceduralDeferredRenderer

较复杂的任务：实现一个中小规模但完整的程序化渲染器，综合考察现代 OpenGL 的多 pass 渲染流程组织能力。

## 题目介绍

以 OpenGL 4.6 core profile 实现一个程序化渲染器，场景内容全部在代码中生成（几何与纹理不得使用任何外部资产）：

- `1600 x 900` 窗口，标题 `OpenGL ProceduralDeferredRenderer Test`
- 程序化 mesh（cube、floor、非 cube 曲面）+ 程序化纹理；instancing 渲染至少 `100` 个可见物体
- 多 pass 渲染流程：shadow pass、geometry pass（G-buffer 或 HDR forward）、lighting pass、post-processing pass
- shadow mapping（含 PCF）、至少 `16` 个 point light、HDR tone mapping + gamma correction、额外后处理效果（bloom/blur 等，可用 ping-pong FBO）
- 使用 UBO/SSBO 等结构化 GPU 数据路径；初始化阶段构建 CPU 侧 render graph 或缓存 pass-command 结构，每帧重放
- 运行时调试：`WASD`+鼠标相机、`B`/`H`/`P` 开关、`1`/`2`/`3` 切换 debug view、支持窗口 resize、标题显示 FPS

完整任务定义：[`ProceduralDeferredRenderer_TASK.md`](https://github.com/yhcedpn/RenderArena/blob/main/OpenGL/ProceduralDeferredRenderer_Codex+gpt-5.6-terra@xhigh/ProceduralDeferredRenderer_TASK.md)

## 相关评审 issue

- [gpt-5.4 执行 ProceduralDeferredRenderer 任务（#10）](https://github.com/yhcedpn/RenderArena/issues/10)
- [gpt-5.6-terra 执行 ProceduralDeferredRenderer 任务（#13）](https://github.com/yhcedpn/RenderArena/issues/13)
