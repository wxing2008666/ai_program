# Demo 代码逐模块解析

本文按业务数据流拆解 Demo。项目模拟一条低延迟行情数据平面：

```text
feed 线程
  -> SpscRing<Tick>                 (relaxed + acquire/release)
  -> 策略线程更新 Metrics            (relaxed)
  -> ConfigCell 发布风控配置          (release + acquire/consume)
  -> RefCountedFrame 跨 worker 共享   (relaxed + acq_rel)
  -> Lifecycle 优雅停机状态机         (acq_rel CAS)
  -> SeqCstGate 双引擎就绪判断        (seq_cst)
```

目标不是做一个完整交易系统，而是把六种内存序放进**真实的所有权、发布、消费、停机、共享**场景中，并用测试断言协议结果。

文件位置使用 `path:line`，行号以当前代码为准。

## 1. SPSC 环：relaxed + acquire/release

文件：`include/mo/spsc_ring.h`

### 1.1 数据结构

```cpp
template <typename T>
class SpscRing {
    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<T[]> buffer_;
    alignas(64) std::atomic<std::size_t> head_;
    alignas(64) std::atomic<std::size_t> tail_;
};
```

- 单生产者只写 `head_`，单消费者只写 `tail_`；
- 两个索引都是单调递增的无符号整数，槽位是 `index & mask_`；
- 容量必须是 2 的幂，才能用位掩码取模；
- `head_` 和 `tail_` 分别 `alignas(64)`，避免生产者和消费者互相踩缓存行；
- `buffer_` 是预先分配的数组，运行期不分配内存。

`head_`/`tail_` 的差值表示在途元素数量：

```text
head - tail == 0        空
head - tail == capacity 满
```

### 1.2 生产者

`include/mo/spsc_ring.h:24`

```cpp
bool try_push(const T& value) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= capacity_) {
        return false;
    }
    buffer_[head & mask_] = value;
    head_.store(head + 1, std::memory_order_release);
    return true;
}
```

逐行解释：

1. `head_` 是生产者自己的索引，只有生产者写，读自己的值不需要跨线程同步，所以 `relaxed`。
2. `tail_` 是消费者写的索引。生产者需要知道“消费者是否已经读完某个槽位”，所以用 `acquire` 读取消费者的 release store。
3. 如果队列满，直接返回 false；调用方决定自旋、丢弃还是降级。
4. 写入槽位。这个普通写在 release store 之前，不能下移。
5. `head_.store(..., release)` 发布“槽位已写好”的事实。消费者 acquire 到新 head 后，才能安全读槽位。

为什么 `head_.store` 不能是 `relaxed`：

```text
生产者：
  buffer_[slot] = value;       // 普通写
  head_.store(release);        // 发布

消费者：
  head_.load(acquire);         // 接收
  read buffer_[slot];          // 必须看到 value
```

如果 head 用 relaxed，编译器和 CPU 可以把槽位写推迟到 head store 之后，消费者可能看到新 head 但读到旧槽位。这是数据竞争。

为什么 `tail_.load` 需要 acquire：

- 消费者读完槽位后，用 release 更新 tail；
- 生产者 acquire 到新 tail，确认“这个槽位已经空出来”，才能覆盖它；
- 否则生产者可能在消费者读完之前覆盖槽位。

### 1.3 消费者

`include/mo/spsc_ring.h:35`

```cpp
bool try_pop(T& value) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail == head) {
        return false;
    }
    value = buffer_[tail & mask_];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
}
```

对应关系：

- `tail_` 是消费者自己的索引，`relaxed`；
- `head_` 由生产者 release，消费者必须 acquire 才能读槽位；
- 读槽位之后，release 更新 tail，通知生产者“槽位可复用”。

生产者和消费者的 acquire/release 形成两条方向相反的发布链：

```text
生产者写数据 -> release head -> 消费者 acquire head -> 读数据
消费者读数据 -> release tail -> 生产者 acquire tail -> 覆盖槽位
```

这正是有界 SPSC 环的正确性基础。

### 1.4 容量与溢出

