
## 1. 总体目标

原始 ABC `rewrite` 的候选选择主要按局部 `Gain` 决策：

```text
Gain = nNodesSaved - nNodesAdded
```

当启用 `rewrite -z` 时，`Gain == 0` 的替换也允许写回网络。传统逻辑在这些 zero-gain 候选之间没有全局共享视角，因此可能错过一些后续能复用的中间结构。

本地魔改新增 `rewrite_share`，核心思路是：

1. 先枚举各 root 的 rewrite 候选，但不立即改网。
2. 对候选会新增的 AND 结构生成 canonical signature。
3. 统计这些 signature 在候选集合中的重复频次。
4. 在局部 `Gain` 不变的前提下，优先选择共享分数更高的候选。
5. 用阈值、score mode、profile refresh 等参数控制激进程度。

一句话概括：`rewrite_share` 是一套 shared-aware zero-cost AIG rewriting，主要改进 zero-gain 或 equal-gain 候选的选择策略。

## 2. 命令层改动

### 2.1 相关命令注册

入口在 `abc/src/base/abci/abc.c`：

```c
Cmd_CommandAdd( pAbc, "Synthesis", "rewrite_profile", Abc_CommandRewriteProfile, 1 );
Cmd_CommandAdd( pAbc, "Synthesis", "rewrite_share",   Abc_CommandRewriteShare,   1 );
Cmd_CommandAdd( pAbc, "Various",   "save",            Abc_CommandAbcSave,        0 );
Cmd_CommandAdd( pAbc, "Various",   "load",            Abc_CommandAbcLoad,        0 );
Cmd_CommandAdd( pAbc, "Various",   "aigstore",        Abc_CommandAigStore,       0 );
Cmd_CommandAdd( pAbc, "Various",   "aigrestore",      Abc_CommandAigRestore,     0 );
```

相关命令分为三类：

- `rewrite_profile` / `rewrite_share`：rewrite 相关算法和统计。
- `save` / `load`：mapped network 的 best design 保存/恢复。
- `aigstore` / `aigrestore`：strashed AIG 空间中的临时网络工作区。

### 2.2 `rewrite_profile`

命令形式：

```abc
rewrite_profile [-K num] [-lzvwh]
```

用途是只 profile rewrite 候选，不修改 AIG。它主要用于观察：

- 有多少候选被枚举。
- positive / zero / negative gain 候选数量。
- zero-gain root 中，原始 best-local 和 max-sharing 候选是否不同。
- 候选新增 signature 的重复情况。

该命令调用：

```c
Abc_NtkRewriteProfile(...)
Rwr_NodeRewriteProfile(...)
Rwr_ProfileFinalize(...)
Rwr_ProfilePrint(...)
```

### 2.3 `rewrite_share`

命令形式：

```abc
rewrite_share [-K num] [-S 0/1] [-T num] [-M mode] [-R num] [-D num] [-G 0/1] [-lzvwh]
```

常用推荐配置：

```abc
rewrite_share -z -K 20 -S 1 -T 2 -M 1 -R 1 -v
```

参数含义：

| 参数 | 默认 | 含义 |
| --- | ---: | --- |
| `-K <num>` | `5` | profile 阶段每个 root 最多保留的候选数。 |
| `-S <0/1>` | `1` | 是否启用 sharing-aware selection。 |
| `-T <num>` | `0` | BestShare 覆盖 BestLocal 所需的最小 sharing-score delta。 |
| `-M <mode>` | `1` | sharing score 计算模式。 |
| `-R <num>` | `1` | profile block 数量。`R=1` 为 static profile。 |
| `-D <num>` | off | 启用 destroyed-share probe，并设置 high-destroy 阈值。 |
| `-G <0/1>` | `0` | 对正增益 equal-gain 候选也启用 sharing tie-break。 |
| `-l` | on | toggle 是否保持 level。 |
| `-z` | off | 允许 zero-gain rewrite 写回网络。 |
| `-v` | off | 打印 rewrite-share summary。 |
| `-w` | off | 为兼容 `rewrite` 接收，当前内部不使用。 |

