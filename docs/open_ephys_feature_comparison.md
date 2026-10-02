# NL_source 与 Open Ephys：功能差距与补齐顺序

核查日期：2026-10-02。代码基线：`f4780565da036695c2521638d9802997b300553c`；同时检查本次 `7.0.0-preview.1` 工作区改动。下文的“原版”指该提交，“预览版”指本次改动，不代表已经发布的稳定版本。代码定位采用“文件 + 符号 + 检查时行号”，后续改动可能使行号偏移。

## 先说结论

NL_source 已经有采集、原始录制/回放、数组显示、FFT/ADC 分析、带通/notch、CAR/median 公共参考和阈值 spike 检测，不能把这些全部列为待开发。

与 Open Ephys 的实际功能相比，优先缺的是：

1. **可持续应用的在线分选规则**：PCA、多边形或波形盒选规则、对后续事件自动赋 unit ID；当前新增的候选分组只给已经留存的事件贴标签
2. **完整分析事件归档**：把全程 spike 时间、波形、unit ID、配置版本写盘，并能重新打开；当前导出只是单个 lane 的有限留存快照
3. **可配置参考与坏道管理**：已有全局 CAR/median，但没有参考通道集合、作用通道集合、分组参考及坏道排除
4. **更稳健、能联合多电极的检测**：已有绝对/RMS 阈值，缺少中位数噪声阈值和 stereotrode/tetrode 式联合事件结构
5. **TTL 事件流和 PSTH**：已有电压阈值触发录制，尚无采样坐标一致的外部事件流、试次条件和事件对齐统计
6. **完整的信号/单元质量工作流**：已有 RMS、饱和、丢包和 FFT；尚缺统一 QC 报告、长期单元稳定性和更完整的分选质量指标
7. **标准格式与自动离线分选接口**：没有 SpikeInterface/Kilosort/Phy 集成；这项属于进一步扩展，不能说 Open Ephys 默认就内置了 Kilosort

本次预览补齐的是连续分析生命周期、可追溯事件时间、缺口/有效性处理、有限窗口的手工候选分组与 JSON 导出。**它仍不能称为已经达到 Open Ephys Spike Sorter 的完整在线分选能力。**

## 1. 对照口径

- Open Ephys 以官方当前文档和 release 页面为依据；核查时 release 页面将 v1.1.0 标为 Latest。[OE-1]
- “内置”与“可安装插件”分开计。Spike Sorter 文档表格与安装说明存在不一致，0.6.x 迁移说明明确它已移到独立仓库，因此这里称“官方可安装插件”。Online PSTH 也按其安装说明列为插件。[OE-2][OE-3]
- “已具备”表示有效代码路径中已有该能力，不表示两个软件的参数、算法、交互或性能完全相同。
- “部分具备”表示有基础能力，但欠缺表中列出的使用闭环。
- “未实现”表示检查了当前 `src/`、构建源文件列表和相关调用路径，未找到可用实现；不是仅凭页面名称推测。
- 没有把未进入构建的旧原型当成功能。`CMakeLists.txt:53–55,58–172` 明确限定编译源；`PreviewPipeline`/`AnalyzerPipeline` 等旧原型中的占位注释不能代表当前实际页面。实际 FFT 页面是 `AnalyzerPanel`。

## 2. 功能矩阵