索引使用 `std::size_t`，单调递增，最终会回绕。只要在途元素数量小于容量，`head - tail` 的无符号减法在回绕后仍然给出正确差值。

前提：

- 只有一个生产者、一个消费者；
- 生产者不会越过消费者超过 capacity；
- `capacity` 是 2 的幂；
- `head`/`tail` 不会在测试周期内溢出到无法区分（64 位下实际上不用担心）。

### 1.5 测试

`tests/test_spsc.cpp:15`：非法容量（0、3、6）必须抛异常。
`tests/test_spsc.cpp:21`：满/空边界、覆盖、FIFO。
`tests/test_spsc.cpp:41`：20000 个 tick 的并发搬运，消费者断言序号严格递增，生产者用哨兵结束。

运行结果：

```text
[ RUN      ] spsc_ring_transfers_ticks_in_order
[==========] ... 0 failed
```

注意：这个测试在 x86 上通过，不能证明 relaxed release 的错误在 ARM 上不会出问题。它验证协议的功能结果，不替代目标架构压力测试。

## 2. ConfigCell：release + acquire/consume

文件：`include/mo/config_cell.h`

### 2.1 业务模型

风控配置是不可变快照：

```cpp
struct Config {
    int max_order;
    int risk_limit_bps;
    bool halt;
};
```

写者在启动或低频热更新时构造配置，发布指针；读者只读。Demo 用固定槽位数组，避免动态内存回收：

```cpp
Config slots_[4];
std::atomic<const Config*> current_;
```

使用约束：

- `prepare(slot)` 只在发布前调用；
- 发布后不再修改该槽位；
- 需要热更新时使用下一个槽位；
- 槽位数量有限，不能无限轮转；真实业务需要 RCU/epoch/hazard pointer 回收旧版本。

### 2.2 发布

`include/mo/config_cell.h:30`

```cpp
void publish(int slot) {
    current_.store(slot_pointer(slot), std::memory_order_release);
}
```

- 写者先完成 `slots_[slot]` 的字段写入；
- release store 保证这些写在指针发布前完成；
- 读者 acquire 到指针后，能看到完整配置。

### 2.3 acquire 读取

`include/mo/config_cell.h:34`

```cpp
const Config* load_acquire() const {
    return current_.load(std::memory_order_acquire);
}
```

`risk_limit_acquire()` 通过 acquire 读取指针，再读取 `risk_limit_bps`。这是标准发布/订阅。

### 2.4 consume 读取

`include/mo/config_cell.h:38`

```cpp
const Config* load_consume() const {
    return current_.load(std::memory_order_consume);
}
```

`max_order_consume()`：

```cpp
const Config* config = load_consume();
return config ? config->max_order : 0;
```

这里 `config->max_order` 通过加载到的指针访问，属于数据依赖链，因此 consume 理论上可以保护它。

但要注意：

- 这是“理论上”；GCC/Clang 通常把 consume 实现为 acquire；
- 如果读者拿到指针后，通过无关的全局表或整数转换访问数据，依赖链断了；
- 新业务代码应该用 `acquire`；
- Demo 保留 consume 是为了覆盖第六种内存序，并让读者理解它为什么被弃用。

### 2.5 生命周期契约

`ConfigCell` 不拥有动态配置，只拥有槽位数组。因此：

- `ConfigCell` 必须比所有读者活得久；
- 发布后的槽位不能再改；
- 如果读者保存指针超过 `ConfigCell` 生命周期，指针悬空；
- 真实热更新需要处理旧配置的回收。

这是用固定槽位换取“零动态分配、零引用计数”的简单性。不要在没有回收机制时无限热更新。

### 2.6 测试

`tests/test_config.cpp:5`：

- 初始为空；
- 发布 slot 0 后 acquire 看到 `risk_limit_bps=250`；
- consume 看到 `max_order=10`；
- 发布 slot 1 后看到新值；
- `halt` 等字段一致。

`tests/test_config.cpp:36`：非法槽位抛 `std::out_of_range`。

## 3. RefCounted：relaxed retain + acq_rel release