输入约束和原始 `rewrite` 基本一致：

- 当前网络不能为空。
- 当前网络必须是 strashed AIG。
- 当前 AIG 不能有 choice nodes。

命令入口会先复制一份当前网络。如果内部更新失败返回 `-1`，会恢复原网络，避免留下半更新状态。

## 3. 核心源码结构

### 3.1 `abcRewriteShare.c`

路径：

```text
abc/src/base/abci/abcRewriteShare.c
```

这是 `rewrite_share` 的顶层 pass 控制文件，主要函数：

```c
static int Abc_NtkRewriteShareRootValid( Abc_Obj_t * pNode );
static Vec_Int_t * Abc_NtkRewriteShareCollectRoots( Abc_Ntk_t * pNtk );
static Rwr_Profile_t * Abc_NtkRewriteShareBuildProfile(...);
static int Abc_NtkRewriteShareRewriteOne(...);
int Abc_NtkRewriteShare(...);
```

它负责：

1. 过滤可处理 root。
2. 构建 profile。
3. 创建 rewrite 阶段 cut manager。
4. 调用 `Rwr_NodeRewriteShare()` 选择候选。
5. 调用 `Dec_GraphUpdateNetwork()` 写回网络。
6. 处理 `R=1` static profile 和 `R>1` block-wise profile refresh。
7. 结束后重新分配 ID、修正 level 并执行 `Abc_NtkCheck()`。

### 3.2 `rwrShare.c` / `rwrShare.h`

路径：

```text
abc/src/opt/rwr/rwrShare.c
abc/src/opt/rwr/rwrShare.h
```

这是 shared-aware 候选选择的核心实现，主要函数：

```c
int Rwr_NodeRewriteShare(...);
void Rwr_ShareProbeDestroyedMffc(...);
void Rwr_ShareStatsPrint(...);
```

关键结构是 `Rwr_ShareBest_t`，用于保存某个 root 下当前最佳候选：

```c
struct Rwr_ShareBest_t_
{
    int          fSet;
    int          Gain;
    int          Share;
    int          ExistingReuse;
    int          nNodesSaved;
    int          nNodesAdded;
    int          fCompl;
    unsigned     uTruth;
    long long    CandId;
    Dec_Graph_t * pGraph;
};
```

每个 root 会同时维护：

- `BestLocal`：传统局部最优候选，主要看最大 `Gain`。
- `BestShare`：共享分数优先的候选，在同 gain 条件下比较 `Share` 和 `ExistingReuse`。

实际选择逻辑：

```text
默认选择 BestLocal。

如果 BestLocal.Gain == 0：
    当 -S 1 且 BestShare 与 BestLocal 不同，且 ShareDelta >= T：
        改选 BestShare。

如果 -G 1 且 BestLocal.Gain > 0：
    当 BestShare.Gain == BestLocal.Gain，且 ShareDelta >= T：
        改选 BestShare。
```

这样可以避免为了共享而牺牲面积收益，因为负增益候选会被过滤，正增益更低的候选也不会覆盖更高 gain 的本地最优。

### 3.3 `rwrEva.c`

路径：

```text
abc/src/opt/rwr/rwrEva.c
```

本地改动在这里新增 profile 相关结构和函数：

```c
Rwr_ProfileCand_t
Rwr_ProfileRoot_t
Rwr_Profile_t

Rwr_ProfileStart(...)
Rwr_ProfileFinalize(...)
Rwr_ProfilePrint(...)
Rwr_NodeRewriteProfile(...)
Rwr_ProfileGraphAddedSigs(...)
Rwr_ProfileScoreAddedSigsMode(...)
```

profile 的职责是：