| 功能 | Open Ephys 基准 | NL_source 原版 | 本次预览版 | 仍要补什么 |
|---|---|---|---|---|
| 采集、原始录制、回放 | Source + Record Node + File Reader；可分别记录连续数据、spike 和事件 [OE-4][OE-5] | **已具备专有链路**：TCP、256×32-bit 帧、分卷 BIN、session.json、回放/seek | 增加录制无效帧区间及回放传播 | 专有 raw 格式可继续保留；缺标准数据适配器和完整分析事件记录，不能说“没有录制” |
| 可重组处理链 | source/filter/sink、分支与插件机制 [OE-6] | **部分具备**：SessionHub 多订阅队列、独立显示/分析/录制 worker | 检测跟随会话，不再跟随 Spike 页是否可见 | 处理模块统一接口、配置快照、选择性旁路和处理图；无通用插件装载或可视化信号链 |
| 带通、notch、LFP 基础 | Bandpass 内置；Notch 为插件；可用分支作不同频段 [OE-7][OE-8] | **已有 DSP**：Butterworth HP/LP、50/60 Hz notch、Wide/Spike/LFP 分支 | 保留；Spike 页使用 spike band | 不必重写滤波器；应补校准、幅频/相位/块边界验证。LFP 有算法分支不等于有完整 LFP 分析工作流 |
| CAR/median 参考 | 内置 CAR 可分别选择参考通道、作用通道和 gain；同一同步流内处理 [OE-9] | **已有全局 CAR/median**：每帧全部 256 路形成参考 | 保留，参考变更重启分析区间并记元数据 | 缺参考/作用通道掩码、坏道排除、按电极组参考；空间 median 参考不等于检测用的时间域 MED/MAD 噪声估计 |
| Spike 阈值检测 | 内置 Detector：µV、STD、MED；single/stereotrode/tetrode [OE-10] | **部分具备**：绝对/RMS 阈值、极性、不应期、逐 lane 阈值覆盖、跨块波形窗 | 事件增加源时间/物理位点/连续段；无效帧隔离 | 缺 MED/MAD 模式、联合多电极事件和相关参数 UI；当前为每 lane 独立检测 |
| 波形对齐与事件定义 | Detector 产生 spike 事件供下游处理 [OE-10] | 阈值穿越对齐的留存波形 | 明确保存 crossing 坐标和 pre_samples | 当前时间是因果滤波后的阈值穿越，不是波峰或真实神经元发放时刻；若要峰对齐/亚采样对齐，须单独实现并记录语义 |
| 多电极联合检测/特征 | Detector 可将 2/4 通道作为一个 electrode [OE-10] | **未实现对应分组**：256/512 个独立 lane | 仍是单 lane 波形、单 lane 特征 | 定义电极组、邻域和跨通道去重；不能把“通道多”当成“多电极联合分选” |
| PCA 与在线手工分选 | Spike Sorter：波形盒、多盒组合、PCA 空间多边形；规则应用到进入的 spike，写 sortedId [OE-2] | **未实现** | **新增部分能力**：peak/P2P/energy 散点、框选/单点选择、给留存事件标 candidate ID | 缺 PCA、波形空间门、持久化分类规则、后续事件自动分类、规则冲突处理；当前标签会随事件淘汰/分析重置消失 |
| Raster、ISI、模板查看 | Spike Viewer 及相关分析插件；PSTH 插件可按 unit 展示 [OE-11][OE-3] | 波形叠加、阈值和近实时计数；无完整单元分析 | **新增**源时间 raster、同连续段 ISI、均值/标准差波形 | 仅保留窗口；缺全会话时间趋势、ACG/CCG、稳定性及完整单元质量评估；均值波形不是模板匹配 sorter |
| 采集信号 QC | Quality Monitor 为插件：RMS、频谱、快照/饱和、阈值活动；可导出结果 [OE-12] | **部分具备**：RMS/P2P/DC/饱和热图、packet loss、FFT/ADC 性能指标 | 新增分析覆盖、缺失/无效/未验证帧、边界事件、pending 窗告警 | 缺统一分组 QC、工频噪声判据、坏道建议/人工确认及可保存 QC 报告；传输完整性不等于电极或分选质量 |
| 频谱/时频/LFP | Spectrum Viewer 插件含功率谱和 spectrogram [OE-13] | **已有 FFT/PSD**、窗口选择、SNDR/SNR/ENOB/IRN；滤波器含 LFP 分支 | 本次未扩充这条分析链 | 缺滚动时频图、频带统计和事件对齐 LFP；不能把现有 ADC 测试型 FFT 页面说成“没有频谱” |
| TTL、试次、PSTH | Online PSTH 接收事件+spike，支持 TTL/消息条件 [OE-3] | **仅相关基础**：模拟电压阈值触发开始录制；StimTrigger 含 ExternalTTL 枚举 | 未新增 TTL 输入/事件流 | 缺带源 sample index 的事件对象、TTL 边沿解码/回放/录制、试次条件、PSTH；枚举和触发录制都不等于该闭环 |
| 全程分析数据持久化 | Record Node 可保存 incoming continuous、spikes、events；在 Sorter 后记录 sorted IDs [OE-4][OE-2] | raw/session 存储已实现，但 spike 只在有界 snippet ring | **部分补齐**：单 lane 留存快照 JSON，时间/标签/波形/配置/覆盖信息 | 缺全程 append-only spike/event 档案、标签/规则保存与重新载入；当前导出不能替代全程记录 |
| 分析复现、无损离线跑批 | Open Ephys 配置与记录体系可支持重建处理链；具体处理器变更记录仍需逐项核查 [OE-6][OE-4] | 原始回放、TDM 元数据、时间线基础 | 保存初始/请求配置变更；回放明确使用当前配置 | 缺逐样本生效配置版本、按原设置重算、端到端不丢分析块的跑批模式；不能因 replay reader 有背压就宣称全链路无损 |
| 标准格式/离线自动分选 | 官方记录/读取体系涵盖 Binary、Open Ephys、NWB；自动离线 sorting 另看 SpikeInterface/Kilosort [OE-6][SI-1][KS-1] | 有 MATLAB 通道二进制导出/加载脚本；无对应通用适配 | 增加自定义 JSON 快照，不是 NWB/Phy 格式 | 可先做 SpikeInterface Recording adapter，再接 CPU sorter/可选 Kilosort；没有必要把 Python sorter 重写成 C++ |

