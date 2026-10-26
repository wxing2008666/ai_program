# 内存序生产落地手册

本文面向需要把无锁/低延迟代码放进生产环境的工程师。重点不是“怎么写一个炫技的无锁队列”，而是：

- 在什么场景应该用哪种内存序；
- 如何设计不会误用的 API；
- 如何验证内存序协议；
- 如何在性能与正确性之间做取舍；
- 代码评审时哪些地方必须拦住。

## 1. 第一原则：正确性优先，顺序从强到弱

推荐路径：

```text
先用 seq_cst 写出正确版本
  -> 用测试/TSan/压力测试确认
  -> 用 profiling 找真实瓶颈
  -> 只在明确的发布/消费边界降低内存序
  -> 在目标架构上重新验证
```

不要从 relaxed 开始“试试看”。relaxed 的错误通常不是崩溃，而是：

- 读到未初始化/半初始化的数据；
- 对象生命周期提前结束；
- 析构函数看到旧状态；
- 线上偶发、无法复现、换机器就消失。

### 1.1 选型决策树

```text
需要跨线程传递数据吗？
├── 不需要，只是单线程/局部状态
│   └── 不用原子，直接普通变量
├── 只是统计/计数，不用于发布其他数据
│   └── relaxed
├── 发布一个指针/标志/缓冲区给读者
│   ├── 读者只需要通过该指针读取其成员
│   │   └── acquire（不要用 consume）
│   └── 需要多个字段的一致快照
│       └── 锁 / 不可变快照 / seqlock / RCU
├── 读-改-写并需要同时接收和发布
│   └── acq_rel
├── 两个线程对多个原子操作的全局顺序有要求
│   └── seq_cst
└── 不确定
    └── seq_cst
```

### 1.2 场景速查

| 场景 | 首选 | 说明 |
| --- | --- | --- |
| 请求计数、丢弃计数 | `relaxed` | 最终计数正确即可 |
| 单生产者本地索引 | `relaxed` | 只有本线程写 |
| 发布配置/缓冲区指针 | `release` + `acquire` | 标准发布/订阅 |
| 引用计数 retain | `relaxed` | 只增加计数 |
| 引用计数 release | `acq_rel` | 归零析构需要可见性 |
| 状态机 CAS | `acq_rel` 成功 / `acquire` 失败 | 唯一赢家 |
| 双引擎互相就绪 | `seq_cst` | 全局 store-load 顺序 |
| 读多写少快照 | 不可变快照 + `acquire` | 旧版本回收要单独设计 |
| 一致的多字段快照 | 锁/seqlock/RCU | 多个独立原子不构成原子快照 |
| 指针发布且只读成员 | `acquire` | `consume` 已弃用 |
| 中断/信号上下文 | 极简原子 + `atomic_signal_fence` | 不能阻塞、不能锁 |

## 2. API 设计规则

### 2.1 不要暴露裸原子给业务层

不推荐：

```cpp
class RiskConfig {
public:
    std::atomic<Config*> current;
};
```

调用方会随意 `load(relaxed)`、`store(relaxed)`，破坏协议。

推荐：

```cpp
class ConfigCell {
public:
    void publish(int slot);
    const Config* load_acquire() const;
    const Config* load_consume() const;
};
```

把内存序写进方法名或文档：

- `publish` 蕴含 release；
- `load_acquire` 明确同步语义；
- `load_consume` 明确弱语义和限制；
- 业务层不需要知道枚举。

### 2.2 每个发布 API 都要写清楚生命周期契约

发布指针时，必须回答：

1. 指针指向的对象由谁拥有？
2. 发布后还能不能修改？
3. 旧版本什么时候回收？
4. 读者能保存指针多久？
5. 最后一个读者如何通知回收器？

Demo 的 `ConfigCell`：

