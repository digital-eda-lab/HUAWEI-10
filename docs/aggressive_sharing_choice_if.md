# 面向 AIG 节点数优化的 choice + if 激进共享方案

## 1. 背景和目标

当前实验中，下列综合流对部分 case 的 AIG 节点数有明显收益：

```abc
strash;
rewrite;
&get -nm;
&synch2;
&choice;
&st;
&if -a -K 6 -C 16 -F 2 -A 3;
&put;
strash;
ps
```

对比结果表明：

```text
&synch2; &choice; &st; &if ...
优于
&synch2;          &st; &if ...
优于
                  &st; &if ...
```

这说明目标 case 中不仅存在结构完全相同的共享逻辑，也存在大量需要通过等价重构、choice 选择、cut 选择之后才能暴露的潜在共享。

本文档的目标是讨论如何在现有 `&synch2 + &choice + &if` 框架下，更激进地寻找共享逻辑，并使最终：

```abc
&put; strash; ps
```

得到更少的 AIG 节点数。

需要注意的是，`&if` 原本的主要目标是 FPGA LUT mapping，优化指标偏向 LUT area、delay、area-flow 和 exact-area recovery。最终 AIG 节点数只是经过 `&put; strash` 后的间接结果。因此，如果要让 `&if` 更服务于 AIG 节点数，需要在 cut ranking、cut pruning、area recovery 中加入 AIG-aware 和 sharing-aware 的代价引导。

## 2. choice + if 中共享机会来自哪里

### 2.1 `&st` 能看到的共享

`&st` 做的是 structural hashing，它只能合并结构完全相同的节点。例如：

```text
n1 = AND(a, b)
n2 = AND(a, b)
```

这种节点 fanin 和极性完全一致，可以直接合并。

但对于下面这种情况：

```text
f1 = (a & b) | (a & c)
f2 = a & (b | c)
```

两者功能等价，但结构不同。`&st` 不会证明布尔等价，因此看不到这种共享。

### 2.2 `&synch2` 和 `&choice` 暴露的共享

`&synch2` 和 `&choice` 的核心价值是建立 structural choices。一个逻辑函数 `F` 可能对应多个不同结构的 GIA root：

```text
F:
  n   原始实现
  n1  choice 实现 1
  n2  choice 实现 2
  n3  choice 实现 3
```

它们满足：

```text
n == n1 == n2 == n3
```

但结构不同，可能有不同的 fanin、level、cut support 和共享边界。

`&choice` 主要通过随机仿真分桶和 SAT 等价验证寻找已有等价节点；`&synch2` 更偏主动生成和同步适合下游 LUT mapping 的等价结构。两者共同提供了更大的等价实现空间。

### 2.3 `&if` 如何利用 choice

普通 AIG 上，`&if` 对逻辑点 `F` 只能枚举原始 root 的 cuts：

```text
cuts(F) = cuts(n)
```

有 choices 之后，`&if` 可以把多个等价实现的 cuts 合并到候选池：

```text
cuts(F) = cuts(n) + cuts(n1) + cuts(n2) + cuts(n3)
```

因此同一个逻辑函数可以选择不同结构对应的 cut。面积优先模式下，`&if -a` 会更偏向 area-flow 和 exact-area 更低的 cut，但它并不知道最终 `&put; strash` 后的 AIG 节点数。

所以当前 flow 的收益来自：

```text
choice 扩大候选结构空间
+ &if 选择更好的 LUT cut
+ &put/strash 重新分解和哈希后暴露更多 AIG 共享
```

## 3. 当前 `&if` 对 AIG 节点数优化的局限

### 3.1 LUT 面积不等于 AIG 节点数

一个 cut 对应一个 K-LUT，但最终转回 AIG 时，这个 LUT 函数需要重新分解。两个 cut 可能 LUT area 相同，但 AIG 分解代价不同：

```text
cut A:
  1 个 6-LUT
  重新分解后需要 8 个 AND 节点

cut B:
  1 个 6-LUT
  重新分解后需要 4 个 AND 节点
```

如果只看 LUT area，二者等价；如果目标是 AIG 节点数，应该优先选择 `cut B`。

