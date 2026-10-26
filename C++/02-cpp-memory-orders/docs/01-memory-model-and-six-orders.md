# C++ 内存模型与六种内存序

本文建立理解 Demo 所需的心智模型。重点不是背诵六种枚举，而是回答：

- 一条普通读写什么时候能被另一个线程看到？
- `std::atomic` 到底保证了什么，没保证什么？
- 为什么 `relaxed` 有时够用，什么时候必须 `acquire/release`？
- 为什么引用计数用 `relaxed + acq_rel`，而不是全 `seq_cst`？
- `memory_order_consume` 为什么被弃用？
- `seq_cst` 多出来的“全局顺序”到底解决什么问题？

## 0. 先给结论

1. **默认从 `seq_cst` 开始**。它是默认内存序，正确性最容易推理；只有在 profiling 或明确的发布/消费协议中才降低强度。
2. **`relaxed` 只保证单个原子对象上的原子性和修改顺序**，不建立跨对象、跨线程的可见性。
3. **`release` 发布，`acquire` 接收**。这是业务中最常见的配对：一方写数据，另一方看到数据。
4. **`acq_rel` 主要用于读-改-写**，例如引用计数递减、CAS 状态迁移。它同时具备 release 和 acquire 方向。
5. **`consume` 是“只保证依赖链”的弱化 acquire**，但实现困难、已被弃用，主流编译器通常按 acquire 处理。新代码不要用它。
6. **`seq_cst` 的价值是全局总顺序和 store-load 顺序**，不是“让所有普通变量都线程安全”。
7. **无锁不等于快，也不等于无等待**。先证明正确，再测目标硬件。

## 1. 为什么需要内存模型

源代码顺序不等于机器执行顺序。至少有四层重排：

```text
C++ 源码
  -> 编译器优化（指令重排、公共子表达式、寄存器缓存）
  -> CPU 乱序执行（load/store 重排、推测执行）
  -> 存储子系统（store buffer、写合并、缓存一致性）
  -> 多核缓存（不同核心看到不同时间点的写入）
```

单线程程序只要求“as-if”语义：只要结果和抽象机器一致，编译器和 CPU 可以任意重排。多线程程序如果没有同步，编译器和 CPU 没有义务让一个线程按另一个线程期待的顺序看到写入。

常见硬件差异：

- **x86-64（TSO）**：load-load、load-store、store-store 基本保持顺序，但允许 store-load 重排；`seq_cst` store 通常需要 `xchg` 或 `mfence` 一类指令。
- **ARM/POWER（弱内存模型）**：更多重排，需要 `dmb`/`isb` 等屏障；`acquire/release` 才能提供可靠发布语义。
- **编译器**：即使 x86 不乱序，编译器也可能把读缓存到寄存器、把写延后、合并循环中的读写。

因此：

- **“我在 x86 上测过没问题”不能证明代码正确。**
- **`volatile` 不是线程同步工具。** 它阻止编译器优化该对象的访问，但不提供原子性、不建立 happens-before、也不保证 CPU 顺序。
- **`std::atomic` 才是 C++ 内存模型的一等公民。**

## 2. C++ 抽象机器的四个核心关系

### 2.1 sequenced-before（同线程顺序）

同一线程内，语句/表达式的求值存在 sequenced-before 关系。它描述“程序顺序”，但不等于其他线程观察到的顺序。

```cpp
x = 1;
y = 2;
```

在单线程中，编译器可以调整这两条普通写的机器指令顺序，只要本线程观察不到差异。

### 2.2 synchronizes-with（跨线程同步）

最典型的同步：

```cpp
std::atomic<bool> ready(false);
int payload = 0;

// 线程 A
payload = 42;
ready.store(true, std::memory_order_release);

// 线程 B
if (ready.load(std::memory_order_acquire)) {
    assert(payload == 42);
}
```

规则：

