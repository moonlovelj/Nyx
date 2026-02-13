# Virtual Shadow Maps (VSM) 实现 README

> 目标：基于 `Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf` 与 `Sparse Virtual Shadow Maps _ J Stephano.pdf`，在 Nyx 当前渲染器中落地可运行、可扩展的 Virtual Shadow Maps（VSM，虚拟阴影贴图）方案。

## 1. 当前工程现状（改造基线）

从现有代码看，方向光阴影仍是传统单图路径：

- 固定阴影图创建：`MiniEngine/Core/BufferManager.cpp:178`
- 方向光矩阵与阴影参数：`MiniEngine/SceneViewer/Main.cpp:531`、`MiniEngine/SceneViewer/Main.cpp:553`
- 方向光阴影采样（`SampleCmp` / PCF / PCSS）：`MiniEngine/Model/Shaders/LightingCommon.hlsli:108`
- 延迟光照仍依赖 `g_ShadowBuffer`：`MiniEngine/Model/LightManager.cpp:370`
- Bindless 入口是 `SRV_SHADOW_MAP`：`MiniEngine/Model/Shaders/BindlessIndices.hlsli:24`
- 全局常量仅 `SunShadowMatrix + ShadowTexelSize` 一套：`MiniEngine/Model/ConstantBuffers.h:85`

同时有两个重要工程事实：

1. `RenderLightShadows()` 已存在但当前主流程未调用：
   - 调用侧：`MiniEngine/SceneViewer/Main.cpp:595`、`MiniEngine/SceneViewer/Main.cpp:625`
   - 实现侧：`MiniEngine/Model/LightManager.cpp:392`
2. `MeshSorter::RenderMeshes()` 里只有 `kVBuffer` 会真正走 `RenderMeshedInternal()`：`MiniEngine/Model/Renderer.cpp:787`

这意味着：在上 VSM 之前，先打通“可控 shadow 渲染通路”会更稳。

---

## 2. VSM 核心概念（先理解再实现）

## 2.1 这里的 VSM 是什么

这里的 VSM 指 **Virtual Shadow Maps**（虚拟阴影贴图），不是 Variance Shadow Maps。

核心思想：

- 维护一个高分辨率“虚拟阴影空间”（例如 16K）
- 把虚拟空间切成固定页（常用 128x128）
- 仅为“当前屏幕实际需要”的虚拟页分配物理内存并渲染
- 其余区域不分配、不渲染

## 2.2 为什么比传统 CSM 更适合大场景

传统 CSM 的每级成本是固定分配，空区域也会付费。
VSM 则按“可见 + 需要精度”分配页：

- 空区域几乎 0 成本
- 成本更接近随屏幕分辨率变化，而非随场景几何总量变化

## 2.3 与 Nanite / GPU-driven 管线的契合点

Karis 方案强调：

- 只更新可见阴影页
- 阴影 LOD 也按“1 texel ~= 1 pixel”匹配
- 多视图并行能显著摊薄调度开销

Nyx 已有 GPU-driven cull + meshlet + 两阶段 culling，天然适合承载 VSM。

---

## 3. VSM 数据结构设计

建议新增模块：`MiniEngine/Model/VirtualShadowMap.h/.cpp`，包含：

1. **Page Table**（虚拟页表）
   - `Texture2DArray<uint>`（每 clipmap 一层）
   - 可选 page-table mip 链，用于层级页 culling
2. **Physical Pool**（物理页池）
   - `Texture2DArray<float>` 或等价格式
   - software sparse：应用手动管理页分配/回收
3. **Page Meta Buffers**
   - free-list / alloc counter
   - dirty list / render list
   - page state（resident/valid/frame marker/clipmap）
4. **调试统计**
   - requested / resident / dirty / rendered / evicted / fault

推荐页表 entry 打包字段：

- physicalPageX
- physicalPageY
- poolIndex
- resident bit
- valid/dirty bit
- frame marker

---

## 4. 渲染流程（每帧执行顺序）

在主视图深度可用后执行：

1. `MarkNeededPagesCS`
   - 从主相机 depth 重建 world pos
   - 投影到光空间与 clipmap
   - 选择“1 texel ~= 1 pixel”级别
   - 标记 needed page
2. `ResolvePagesCS`
   - needed 与 resident/valid 比较
   - 产出 dirty/render list
   - 复用缓存页；不足则分配新页
3. `ClearDirtyPagesCS`
   - 清理待重渲染物理页
4. `RenderVsmPages`
   - 仅渲染 dirty pages
5. `DeferredLighting -> SampleVirtualShadow()`
   - shading 阶段查 page table，完成虚拟到物理映射
6. `FrameMarker/Eviction`
   - 更新页面年龄，回收长期不用页面

---

## 5. 分阶段落地计划（建议顺序）

## Phase 0：打通 Shadow Pass 基线（1-2 天）

目标：先拥有稳定“可单独执行”的 shadow pass。

- 在主流程插入方向光 shadow 更新阶段（位于 deferred lighting 前）
- 增加真正的 shadow draw pass，不再依赖 `kVBuffer` 独占渲染路径
- 保留单图阴影开关做 A/B 对照

验收：GPU Capture 中可见独立 shadow pass，且结果稳定。