### 3.2 原始 cone 节点数也不是可靠指标

不能简单使用“cut 覆盖的原始 AIG 节点数少”作为目标。因为一个覆盖较大 cone 的 cut，可能对应一个更容易分解的布尔函数，最终 AIG 反而更小。

更合理的指标应该是：

```text
estimated_aig_decomp_cost(cut)
```

即该 cut 的布尔函数重新分解成 AIG 的估计代价。

### 3.3 单输出 cut 容易错过跨输出共享

当前 `&if` 基本按单个 root 选择 cut。两个 root 如果有相似函数或共同 support，单输出选择可能分别得到局部最优，却错过联合共享。

例如：

```text
F1 = t & c
F2 = t & d
t  = a & b
```

如果单输出分解没有显式提取 `t`，后续 `strash` 未必能稳定合并出最优共享。

## 4. 激进寻找共享的设计方向

建议按风险从低到高逐步实现。

### 4.1 方向一：在 cut ranking 中加入 sharing score

为每个候选 cut 计算一个共享倾向分数：

```text
sharing_score(cut)
```

可以包含：

```text
1. cut leaf 的 fanout 数量
2. cut leaf 是否已被当前 mapping 使用
3. cut support 与其他 selected cuts 的重叠程度
4. cut 是否来自 choice 实现
5. cut 是否复用高引用的中间节点
6. cut support 是否更小、更集中
```

初始阶段建议只作为 tie-breaker：

```text
如果原始 if cost 差异小于 epsilon：
    选择 sharing_score 更高的 cut
```

后续可以升级为加权 cost：

```text
cost(cut) =
    original_if_cost(cut)
  - lambda * sharing_score(cut)
```

这种改动风险较低，不会大幅破坏原有 `&if` 的面积和时序策略。

### 4.2 方向二：避免 choice cuts 被过早剪掉

`&if -C 16` 表示每个节点最多保留 16 个 priority cuts。当前 pruning 如果主要按 delay、area-flow 排序，一些对共享有价值的 choice cuts 可能早期就被剪掉。

可以将 cut slots 分桶保留：

```text
16 个 cut slots:
  4 个 delay 最优 cuts
  4 个 area-flow 最优 cuts
  4 个 choice-derived cuts
  4 个 high-sharing-score cuts
```

或者采用软约束：

```text
每个有 choice 的 root 至少保留若干来自 choice nodes 的 cuts
```

这样比单纯增大 `-C` 更有针对性。直接把 `-C 16` 改成 `-C 32` 会增加 runtime 和内存，但不保证新增 cuts 都对共享有帮助。

### 4.3 方向三：在 exact-area recovery 中加入共享收益

`&if -A 3` 会做 exact-area recovery。当前核心是评估替换 cut 后 LUT 面积是否下降。可以扩展为：

```text
gain =
    removed_estimated_aig_nodes
  + shared_boundary_bonus
  - added_estimated_aig_nodes
  - delay_penalty
```

替换 cut 时，不只看当前节点的 LUT area，还看：

```text
1. 新 cut 的 leaves 是否已经被其他 selected cuts 使用
2. 新 cut 是否让多个 root 形成相同或相近 support
3. 旧 cut 移除后哪些节点会失去引用
4. 新 cut 引入后是否增加可共享边界
5. 替换是否增加关键路径 level
```

如果 `gain > 0`，接受替换。

这一步比 cut ranking 更激进，因为它直接改变 recovery 的接受准则，更贴近最终 AIG 节点数目标。

### 4.4 方向四：引入 AIG-aware cut cost

对每个 K-input cut，估计其布尔函数分解成 AIG 的代价：

```text
estimated_aig_decomp_cost(cut)
```

因为当前使用 `-K 6`，cut 函数最多 6 输入，truth table 规模可控。可以尝试：

```text
1. 对 cut truth table 做简单 AIG 分解估计
2. 统计分解后 AND 节点数
3. 对常见结构如 MUX、XOR、AND/OR tree 给更准确代价
4. 将结果缓存到 cut 上，避免重复计算
```

新的综合 cost 可以写成：