- A 的 release store **synchronizes-with** B 的 acquire load；
- 如果 B 的 load 读到了 A 的 store 写入的值（或 release sequence 中的值），则 A 在 store 之前的所有写对 B 可见；
- 于是 `payload = 42` happens-before `assert(payload == 42)`。

如果 store/load 都用 `relaxed`，则没有 synchronizes-with，B 可能看到 `ready == true` 但 `payload` 仍是旧值。这不是“概率问题”，而是数据竞争/未定义行为。

### 2.3 happens-before（可见性与顺序的传递闭包）

happens-before 由三部分组成：

1. 同线程的 sequenced-before；
2. 跨线程的 synchronizes-with；
3. 传递闭包。

```text
A: payload = 42
A: ready.store(release)
        | synchronizes-with
B: ready.load(acquire) == true
B: read payload
```

因此 `payload = 42` happens-before B 的读。happens-before 是判断数据竞争的核心：两个冲突访问（至少一个是写、访问同一内存位置、非原子）如果没有 happens-before 关系，就是数据竞争，行为未定义。

### 2.4 modification order（同一原子对象的修改顺序）

对每一个原子对象，所有修改构成一个总顺序，称为 modification order。所有线程看到的该对象的修改顺序一致（coherence）。

例如：

```cpp
std::atomic<int> value(0);
// 多个线程 fetch_add(1, relaxed)
```

虽然线程之间没有顺序保证，但每个 `fetch_add` 都是原子的，最终计数精确，且所有线程对 `value` 的修改顺序有一致看法。

注意：

- modification order 只针对**同一个原子对象**；
- 它不自动给其他变量排序；
- `relaxed` RMW 仍然会读取该原子对象在修改顺序中“之前”的最新值，不会丢失更新。

### 2.5 data race（数据竞争）

C++ 定义：两个冲突操作（同一内存位置、至少一个写），如果至少一个不是原子操作，且二者之间没有 happens-before 关系，则程序有数据竞争，行为未定义。

要点：

- 数据竞争不是“偶尔读到旧值”，而是 UB；编译器可以据此做任意优化。
- 原子操作本身不会与同对象的其他原子操作构成数据竞争。
- 原子对象旁边的普通字段仍然需要发布/获取协议或锁。
- `volatile` 不消除数据竞争。
- 用 `std::atomic` 包住一个字段，不会自动保护结构体中的其他字段。

volatile 管"编译器别优化我的访问"，mutable 管"const 函数里也允许改这个成员"。
volatile: 告诉编译器，这个变量的值可能在程序控制流之外被修改，所以每次访问都必须真实地读写内存，不能缓存到寄存器，也不能删除或合并读写。
mutable: 修饰类的非静态数据成员，允许它在 const 成员函数中被修改；或者修饰 lambda，使按值捕获的副本可被修改。

## 3. 原子性不等于顺序性

`std::atomic<T>` 提供：

- 原子性：不会看到“半个值”；
- 修改顺序：同对象的所有修改有共识顺序；
- 可选的内存序：是否建立跨线程 happens-before。

它不提供：

- 对多个原子对象的原子快照（除非用锁/seq_cst 协议/seqlock）；
- 对普通字段的保护；
- 自动的业务不变量；
- 无锁保证（`is_lock_free()` 可能为 false）；
- 无等待保证（CAS 循环可能一直失败）。

`std::atomic` 的默认内存序是 `seq_cst`：

```cpp
std::atomic<int> x(0);
x.store(1);              // seq_cst
int v = x.load();       // seq_cst
```

## 4. 六种内存序

先给一张“作用方向”表，后面逐项解释：