## 3. 关键代码证据与边界

### 3.1 已有功能，不能重复算作“本次新加”

- **全局参考**：原版 `git show f4780565:src/service/spike_detect_worker.cpp` 的 `run():226–250` 已实现 CAR 与 median。当前对应 `src/service/spike_detect_worker.cpp:289–325`，对 `kChannelsTotal` 全部通道求参考，没有参考/作用通道列表参数。显示链 `src/network/data_sorter.cpp:207–244` 也实现同样机制。
- **滤波/阈值**：`src/signal/spike_filter.h:43–62` 的 `SpikeProcConfig`；`spike_filter.cpp:99–114,183–256` 的 `configure/processChannel`。RMS 是平方值的 EMA，约 250 ms 时间常数；每通道独立状态。这些文件在本次工作区没有被改动。
- **采集 QC**：`src/network/data_sorter.cpp:249–269` 计算 mean、去均值 RMS、P2P、ADC 轨到轨饱和和 packet loss；`src/ui/widgets/activity_map_view.h:17–23` 明确四种热图指标。
- **FFT**：`src/ui/analyzer_panel.cpp:649–690,1696` 有 FFT 开关、点数、窗口、PSD/噪声密度与后台计算；`src/signal/sndr_calculator.cpp:197–239,316–333` 有有效计算结果，不能被旧 `AnalyzerPipeline` 占位文件误导。
- **数据导出**：`src/io/matlab_channel_exporter.cpp:136–170,250–328,541–598` 已有 ADC/TDM 二进制导出、MATLAB loader 和时间线处理；但没有 SpikeInterface/NWB/Phy 适配。

### 3.2 本次实际新增了什么

- `src/signal/spike_event.h:7–35`：增加 epoch、continuitySegment、sourceFrame、sourceSampleRate、ADC/电极/TDM phase、gain、stride、preSamples、候选 ID。
- `src/service/spike_detect_worker.cpp:91–145,254–286`：原子提交完整 waveform+event；源坐标关联；缺口/无效帧断开 detector 历史，未完成后窗单独计数。
- `src/ui/spike_panel.cpp:635–721,837–878,894–909`：检测跟随 session；切页仅停止绘图；累计 Hz 使用实际处理的 lane 源时间。原版 `onDeactivated():641–650` 会停止检测，原版 `refresh():588` 用显示间隔作速率分母。
- `src/signal/spike_sorting_model.h:10–38,41–72` 与 `.cpp:18–41,108–169`：peak/P2P/energy、单 lane 有限快照、均值波形/标准差、连续段内 ISI。**未做 PCA、聚类、自动 unit 隔离或模板匹配。**
- `src/ui/widgets/spike_sorting_widget.cpp:86–115,290–323,458–464`：框选散点后按事件 sequence 赋候选标签。`spike_snippet_store.h:91–93` 与 `.cpp` 的 `setCandidateUnit` 仅处理仍留存的事件，没有把框选区域保存成未来事件的分类器。
- `src/io/spike_event_exporter.cpp:45–121`：单 lane 原子 JSON 导出；`scope` 明确是 retained window。`src/signal/spike_snippet_store.h:55,125–133` 是 128 MiB 预算的 ring，不是全程归档。
- `src/io/session_manifest.h:15–20,37,51–84`、`src/network/replay_controller.cpp:308–324`：记录和传播 file-frame 无效区间。
- `src/ui/spike_panel.cpp:724–749,846–865`：配置变更记的是请求源帧位置，并明确回放使用当前设置；尚不能承诺原始混合参数运行的逐帧复现。