文件：`include/mo/ref_counted.h`

### 3.1 引用计数实现

`include/mo/ref_counted.h:9`

```cpp
class RefCounted {
public:
    void retain() const {
        refs_.fetch_add(1, std::memory_order_relaxed);
    }

    void release() const {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    long use_count() const {
        return refs_.load(std::memory_order_relaxed);
    }

protected:
    RefCounted() : refs_(1) {}
    virtual ~RefCounted() {}
};
```

初始计数为 1：对象创建后默认有一个所有者。

### 3.2 为什么 retain 用 relaxed

增加引用只改变计数，不发布对象内容：

- 新引用通常来自一个已经可见的对象；
- 增加计数不需要让其他线程看到额外数据；
- 只需要原子性，防止丢失更新。

所以 `relaxed` 足够。

### 3.3 为什么 release 用 acq_rel

递减到 0 时，当前线程要执行 `delete this`，析构函数可能读取对象字段。必须保证：

1. 当前线程在 release 之前的写，对最后一个释放者可见（release 方向）；
2. 最后一个释放者能看到其他线程在各自 release 之前的写（acquire 方向）。

`acq_rel` 同时满足两者。

错误写法：

```cpp
if (refs_.fetch_sub(1, std::memory_order_relaxed) == 1) {
    delete this;
}
```

计数可能正确地归零，但析构函数看到的字段可能是旧值，甚至触发未定义行为。计数原子性不等于对象状态可见性。

### 3.4 与 `shared_ptr` 的对比

- `std::shared_ptr` 的控制块内部也维护强/弱计数；
- 标准库实现会处理引用计数的同步；
- 手写侵入式计数把计数放进对象，尺寸更小，但要求对象继承 `RefCounted`；
- 本 Demo 不实现 `intrusive_ptr`，避免偏离内存序主题；只展示计数协议本身。

### 3.5 测试

`tests/test_ref_counted.cpp:9`：

- 初始计数 1；
- retain 到 2；
- release 回到 1，对象仍存活；
- 最后一次 release 后 alive 归零。

`tests/test_ref_counted.cpp:24`：

- 主线程持有 1 个引用；
- 8 个 worker 各 retain 一个引用；
- worker 读取不可变字段后 release；
- join 后计数回到 1；
- 主线程 release 后对象销毁。

这个测试验证计数平衡和析构时机。它不能证明 release 的 acquire 方向在所有架构上都正确；那需要 TSan/目标硬件压力测试。

## 4. Lifecycle：acq_rel CAS 状态机

文件：`include/mo/lifecycle.h`

### 4.1 状态

```cpp
enum class State { Running, Draining, Stopped };
std::atomic<int> state_;
```

业务语义：

- `Running`：正常处理；
- `Draining`：停止接收新任务，排空在途任务；
- `Stopped`：完全停止。

多个线程可能同时请求停机，但只能有一个赢家。

### 4.2 CAS 迁移

`include/mo/lifecycle.h:34`

```cpp
bool transition(State from, State to) {
    int expected = static_cast<int>(from);
    return state_.compare_exchange_strong(
        expected,
        static_cast<int>(to),
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}
```

- `compare_exchange_strong` 比较当前值是否等于 `from`；
- 相等则写入 `to`，返回 true；
- 不等则把当前值写回 `expected`，返回 false；
- 成功序 `acq_rel`：发布新状态，同时接收旧状态之前的数据；
- 失败序 `acquire`：只读取当前状态，不做写入；
- 失败序不能是 release/acq_rel。

### 4.3 为什么需要 CAS

如果写成：

```cpp
if (state.load(acquire) == Running) {
    state.store(Draining, release);
}
```

两个线程可能同时看到 Running，然后都执行 store，导致重复停机/重复排空。CAS 把“检查 + 修改”合成一个原子操作，只有一个线程能成功。

### 4.4 测试

`tests/test_lifecycle.cpp:9`：

- 8 个线程同时 `begin_drain()`；
- 恰好 1 个返回 true；
- 状态为 Draining；
- 再次 begin 失败；
- finish 成功，状态 Stopped。