| 内存序 | 适用操作 | 核心保证 | 典型用途 |
| --- | --- | --- | --- |
| `relaxed` | load/store/RMW | 仅原子性与修改顺序 | 统计计数、局部索引、序列号 |
| `acquire` | load/RMW | 后续读写不能上移；接收 release 发布的数据 | 消费者读取发布数据 |
| `release` | store/RMW | 之前的读写不能下移；发布数据 | 生产者发布数据 |
| `acq_rel` | RMW | 同时具备 acquire + release | 引用计数、CAS 状态机 |
| `consume` | load/RMW | 只对数据依赖链排序 | 指针发布（已弃用） |
| `seq_cst` | 全部 | 全局总顺序 + store-load 顺序 | 全局不变量、Dekker、默认安全选择 |

### 4.1 `memory_order_relaxed`

C++ 原子操作里最弱的内存序，适合“只算数、不传话”的原子变量。

语义：

- 操作本身原子；不保证它和其他内存操作之间的顺序；
- 同一原子对象有 modification order；
- 不建立跨线程 happens-before；
- 编译器和 CPU 可以自由重排该操作与其他无关访问。

它到底保证了什么：
- 保证原子性：不会出现撕裂读、部分写。比如 atomic<int> 的 fetch_add 不会读到一半的值。
- 不保证顺序：编译器、CPU 可以把 relaxed 操作和其他读写重排。
- 不保证可见性时机：一个线程用 relaxed 写入后，另一个线程可能“晚一点”才看到新值。
- 不建立 happens-before：它不能用来传递数据、保护临界区、发布资源。

Demo 中的使用：

```cpp
const std::size_t head = head_.load(std::memory_order_relaxed);
const std::size_t tail = tail_.load(std::memory_order_acquire);
```

`head_` 是生产者自己的索引，只有生产者写；生产者读自己的索引不需要和别的线程同步，所以 `relaxed` 足够。真正的跨线程交接发生在：

```cpp
buffer_[head & mask_] = value;
head_.store(head + 1, std::memory_order_release);
```

以及消费者的：

```cpp
const std::size_t head = head_.load(std::memory_order_acquire);
```

如果这里把 `head_.store` 改成 `relaxed`，消费者可能看到新索引却看不到槽位里的数据。x86 上这种错误可能长期不显现，ARM 上更容易暴露。

`relaxed` 的合理用途：

- 统计计数器：`ticks.fetch_add(1, relaxed)`；
- 单写者本地索引；
- 只关心最终计数，不关心与其他字段的顺序；
- 序列号的“值”本身，不用于发布其他数据。

`relaxed` 的错误用途：

- 用 `ready` 标志发布 `payload`；
- 用 `relaxed` 保护非原子共享结构；
- 认为 `relaxed` 等于“普通变量但不会撕裂”。

### 4.2 `memory_order_acquire`

只能用在读操作上（load、RMW 的读端）。

语义（用于 load/RMW）：

- 该操作之后的所有读写，不能重排到该操作之前；
- 如果它读到了 release store（或 release sequence）的值，则与对方建立 synchronizes-with；
- 因而能看到对方在 release 之前发布的数据。

典型：

```cpp
std::atomic<bool> ready(false);
int payload = 0;

// 读者
if (ready.load(std::memory_order_acquire)) {
    use(payload);
}
```

Demo 中的使用：

```cpp
const std::size_t head = head_.load(std::memory_order_acquire);
value = buffer_[tail & mask_];
```

消费者必须先 `acquire` 看到生产者的 `head_`，才能读槽位。否则数据竞争。

注意：

- `acquire` 只约束它之后的访问，不约束它之前的访问；
- `acquire` 必须和 release 配对，单独使用没有发布语义；
- `acquire` 不是“锁”，不会阻止其他线程写同一数据。

### 4.3 `memory_order_release`

只能用在写操作上（store、RMW 的写端）。

语义（用于 store/RMW）：

- 该操作之前的所有读写，不能重排到该操作之后；
- 与读取该值的 acquire load 建立 synchronizes-with；
- 把之前写入的数据“发布”给读者。

典型：

```cpp
payload = 42;
ready.store(true, std::memory_order_release);
```

Demo 中的使用：