1. 给已有 AIG 结构生成 signature，并标记为 existing。
2. 对候选分解图生成新增 signature。
3. 只统计候选新增、而当前 AIG 中不存在的 signature。
4. 统计 signature 在候选集合中的频率和跨 root 出现情况。
5. 为 `rewrite_share` 提供 sharing score。

## 4. Signature 和 Sharing Score

### 4.1 Signature 建模

profile 使用轻量级结构 signature 表示候选图中的 AND 结构：

- 叶子 signature：`Type = 0, A = ObjId`
- AND signature：`Type = 1, A = Lit0, B = Lit1`

AND 输入会排序，因此 commutative 等价的 AND 会映射到同一个 signature。

候选图新增 signature 的计算逻辑：

```text
candidate_internal_and_sigs - existing_aig_sigs
```

也就是说，如果候选图中的某个 AND 结构已经存在于当前 AIG 中，它不会作为新增共享机会计分；真正计分的是“候选可能新增、且其他候选也可能新增”的结构。

### 4.2 Score mode

`rewrite_share` 目前支持三种分数模式：

| 模式 | 名称 | 公式 |
| ---: | --- | --- |
| `M=1` | raw frequency | `sum(max(0, freq(sig) - 1))` |
| `M=2` | root frequency | `sum(max(0, root_freq(sig) - 1))` |
| `M=3` | normalized root frequency + existing reuse | `100 * raw_root_share / max(1, added_count) + 20 * existing_reuse` |

含义：

- `M=1` 统计 signature 在所有保留候选中的重复出现次数，当前实验中最稳定。
- `M=2` 按 root 去重后统计跨 root 复用，更保守。
- `M=3` 加入归一化和已有节点复用奖励，但当前 benchmark slice 上不够稳定。

## 5. Profile Refresh

`-R` 控制 profile block 数量。

### 5.1 `R=1`

`R=1` 是 static profile：

```text
1. 基于 pass 开始时的 AIG 构建一次 profile。
2. 使用该 profile 遍历并 rewrite 当前 AIG。
```

优点：

- 行为稳定。
- 运行开销低。
- 当前实验中无回退配置基于 `R=1`。

### 5.2 `R>1`

`R>1` 是 block-wise refresh：

```text
1. 先收集 pass 开始时的 root ID 列表。
2. 把 root 列表按顺序切成 R 个 block。
3. 每个 block 开始前，基于当前 AIG 重新构建全局 profile。
4. 只 rewrite 当前 block 中的原始 roots。
```

这样可以让后半段 rewrite 看到前半段已经改动后的 AIG 状态。实验显示它能带来更大的面积收益，但也更容易引入小回退。

## 6. Destroy-Share Probe

`-D <num>` 启用 destroyed-share probe。该功能不会直接改变候选选择，而是用于诊断：

```text
本次 rewrite 删除的 MFFC 里，是否包含原本在 profile 中高频、跨 root 复用的结构。
```

统计项包括：

- deleted MFFC nodes
- destroy raw score
- destroy root score
- deleted cross-root sig occurrences
- high-destroy roots
- positive / zero rewrite 下的 destroy 分布

用途是分析“新增共享收益”和“破坏已有共享结构”之间的冲突，为后续设计 guard 或 adaptive policy 提供依据。

## 7. 保存/恢复类命令

当前项目里有两套容易混淆的保存/恢复命令：

- `save/load`：面向 mapped network，使用 `pAbc->pNtkBest` 保存当前最优 mapped design。
- `aigstore/aigrestore`：面向 strashed AIG，使用 `pAbc->pNtkAigStore[]` 保存多个普通 AIG 副本。

### 7.1 `save` / `load`

命令形式：

```abc
save [-a] [-h]
load [-h]
```

实现位置：

```text
abc/src/base/abci/abc.c
    Abc_CommandAbcSave()
    Abc_CommandAbcLoad()
```

`save` 的语义：

