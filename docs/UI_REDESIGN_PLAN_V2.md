# Neural Signal Command Center V2 — UI 改造执行计划 (v2)

> 基于 `NSCC_UI_Redesign.md` 的设计方向，结合当前 v3_redesign 代码库的实际状态，制定可执行的分阶段改造计划。

---

## 设计方向

**深色精密仪器风 (Dark Precision Instrument)**

参考：Keysight PathWave、BlackMagic DaVinci Resolve、Rohde & Schwarz

核心原则：
- 深色背景 `#0D1117` + 高对比度数据 → 长时间使用不疲劳
- 颜色即信息 → 连接绿、断开红、录制红闪、刺激黄
- 数据优先 → 波形和电极地图是主角，控件是配角
- 等宽字体 → JetBrains Mono 显示数值，专业仪器感
- 操作分层 → 常用一键可达，高级折叠隐藏

---

## 当前已完成

### ✅ 基础架构 (Phase 0)
- TopStatusBar：全局状态栏（连接/Fs/CH/Loss/REC/FPGA/STIM）
- LeftNavigation：图标+文字+sub-status，激活态蓝色左边框
- SettingsPanel：弹出式 Host/Port/Theme 配置
- SystemStatusModel：200ms 节流的全局状态模型
- ChannelMapModel：1024 electrode ↔ 256 ADC 双向映射
- AcquisitionService：TCP 长连接 + retry
- PreviewPipeline / AnalyzerPipeline：双管线骨架