```cpp
buffer_[head & mask_] = value;
head_.store(head + 1, std::memory_order_release);
```

以及配置发布：

```cpp
current_.store(slot_pointer(slot), std::memory_order_release);
```

注意：

- release 只约束它之前的访问，不约束之后的访问；
- release store 必须被 acquire load 读到，才有同步；
- 多个读者可以各自 acquire 同一个 release store；
- release 不保证“所有线程立刻看到”，只保证看到该原子值后能看到之前的数据。

### 4.4 release sequence

一个 release store 后面可能跟着一串 RMW。C++11 的 release sequence 允许 acquire load 读到这串 RMW 的某个值时，仍然与最初的 release store 建立同步。

```text
T1: data = 42
T1: flag.store(true, release)
T2: flag.fetch_add(0, relaxed)     // RMW，仍在 release sequence 中
T3: flag.load(acquire) == true     // 与 T1 release 同步
```

语义：
它让 acquire 即使读到的是后续 RMW 操作写入的值，也能追溯到最初那个 release 操作之前的所有写入。

这解释了为什么引用计数、标志位和 RMW 链可以配合使用。

链条由什么构成：
1. 对一个原子变量 M，以一次 release 操作 A 为起点，release sequence 包含之后 M 修改顺序中连续的一段，由以下操作组成：
- 同一线程对 M 的后续写入（任意内存序）；
- 任意线程对 M 的 RMW 操作（fetch_add、compare_exchange 等，任意内存序）。
2. 只要 acquire load 读到这条链上的任意一个值，就能同步到最初的 release 操作 A 之前的所有写入。

什么会打断链条：
- 其他线程的普通 store（非 RMW）会打断链条；
- 中间插入了非 RMW、非同一线程写入的操作，链条断裂，后续 acquire 无法追溯到最初的 release。

重要版本差异：

- C++11：release sequence 可以包含同一线程后续的普通写；
- C++20 起：release sequence 收窄为只包含 RMW。
- 跨标准版本时，不要依赖“普通写也在 release sequence 里”这种细节；优先使用明确的 release/acquire 或 RMW。

只有当 acquire 读到了 release 写入的值时，两者才形成 synchronizes-with 关系，从而建立 happens-before；

### 4.5 `memory_order_acq_rel`

只能用在 RMW 操作上（fetch_add、compare_exchange 等）。

语义（用于 RMW）：

- 同时具备 acquire 和 release；
- 作为 release：之前的写不会下移；
- 作为 acquire：之后的读写不会上移；
- 适合“读旧值、改新值、同时发布/接收”的操作。

Demo 中的引用计数：

```cpp
void retain() const {
    refs_.fetch_add(1, std::memory_order_relaxed);
}

void release() const {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
    }
}
```

为什么 release 用 `acq_rel`：

1. **release 方向**：当前线程在 `release()` 之前对对象的修改，必须对最后一个释放者可见；
2. **acquire 方向**：最后一个释放者删除对象时，需要看到其他线程在各自 `release()` 之前的修改，保证析构函数读取到的状态是同步过的；
3. `retain` 只需要 `relaxed`：增加引用不会发布新数据，只需要计数原子性。

如果把 `release` 改成 `relaxed`，可能出现“计数归零并 delete，但析构函数看不到其他线程写入”的未定义行为。

Demo 中的生命周期 CAS：

```cpp
return state_.compare_exchange_strong(
    expected,
    static_cast<int>(to),
    std::memory_order_acq_rel,
    std::memory_order_acquire);
```

- 成功时：`acq_rel`，完成状态迁移并向等待者发布；
- 失败时：`acquire`，读取当前状态，不发布任何东西；
- 失败序不能是 `release` 或 `acq_rel`，因为失败的 CAS 不写入。

### 4.6 `memory_order_consume`

语义（用于 load/RMW）：