```text
cost(cut) =
    alpha * lut_area_cost
  + beta  * estimated_aig_decomp_cost
  - gamma * sharing_score
  + delta * delay_penalty
```

如果主要目标是 AIG 节点数，可以逐渐增大 `beta` 和 `gamma`。

### 4.5 方向五：choice-aware support clustering

共享通常不只发生在一个 root 内部，而是发生在多个相关 root 之间。可以在 mapping 前或 recovery 中对 root 做轻量聚类：

```text
1. 收集每个 root 的候选 cuts
2. 计算 candidate support overlap
3. 对 overlap 高的 roots 建立 sharing group
4. 在 group 内鼓励选择 support 相近的 cuts
```

例如：

```text
F1 candidate cuts:
  {a,b,c,d}
  {p,q,r,s}

F2 candidate cuts:
  {a,b,c,e}
  {x,y,z}
```

普通 `&if` 可能分别选择局部最优 cut；sharing-aware 策略会倾向：

```text
F1 -> {a,b,c,d}
F2 -> {a,b,c,e}
```

因为二者共享 `{a,b,c}`，后续分解更容易提取公共逻辑。

### 4.6 方向六：post-mapping 的 pairwise multi-output sharing recovery

不建议一开始实现完整双输出 LUT mapper，因为搜索空间会明显膨胀。更可控的方案是在正常 `&if` 完成后，做局部 pairwise recovery：

```text
1. 找 selected LUT/root 的候选 pair
2. 判断二者 support 交集是否较大
3. 判断 truth table 是否有共同 divisor
4. 估计单独分解代价和联合分解代价
5. 如果联合分解更小，则接受替换
```

候选 pair 可以来自：

```text
1. support overlap 高的两个 selected cuts
2. 拓扑距离近的 sibling roots
3. 同一个 PO cone 内的相邻 roots
4. 共享同一组控制信号的 MUX 结构
5. arithmetic bit-slice 中相邻位的逻辑
```

代价判断：

```text
gain =
    cost_aig(F1) + cost_aig(F2)
  - cost_aig_shared(F1, F2)
```

只有当 `gain > 0` 且 delay 不明显恶化时，才接受。

这本质是 multi-output AIG-aware sharing recovery，而不是单纯 FPGA 双输出 LUT packing。后者减少的是物理 LUT cell 数，不一定能稳定降低 `strash; ps` 的 AIG 节点数。

## 5. 推荐实现路线

### 5.1 第一阶段：低风险 tie-breaker

目标：验证 sharing-aware 信息是否能继续降低 AIG node。

实现内容：

```text
1. 在 cut 结构中增加 sharing_score 字段
2. 在 cut ranking 时计算 sharing_score
3. 当原始 cost 接近时，用 sharing_score 打破平局
4. 增加 verbose 统计，打印被 sharing tie-break 改选的次数
```

推荐实验：

```abc
&synch2; &choice; &st; &if -a -K 6 -C 16 -F 2 -A 3; &put; strash; ps
&synch2; &choice; &st; &if_new -a -K 6 -C 16 -F 2 -A 3; &put; strash; ps
```

需要记录：

```text
1. 最终 AIG node
2. level
3. runtime
4. 被 sharing tie-break 改选的 cut 数量
5. 改选来自 choice cut 的比例
```

### 5.2 第二阶段：choice cut 保留策略

目标：避免有价值的 choice cuts 在 `-C` pruning 时被过早删除。

实现内容：

```text
1. 标记 cut 是否来自 choice node
2. 标记 cut 的 sharing_score
3. 修改 priority cut 保留策略
4. 至少保留若干 choice-derived / high-sharing cuts
```

建议先使用保守参数：

```text
每个 root 至少保留 2 个 choice-derived cuts
每个 root 至少保留 2 个 high-sharing cuts
```

如果效果好，再提高比例。

### 5.3 第三阶段：AIG-aware cost

目标：让 cut 选择更直接服务于 `&put; strash; ps`。

实现内容：