1. 当前网络必须存在。
2. 当前网络必须有 mapping，即 `Abc_NtkHasMapping(pAbc->pNtkCur)` 为真。
3. 调用 `Abc_NtkCompareWithBest()` 比较当前网络和 `pAbc->pNtkBest`。
4. 如果当前网络更优，则用 `Abc_NtkDup()` 保存到 `pAbc->pNtkBest`。

比较策略：

- 默认 delay 优先，delay 相同再比较 area。
- 加 `-a` 后 area 优先，area 相同再比较 delay。

`load` 的语义：

1. 要求 `pAbc->pNtkBest` 已经存在。
2. 删除当前网络 `pAbc->pNtkCur`。
3. 复制 `pAbc->pNtkBest` 作为当前网络。

注意：`save/load` 保存的是 mapped network，不适合直接配合普通 strashed AIG 上的 `rewrite`、`rewrite_share`、`refactor` 等命令做多路 AIG 暂存。这也是后来增加 `aigstore/aigrestore` 的原因。

### 7.2 `aigstore` / `aigrestore`

本地新增了传统 ABC AIG 空间的临时工作区命令：

```abc
aigstore [-i num] [-c] [-h]
aigrestore [-i num] [-a] [-h]
```

相关改动：

- `abc/src/base/main/mainInt.h` 新增 `ABC_AIG_WORKSPACE_NUM 3`。
- `Abc_Frame_t` 新增 `pNtkAigStore[3]`。
- `abc/src/base/main/mainFrame.c` 在 frame 释放时清理这些网络副本。
- `abc/src/base/abci/abc.c` 实现 `Abc_CommandAigStore()` 和 `Abc_CommandAigRestore()`。

命令语义：

- `aigstore -i N`：把当前 strashed AIG 复制到 slot `N`。
- `aigstore -i N -c`：清空 slot `N`。
- `aigrestore -i N`：如果 slot `N` 的网络比当前网络更优，则恢复。
- `aigrestore`：扫描所有 slot，恢复其中最优网络。
- `aigrestore -a`：面积优先比较；默认 level 优先。

这套命令用于在普通 ABC AIG 网络空间中暂存中间结果，不进入 ABC9/GIA 的 `&` 空间，也不写磁盘文件。

## 8. 构建接入

新增源码已经接入 make 模块：

```text
abc/src/base/abci/module.make
    + src/base/abci/abcRewriteShare.c

abc/src/opt/rwr/module.make
    + src/opt/rwr/rwrShare.c
```

公共声明也已补充：

```text
abc/src/base/abc/abc.h
    Abc_NtkRewriteShare(...)

abc/src/opt/rwr/rwr.h
    Rwr_ProfileStart(...)
    Rwr_ProfileStop(...)
    Rwr_ProfileFinalize(...)
    Rwr_ProfilePrint(...)
    Rwr_NodeRewriteProfile(...)
```

`rwrShare.h` 则暴露 shared-aware rewrite 和 profile 评分需要的接口。

## 9. 实验框架

实验目录：

```text
abc/experiments/rewrite_share_sweep/
```

主要文件：

| 文件 | 作用 |
| --- | --- |
| `README.md` | 参数、流程、输出说明。 |
| `run_sweep.py` | 运行 ABC，生成 `.abc` script、log、输出 AIG、CSV 和 report。 |
| `parse_logs.py` | 从已有 log 重新生成 summary/report。 |
| `tc_public_1_15_conclusion.md` | `tc_public_1..15` score mode sweep 结论。 |
| `profile_refresh_R_conclusion.md` | `R=1,2,4,8` profile refresh 扫描结论。 |

实验 harness 比较两类 flow：

- `single_command`：直接比较 `rewrite -z` 和 `rewrite_share -z ...`。
- `short_flow_no_dc2`：比较短局部 flow 中替换 rewrite 后的效果。

每个成功输出都会跑：

```abc
cec -n <input> <output>
```

CEC 结果记录为 `equivalent`、`not_equivalent`、`cec_failed`、`timeout` 或 `cec_not_run`。