- 比 acquire 弱：只对“携带数据依赖”的操作排序；
- 如果 load 读到 release 发布的值，则通过该值依赖链访问的数据是可见的；
- 不保证独立的后续读写。

典型：

```cpp
struct Config { int max_order; int risk_limit_bps; };

std::atomic<Config*> current(nullptr);
Config config{10, 250};

// 写者
current.store(&config, std::memory_order_release);

// 读者
Config* cfg = current.load(std::memory_order_consume);
int limit = cfg->risk_limit_bps; // 依赖 cfg，受 consume 保护
```

Demo 中的使用：

```cpp
const Config* config = load_consume();
return config ? config->max_order : 0;
```

关键限制：

- `consume` 只保护**通过加载值依赖访问**的数据；
- 如果先把指针转成整数、走无关的全局表、或者用 `std::kill_dependency` 打断依赖，保护就消失；
- 编译器实现非常困难；GCC/Clang 等通常把 `consume` 提升为 `acquire`，所以性能优势经常不存在；
- C++17 起被弃用；新代码用 `acquire`；
- Demo 保留它是为了完整展示六种内存序，不代表推荐用于生产。

`std::kill_dependency` 的作用是显式打断依赖链：

```cpp
Config* cfg = current.load(std::memory_order_consume);
int value = std::kill_dependency(cfg)->max_order;
```

这告诉编译器“后续访问不依赖 cfg 的发布语义”，通常只用于极端优化；业务代码不应随意使用。

### 4.7 `memory_order_seq_cst`

C++ 所有原子操作的默认内存序，最严格的内存序。sequential consistency（顺序一致性）。

语义：

- 所有 `seq_cst` 操作存在一个全局总顺序 S；
- 每个线程观察到的 `seq_cst` 操作顺序与 S 一致；
- 提供 store-load 顺序：同一线程中，`seq_cst` store 之后的 `seq_cst` load 不会被重排到 store 之前；
- 是 `std::atomic` 的默认内存序。

两层保证：
1. 具备 acquire/release 语义
- seq_cst 的 load 操作具有 acquire 语义：当前线程中后续的读写不能重排到该 load 之前。
- seq_cst 的 store 操作具有 release 语义：当前线程中之前的读写不能重排到该 store 之后。
- seq_cst 的 RMW 操作同时具备 acquire + release 语义。
2. 全局单一全序（Single Total Order）
这是 seq_cst 比 acq_rel 多出来的关键保证：所有线程看到的所有 seq_cst 操作的执行顺序是完全一致的。不存在"线程 A 看到 x 先于 y，线程 B 看到 y 先于 x"的矛盾情况。

经典 Dekker 模式：

```cpp
std::atomic<bool> a(false), b(false);

// 线程 A
a.store(true, std::memory_order_seq_cst);
bool saw_b = b.load(std::memory_order_seq_cst);

// 线程 B
b.store(true, std::memory_order_seq_cst);
bool saw_a = a.load(std::memory_order_seq_cst);
```

`seq_cst` 保证 **`saw_a` 和 `saw_b` 不可能同时为 false**。

为什么：

- 如果 A 先执行 store，B 的 load 在总顺序中位于 A 的 store 之后，B 必看到 `a == true`；
- 如果 B 先执行 store，A 必看到 `b == true`；
- 两个 load 都在各自 store 之前看到 false 的情况会违反总顺序。

如果改成 `acquire/release`：

```cpp
a.store(true, release);
bool saw_b = b.load(acquire);

b.store(true, release);
bool saw_a = a.load(acquire);
```

在弱内存模型上，两个线程都可能先读对方为 false，再各自 store；这不是 `seq_cst` 保证的范围。x86 TSO 可能掩盖这个问题，ARM/POWER 更容易暴露。

Demo 中的 `SeqCstGate` 就是该模式。测试循环 100 轮，断言至少一方看到对方。

`seq_cst` 的代价：

