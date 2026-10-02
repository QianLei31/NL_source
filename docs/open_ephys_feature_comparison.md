# NL_source 与 Open Ephys：功能对照与剩余差距

核查日期：2026-10-02；本版核查时间截至 07:31 UTC。原始代码基线：`f4780565da036695c2521638d9802997b300553c`。本报告区分原版、preview1 和已完成本轮软件回归的 **7.0.0-preview.2**；预览版软件验收不等于实体硬件或生物信号有效性验收。源码采用“文件 + 符号 + 核查时行号”定位，后续改动可能使行号偏移。

## 先说结论

NL_source 原版已经有采集、raw 录制/回放、数组显示、FFT/ADC 分析、带通/notch、CAR/median 参考和阈值 spike 检测。

**preview2 又补上三块原先确实缺失的能力：**

1. **持续波形盒分类**：保存规则，并按实际生效的规则给后续完整波形事件赋候选 unit ID；多个规则同时命中明确标为 ambiguous，配置不兼容明确标为 incompatible
2. **独立事件归档**：原始事件与波形从 detector 写入有界 writer，使用稳定 run/event ID，保留实际配置/规则版本；可重新打开、分页查阅和追加人工标签，不再靠显示 ring 保存全程结果
3. **独立全文件离线分析**：输入按块读取、队列满时等待、EOF 排空 detector 和 writer，保留原始有效性信息；有取消、失败及最终提交状态

因此，不能继续说“没有持续规则、完整事件归档或离线跑批”。**最终 clean 构建通过，64/64 CTest 通过，零失败，用时 254.02 秒；新增实际 Dummy 神经档案端到端也已通过。** 这些结果建立软件回归基线，但不能据此声称已全面达到或超过 Open Ephys。

**仍然明确缺少的主要能力：**

- PCA 投影、PCA 多边形分选；现有持续规则是 waveform boxes，不是自动聚类
- MED/MAD 类稳健噪声阈值、参考/作用通道分组与坏道排除
- stereotrode/tetrode 式多电极联合事件、跨通道特征与去重
- 带统一源采样坐标的 TTL 事件流、试次条件及 PSTH
- 完整信号 QC 报告、长期单元质量/稳定性与神经科学时频分析
- SpikeInterface/NWB/Phy 等标准适配，以及可选自动离线 sorter
- 通用可重组处理链/插件接口

Open Ephys 的 Spike Sorter 是官方插件，支持波形盒与 PCA 手工分选；不能把 Open Ephys 本身说成默认内置 Kilosort。[OE-2][OE-6][SI-1][KS-1]

## 1. 对照与验证口径

- Open Ephys 依据官方文档和 release 页面；本次研究中页面将 v1.1.0 标为 Latest。[OE-1]
- 内置和可安装插件分开计。Spike Sorter 的页面表格与安装说明有不一致，0.6.x 迁移说明明确它移入独立仓库，因此称“官方可安装插件”；Online PSTH 也按安装说明计为插件。[OE-2][OE-3]
- **已实现**：在有效构建源和调用路径中已存在；不表示算法、交互或性能与对方等价。**本轮已验证**：列入最终 clean 构建与软件回归；范围仍受相应测试输入和断言限制。
- **未实现**：检查 `src/`、构建列表及相应调用后，未找到可用实现。没有把 `PreviewPipeline`/`AnalyzerPipeline` 等未进入当前主程序构建的旧原型当成功能；实际 FFT 页面为 `AnalyzerPanel`。
- 下列验证状态依据最终构建、64 项 CTest 终态与对应 LastTest 输出，区分测试目标数、内部断言数和事件数量，不将它们混加。

### 从 preview1 到 preview2，哪些结论已经变了

| 能力 | preview1 历史状态 | preview2 当前实现与边界 |
|---|---|---|
| 手工候选分组 | 框选 peak/P2P/energy 后，只给留存事件贴标签 | 原有历史标注保留；新增独立 waveform-box 规则，对后续事件持续分类。二者目的和持久化路径不同 |
| 事件保存 | 单 lane 有限留存 JSON 快照 | 新增独立不可变事件档案、实际配置/规则版本和 append-only 标签；旧快照仍可用，但不是主档案 |
| 离线分析 | 依赖回放与可淘汰订阅队列，不能保证全文件分析 | 新增不走 SessionHub 订阅的专用全文件任务，禁丢弃入队并排空尾部；实时路径的溢出仍必须显式报告 |
| 配置可追溯性 | 主要记录 GUI 请求位置，不能代表实际应用时刻 | 新档案按每事件记录 crossing 使用的 detector/rule revision、完整定义和实际应用的 epoch/source-frame/continuity 边界；不等于已实现任意旧录制混合配置的自动逐段重放 |