- 槽位由 `ConfigCell` 拥有；
- 发布后不修改；
- 槽位数量固定，不回收；
- 读者不能超过 `ConfigCell` 生命周期；
- 因此简单，但不适合无限热更新。

生产环境如果达不到这些条件，就需要 RCU/epoch/hazard pointer/引用计数，不能只靠一个 `atomic<Config*>`。

### 2.3 不要用 `const shared_ptr<T>&` 表达“不持有”

这是智能指针项目的结论，也和内存序有关：

- 传 `shared_ptr` 会引入控制块和原子计数；
- 如果函数只是同步读取，传 `const T&`；
- 如果异步持有，按值捕获 `shared_ptr`；
- 如果只是观察，传 `T*`/`T&` 并明确生命周期。

### 2.4 发布/获取命名约定

推荐：

```cpp
ready.store(true, std::memory_order_release);
if (ready.load(std::memory_order_acquire)) { ... }
```

不推荐：

```cpp
ready.store(true, std::memory_order_relaxed);
if (ready.load(std::memory_order_relaxed)) { ... }
```

命名应该让 review 一眼看出配对关系：

- `publish_*` / `wait_*`
- `store_release` / `load_acquire`
- `begin_drain` / `finish_drain`

### 2.5 失败路径也要定义

- 队列满：自旋、丢弃、阻塞还是降级？
- CAS 失败：重试、放弃还是记录指标？
- 配置未发布：返回默认值还是抛异常？
- 热升级旧版本：等待在途调用还是立即释放？
- 停机超时：强制退出还是继续等待？

内存序只保证可见性，不定义业务策略。

## 3. 核心模式

### 3.1 发布/订阅：release store + acquire load

```cpp
std::atomic<bool> ready(false);
Payload payload;

void publish() {
    payload = build_payload();
    ready.store(true, std::memory_order_release);
}

bool consume(Payload& out) {
    if (!ready.load(std::memory_order_acquire)) {
        return false;
    }
    out = payload;
    return true;
}
```

变体：

- 发布指针而不是 bool；
- 发布索引而不是指针；
- 发布 generation/epoch，读者检查是否变化。

注意：

- `payload` 不能与发布并发修改；
- 读者拿到 true 后，payload 必须保持稳定；
- 多读者可以同时 acquire 同一个 release store；
- 发布后修改 payload 会破坏协议。

### 3.2 有界 SPSC 环

适用：

- 单生产者单消费者；
- 固定容量、低延迟；
- 行情/日志/任务传递。

关键点：

- 生产者只写 head，消费者只写 tail；
- 生产者 acquire tail，消费者 acquire head；
- head/tail 分缓存行；
- 容量 2 的幂；
- 队列满/空策略明确；
- 不要在环内存放需要复杂生命周期的对象，除非移动语义和异常安全都验证过。

不适用：

- 多生产者/多消费者（需要 CAS 或分片）；
- 需要阻塞等待（需要条件变量/信号量）；
- 元素很大且拷贝昂贵（考虑指针/索引）。

### 3.3 引用计数

```cpp
retain: fetch_add(1, relaxed)
release: fetch_sub(1, acq_rel)，归零 delete
```

适用：

- 跨模块共享不可变对象；
- 跨 ABI 的侵入式计数；
- 高频创建/销毁。

注意：

- 对象初始计数必须明确（1 还是 0）；
- `retain` 不能对已归零对象调用；
- `release` 后不能再访问对象；
- 计数溢出（long 足够大，但理论上要防）；
- 与 `shared_ptr` 不要混用两套计数；
- 循环引用仍需弱引用/断链。

### 3.4 状态机 CAS

```cpp
int expected = Running;
if (state.compare_exchange_strong(expected, Draining,
                                  std::memory_order_acq_rel,
                                  std::memory_order_acquire)) {
    // 唯一赢家
}
```

适用：

- 优雅停机；
- 单活选举；
- 连接状态迁移；
- 一次性初始化。

注意：