`tests/test_lifecycle.cpp:33`：

- 先进入 Draining；
- 8 个线程竞争 finish；
- 恰好 1 个成功；
- 最终 Stopped。

### 4.5 业务扩展

真实停机会在 `Draining` 状态做：

- 拒绝新请求；
- 等待在途任务；
- 刷新指标/日志；
- 释放资源；
- 最后 CAS 到 `Stopped`。

状态迁移的 acq_rel 只保证状态字段和其发布的数据可见；在途任务队列、引用计数、配置快照仍需要各自的同步协议。

## 5. SeqCstGate：全局总顺序

文件：`include/mo/seq_cst_gate.h`

### 5.1 业务类比

两个引擎同时启动，各自宣布“我已就绪”，然后检查对方是否就绪。业务要求：

- 不能出现“两个都认为自己没看到对方”的情况；
- 否则可能同时进入单活模式或同时放弃。

这就是 Dekker 模式。

### 5.2 实现

`include/mo/seq_cst_gate.h:25`

```cpp
Observation run() {
    a_.store(false, seq_cst);
    b_.store(false, seq_cst);

    bool a_saw_b = false;
    bool b_saw_a = false;

    std::thread a([&]() {
        a_.store(true, seq_cst);
        a_saw_b = b_.load(seq_cst);
    });
    std::thread b([&]() {
        b_.store(true, seq_cst);
        b_saw_a = a_.load(seq_cst);
    });

    a.join();
    b.join();
    return Observation{a_saw_b, b_saw_a};
}
```

### 5.3 为什么必须是 seq_cst

`seq_cst` 保证所有 seq_cst 操作存在全局总顺序。考虑两个 load 都读到 false：

- A 的 store 必须在 A 的 load 之前；
- B 的 store 必须在 B 的 load 之前；
- 如果 A 的 load 没看到 B 的 store，则 A 的 load 在总顺序中位于 B 的 store 之前；
- 如果 B 的 load 没看到 A 的 store，则 B 的 load 位于 A 的 store 之前；
- 合并得到 A.store < A.load < B.store < B.load < A.store，矛盾。

因此至少一方看到对方。

如果换成 acquire/release：

- release store 只保证之前的写不下移；
- acquire load 只保证之后的读不上移；
- 它们不建立两个不同原子对象之间的全局 store-load 顺序；
- 在弱内存模型上，两个线程都可能先读对方为 false。

x86 TSO 通常不允许 store-load 重排到这种程度，所以错误代码可能在 x86 上“看起来正确”。这正是不能只用 x86 验证的原因。

### 5.4 测试

`tests/test_seq_cst.cpp:5` 循环 100 轮，断言：

```cpp
observation.at_least_one_saw_the_other()
```

测试不会断言“双方都看到”，因为业务只要求至少一方看到。两个都看到是允许的。

## 6. Metrics：relaxed 的边界

文件：`include/mo/metrics.h`

### 6.1 实现

```cpp
void record_tick(std::uint64_t sequence) {
    ticks_.fetch_add(1, std::memory_order_relaxed);
    sequence_.store(sequence, std::memory_order_relaxed);
}

void record_drop() {
    drops_.fetch_add(1, std::memory_order_relaxed);
}

MetricsSnapshot snapshot() const {
    return {
        ticks_.load(std::memory_order_relaxed),
        drops_.load(std::memory_order_relaxed),
        sequence_.load(std::memory_order_relaxed)};
}
```

### 6.2 为什么 relaxed 合适

- 指标是“最终计数”，不用于发布业务数据；
- 读快照不要求与其他字段构成一致快照；
- 多个线程并发累加，只需要原子性；
- 用 seq_cst 会让每次计数都变重，却没有业务收益。

### 6.3 快照不一致

`Metrics::snapshot()` 的三次 load 之间没有同步：

```text
ticks = 100
drops = 3
sequence = 99
```

这个组合可能对应某个瞬间，也可能不对应任何真实瞬间。比如 ticks 已增加但 sequence 还没更新。