## 2. 功能矩阵

| 功能 | Open Ephys 基准 | NL_source 既有基础 | preview2 当前状态 | 仍缺/适用边界 |
|---|---|---|---|---|
| 采集、raw 录制/回放 | Source、Record Node、File Reader；连续数据/spike/event 记录 [OE-4][OE-5] | TCP、256×32-bit 帧、分卷 BIN、session.json、seek；preview1 加入无效帧区间传播 | **保留并扩展**：raw 与分析 sidecar 分离 | 专有格式不是标准适配器；事件归档是否启用、输入是否有缺口须单独判断 |
| 处理链与生命周期 | source/filter/sink、分支与插件机制 [OE-6] | SessionHub 订阅、独立 worker；preview1 已让检测不随切页停止 | **扩展**：producer → detector → writer 正常排空；专用离线任务 | 无通用处理图/插件 loader；停止排空不能追回此前源丢失或实时订阅丢失 |
| 滤波、notch、LFP 基础 | Bandpass 内置；Notch 插件；Splitter 可建不同频段 [OE-7][OE-8] | Butterworth HP/LP、50/60 Hz notch、Wide/Spike/LFP DSP 分支 | **已有，保留** | 有 LFP DSP 分支不等于完整 LFP 分析；没有必要重复重写现有滤波器 |
| 参考与坏道 | CAR 可选参考通道、作用通道和 gain，同步流内处理 [OE-9] | 全局 CAR/median：每帧所有 256 路参与 | **部分具备**，规则兼容性包含 reference | 缺参考/作用掩码、分组参考、坏道排除；空间 median 不是时间域 MAD 噪声估计 |
| 阈值 spike 检测 | µV/STD/MED；single/stereotrode/tetrode [OE-10] | 固定/RMS 阈值、极性、不应期、逐 lane 覆盖、跨块波形窗 | **部分具备**；事件固定实际 detector revision | 缺 MED/MAD、多电极联合事件；RMS 噪声估计仍会受大事件/伪迹影响 |
| 波形/事件定义 | Detector 产生下游 spike 事件 [OE-10] | 因果滤波后的阈值穿越对齐 | **可追溯**：crossing 源帧、pre_samples、gain、stride、epoch/segment | 不是峰对齐、亚采样对齐或真实生物发放时刻；变换必须保持明确语义 |
| 多电极联合检测 | 2/4 通道可构成一个 electrode [OE-10] | 多 lane 独立检测 | **仍未实现联合分组**；规则按物理电极/配置匹配 | 多通道数量不代表联合排序；TDM 非同时采样也不能直接当同步 tetrode |
| 波形盒在线分类 | 多个 waveform box、按规则赋 sortedId [OE-2] | preview1 只有历史候选标注 | **新增实现，本轮已验证**：多盒 AND、后续事件分类、JSON 保存/读取、实际应用版本、ambiguous/incompatible | 属用户定义候选类别；不是自动发现神经元。规则编辑请求与实际应用确认已分开 |
| PCA/多边形分选 | PCA 投影、多边形、重新 PCA [OE-2] | peak/P2P/energy 特征散点 | **仍缺 PCA**；持续分类采用时间-电压盒 | 后续应增加固定训练/测试边界、投影版本和 PCA 变化后规则迁移/失效策略 |
| Raster、ISI、单元 QC | Spike Viewer、PSTH 等分析插件 [OE-11][OE-3] | preview1 加入源时间 raster、连续段内 ISI、均值±SD 波形 | **已有窗口视图 + 新增档案浏览** | 当前统计视图仍以有限快照为主；分页保存不是全会话 ACG/CCG、漂移与稳定性分析 |
| 信号 QC 与频谱 | Quality Monitor、Spectrum Viewer 插件 [OE-12][OE-13] | RMS/P2P/DC/饱和/丢包、FFT/PSD、SNDR/SNR/ENOB/IRN、分析覆盖告警 | **部分具备**；归档状态与源完整性分别显示 | 缺统一 QC 报告、工频判据、坏道工作流、滚动 spectrogram 与事件对齐 LFP；不能说“没有频谱” |
| TTL/试次/PSTH | TTL/消息条件 + spike 的事件对齐统计 [OE-3] | 模拟电压阈值触发录制；ExternalTTL 枚举 | **未新增该闭环** | 缺 TTL 边沿事件的源坐标、录制/回放、条件与试次模型、PSTH；触发录制不等于 PSTH |
| 完整窗事件归档 | Record Node 可在 Sorter 后保存 spikes、events 与 sorted ID [OE-4][OE-2] | raw 存储 + bounded ring；preview1 单 lane JSON | **新增实现，本轮已验证**：独立 writer、稳定 run/event ID、原始波形/自动分类不可变、追加标签、异步重开/分页 | 实时开档只覆盖开档后输出的完整窗事件；未开启时不能回填已淘汰历史。源缺口/边界排除仍显式保留 |
| 专用离线跑批/复现 | 文件输入与配置链支持离线使用，具体插件语义须核查 [OE-5][OE-6] | 旧定时回放不能保证分析订阅不丢块 | **新增实现，本轮核心与集成验证通过**：冻结任务配置、全文件 pull、禁丢弃入队、EOF drain、取消/失败/finalizing | 完成的是本次指定配置的检测/盒规则分析；不自动恢复任意旧混合配置。不补回原始文件已缺失的数据 |
| 标准格式/自动离线 sorter | Binary/Open Ephys/NWB；自动排序另看 SpikeInterface/Kilosort [OE-6][SI-1][KS-1] | MATLAB 通道二进制导出和 loader | 自定义 versioned sidecar 与规则 JSON 已新增 | SpikeInterface/NWB/Phy 适配、自动聚类/模板排序/漂移校正仍未集成；“离线分析”不等于“自动离线 spike sorting” |