### 3.3 与 spike sorting 直接相关的硬约束

1. **源帧率不等于 TDM 每电极采样率。** `src/ui/spike_panel.cpp:100–106`：TDM 模式每电极 fs = 源 fs / 4，并只建立当前相位对的 512 个 lane。若源帧率为默认 20 kHz，每电极是 5 kHz；Nyquist 为 2.5 kHz。`:361` 将 spike 低通限制为 `min(6000, fs×0.45)`，此时是 2.25 kHz。不能用软件补回没有采到的高频，也不能把源帧率直接填进离线 sorter 的每通道采样率。
2. **当前只分析 512/1024 TDM 物理位点。** 原始文件保留四相数据，不代表实时 detector 处理了全部四相。若要扩到 1024，必须分离显示选择与分析选择，并验证每位点时间线/吞吐量。
3. **gain 仍是人工统一值。** `spike_panel.cpp:122–128` 为 60×/180×选择，`spike_event_exporter.cpp:18` 明确未由硬件验证；分选前要确认每通道校准、极性、ADC 转换和物理 geometry。
4. **已有背压不是离线分析零丢失保证。** `replay_controller.cpp:273–278` 对 reader 入队提供背压，但 `session_hub.cpp:550–561` 的订阅分发仍用可淘汰队列。预览会报告分析缺口；完整离线基准应使用能等待处理完毕的独立 delivery/drain 路径。

### 3.4 “未实现”如何核查

对 `src/` 和 `CMakeLists.txt` 检索并阅读相应命中：PCA/主成分、PSTH/事件对齐、SpikeInterface/Kilosort/NWB/Phy、坏道/参考通道集合、MED/MAD、插件装载接口；同时检查有效构建列表、检测/导出模型和 GUI 控件。无关子串命中已排除。

- `SorterWorker`/`DataSorter` 是帧解析、拆通道/显示数据处理，不是神经元 spike sorter：见 `src/network/sorter_worker.h:20–37` 的输入/输出接口。
- `src/core/stim_protocol.h:14,25` 仅有 `ExternalTTL` 等触发类型声明；不能据此认定 TTL 事件采集链已完成。
- `src/service/session_hub.h:87–99` 的 `TriggerConfig` 和 `command_center_main_window.cpp:272` 实际作用是电压穿越后开始录制，不产生供 PSTH 使用的逐事件时间序列。
- 未发现跨通道 `SpikeEvent` 波形结构、PCA 投影器、可持续分类规则、标准排序文件适配器或通用插件 loader。

## 4. 建议补齐顺序与验收

### 第一阶段：先把已有检测结果完整留住

- 实现全部 lane 的事件流写盘与可重新打开的分析会话：原始 recording 标识、源采样坐标、有效性、检测配置版本、波形和候选标签分别有明确 schema。
- GUI 仍只保留有限显示窗口；事件归档不能因 ring 淘汰、切页、慢绘制而遗漏。
- 离线跑批提供 EOF drain、取消/失败/缺口结果，能等待每个分析订阅者完成。
- 验收：超过 ring 容量、慢消费者、seek、分卷、EOF pending、取消及故障恢复；事件数量与源坐标一致，缺失显式记录。同一文件不同 replay 速度不改变结果。

### 第二阶段：补到 Open Ephys 的实用在线分选级别

- 增加 MED/MAD 噪声估计、参考/作用通道分组与坏道掩码。
- 增加 1/2/4 通道电极组、共享 waveform window 和去重策略；TDM 的非同时采样要保留各通道真实坐标，不能直接假装同步 tetrode。
- 增加固定/可重算 PCA、PCA 多边形和波形盒规则；规则在后续事件上执行，并把规则版本/unit ID 写到第一阶段的事件档案。
- 验收：相同参数重复运行一致；训练/绘制规则与测试事件分开；边界条件、规则冲突、重新 PCA 后旧规则失效/迁移有明确行为；合成多通道重复事件不重复计作多个神经元。

### 第三阶段：实验分析工作流