- 失败序只能用 relaxed/acquire/seq_cst；
- CAS 循环的 expected 会被失败路径改写；
- `weak` 需要循环，`strong` 适合单次尝试；
- 状态机的业务不变量不能只靠 CAS，还要检查前置条件。

### 3.5 seq_cst 全局判断

适用：

- 两个线程互相检查对方是否就绪；
- 需要避免“双方都没看到对方”的全局矛盾；
- 初始化/选举/屏障。

注意：

- seq_cst 只对 seq_cst 操作建立总顺序；
- 不要把普通字段当成自动受保护；
- 成本通常高于 acquire/release，但正确性优先；
- 如果只是单向发布，acquire/release 足够。

### 3.6 统计指标

```cpp
ticks.fetch_add(1, std::memory_order_relaxed);
```

适用：

- 监控计数；
- 吞吐统计；
- 丢弃计数；
- 只要求最终总量，不要求与其他字段一致。

注意：

- 快照可能不一致；
- 不要用指标值做业务决策；
- 高竞争时考虑分片计数（每线程/每核一个计数器，读取时汇总）；
- 分片计数器又引入“汇总快照不一致”的问题。

### 3.7 不可变快照与 RCU

读多写少场景：

```text
写者：构造新版本 -> release 发布指针
读者：acquire 读指针 -> 使用旧/新版本
回收：等所有读者离开旧版本 -> 释放
```

难点不在发布，而在**回收**：

- 读者可能持有旧指针；
- 写者不能立刻 delete 旧版本；
- 需要 epoch、hazard pointer、引用计数或垃圾回收。

C++11 没有标准 RCU。不要用一个 `atomic<shared_ptr>` 就以为解决了所有问题；它只解决生命周期，不解决多字段一致性，而且原子 shared_ptr 操作有成本。

### 3.8 多字段一致快照

多个独立原子对象不构成一个原子快照。可选方案：

1. **锁**：最简单，适合非极端热路径；
2. **不可变对象 + 指针发布**：读者拿到一个版本，所有字段一致；
3. **seqlock**：写者递增序列，读者重试；C++ 中必须避免普通字段的数据竞争，建议数据字段也用原子类型；
4. **双缓冲 + 版本号**：写者写非当前缓冲，发布版本；
5. **RCU/epoch**：适合读多写少，但实现复杂。

选择顺序：锁 -> 不可变快照 -> 成熟库 -> 自己写无锁结构。不要为了“无锁”引入难以验证的 seqlock。

## 4. 性能

### 4.1 先测量再优化

内存序优化必须回答：

- 瓶颈真的是原子操作吗？
- 是缓存行争用、分支预测、分配、拷贝还是系统调用？
- 目标 CPU 架构是什么？
- 竞争程度如何？
- 延迟分布（p99/p999）还是吞吐？

没有测量的“弱内存序优化”通常只是把正确性换成了心理安慰。

### 4.2 伪共享

两个线程写同一缓存行的不同变量，会导致缓存行来回迁移。

处理：

- 用 `alignas(64)` 分离高频写变量；
- 把只读数据和写数据分开；
- 批量处理，减少写次数；
- 用性能工具确认，而不是盲目填充。

Demo 的 `SpscRing` 用 `alignas(64)` 分离 head/tail。

### 4.3 竞争与退避

- 高竞争 CAS 循环会浪费 CPU；
- 自旋前先 `yield`/`pause`；
- 考虑指数退避；
- 能分片就分片；
- 能用每线程本地聚合就不要全局 RMW；
- 无锁不等于低延迟，锁在小临界区可能更快。

### 4.4 原子操作宽度

- 不要用 `atomic<long long>` 代替 `atomic<int>` 如果不需要 64 位；
- 某些平台上宽原子可能不是 lock-free；
- 用 `is_lock_free()` 在目标平台确认；
- 对齐影响原子性和性能；
- `atomic_flag` 是唯一保证 lock-free 的原子类型（C++11）。

