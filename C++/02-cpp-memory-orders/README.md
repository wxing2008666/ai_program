# C++ 六种内存序工程实验室

这是一个以 **C++11** 为基线、面向 Linux 编译环境的六种内存序深度 Demo。它不是一个“打印几个原子变量”的示例集合，而是一个可运行的**低延迟行情数据平面**切片：

- 单生产者/单消费者行情环；
- 不可变风控配置发布；
- 跨 worker 的侵入式引用计数；
- 优雅停机 CAS 状态机；
- 双引擎就绪的 seq_cst 全局判断；
- relaxed 统计指标。

六种内存序全部出现在有业务含义的路径中，而不是为了凑枚举。

## 项目目标

- 覆盖六种内存序约 90% 的工程知识点。
- 用可运行测试证明发布/消费、引用计数、状态机、全局顺序的协议。
- 代码约 890 行，包含头文件、Demo 和测试，无第三方依赖。
- 文档解释“为什么这样排序”，以及什么时候不该使用弱内存序。

## 六种内存序覆盖

| 内存序 | Demo 位置 | 业务语义 |
| --- | --- | --- |
| `memory_order_relaxed` | `SpscRing` 本地索引、`Metrics` | 只保证原子性，适合计数和本地索引 |
| `memory_order_acquire` | `SpscRing` 消费者、`ConfigCell` | 接收发布数据 |
| `memory_order_release` | `SpscRing` 生产者、`ConfigCell::publish` | 发布槽位/配置 |
| `memory_order_acq_rel` | `RefCounted::release`、`Lifecycle` | 引用计数归零、状态迁移 |
| `memory_order_consume` | `ConfigCell::load_consume` | 依赖链读取；演示其限制与弃用原因 |
| `memory_order_seq_cst` | `SeqCstGate` | 两线程全局总顺序，避免双方都未见对方 |

## 快速开始

要求：Linux + GCC/Clang，支持 C++11，链接 pthread，无第三方依赖。

```bash
cd 02-cpp-memory-orders

make -j
./build/mo_demo
./build/mo_tests
```

也可以使用 CMake（CMake 3.5+）：

```bash
mkdir -p build-cmake
cd build-cmake
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
ctest --output-on-failure
./mo_demo
cd ..
```

直接编译：

```bash
g++ -std=c++11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -pthread \
    -Iinclude tests/*.cpp -o build/mo_tests
g++ -std=c++11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -pthread \
    -Iinclude src/main.cpp -o build/mo_demo
```

## Demo 输出

以下为本机实测输出；`backpressure`、指针地址等随调度和平台变化：

```text
=== atomic layout and lock-free status ===
SpscRing<Tick> size=192, align=64
atomic<size_t> lock_free=true, atomic<const Config*> lock_free=true

=== SPSC ring: relaxed local index + acquire/release publication ===
received=50000, backpressure=288, checksum=1249975000
ticks=50000, drops=0, last_sequence=49999

=== config publication: release store + acquire/consume loads ===
acquire risk_limit_bps=250, consume max_order=10, halt=false

=== intrusive refcount: relaxed retain + acq_rel release ===
workers_done=4, alive=1, use_count=1
after creator release alive=0, delta=0

=== lifecycle state machine: acq_rel compare_exchange ===
drain_winners=1, state=1, finish_ok=true, final_state=2

=== seq_cst: total order prevents both-saw-nothing ===
a_saw_b=false, b_saw_a=true, invariant=true

=== summary ===
all memory-order modules executed
```

测试结果：

```text
[==========] 11 tests, 156 checks, 0 failed
```

`seq_cst` 输出中哪一方看到对方取决于调度；`invariant=true` 是必须成立的条件。`state=1/2` 分别对应 `Draining`/`Stopped`。

## 架构

```text
feed 线程
  |
  v
SpscRing<Tick>  --- relaxed head/tail + acquire/release
  |
  v
策略线程 ---> Metrics (relaxed)
  |
  +--> ConfigCell (release + acquire/consume)
  |
  +--> RefCountedFrame (relaxed retain + acq_rel release)
  |
  +--> Lifecycle (acq_rel CAS)
  |
  +--> SeqCstGate (seq_cst)
```

### 模块

| 文件 | 职责 | 内存序重点 |
| --- | --- | --- |
| `include/mo/spsc_ring.h` | 有界单生产者单消费者环 | relaxed 本地索引、acquire/release 交接、伪共享 |
| `include/mo/config_cell.h` | 不可变配置快照发布 | release 发布、acquire/consume 读取 |
| `include/mo/ref_counted.h` | 侵入式引用计数 | relaxed retain、acq_rel release |
| `include/mo/lifecycle.h` | 优雅停机状态机 | acq_rel CAS、失败序 |
| `include/mo/seq_cst_gate.h` | 双引擎就绪判断 | seq_cst 全局总顺序 |
| `include/mo/metrics.h` | 运行指标 | relaxed 计数与快照边界 |
| `include/mo/tick.h` | 行情 tick 数据类型 | 无 |

### 关键设计