```text
1. 对 6-input cut truth table 估计 AIG 分解代价
2. 缓存 estimated_aig_decomp_cost
3. 在 cost 中加入 beta * estimated_aig_decomp_cost
4. 增加命令参数控制 beta
```

建议新增参数类似：

```abc
&if -AIG 1 -BETA 1
```

或者在自定义命令中实现，避免污染原始 `&if` 行为。

### 5.4 第四阶段：sharing-aware exact recovery

目标：在 PO 到 PI 的真实面积恢复过程中，使用更贴近 AIG 节点数的增量收益。

实现内容：

```text
1. 基于当前 selected mapping 建立 ref_count
2. 评估替换 cut 的 added/removable AIG cost
3. 加入 shared_boundary_bonus
4. 如果 gain 为正则接受替换
5. 支持多轮迭代
```

这一阶段风险较高，需要重点监控 level 和 runtime。

### 5.5 第五阶段：pairwise multi-output recovery

目标：捕获单输出 cut 难以发现的跨 root 共享。

实现内容：

```text
1. 在 mapping 后收集 selected cuts
2. 生成候选 root pair
3. 对 pair 做共同 support / divisor 分析
4. 评估联合分解是否减少 AIG 节点
5. 接受收益明确的局部替换
```

该阶段适合针对剩余难优化 case 做增强。

## 6. 实验指标和 A/B 方法

推荐固定 baseline：

```abc
strash;
rewrite;
&get -nm;
&synch2;
&choice;
&st;
&if -a -K 6 -C 16 -F 2 -A 3;
&put;
strash;
ps
```

每次只打开一个新特性，避免多个启发式叠加后无法定位收益来源。

建议记录：

```text
1. AIG node count
2. AIG level
3. runtime
4. memory
5. &choice 产生的 choice 数量
6. &if 保留的 cut 数量
7. choice-derived cut 被选中的数量
8. sharing tie-break 触发次数
9. AIG-aware cost 改选次数
10. exact recovery 接受的 sharing 替换次数
```

如果某个 case AIG node 下降但 level 大幅上升，需要单独标记。对于最终目标只看面积的 case，可以允许小幅 level 增加；如果有时序约束，则需要加入 delay penalty。

## 7. 主要风险

### 7.1 局部共享不等于全局最优

某个 cut 的 sharing_score 高，不代表最终 AIG 一定小。它可能引入复杂函数，导致重新分解代价变大。

应对方式：

```text
先作为 tie-breaker 使用
后续引入 estimated_aig_decomp_cost 抵消复杂函数风险
```

### 7.2 choices 过多会增加开销

更激进保留 choice cuts 会增加 cut 枚举、排序、recovery 的时间和内存。

应对方式：

```text
限制每类 cut slot 数量
只对有明显共享潜力的 root 启用
通过 verbose 统计观察有效改选比例
```

### 7.3 AIG cost 估计可能不准确

cut truth table 的局部分解代价和最终 `strash` 后的全局节点数仍有差距。

应对方式：

```text
将 AIG cost 与 sharing_score、ref_count 结合
不要单独依赖局部分解代价
```

### 7.4 multi-output recovery 搜索空间大

任意 root pair 都做联合分析会不可控。

应对方式：

```text
只分析 support overlap 高、拓扑距离近、同 PO cone 内的候选 pair
设置 pair 数量上限
只接受 gain 明确为正的替换
```

## 8. 总结

当前实验已经说明 `&synch2 + &choice + &if` 对部分 case 有明显帮助，原因是它能把原始 AIG 中隐藏的功能等价结构暴露给 mapper。

下一步不应只是盲目增加 choices 或增大 `-C`，而应让 `&if` 的决策更偏向共享：

```text
1. cut ranking 中加入 sharing_score
2. cut pruning 中保留 choice-derived / high-sharing cuts
3. exact recovery 中加入 AIG-aware sharing gain
4. 对 cut truth table 估计 AIG 分解代价
5. 对相关 root 做 pairwise multi-output sharing recovery
```

推荐从低风险的 sharing tie-breaker 开始，实现简单、可解释性强，也方便通过 A/B 实验确认收益。如果该方向有效，再逐步推进 AIG-aware cost 和 multi-output recovery。