### 4.5 批量与缓存

- 批量发布减少原子操作次数；
- 生产者一次发布多个槽位，消费者批量消费；
- 但批量会增加延迟和内存占用；
- 根据业务 SLA 选择批量大小。

## 5. 验证

### 5.1 单元测试

必须覆盖：

- 空/满/边界；
- 单线程功能；
- 多线程压力；
- 关闭/销毁竞争；
- 异常路径；
- 生命周期断言（alive/use_count/弱引用）；
- 状态机唯一赢家。

Demo 的 11 个测试覆盖了核心协议。

### 5.2 ThreadSanitizer

```bash
g++ -std=c++11 -fsanitize=thread -g -O1 -pthread ...
```

TSan 能发现数据竞争和部分锁顺序问题，但：

- 不能证明内存序完全正确；
- 对无锁代码可能产生大量报告；
- 需要目标平台/编译器支持；
- 通过 TSan 不等于通过形式化验证。

### 5.3 AddressSanitizer / UBSan

```bash
g++ -std=c++11 -fsanitize=address,undefined -g -O1 -pthread ...
```

用于发现：

- 悬空指针；
- 越界；
- 生命周期错误；
- 未定义行为。

内存序错误可能表现为生命周期错误，ASan 很有价值。

### 5.4 目标架构

必须在实际部署架构上验证：

- x86-64；
- ARM64；
- 如果支持，POWER；
- 不同编译器/标准库版本。

x86 TSO 会掩盖很多 store-load 重排问题。ARM 上通过才有意义。

### 5.5 Litmus / 微基准

对关键协议写最小 litmus 测试：

- 两个线程、两个原子变量；
- 重复大量轮次；
- 统计是否出现不允许的结果；
- 在目标架构上跑。

注意：litmus 只能证明“某种结果出现过”，不能证明“永远不会出现”；需要结合标准推理。

### 5.6 形式化与模型检查

高安全/高并发场景可考虑：

- CDSChecker/Relacy 等模型检查工具；
- 将协议抽象成小状态机；
- 证明 happens-before 和修改顺序。

这些工具不在本 Demo 的依赖范围内，但值得在架构评审中提出。

## 6. 代码评审清单

### 6.1 原子操作

- [ ] 每个原子操作的内存序是否有理由？
- [ ] release 是否有对应的 acquire？
- [ ] acquire 读到的值是否可能来自非 release 写？
- [ ] `acq_rel` 是否只用于 RMW？
- [ ] CAS 失败序是否合法（relaxed/acquire/seq_cst）？
- [ ] `compare_exchange_weak` 是否在循环中？
- [ ] 是否误用 `memory_order_consume`？
- [ ] 是否在没有必要时使用 fence？
- [ ] 是否把 `seq_cst` 当成普通数据的保护伞？

### 6.2 数据与生命周期

- [ ] 被发布的普通数据是否在发布后不再修改？
- [ ] 读者是否只在 acquire 之后访问数据？
- [ ] 旧版本何时回收？读者能否持有旧指针？
- [ ] 引用计数 release 是否使用 acq_rel？
- [ ] 对象销毁后是否还有线程访问？
- [ ] 是否存在循环引用？
- [ ] 缓存/注册表是否意外保活？

### 6.3 并发结构

- [ ] SPSC 是否真的只有一个生产者和一个消费者？
- [ ] 队列满/空策略是否明确？
- [ ] 索引回绕是否正确？
- [ ] 容量是否为 2 的幂（如果使用掩码）？
- [ ] 是否有 ABA 风险？
- [ ] 是否有饥饿/活锁风险？
- [ ] 自旋是否有退避？
- [ ] 关闭时是否还有线程在自旋？

### 6.4 性能