## Phase 1：接入 VSM 资源与常量（2-3 天）

- 新增 `VirtualShadowMap` 模块
- 扩展 bindless 索引：page table / physical pool / meta buffers
- 扩展 `GlobalConstants`：clipmap 参数、页大小、物理池参数、预算
- 加调试可视化（residency、dirty map）

验收：不渲染阴影也可正确显示页状态。

## Phase 2：页面需求与分配（3-4 天）

- 实现 `MarkNeededPagesCS`
- 实现 `ResolvePagesCS`（含复用与分配）
- 实现 `ClearDirtyPagesCS`

验收：静态镜头首帧后 dirty 页显著下降；平移时仅边缘页更新。

## Phase 3：渲染到物理页（4-6 天）

- 按 dirty list 构建 shadow views（clipmap + page）
- 渲染输出写入 physical pool
- 渲染完更新 page state（valid/resident）

验收：只有 dirty 页有写入，阴影可见。

## Phase 4：采样与过滤替换（3-4 天）

- `GetDirectionalShadow()` 替换为 `SampleVirtualShadow()`
- world -> clipmap 选择
- page table 解包 -> physical uv
- 缺页回退到更粗 clipmap
- 过滤策略先 Hybrid（边界软件过滤，页内硬件过滤）

验收：无明显页边界缝，缺页时平稳退化。

## Phase 5：缓存失效与预算（2-3 天）

- 失效触发：相机移动、光方向变化、动态物体 AABB 覆盖
- 页面预算：每帧限制细级页更新数
- 粗级页全量或高优先级更新

验收：快速运动下帧时稳定，阴影细节渐进补齐。

## Phase 6：高级优化（长期）

- Hierarchical Page Table / Page-mask culling
- 多视图合批渲染（multi-view）
- 静态/动态 clipmap 分离

---

## 6. 采样与过滤注意事项

1. **页边界过滤串页问题**
   - 方案 A：页面加 1 像素 border
   - 方案 B：Hybrid（边界软件过滤，内部硬件过滤）
   - 方案 C：Manual 全软件 PCF

2. **缺页回退策略**
   - 若 fine clipmap 页缺失：回退到 coarse clipmap
   - 避免黑块/闪烁

3. **光方向变化导致全局失效**
   - 可通过渐进更新 + 预算 + 低频旋转缓解

4. **动态物体处理策略**
   - 简化方案：移动物体 AABB 覆盖页直接标 dirty
   - 进阶方案：静态/动态 clipmap 分离

---

## 7. 建议初始参数

- `PageSize = 128`
- `VirtualResolution = 16384`（每 clipmap）
- `ClipmapCount = 6`（可在 6~8 调）
- `PhysicalPool` 先从 `4096x4096` 起（按 VRAM 调）
- `PageBudgetPerFrame = 256~1024`

建议先目标：

- 1080p 下稳定运行
- 静态镜头基本 0 增量更新
- 平移时仅边界更新

---

## 8. 与现有代码的替换点（实施导图）

1. 资源创建层
   - 从 `g_ShadowBuffer` 单图扩展到 VSM 资源组
   - 起点：`MiniEngine/Core/BufferManager.cpp:178`

2. 全局描述符
   - 现 `SRV_SHADOW_MAP` 改为 VSM 多 SRV/UAV
   - 入口：`MiniEngine/Model/Shaders/BindlessIndices.hlsli:24`

3. 常量缓冲
   - 扩展 `GlobalConstants`
   - 入口：`MiniEngine/Model/ConstantBuffers.h:75`

4. 主渲染时序
   - 在 deferred lighting 前插入 VSM 更新
   - 入口：`MiniEngine/SceneViewer/Main.cpp:595`

5. 阴影采样
   - 替换 `GetDirectionalShadow()`
   - 入口：`MiniEngine/Model/Shaders/LightingCommon.hlsli:108`

6. deferred lighting 资源状态
   - 引入 VSM 资源状态切换
   - 入口：`MiniEngine/Model/LightManager.cpp:360`

---

## 9. 里程碑验收清单

## M1：可视化与数据正确

- 能显示 needed/resident/dirty 热力图
- 页分配与回收计数正确

## M2：功能正确

- 阴影结果正确
- 缺页时可回退，不闪烁

## M3：性能正确

- 静态镜头时 dirty 页很低
- 相机平移时更新集中在边缘
- 页面预算可有效控时

## M4：质量正确

- 页边界无明显接缝
- 动态物体失效范围可控

---

## 10. 参考资料

- `Docs/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf`（重点页：115-120）
- `Docs/Sparse Virtual Shadow Maps _ J Stephano.pdf`（重点页：3-30）

在线原文：

- https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf
- https://ktstephano.github.io/rendering/stratusgfx/svsm

---

## 11. 结语

这套方案的核心不是“把分辨率拉高”，而是把阴影渲染从“全图固定成本”改成“按需分页成本”。

对 Nyx 这种 Nanite-style、GPU-driven 架构，VSM 的价值主要体现在：

- 大场景稳定扩展能力
- 阴影成本可预算、可渐进
- 与现有 culling/LOD 逻辑高度一致

建议执行策略：**先打通通路，再虚拟化，再优化**。