- **SPSC 环**：生产者只写 head，消费者只写 tail；head/tail 用 `alignas(64)` 分离缓存行；容量必须是 2 的幂。
- **ConfigCell**：固定槽位、发布后不可变；避免动态生命周期回收，让焦点保持在内存序本身。
- **RefCounted**：初始计数 1；retain 用 relaxed；release 用 acq_rel，保证归零析构前的可见性。
- **Lifecycle**：CAS 把“检查 + 修改”合成原子操作，保证多个线程只有一个停机赢家。
- **SeqCstGate**：Dekker 模式，必须用 seq_cst 才能保证至少一方看到对方。
- **Metrics**：只保证最终计数，不假装提供一致快照。

## 目录结构

```text
02-cpp-memory-orders/
├── include/mo/
│   ├── config_cell.h
│   ├── lifecycle.h
│   ├── metrics.h
│   ├── ref_counted.h
│   ├── seq_cst_gate.h
│   ├── spsc_ring.h
│   └── tick.h
├── src/main.cpp
├── tests/
│   ├── test_framework.h
│   ├── test_main.cpp
│   ├── test_spsc.cpp
│   ├── test_config.cpp
│   ├── test_ref_counted.cpp
│   ├── test_lifecycle.cpp
│   ├── test_seq_cst.cpp
│   └── test_metrics.cpp
├── docs/
│   ├── 01-memory-model-and-six-orders.md
│   ├── 02-demo-walkthrough.md
│   └── 03-production-playbook.md
├── CMakeLists.txt
├── Makefile
└── README.md
```

## 文档

1. `docs/01-memory-model-and-six-orders.md`
   - C++ 内存模型、happens-before、modification order、数据竞争；
   - 六种内存序逐项解析；
   - release sequence、fence、CAS、失败序、ABA；
   - x86/ARM、缓存一致性、伪共享、性能；
   - 常见模式与 20 条陷阱。

2. `docs/02-demo-walkthrough.md`
   - 逐文件解析代码；
   - 证明 SPSC、配置发布、引用计数、状态机、Dekker 协议；
   - 测试矩阵与运行输出；
   - 建议的动手实验。

3. `docs/03-production-playbook.md`
   - 内存序选型决策树；
   - API 设计与生命周期契约；
   - 生产模式、性能优化、验证方法；
   - 代码评审清单、反模式、线上排查；
   - C++11 到 C++17/20 的迁移。

建议阅读顺序：先跑 Demo，再读 01 建立模型，然后读 02 对照代码，最后读 03 做工程落地。

## 测试覆盖

| 测试 | 覆盖 |
| --- | --- |
| `spsc_ring_rejects_invalid_capacity` | 容量约束 |
| `spsc_ring_reports_full_and_empty` | 满/空边界 |
| `spsc_ring_transfers_ticks_in_order` | 并发发布/消费 |
| `config_cell_publishes_immutable_snapshot` | release/acquire/consume |
| `config_cell_rejects_invalid_slots` | 槽位边界 |
| `ref_counted_destroys_after_last_release` | 计数与析构 |
| `ref_counted_survives_concurrent_retain_release` | 并发计数 |
| `lifecycle_allows_exactly_one_drain_winner` | CAS 唯一赢家 |
| `lifecycle_allows_exactly_one_finish_winner` | 状态迁移 |
| `seq_cst_gate_never_observes_both_misses` | 全局顺序 |
| `relaxed_metrics_preserve_exact_totals` | relaxed 计数 |

## 验证与限制

已在本机使用 MinGW g++ 8.1 以 `-std=c++11 -Wall -Wextra -Wpedantic -Wshadow` 编译并运行：

- Demo 全流程通过；
- 11 个测试、156 个断言、0 失败；
- 无编译警告。

限制：

- 本机是 x86-64 Windows + MinGW，未在真实 Linux/ARM 上运行；
- x86 TSO 会掩盖部分 store-load 重排问题，弱内存序必须在目标架构验证；
- `memory_order_consume` 在主流编译器上通常被提升为 acquire，不能据此判断性能；
- 测试是功能/压力测试，不是形式化 litmus 验证；
- 未运行 ThreadSanitizer/AddressSanitizer；生产前应在 Linux 上补跑。

## 代码规模

- C++ 总行数：890 行；
- 非空行：722 行；
- 头文件 + Demo + 测试，共 16 个 C++ 文件；
- 无第三方依赖。

## 设计取舍

- **不用 `shared_ptr` 承载核心同步**：避免控制块和原子计数掩盖内存序问题；引用计数单独实现。
- **不做一个“大而全”的无锁队列**：SPSC 足以展示发布/消费，且正确性可验证。
- **不隐藏生命周期问题**：ConfigCell 固定槽位、RefCounted 显式计数，让限制清晰可见。
- **consume 保留但明确不推荐**：覆盖六种内存序，同时说明它为什么被弃用。
- **测试断言协议结果，而不是断言时序**：避免依赖“先睡 10ms”的脆弱测试。

## 后续扩展

- 将 SPSC 扩展为批量 push/pop，比较吞吐；
- 用 epoch/hazard pointer 实现可回收的配置热更新；
- 增加 seqlock 一致快照，与 relaxed metrics 对比；
- 在 ARM64 上跑 litmus 和压力测试；
- 用 TSan/ASan/UBSan 建立 CI 矩阵；
- 用 `perf c2c` 对照 `alignas(64)` 前后的伪共享。