- 在 x86 上，`seq_cst` store 通常需要 `xchg`/`mfence`，比 release store 的普通 `mov` 重；
- 在 ARM 上需要更强的屏障；
- 但错误使用弱内存序的代价是数据竞争、偶发崩溃和难以复现的线上事故。

工程策略：

- 先 `seq_cst` 写正确；
- 用 TSan、压力测试和性能数据证明瓶颈；
- 再把明确的发布/消费路径降为 acquire/release；
- 统计计数器降为 relaxed；
- 不要为了“看起来高级”全局使用弱内存序。

## 5. 六种内存序对比

### 5.1 语义对比

| 内存序 | 原子性 | 同对象修改顺序 | 跨线程同步 | 全局总顺序 | 典型风险 |
| --- | --- | --- | --- | --- | --- |
| `relaxed` | 是 | 是 | 否 | 否 | 发布数据丢失 |
| `acquire` | 是 | 是 | 与 release 配对 | 否 | 缺少 release、误当锁 |
| `release` | 是 | 是 | 与 acquire 配对 | 否 | 发布前写入未完成 |
| `acq_rel` | 是 | 是 | 双向 RMW | 否 | 用于非 RMW、失败序错误 |
| `consume` | 是 | 是 | 仅依赖链 | 否 | 依赖被打断、实现不生效 |
| `seq_cst` | 是 | 是 | 是 | 是 | 性能成本、误以为保护普通数据 |

### 5.2 使用决策速查

```text
只是计数/统计，不发布其他数据          -> relaxed
发布指针/配置/缓冲区给读者              -> release store + acquire load
读-改-写并需要同时接收/发布             -> acq_rel
只需要通过指针读取其成员                -> acquire（不要用 consume）
需要两个线程对多个原子操作的全局顺序     -> seq_cst
不确定                                  -> seq_cst
```

## 6. 内存屏障（fence）

内存序不仅能放在原子操作上，还能通过 fence 表达：

```cpp
std::atomic_thread_fence(std::memory_order_release);
std::atomic_thread_fence(std::memory_order_acquire);
std::atomic_thread_fence(std::memory_order_seq_cst);
```

典型 release fence + relaxed store：

```cpp
payload = 42;
std::atomic_thread_fence(std::memory_order_release);
ready.store(true, std::memory_order_relaxed);
```

读者：

```cpp
if (ready.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    use(payload);
}
```

在满足 fence-fence 同步条件时，这可以建立与 release store/acquire load 类似的发布语义。

但要非常小心：

- fence 的同步规则比原子操作的内存序更绕；
- fence 必须放在正确的位置，且依赖具体的原子读写；
- `seq_cst` fence 不是“万能全屏障”，它和普通变量、relaxed 原子、seq_cst 原子的组合有精细规则；
- 多数业务代码应优先使用 release store / acquire load，而不是手写 fence；
- `std::atomic_signal_fence` 只约束编译器，不生成 CPU 屏障，主要用于信号处理上下文。

seqlock 是 fence 的经典应用，但 C++ 中要避免读者与写者对普通字段的并发访问造成数据竞争；生产实现通常把数据字段做成原子类型，或使用经过验证的库。

## 7. RMW、CAS 与失败序

### 7.1 RMW 的原子性

`fetch_add`、`exchange`、`compare_exchange` 都是读-改-写（RMW）：

- 读和写是一个原子操作；
- 不会插入其他线程对同一对象的修改；
- 但不同对象之间仍然可以重排。

### 7.2 `compare_exchange_weak` vs `strong`

- `weak` 允许伪失败（spurious failure），必须在循环中重试；
- `strong` 不会伪失败（但仍可能因为值不匹配失败）；
- 在循环里通常用 `weak`，性能可能更好；
- 单次尝试且不能接受伪失败时用 `strong`。

```cpp
int expected = 0;
while (!state.compare_exchange_weak(expected, 1,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire)) {
    if (expected == 2) {
        break;
    }
}
```