- 增加统一信号 QC：已知采样率下的工频、噪声、饱和、掉线判据和 JSON/图像报告；低频率不能直接标定为坏电极。
- 增加 TTL/消息事件模型、事件录制和回放，再加试次 raster/PSTH 与必要的 LFP 事件平均。
- 增加滚动 PSD/spectrogram；保留现有 ADC 性能分析页面。
- 验收：已知频率/幅值波形、已知 TTL 时间、跨连续段/seek、不同 bin 宽度和未完成试次；不能把操作系统收到消息时间当成采样时刻。

### 第四阶段：自动离线分选与高密度数据

- 用独立进程/可固定版本的 Python 环境接 SpikeInterface；先接 CPU 可跑的排序器，再按 geometry、数据质量和硬件条件选择 Kilosort。Kilosort 的推荐加速路径需要检查 NVIDIA/PyTorch 环境。[SI-1][KS-1]
- 先做数据适配：每通道有效 fs、gain、offset、单位、geometry、坏道、连续段和缺口，明确哪些预处理已做，避免双重滤波/参考。
- 后续结果导入单位模板、全程 firing rate、ISI/ACG/CCG、幅度/漂移和质量指标；保留人工审查。
- 验收：ground truth 按明确容差匹配（SpikeInterface 默认 0.4 ms）、报告 precision/recall、agreement、过分裂/合并和未匹配单位；无 ground truth 时只能说一致性/QC，不能说准确率。[SI-2]

## 5. 标准数据与测试结论应怎么解释

- **单通道 Quiroga**：适合阈值检测、噪声、重叠及单通道分组测试；复制成 256 路只验证传输、GUI 和部分负载，不会变成多电极 ground truth。[DATA-1]
- **小型 MEArec**：官方示例 `mearec_test_10s.h5` 为 32 通道、32 kHz、10 s、10 个真值单位；适合多通道导入/事件匹配/回归。39.06 MiB 是 float32 trace 数据体积，不等于 HDF5 下载总大小。10 s 不足以验证长时漂移。[DATA-2]
- **更长、不同噪声和漂移条件**：应使用分开的 seeded 合成测试集；最终再加 paired 真实记录。合成、hybrid 注入和只覆盖少数细胞的 paired truth 必须分开报告。[DATA-3]
- **检测测试不是 sorting 测试**：当前实现尚未自动预测真实 unit ID，不能把源数据的 truth 标签原样返回，再报告成分选准确率。
- 本报告是代码与功能对照，不报告未执行的硬件验收或本机实时吞吐结果；最终构建、测试数量和标准数据指标见对应验证报告。

## 官方依据

- [OE-1] [Open Ephys releases](https://github.com/open-ephys/plugin-GUI/releases)
- [OE-2] [Spike Sorter](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Spike-Sorter.html)；[0.6.x 插件迁移说明](https://open-ephys.github.io/gui-docs/User-Manual/Whats-new-in-v06x.html)
- [OE-3] [Online PSTH](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Online-PSTH.html)
- [OE-4] [Record Node](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Record-Node.html)
- [OE-5] [File Reader](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/File-Reader.html)
- [OE-6] [插件架构、数据类型与格式](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/index.html)
- [OE-7] [Bandpass Filter](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Bandpass-Filter.html)
- [OE-8] [Notch Filter](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Notch-Filter.html)；[Splitter](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Splitter.html)
- [OE-9] [Common Average Reference](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Common-Average-Reference.html)
- [OE-10] [Spike Detector](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Spike-Detector.html)
- [OE-11] [Spike Viewer](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Spike-Viewer.html)
- [OE-12] [Quality Monitor](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Quality-Monitor.html)
- [OE-13] [Spectrum Viewer](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Spectrum-Viewer.html)
- [SI-1] [SpikeInterface sorters](https://spikeinterface.readthedocs.io/en/latest/modules/sorters.html)
- [SI-2] [SpikeInterface ground-truth comparison](https://spikeinterface.readthedocs.io/en/latest/modules/comparison.html)
- [KS-1] [Kilosort 官方仓库与要求](https://github.com/MouseLand/Kilosort)
- [DATA-1] [Quiroga 大学数据仓库；CC BY 4.0](https://figshare.le.ac.uk/articles/dataset/Simulated_dataset/11897595)
- [DATA-2] [SpikeInterface MEArec 官方示例及元数据](https://spikeinterface.readthedocs.io/en/0.102.0/tutorials/widgets/plot_3_waveforms_gallery.html)
- [DATA-3] [SpikeForest：真值数据类型与比较限制](https://elifesciences.org/articles/55167)