## 3. 代码证据

### 3.1 原版已有，不能算成本次新做

- **参考**：原版 `spike_detect_worker.cpp` 的 `run():226–250` 已有 CAR/median；preview2 对应 `src/service/spike_detect_worker.cpp` 的 `run()` 参考计算（核查时 :521 起）。参考仍对 `kChannelsTotal` 全部通道求值，没有参考集合/坏道掩码。显示链 `src/network/data_sorter.cpp:207–244` 也有同类实现。
- **滤波/阈值**：`src/signal/spike_filter.h:43–62` 的 `SpikeProcConfig`；`.cpp:99–114,183–256` 的 `configure/processChannel`。RMS 为平方值 EMA，约 250 ms 时间常数；各通道独立状态。
- **信号 QC**：`src/network/data_sorter.cpp:249–269` 计算去均值 RMS、P2P、DC、ADC 轨饱和和丢包；`src/ui/widgets/activity_map_view.h` 的 `MetricMode` 有 RMS/P2P/Mean/Saturation。
- **FFT 与导出**：`src/ui/analyzer_panel.cpp` 的 FFT 控件与后台分析、`src/signal/sndr_calculator.cpp:197–239,316–333` 的频谱/指标、`src/io/matlab_channel_exporter.cpp` 的 ADC/TDM 导出均已存在。旧占位 pipeline 不代表这些实际页面未实现。

### 3.2 preview2 持续规则：不再只是留存事件标签