### 7.3 成功序与失败序

`compare_exchange` 有两个内存序：

```cpp
compare_exchange_strong(expected, desired, success_order, failure_order);
```

规则：

- 成功序可以是 `relaxed`、`acquire`、`release`、`acq_rel`、`seq_cst`；
- 失败序只能是 `relaxed`、`acquire`、`seq_cst`；
- 失败序不能比成功序更强；
- 失败不发生写入，所以不能用 release/acq_rel。

Demo 的 `Lifecycle`：

```cpp
std::memory_order_acq_rel,  // 成功：发布新状态 + 接收旧状态
std::memory_order_acquire   // 失败：只读取当前状态
```

### 7.4 ABA 问题

CAS 只比较值，不比较“这段时间有没有变过”。如果一个值从 A 变成 B 又变回 A，CAS 可能误以为没有变化。

典型场景：

- 无锁栈/队列的节点回收；
- 指针复用；
- 版本号不足的乐观锁。

解决方向：

- 加版本号/标记位（tagged pointer）；
- 使用 epoch/hazard pointer 管理生命周期；
- 避免无界 CAS 循环；
- 用成熟的无锁容器库。

本 Demo 的 SPSC 环不依赖 CAS，因此没有 ABA；引用计数和生命周期 CAS 也不涉及节点回收。

## 8. 硬件、缓存与性能

### 8.1 缓存一致性不等于内存顺序

缓存一致性（MESI 等）保证同一地址的写入最终有单一顺序，但不保证不同地址之间的顺序。CPU 仍可通过 store buffer 让后续 load 先执行。

### 8.2 伪共享（false sharing）

两个线程频繁写同一缓存行中的不同变量，会导致缓存行来回弹跳，性能急剧下降。

Demo 的 `SpscRing`：

```cpp
alignas(64) std::atomic<std::size_t> head_;
alignas(64) std::atomic<std::size_t> tail_;
```

生产者和消费者分别写 `head_`/`tail_`，用 64 字节对齐把它们放到不同缓存行，避免伪共享。

注意：

- 64 是常见缓存行大小，不是所有平台都固定；
- C++17 可用 `std::hardware_destructive_interference_size`；
- 过度对齐会增加对象尺寸和缓存占用；
- `SpscRing<Tick>` 在 x86-64 上实测为 192 字节，包含缓冲区指针、容量、两个对齐索引。

### 8.3 原子操作的成本

- relaxed load/store 通常与普通读写接近，但仍可能阻止某些优化；
- acquire/release 在 x86 上通常只是编译器屏障 + 普通指令；
- seq_cst store 在 x86 上通常更重；
- RMW（尤其是 `lock` 前缀）昂贵；
- 高竞争下 CAS 循环会空转，吞吐下降；
- 无锁不等于低延迟，争用和缓存行迁移可能比锁更糟。

### 8.4 目标平台验证

至少做三件事：

1. 在目标架构上跑压力测试；
2. 用 ThreadSanitizer 跑测试；
3. 用 litmus/微基准验证关键协议，而不是只看 x86 开发机。

ThreadSanitizer 能发现数据竞争，但不能证明内存序完全正确；它也不是万能的。

## 9. 常见业务模式

### 9.1 发布/订阅（acquire/release）

```text
写者：初始化数据 -> release store 指针/标志
读者：acquire load 指针/标志 -> 读取数据
```

Demo：`SpscRing`、`ConfigCell`。

### 9.2 引用计数（relaxed + acq_rel）

```text
retain: fetch_add(relaxed)
release: fetch_sub(acq_rel)，归零 delete
```

Demo：`RefCounted`。

### 9.3 状态机 CAS（acq_rel）

```text
Running -> Draining -> Stopped
```

多个线程竞争迁移，只有一个成功；失败者 acquire 读取当前状态。Demo：`Lifecycle`。

### 9.4 全局顺序（seq_cst）