### ✅ 视觉系统 (Phase 1) — 已应用 Keysight/BlackMagic 风格
- `tokens-dark.qss`：GitHub Dark 调色板 (#0D1117 / #161B22 / #388BFD / #39C5CF)
- `widgets.qss`：按钮 4 级 (Primary/Secondary/Danger/Ghost)、输入框、卡片、Badge、导航、表格、滚动条、Console
- `pages.qss`：Banner / Inspector / Page header
- ThemeManager：token 展开 + seriesPalette 调和色阶

### ✅ 新 Widget 库
- InspectorPanel：8 字段通道详情卡片
- ElectrodeOverviewWidget：64-block 8×8 + 16-AFE 4×4 detail
- StackWaveformView：OpenGL 多通道堆叠波形
- TileWaveformView：小通道数网格
- ActivityMapView：16×16 RMS 热图
- MetricCard：数值卡片 (label/value/unit/badge)
- ConsoleWidget：结构化日志 (搜索/导出/Clear)

---

## 待执行改造

### Phase 2：旧页面视觉统一 (1-2 周)

目标：不改功能逻辑，只改视觉呈现。让 4 个旧页面在新主题下看起来统一。

| 任务 | 文件 | 改动 |
|------|------|------|
| 2.1 Channel Map 电极颜色 | `channel_map_panel.cpp` | 删除硬编码 `#fff3cf/#f6fbff`，改用 ThemePalette 注入 |
| 2.2 Channel Map 右侧面板分组 | `channel_map_panel.cpp` | 按 NSCC_UI_Redesign §6.1 重组为"快速筛选/批量操作/选中信息/地址输出"4 个 GroupBox |
| 2.3 SPI 页面按钮分级 | `spi_control_panel.cpp` | 给每个 QPushButton 设 objectName (primary/danger/ghost) |
| 2.4 SPI 页面配置卡片化 | `spi_control_panel.cpp` | Configuration 区改为参数名灰色+值白色的 form layout |
| 2.5 Array Preview 波形颜色 | `realtime_plot_panel.cpp` | 波形色改用 `ThemeManager::seriesPalette(n)` |
| 2.6 Unified Monitor 参数分组 | `unified_monitor_panel.cpp` | 左侧面板按"连接/采集参数/存储/控制"4 个 GroupBox 重组 |
| 2.7 所有页面删除内部 Host/Port | 各页面 | 删除每页重复的网络配置控件（已移入 SettingsPanel） |

### Phase 3：交互升级 (2-4 周)

| 任务 | 改动 |
|------|------|
| 3.1 Channel Map 电极悬停效果 | 鼠标悬停时电极块半透明蓝 + Tooltip 显示 (Block/CH/Addr) |
| 3.2 Channel Map 缩放浮层 | 缩放控件改为右下角浮层，不占布局空间 |
| 3.3 Array Preview 通道标签 | 每个波形图左上角加半透明背景的 CH 标签 |
| 3.4 Array Preview 峰值指示 | 每个图右上角显示当前 max/min 数值 |
| 3.5 Channel Analyzer 分割条 | 时域图和 FFT 图之间加可拖动 QSplitter |
| 3.6 Sequence 下拉加描述 | QComboBox 改为自定义 delegate，每项显示名称+简短说明 |
| 3.7 状态栏动画 | 连接成功绿点 scale 弹簧；REC 红点闪烁 (QPropertyAnimation) |

### Phase 4：数据可视化升级 (1-2 月)

| 任务 | 改动 |
|------|------|
| 4.1 波形图网格线 | 加 `rgba(255,255,255,0.05)` 极淡网格 |
| 4.2 FFT 渐变填充 | 频谱用从 `--accent-blue` 到透明的渐变填充 |
| 4.3 频段标注线 | 50/60 Hz 工频、LFP 频段、Spike 频段可选标注 |
| 4.4 十字准线 Tooltip | 鼠标悬停波形时显示坐标 |
| 4.5 布局密度切换 | 工具栏加 [紧凑/标准/宽松] 三档 |
| 4.6 MetricCard 接入真实数据 | 从 AnalyzerPipeline 的 FFT 结果填充 9 个指标卡片 |

### Phase 5：完全替换旧页面 (可选，长期)

当 Phase 2-4 完成后，旧页面的视觉已经统一，功能已经增强。如果需要进一步重构：
- 把旧页面的业务逻辑提取到 Model/Service 层
- 用新的 PageBase 派生类重新组装 UI
- 彻底删除旧页面文件

---

## 色彩系统 (已实现)

```
背景层次：
  --bg-base:       #0D1117   (app-bg)
  --bg-surface:    #161B22   (card-bg)
  --bg-elevated:   #1C2128   (card-bg-elevated)

前景/文字：
  --text-primary:  #E6EDF3
  --text-secondary:#8B949E
  --text-muted:    #484F58

品牌色：
  --accent-blue:   #388BFD   (primary)
  --accent-cyan:   #39C5CF   (plot-wave, info)
  --accent-green:  #3FB950   (success)
  --accent-yellow: #D29922   (warning)
  --accent-red:    #F85149   (error)

边框：
  --border-subtle: #30363D   (card-border)
  --border-default:#6E7681   (card-border-hi)
```

## 字体系统 (已实现)

```
主字体：JetBrains Mono / IBM Plex Mono / Consolas
辅助：  Segoe UI (fallback)

字号：
  11px — 坐标轴、Badge、GroupBox title
  12px — 次要标签、Console、表头
  13px — 正文、输入框
  16px — 页面标题
  20px — MetricCard 数值
```

## 按钮系统 (已实现)

```
Primary:   填充蓝 #388BFD，白字，用于 Start/Apply/Capture
Secondary: 透明 + 蓝边框，蓝字，用于 Save/Load/Refresh (默认)
Danger:    透明 + 红边框，红字，用于 Stop/Reset/DAC Off
Ghost:     无边框，灰字，用于 Settings/Clear/Copy
```

---

## 文件结构

```
src/ui/theme/
├── tokens-dark.qss      ← 深色主题 token (Keysight/BlackMagic 风格)
├── tokens-light.qss     ← 浅色主题 token (备用)
├── widgets.qss          ← 控件样式 (按钮/输入/卡片/Badge/导航/表格/Console)
├── pages.qss            ← 页面布局 (Banner/Inspector)
└── plot.qss             ← 波形图样式

src/ui/widgets/
├── top_status_bar.*     ← 全局状态栏
├── left_navigation.*    ← 左侧导航
├── settings_panel.*     ← Settings 弹出面板
├── inspector_panel.*    ← 通道详情卡片
├── electrode_map_view.* ← 64-block + 16-AFE 地图
├── stack_waveform_view.*← OpenGL 堆叠波形
├── tile_waveform_view.* ← 网格 tile 波形
├── activity_map_view.*  ← 16×16 RMS 热图
├── metric_card.*        ← 数值指标卡片
└── console_widget.*     ← 结构化日志

src/model/
├── system_status_model.*← 全局状态 (200ms 节流)
├── channel_map_model.*  ← 1024↔256 映射
├── console_log_model.*  ← 日志模型
└── stimulation_model.*  ← 刺激参数 + evaluateSafety

src/service/
├── acquisition_service.*← TCP 采集 + retry
├── preview_pipeline.*   ← 降采样管线
├── analyzer_pipeline.*  ← 全速分析管线
├── safety_interlock.*   ← 刺激安全互锁
└── spi_command_service.*← SPI 命令构建
```

---

*版本：v2.0 | 基于 NSCC_UI_Redesign.md + 当前代码实际状态 | 2026-05-15*