- `src/signal/spike_rule_classifier.h` 的 `SpikeWaveformBox/SpikeCandidateRule/SpikeRuleContext/SpikeRuleSet`：时间 ms 相对 causal crossing、电压为 input µV；一候选的多个盒用 AND。
- `src/signal/spike_rule_classifier.cpp:445–525` 的 `load/save/classify`：规则可持久化；零命中为 Unassigned，唯一命中为 Assigned，多候选命中为 Ambiguous；物理位点、采样率、gain、reference、滤波/阈值/窗口不符为 Incompatible。
- `src/service/spike_detect_worker.cpp:137–225` 的 `revisionMetadataLocked/applyConfigurationBoundary`：在处理边界应用请求快照，注册实际 detector/rule revision，并记录 `applied_epoch`、`applied_source_frame`、`applied_continuity_segment` 与 `boundary_semantics`。`:268–278` 给 pending crossing 固定快照/上下文；`:314–320` 在后窗完整后按该快照分类。因此规则编辑不会倒改先前 crossing；应用边界不再仅由 GUI 请求位置推测。
- `src/ui/widgets/spike_rule_editor.cpp:137–171,186–251`：盒绘制/数值编辑、JSON 草稿载入/保存、requested/applied 区分；`src/ui/spike_panel.cpp:1103–1129` 将服务的实际上下文和应用版本接到编辑器。
- 原有 `SpikeSortingWidget` 的散点框选仍是历史标注，**不**因为新增 rule editor 就变成 PCA 分类器。`SpikeSnippetStore::setCandidateUnit` 通过注释回调保存有归档身份的人工标签；未进入档案的留存事件仍只有窗口内标签。

### 3.3 preview2 事件档案：与显示 ring 解耦

- `src/signal/spike_event.h` 增加 `runId/eventId/detectorRevision/ruleRevision/classification`。
- `src/service/spike_detect_worker.cpp:228–245` 的 `publishCompletedEvents`：先赋稳定 ID，经 callback 送不可变事件/波形，再插入显示 ring。存储不从 ring 抓取数据，因此不会因 ring 淘汰而失去已接受的归档事件。
- `src/io/spike_event_archive.h` 的 writer/reader API 与 `.cpp:185–243,268–315`：有界队列、revision 先注册、显式终态和 finish marker；实时队列耗尽是 Failed，不悄悄截短后报成功。离线 writer 可阻塞等待。
- `src/io/spike_event_archive.cpp:459` 的 `readPage` 有分页/扫描预算；`:519–545` 的 `appendManualLabel(s)` 校验稳定身份并追加历史，保留原始自动分类/波形。
- `src/ui/widgets/spike_archive_browser.cpp:219–334`：重开、分页、标签追加与独立重验均在异步 worker；`src/ui/spike_panel.cpp:292–328` 将控件接入实际页面。实际页面端到端与独立 widget 测试均已通过，验证范围见第 5 节。
- 详见 [档案格式](spike_event_archive_format.md)、[规则格式](spike_rule_format.md)、[GUI 合约](spike_archive_rule_widgets.md)。冷启动重开/查询仍可能线性扫描，不能宣称大档案随机访问为常数时间；校验/恢复也不是断电 fsync 耐久性保证。

### 3.4 preview2 专用离线任务与停止顺序

- `src/service/spike_offline_analysis.cpp:81–112`：独立 ReplayController，恢复 manifest 的源 fs/TDM/validity，冻结本次配置，并新建 sidecar。
- `:130–166`：事件及 revision 使用 OfflineBlocking 写入；输入 `queue->push(..., false, ...)` 禁止丢弃，满时检查取消/失败并等待，不走 SessionHub 的实时订阅。
- `:173–203`：EOF 停 producer、请求 detector drain、检查 processed == total、再完成 writer；取消/读写故障分支独立。提交 accepted tail 时 `cancellable=false/finalizing=true`，避免宣称此时仍可取消。
- `src/service/session_hub.cpp` 的 `sourceDrained`，与 `src/ui/spike_panel.cpp:1085–1100` 的 `drainWorker`，构成普通停止/回放 EOF 的 producer → detector → writer 屏障；EOF 不完整后窗转为边界排除，不能伪造后续样本。
- 实时订阅仍有可淘汰策略：`src/service/session_hub.cpp:583`。**专用离线任务不丢软件输入块**，不能推导实时采集永不丢失，也不代表 source 原先就完整。

### 3.5 仍缺能力的核查

重新检索有效源码与构建列表中的 PCA/主成分、MAD/median absolute deviation、坏道/参考集合、PSTH、SpikeInterface/Kilosort/NWB/Phy 和插件 loader，并检查模型/控件：未发现可用实现。`SpikeEvent` 仍是单 lane 波形；`SpikeRuleSet` 是按单个物理电极兼容上下文匹配，没有多电极 waveform 特征。