- [ ] head/tail 是否伪共享？
- [ ] 高频写变量是否在同一缓存行？
- [ ] 是否在热路径做全局 RMW？
- [ ] 是否可以用每线程计数/批量处理？
- [ ] 是否在目标架构上测量过？
- [ ] 是否比较了 p99/p999 而不只是平均值？

### 6.5 可移植性

- [ ] 是否假设了 64 位原子一定 lock-free？
- [ ] 是否假设了缓存行 64 字节？
- [ ] 是否依赖 x86 TSO？
- [ ] 是否使用了 C++11 不存在的 API？
- [ ] 是否在目标编译器上开启警告并清零？
- [ ] 是否在 ARM 上跑过压力测试？

## 7. 常见反模式

### 7.1 全 relaxed

症状：

- 所有原子操作都用 relaxed；
- 代码“能跑”，但在弱内存模型上偶发错误；
- 测试覆盖率很高，但线上仍出问题。

修复：

- 发布/消费路径改 acquire/release；
- 生命周期/状态机改 acq_rel；
- 全局判断改 seq_cst；
- 只保留统计计数为 relaxed。

### 7.2 全 seq_cst

症状：

- 所有原子操作都是默认 seq_cst；
- 性能不达标；
- 以为 seq_cst 能保护普通字段。

修复：

- 识别发布/消费边界；
- 统计改 relaxed；
- 单向发布改 acquire/release；
- 保留真正需要全局顺序的地方为 seq_cst；
- 普通字段仍然需要锁或不可变快照。

### 7.3 用 volatile 做同步

错误：

```cpp
volatile bool ready = false;
int payload = 0;
```

问题：

- 不是原子；
- 不建立 happens-before；
- 不保证 CPU 顺序；
- 编译器仍可重排非 volatile 访问。

修复：用 `std::atomic` + 正确内存序。

### 7.4 relaxed 标志发布数据

错误：

```cpp
payload = 42;
ready.store(true, std::memory_order_relaxed);

if (ready.load(std::memory_order_relaxed)) {
    use(payload);
}
```

修复：

```cpp
ready.store(true, std::memory_order_release);
if (ready.load(std::memory_order_acquire)) { ... }
```

### 7.5 引用计数 release 用 relaxed

错误：

```cpp
if (refs.fetch_sub(1, std::memory_order_relaxed) == 1) {
    delete this;
}
```

修复：`acq_rel`。

### 7.6 CAS 失败序用 release

错误：

```cpp
state.compare_exchange_strong(expected, desired,
                              std::memory_order_acq_rel,
                              std::memory_order_release);
```

失败不写入，不能用 release。修复：失败序用 acquire 或 relaxed。

### 7.7 用 consume 但依赖链断了

错误：

```cpp
Config* cfg = current.load(std::memory_order_consume);
uintptr_t bits = reinterpret_cast<uintptr_t>(cfg);
Config* again = reinterpret_cast<Config*>(bits);
use(again->max_order);
```

依赖链被破坏，consume 不保护。新代码直接用 acquire。

### 7.8 认为原子计数 = 线程安全对象

错误：

```cpp
struct State {
    std::atomic<int> count;
    std::vector<int> items;
};
```

`count` 原子，不代表 `items` 线程安全。需要锁、不可变快照或独立同步。

### 7.9 认为无锁 = 更快

高竞争下：

- CAS 失败重试浪费 CPU；
- 缓存行迁移；
- 内存回收复杂；
- 调试成本高。

小临界区用 `std::mutex` 往往更快、更稳。

### 7.10 在 x86 上验证弱内存序

x86 TSO 会隐藏 store-load 重排。必须在 ARM/目标架构验证。

## 8. 线上问题排查

### 8.1 症状

- 偶发崩溃/数据错乱；
- 换 CPU/编译器后出现；
- 压测难复现；
- 日志显示“看到了新标志但数据是旧的”；
- 对象在仍有线程使用时被销毁。

### 8.2 排查顺序