## 10. 当前实验结论

基于 `tc_public_1` 到 `tc_public_15` 的现有结论，当前稳定默认候选为：

```text
rewrite_share -z -K 20 -S 1 -T 2 -M 1 -R 1 -v
```

在 `short_flow_no_dc2` 上的汇总结果：

| 指标 | 值 |
| --- | ---: |
| sum delta AND | `-201` |
| avg delta AND | `-13.40` |
| improved cases | `4` |
| regressed cases | `0` |
| max AND regression | `0` |
| sum delta level | `-1` |
| avg actually_changed | `42.60` |

主要变化 case：

| benchmark | delta AND | delta level |
| --- | ---: | ---: |
| `tc_public_3` | `-1` | `0` |
| `tc_public_11` | `-64` | `0` |
| `tc_public_13` | `-61` | `0` |
| `tc_public_14` | `-75` | `-1` |

其他观察：

- `M=1,K12,T2` 和 `M=1,K10,T2` 总面积略好，但存在小回退。
- `M=2,K20,T2` 更保守，无回退但总收益更低。
- `M=3` 在当前 benchmark slice 上不建议推广。
- `R>1` profile refresh 能显著增强 `tc_public_14`，但会引入 `tc_public_3` / `tc_public_4` 的小回退。

因此当前建议：

```text
默认使用 M=1, K=20, T=2, R=1。
R>1 暂时作为后续带 guard/adaptive policy 的实验方向，不直接替换默认。
```

## 11. 和原始 ABC rewrite 的差异

| 项目 | 原始 `rewrite` / `rewrite -z` | 本地 `rewrite_share` |
| --- | --- | --- |
| 候选枚举 | 每个 root 枚举 cut/subgraph | 复用原有枚举方式 |
| 主目标 | 最大局部 gain | 保持 gain 优先，同时考虑共享分数 |
| zero-gain 处理 | 可接受，但没有共享选择 | zero-gain 下可用 BestShare 覆盖 BestLocal |
| 正增益同 gain | 默认不看共享 | `-G 1` 时可用 sharing tie-break |
| 全局统计 | 无 | profile signature frequency/root frequency |
| profile refresh | 无 | `-R` block-wise refresh |
| 诊断信息 | rewrite stats | rewrite-share summary + destroy-share probe |
| 实验 harness | 无专用 sweep | 新增参数 sweep、CEC、CSV/report |

## 12. 风险和注意事项

1. 当前改动还在 working tree 中，尚未提交。
2. `rewrite_share` 增加了 profile pass，运行时间高于普通 `rewrite`。
3. `-K` 越大 profile 越充分，但内存和时间开销越高。
4. `-R>1` 会更激进，当前已有实验显示可能引入小回退。
5. `-G 1` 会扩展到正增益 equal-gain tie-break，需要更多 benchmark 验证。
6. `-D` 目前是诊断功能，不直接阻止高 destroy 的 rewrite。
7. `M=3` 的阈值尺度和 `M=1/M=2` 不可直接比较。

## 13. 推荐后续工作

短期建议：

1. 固化当前 no-regression 配置：`rewrite_share -z -K 20 -S 1 -T 2 -M 1 -R 1`。
2. 扩大 benchmark 范围，确认 `tc_public_1..15` 以外是否仍无回退。
3. 对 `R>1` 增加 guard，尝试保留 `tc_public_14` 的大收益，同时避免小 case 回退。
4. 将 `-D` 的 destroy-share 统计纳入选择策略，例如对高 destroy rewrite 加惩罚。
5. 在完整 flow 中验证，而不仅是 `single_command` / `short_flow_no_dc2`。

相关详细文档：

- `docs/rewrite_profile.md`
- `docs/rewrite_share_current_algorithm.md`
- `docs/rewrite_share_technical_note.md`
- `docs/rewrite_share_ST_controls.md`
- `docs/rewrite_share_progress.md`
- `docs/aig_workspace_plan.md`