如果业务要求一致快照：

- 用锁保护整个结构；
- 用 seqlock；
- 用单写者发布不可变快照；
- 用 seq_cst 也不够：seq_cst 不给多个独立原子对象提供原子快照。

### 6.4 测试

`tests/test_metrics.cpp:8`：

- 4 个线程各记录 10000 个 tick；
- join 后 ticks 精确为 40000；
- drops 精确为 40。

这验证 relaxed RMW 的原子性，不验证快照一致性。

## 7. Demo 主线

文件：`src/main.cpp`

### 7.1 启动检查

```cpp
std::atomic<std::size_t>().is_lock_free()
std::atomic<const mo::Config*>().is_lock_free()
```

这会打印当前平台的锁自由状态。`is_lock_free()` 是运行期查询，C++11 没有 `is_always_lock_free`。

### 7.2 行情环

- 生产者线程生成 50000 个 tick；
- 队列容量 1024；
- 队列满时自旋并计数 `backpressure`；
- 发送哨兵 `sequence == kTickCount`；
- 消费者按序取出，累加 checksum，更新 relaxed metrics；
- join 后打印 received/backpressure/checksum。

输出示例：

```text
received=50000, backpressure=288, checksum=1249975000
ticks=50000, drops=0, last_sequence=49999
```

`backpressure` 的具体值取决于调度，不是稳定断言。

### 7.3 配置读取

发布配置后同时用 acquire 和 consume 读取：

```text
acquire risk_limit_bps=250, consume max_order=10, halt=false
```

### 7.4 引用计数

4 个 worker 各持有 frame 的一份引用，读取不可变字段后 release。主线程最后 release，打印：

```text
workers_done=4, alive=1, use_count=1
after creator release alive=0, delta=0
```

### 7.5 生命周期

4 个线程竞争 `begin_drain()`：

```text
drain_winners=1, state=1, finish_ok=true, final_state=2
```

`state` 的数值对应 `Running=0`、`Draining=1`、`Stopped=2`。

### 7.6 seq_cst

```text
a_saw_b=false, b_saw_a=true, invariant=true
```

具体哪一方看到对方取决于调度；`invariant` 必须为 true。

## 8. 测试矩阵

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

当前结果：

```text
[==========] 11 tests, 156 checks, 0 failed
```

这个数字会随断言调整变化，以实际运行输出为准。

## 9. 建议的动手实验

1. 把 `SpscRing::try_push` 的 `head_.store` 改成 `relaxed`，在 ARM 机器上跑压力测试；观察消费者是否读到未发布槽位。
2. 把 `ConfigCell::publish` 改成 `relaxed`，在弱内存模型上观察配置字段的可见性。
3. 把 `RefCounted::release` 改成 `relaxed`，用 TSan/压力测试观察析构可见性问题。
4. 把 `Lifecycle` 的成功序改成 `relaxed`，观察状态迁移与数据可见性。
5. 把 `SeqCstGate` 改成 acquire/release，在 ARM/POWER 上做 litmus 测试；不要只依赖 x86。
6. 给 `SpscRing` 增加批量 push/pop，比较吞吐和缓存行争用。
7. 用 `perf c2c` 或缓存未命中工具观察伪共享；去掉 `alignas(64)` 做对照。
8. 用 TSan 跑全部测试，记录 race 报告；再用 ASan/UBSan 检查生命周期和未定义行为。
9. 给 `Metrics` 加 seqlock 版本，比较“最终计数”和“一致快照”的语义差异。
10. 把 `ConfigCell` 改成 RCU/epoch 回收，处理无限热更新的生命周期。

## 10. 代码规模

当前实现（`include + src + tests`）约 890 行，非空约 720 行：

- 头文件：6 个核心模块 + 1 个数据类型；
- Demo 主线：约 150 行；
- 测试：7 个文件 + 轻量测试框架；
- 无第三方依赖，C++11，Linux `g++/clang++ -pthread`。

代码刻意保持小，重点在协议和验证，而不是堆砌无锁数据结构。