1. 用 TSan/ASan 跑最小复现；
2. 检查所有发布/消费配对；
3. 检查引用计数 release 的内存序；
4. 检查对象生命周期和缓存保活；
5. 检查是否只在一个架构上验证；
6. 检查是否把 `seq_cst` 当万能；
7. 用 litmus 复现关键交错；
8. 在目标架构上跑压力测试。

### 8.3 不要做的事

- 不要用“加个 sleep”掩盖竞争；
- 不要用“改成 seq_cst”作为唯一修复而不理解协议；
- 不要在没有测试的情况下重写无锁结构；
- 不要把偶发失败归因于“机器问题”。

## 9. C++11 到现代标准

| C++11 写法 | 现代写法 | 说明 |
| --- | --- | --- |
| `std::atomic_load(&sp)` | `std::atomic<std::shared_ptr<T>>` | C++20；C++11 自由函数仍可用 |
| 运行时 `is_lock_free()` | `is_always_lock_free` | C++17 |
| 普通变量 + 锁 | `std::atomic_ref<T>` | C++20；要求对齐/生命周期 |
| 自旋等待 | `atomic::wait/notify` | C++20；避免忙等 |
| `memory_order_consume` | `memory_order_acquire` | consume 已弃用 |
| 手动 fence 发布 | release/acquire 原子操作 | 更易评审 |
| 手写无锁队列 | 成熟库/框架 | 正确性成本高 |

迁移建议：

1. 先替换机械 API；
2. 不要因为标准升级就全面改内存序；
3. 保留原有发布/消费协议；
4. 重新跑 TSan/ASan/目标架构测试；
5. 注意 C++20 release sequence 规则变化；
6. 注意 `atomic<shared_ptr>` 的原子性只针对 shared_ptr 对象，不保护其指向对象。

## 10. 决策表：业务问题到方案

| 业务问题 | 方案 | 关键风险 |
| --- | --- | --- |
| 行情队列传递 | SPSC + acquire/release | 多生产者误用、满队列策略 |
| 风控配置热更新 | 不可变快照 + release/acquire | 旧版本回收 |
| 跨线程对象共享 | 引用计数 relaxed/acq_rel | 归零析构可见性、循环引用 |
| 优雅停机 | CAS 状态机 acq_rel | 重复停机、在途任务 |
| 双引擎就绪 | seq_cst 互相检查 | x86 假象、全局顺序 |
| 监控计数 | relaxed 分片计数 | 快照不一致、汇总成本 |
| 多字段统计快照 | 锁/seqlock/不可变快照 | 数据竞争、重试饥饿 |
| 高频缓存 | 每线程缓存 + 批量发布 | 内存增长、回收 |
| 跨 ABI 句柄 | 显式 retain/release | ABI/CRT 一致性 |
| 读多写少注册表 | 原子快照/COW | 写放大、旧版本生命周期 |
| 日志/事件总线 | 有界队列 + 批量 | 阻塞、丢弃策略 |
| 任务取消 | 弱引用/代际号 + acquire | 悬空、取消延迟 |

## 11. 十二条落地规则

1. **先正确，再快。** 默认 seq_cst，按证据降级。
2. **每个原子操作都要能说出它和谁配对。** 说不出就是 review 阻塞项。
3. **发布数据必须 release，读取必须 acquire。**
4. **统计计数可以 relaxed，但不能用计数发布其他数据。**
5. **引用计数 retain relaxed，release acq_rel。**
6. **CAS 成功 acq_rel，失败 acquire/relaxed。**
7. **consume 只用于理解历史，不用于新代码。**
8. **seq_cst 只解决全局顺序，不保护普通字段。**
9. **多个独立原子不构成一致快照。**
10. **无锁结构的难点是生命周期和回收，不是 CAS。**
11. **在目标架构上验证，别用 x86 代表世界。**
12. **没有测量数据，不要做内存序“优化”。**