`SorterWorker/DataSorter` 仍是帧解析、拆通道/显示处理，不是神经元聚类器。`StimTrigger::ExternalTTL` 枚举和 `SessionHub::TriggerConfig` 的电压触发录制，不能当作完整 TTL 事件采集与 PSTH。

## 4. 必须保留的边界

1. **TDM 每电极采样率仍为源帧率 / 4。** `src/ui/spike_panel.cpp:108–115`；默认源帧率 20 kHz 时每电极 5 kHz，Nyquist 2.5 kHz；`:437` 的低通上限为 `min(6000, fs×0.45)`，此时 2.25 kHz。分类器不能补回未采到的高频。
2. **仍只分析所选 512/1024 TDM 位点。** 四相 raw 都记录，不等于 detector 同时分析四相；专用离线任务也记录这一 scope，见 `spike_offline_analysis.cpp:92,106`。
3. **gain 仍为人工统一 60×/180×，未逐通道硬件验证。** 新规则/档案会固定其上下文，但保存参数不等于校准已正确。
4. **实时开档的 scope 有起点。** `src/service/neural_archive_controller.cpp:38–40,104–106` 明确只覆盖开档后输出的完整窗事件，`whole_source_coverage_claimed=false`。不能恢复开档前已淘汰记录；停止档案也不等于停止采集。
5. **Completed 是档案写入完成，不自动证明源完整。** `spike_offline_analysis.cpp:40–53` 分别报告缺失/无效/未验证、边界排除及处理总量。有效性未知的 raw BIN 可以处理，但不能变成“已验证完整”。损坏档案只能显示已验证前缀。
6. **实际 revision 可追溯，不等于自动重演所有旧会话配置。** 新事件能指向真正应用的参数/规则；离线请求仍用本次冻结的设置，保存的旧来源配置不会自动逐段回灌。跨 run 结果比较还须指定配置和身份映射。
7. **规则候选 ID 不是已证明的单神经元。** 盒选、少量真值测试和可重开档案建立的是功能正确性，不能替代真实记录上的单元隔离/漂移质量分析。

## 5. 最终软件验证结果

**最终结果：clean 构建完成；64/64 CTest 通过，零失败，总用时 254.02 秒。** 已核查 `build-preview2-final-clean.log` 无编译器 warning/error 诊断、`ctest-preview2-final-clean.log` 的终态，以及最终 `Testing/Temporary/LastTest.log` 的详细输出。

| 验证范围 | 最终确认结果 | 能说明什么 / 不能说明什么 |
|---|---|---|
| preview2 主程序与全部目标 | **clean 构建完成；64/64 测试通过** | 包括原有功能和新增神经档案相关回归；不是实体硬件或所有未来输入条件的保证 |
| 持续规则核心 | **116 个断言；240 个后续 held-out 合成事件**，目标通过 | 验证规则几何、兼容性和后续事件分类；不是 240 个真实神经元或跨数据集生物分选准确率 |
| Detector/规则 worker | **398 个断言**，含 **768 事件**跨块一致性及实际应用边界 | 验证事件身份、配置/规则边界与 chunk parity；不是硬件延迟/吞吐基准 |
| 归档、controller、offline、连续生命周期、源时间线 | **对应 5 个 CTest 目标全部通过** | 覆盖记录/重开、有效性、停止顺序和离线处理；具体场景以各测试源码和格式文档为准 |
| 专用 offline 的实际 Dummy 字节 | **32,000 帧、3,991 个归档事件、128 个显示留存事件** | 验证档案独立于有限 ring；Dummy 是模拟源，不是实体 ADC/电极验收 |
| 独立归档/规则 widget | **323 项检查，0 失败** | 覆盖控件、分页、规则应用/标签历史与异步生命周期；独立 widget 与实际页面验证分别保留 |
| 实际 Spike 面板 Dummy 神经档案端到端 | **1,631 项检查通过，12.51 秒** | 覆盖实际采集、超出显示留存的事件保存、后续分类/歧义、手工标签重开、EOF 排空、离线与回放逐事件/float 波形一致、取消及源文件哈希不变 |
| 完整主窗口与主题回归 | **full_app_visual_smoke 通过**；实际回放保存 **3,758 个事件**、应用规则 revision **1**；四套现有主题覆盖 | 验证原有完整窗口中的新功能和主题接线；不扩大为所有分辨率/平台均已人工验收 |