两个线程互相检查对方是否就绪，要求至少一方看到对方。Demo：`SeqCstGate`。

### 9.5 统计计数（relaxed）

只要求最终计数正确，不要求与其他字段一致。Demo：`Metrics`。

### 9.6 观察者/配置快照

读多写少时，发布不可变快照，读者 acquire 读取；写者复制新版本，release 发布。Demo 的 `ConfigCell` 是简化版，真实业务还要处理旧版本回收（RCU/epoch/hazard pointer）。

## 10. Demo 覆盖矩阵

| 知识点 | Demo 位置 | 说明 |
| --- | --- | --- |
| relaxed load/store | `SpscRing` 本地索引、`Metrics` | 原子但不建立跨线程顺序 |
| relaxed RMW | `Metrics::record_tick` | 精确计数 |
| release store | `SpscRing::try_push`、`ConfigCell::publish` | 发布槽位/配置 |
| acquire load | `SpscRing::try_pop`、`ConfigCell::load_acquire` | 接收发布 |
| acq_rel RMW | `RefCounted::release`、`Lifecycle::transition` | 引用计数/状态迁移 |
| consume load | `ConfigCell::load_consume` | 依赖链读取 |
| seq_cst | `SeqCstGate` | 全局总顺序/Dekker |
| CAS 成功/失败序 | `Lifecycle` | 状态机 |
| 伪共享 | `SpscRing` 的 `alignas(64)` | 索引分缓存行 |
| 数据竞争 | 文档反例、测试 | relaxed 发布错误等 |
| 原子尺寸/锁自由 | Demo 启动输出 | `is_lock_free` |
| 无锁队列协议 | `SpscRing` | 单生产者单消费者 |

## 11. 常见陷阱清单

1. 用 `relaxed` 标志发布普通数据。
2. 只写 release 没有 acquire，或只写 acquire 没有 release。
3. 认为 `seq_cst` 能保护结构体里的普通字段。
4. 认为 `volatile` 可以替代 `std::atomic`。
5. 在 `compare_exchange` 失败序用 `release`/`acq_rel`。
6. 忘记 `compare_exchange_weak` 会伪失败。
7. 把 `acq_rel` 用在普通 load/store 上。
8. 用 `memory_order_consume` 但通过无依赖路径访问数据。
9. 认为 x86 测试通过就代表 ARM 正确。
10. 认为原子计数正确就代表对象生命周期安全。
11. 引用计数 `retain` 用 `acq_rel`，造成不必要的开销（通常不是错，但没必要）。
12. 引用计数 `release` 用 `relaxed`，析构可能看不到其他线程的写。
13. 多个线程同时写同一个 `shared_ptr` 实例（不是不同实例）。
14. 用 `owner_before` 做值比较。
15. 让缓存/注册表持有强引用，意外延长生命周期。
16. 在信号处理函数里使用非原子共享数据。
17. 使用锁自由结构但没有处理 ABA/内存回收。
18. 认为 `is_lock_free()` 为 true 就一定是 wait-free。
19. 用 `memory_order_seq_cst` 包住所有东西，却不分析真实瓶颈。
20. 没有在目标架构和 TSan 下验证。

## 12. 阅读 Demo 的顺序

1. 先看 `include/mo/spsc_ring.h`，理解 relaxed + acquire/release 的最小发布协议。
2. 再看 `config_cell.h`，理解指针发布和 consume 的依赖链。
3. 看 `ref_counted.h`，理解 relaxed retain + acq_rel release。
4. 看 `lifecycle.h`，理解 CAS 成功/失败序。
5. 看 `seq_cst_gate.h`，理解全局总顺序解决的具体问题。
6. 看 `metrics.h`，理解 relaxed 的适用边界。
7. 最后看 `src/main.cpp` 和 `tests/`，把协议放进并发场景。

下一篇 `02-demo-walkthrough.md` 会逐文件证明这些协议为什么正确。