早期运行中的测试时序、输入长度及源坐标假设已修正并重跑；它们不再是当前未解决失败。最终公开判断以上述 clean 终态为准，不把中间的失败或先前 57 项基础套件当作当前总结果。断言数量与 CTest 目标数量不同，不相加。详见 [Dummy 神经档案集成覆盖](dummy_neural_archive_coverage.md)。

## 6. 下一步优先级与验收标准

### P0：保持本轮已通过的软件回归，补实体硬件验收

- 已通过的实际 GUI 用例继续作为发布回归：开档/停档、正常源停止、EOF、配置变更、seek、关闭重开、标签追加、规则草稿/请求/实际应用、取消和不可取消提交阶段。
- 独立 reader 重开核对 event ID、原始波形、revision、标签历史；超过 ring 容量、慢 writer、错误恢复后仍不把前缀误报完整。
- 同一 recording 的专用离线结果在不同块大小/慢盘条件下相同；对比定时回放时必须先核对覆盖范围与相同配置。保存源文件/manifest 哈希不变。
- dark/light 及已有主题、compact 布局、原有采集/录制/触发/SPI 页面继续回归。后续实体硬件要另测 gain、时钟、网络/磁盘持续吞吐、电极映射和刺激路径；没有硬件结果时明确列为未验证。

### P1：补 Open Ephys 在线分选仍缺的核心能力

- PCA 训练/投影、多边形规则、重新 PCA 后规则版本/兼容性；不要让训练样本直接充当评测样本。
- MED/MAD 噪声阈值、参考/作用通道分组与坏道掩码；验证坏道不会污染参考、噪声估计或电极映射。
- 1/2/4 通道 electrode group、联合 waveform、邻域去重；明确 TDM 非同时采样策略。验收重叠事件、边界、噪声水平、漂移以及 512/1024 覆盖。

### P2：实验事件与神经分析工作流

- 先建可录制/回放的 TTL/消息源坐标事件，再做 trial/raster/PSTH；用已知 TTL 时间、不同 bin 宽度和未完成试次验证。
- 增加统一信号 QC、工频/频带统计、spectrogram 与事件对齐 LFP，保留现有 ADC 性能分析。
- 在完整档案上做全会话 firing rate、ACG/CCG、幅度/稳定性和人工审查；不能用短留存窗口代替长期指标。

### P3：标准适配与自动离线分选

- 先实现 SpikeInterface Recording adapter，完整表达 fs/gain/offset/单位、geometry、bad channels、segment 和 gap，再接 CPU sorter 或条件合适时的 Kilosort。[SI-1][KS-1]
- 同时明确哪些预处理已做，避免双重滤波/参考；输出标准 unit/spike 数据并保留算法版本、配置、输入校验值和人工修改历史。
- 用 ground truth 指定匹配容差，报告 precision/recall、agreement、过分裂/合并和未匹配单位。SpikeInterface 默认匹配容差 0.4 ms；没有真值时只能报告一致性/QC。[SI-2]

## 7. 标准数据如何用

- **单通道 Quiroga**：适合阈值/噪声/重叠及单通道规则测试。复制成 256 路只验证传输和部分负载，不会形成多电极 ground truth。[DATA-1]
- **小型 MEArec**：官方 `mearec_test_10s.h5` 为 32 通道、32 kHz、10 s、10 真值单位；适合多通道导入、匹配和回归。39.06 MiB 是 float32 trace 体积，不是完整下载大小。10 s 不够验证长期漂移。[DATA-2]
- **不同噪声/漂移的长记录**：使用分开的 seeded 合成集，再加 paired 真实记录。合成、hybrid 注入和只标注少数细胞的 paired truth 分开评价。[DATA-3]
- preview2 已能执行用户定义盒规则，但仍没有自动发现/拟合所有神经元的 sorter。不能把注入的 truth 标签作为预测结果，也不能用“事件成功写盘”代替分选准确率。
- 软件测试不验证实体 FPGA 时序、网络极限、硬件 gain、ADC 噪声、电极连接、刺激安全或生物单元隔离。

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
