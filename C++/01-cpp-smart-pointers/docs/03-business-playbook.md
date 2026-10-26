# 智能指针业务落地手册

本文不再解释 API，而是回答工程问题：

- 这个对象应该由谁拥有？
- 接口应该传 `T*`、`T&`、`unique_ptr<T>` 还是 `shared_ptr<T>`？
- 缓存、观察者、异步任务、插件、C ABI 分别该用什么？
- 代码评审时，哪些写法必须拦下？
- 从 C++11 升级到 C++17/20 时，哪些模式可以简化？

## 1. 所有权决策树

```text
谁负责销毁对象？
├── 只有一个明确所有者
│   ├── 生命周期与作用域一致          -> 栈对象 / 成员对象
│   ├── 需要延迟创建、可空、可转移    -> std::unique_ptr<T>
│   └── 数组                          -> std::unique_ptr<T[]>
├── 多个模块共享生命周期
│   ├── 对象由 shared_ptr 创建        -> std::shared_ptr<T>
│   ├── 对象来自 C API / 第三方库     -> shared_ptr<T>(raw, deleter)
│   ├── 对象需要跨 C ABI / 高频分配   -> intrusive_ptr + retain/release
│   └── 只是观察，不拥有              -> T* / T& / weak_ptr<T>
└── 不确定
    ├── 先按 unique_ptr 设计
    └── 确实需要共享时再提升为 shared_ptr
```

经验规则：

1. **默认 `unique_ptr`**。独占所有权最容易推理，性能最好，也最容易改成共享。
2. **`shared_ptr` 是共享所有权的表达，不是“方便的内存管理”**。如果一个对象实际上只有一个所有者，用 `shared_ptr` 会掩盖生命周期，并引入原子计数成本。
3. **观察者永远不拥有被观察者**。成员回调、缓存、父指针、订阅列表默认 `weak_ptr` 或裸指针（前提是生命周期由外部保证）。
4. **工厂返回 `unique_ptr`，在共享点转换**。不要所有工厂一上来就返回 `shared_ptr`。
5. **跨 ABI 优先 C 句柄 + 显式 retain/release**，不要把 `std::shared_ptr` 放进 C 接口。

### 1.1 选型对照表

| 场景 | 推荐 | 不推荐 | 原因 |
| --- | --- | --- | --- |
| 局部资源 | 栈对象 | `new` + 手动 delete | 异常安全、无分配 |
| 可空成员 | `unique_ptr<T>` | `T*` + 手写析构 | 所有权清晰 |
| 数组 | `unique_ptr<T[]>` / `vector` | `shared_ptr<T>` 包数组 | 需要正确 delete[] |
| 工厂 | `unique_ptr<T>` | 裸指针 | 调用方必须接管 |
| 共享配置 | `shared_ptr<const Config>` | 可变共享对象 | 不可变快照避免锁 |
| 缓存 | `weak_ptr<T>` + 容量策略 | `shared_ptr<T>` 无上限 | 避免缓存保活 |
| 父指针 | `weak_ptr` | `shared_ptr` | 避免循环 |
| 订阅者 | `weak_ptr` | `shared_ptr` | 订阅不应延长寿命 |
| 异步任务保活 | 值捕获 `shared_ptr` | 捕获裸 `this` | 防止悬空 |
| 异步任务可取消 | 值捕获 `weak_ptr` | 捕获裸 `this` | 对象销毁后自动跳过 |
| C 资源 | `unique_ptr<T, Deleter>` | 手动 close | 所有路径统一释放 |
| 跨 C ABI 对象 | 不透明句柄 + retain/release | 传 `shared_ptr` | ABI 和标准库耦合 |
| 高频小对象 | 对象池 / `make_shared` | 每次 `shared_ptr(new T)` | 分配次数与局部性 |
| 多态删除 | 虚析构 + `unique_ptr<Base>` | 无虚析构 | 未定义行为 |

## 2. 接口设计规则

### 2.1 函数参数

| 参数语义 | 写法 | 说明 |
| --- | --- | --- |
| 只读，不持有 | `const T&` | 最通用；调用方保证生命周期 |
| 可空，不持有 | `const T*` | 明确表达“可能没有” |
| 只使用，不涉及所有权 | `T&` / `T*` | 不要传 `shared_ptr` 只为了调用方法 |
| 转移所有权 | `std::unique_ptr<T>` 按值 | 调用方必须 `std::move` |
| 共享所有权 | `std::shared_ptr<T>` 按值 | 需要长期持有时才复制 |
| 可能共享，但不一定要 | `const std::shared_ptr<T>&` | 只在确实可能不复制时使用 |

反例：

```cpp
void process(const std::shared_ptr<Frame>& frame); // 不持有却要求 shared_ptr
```

问题：

- 强制调用方使用 `shared_ptr`，即使栈对象完全够用；
- 增加一次间接层；
- 暗示函数可能保存所有权，但实际没有。

更好的设计：

```cpp
void process(const Frame& frame);          // 同步、不持有
void enqueue(Frame frame);                 // 需要异步持有时按值
void attach(std::shared_ptr<Observer> o);  // 明确共享
```

### 2.2 返回值

```cpp
std::unique_ptr<Decoder> create_decoder();     // 独占、可空
Frame make_frame(...);                          // 值语义、RAII
std::shared_ptr<const Config> snapshot() const; // 不可变共享快照
```

不要返回裸指针表达所有权；不要返回 `shared_ptr<T>&` 暴露内部状态；不要返回 `unique_ptr<T>` 的引用。

### 2.3 工厂与转换点

推荐路径：

```cpp
std::unique_ptr<Resource> owned = make_resource(); // 独占创建
std::shared_ptr<Resource> shared = std::move(owned); // 确认共享时提升
```

原因：

- 创建阶段不需要原子计数；
- 提升到 `shared_ptr` 时控制块只分配一次；
- 所有权变化在代码中可见。

不推荐：

```cpp
std::shared_ptr<Resource> r(new Resource); // 两次分配，且可能被异常路径影响
```

至少在 C++11 用 `std::make_shared`；需要自定义删除器时才用 `shared_ptr(raw, deleter)`。

### 2.4 类成员

```cpp
class Decoder {
    struct Impl;
    std::unique_ptr<Impl> impl_; // pimpl，独占
};

class Node {
    std::weak_ptr<Node> upstream_;                 // 反向边
    std::vector<std::shared_ptr<Node>> children_;  // 正向拥有
};

class FrameCache {
    std::unordered_map<Id, std::weak_ptr<FrameBuffer>> entries_; // 不保活
};
```

成员选择的核心问题不是“能不能编译”，而是“谁应该决定这个成员的死亡时间”。

### 2.5 不要滥用 `shared_ptr`

以下场景通常不需要 `shared_ptr`：

- 函数内部临时对象；
- 生命周期与父对象完全一致的成员；
- 只需要在函数调用期间借用；
- 单线程、明确所有者的资源；
- 可以用 `unique_ptr` + 移动表达转移的地方。

`shared_ptr` 的代价：

- 控制块分配（除非 `make_shared` 合并）；
- 原子引用计数；
- 生命周期不再由单个作用域决定；
- 容易形成环；
- 容易让缓存/注册表/回调意外保活。

## 3. 业务模式

### 3.1 零拷贝视图：aliasing constructor

适用场景：

- 图像 ROI、音频切片、网络包分段；
- 大缓冲区上的多个逻辑对象；
- 需要共享底层存储，但每个视图有自己的元数据。

模式：

```cpp
std::shared_ptr<const uint8_t> pixels(owner, owner->data() + offset);
```

注意：

- 视图持有控制块，会延长底层缓冲区生命周期；
- 视图的 `get()` 不等于底层对象地址；
- 不能用 `shared_ptr::get()` 反推 owner；
- 需要身份比较时用 `owner_before`/`owner_less`；
- 不要长期保存 owner key，除非确实需要保活。

测试参照：`tests/test_frame.cpp`。

### 3.2 弱缓存

适用场景：

- 解码帧缓存；
- 纹理/中间结果缓存；
- 任何“有则加速，无则重算”的数据。

模式：

```cpp
std::weak_ptr<Value> weak = entry;
if (auto value = weak.lock()) return value;
// 重新计算并 put
```

注意：

- 缓存条目过期后要清理，否则 map 无限增长；
- `size()` 是否包含过期条目要定义清楚；
- 多线程下 `lock()` 和 erase 要在同一锁策略下；
- 不要在持有锁时执行昂贵回调；
- 需要“缓存保活”时改用 `shared_ptr` + LRU 容量上限，不要假装弱缓存。

测试参照：`tests/test_cache.cpp`。

### 3.3 观察者/订阅

适用场景：

- UI 事件；
- Pipeline 节点通知；
- 日志/监控订阅；
- 插件监听配置变化。

模式：

```cpp
std::vector<std::weak_ptr<Observer>> observers_;
```

通知时：

1. 锁内 `lock()` 并复制到局部强引用列表；
2. 删除过期条目；
3. 锁外回调。

注意：

- 观察者回调中可能再次注册/注销，不能在遍历原列表时直接修改；
- 回调异常策略要明确：传播、隔离还是记录；
- 如果业务要求“订阅即保活”，必须显式说明，并检查环；
- 跨线程通知要定义执行线程和顺序。

测试参照：`tests/test_pipeline.cpp`。

### 3.4 父子/图关系

推荐：

```cpp
class Node {
    std::weak_ptr<Node> parent_;
    std::vector<std::shared_ptr<Node>> children_;
};
```

规则：

- 强边方向 = 所有权方向；
- 反向边默认弱；
- 图允许环时，至少要有一条边弱，或者使用外部生命周期管理；
- `enable_shared_from_this` 只用于“确定由 shared_ptr 管理”的类；
- 构造/析构期间不要调用 `shared_from_this()`；
- 加边前做环检测和重复父节点检测。

测试参照：`tests/test_graph.cpp`。

### 3.5 异步任务

三种捕获策略：

| 策略 | 写法 | 语义 | 适用 |
| --- | --- | --- | --- |
| 保活 | `[self = shared_from_this()]` | 任务期间对象不销毁 | 必须完成的后台工作 |
| 可取消 | `[weak = weak_ptr(shared_from_this())]` | 对象销毁后跳过 | UI 回调、周期刷新 |
| 不持有 | `[&]` / 裸指针 | 调用方保证生命周期 | 同步、短生命周期，谨慎 |

C++11 没有初始化捕获，写成：

```cpp
std::weak_ptr<Node> weak_node = shared_from_this();
queue.post([weak_node]() {
    if (auto node = weak_node.lock()) node->work();
});
```

注意：

- 不要捕获裸 `this` 进入异步任务；
- 任务队列的关闭语义要明确：丢弃、执行完还是取消；
- 长任务需要取消标志，不能只靠 `weak_ptr`；
- `shutdown()` 不要从 worker 内部调用；
- 异常必须被隔离，否则 worker 可能退出；
- 任务持有 `shared_ptr` 会延长对象寿命，可能掩盖资源泄漏。

测试参照：`tests/test_async.cpp`。

### 3.6 插件/配置热升级

适用场景：

- 算法插件热替换；
- 运行时配置快照；
- 读多写少的注册表；
- 灰度/AB 实验。

模式：

```cpp
using Map = std::map<std::string, std::shared_ptr<Plugin>>;
std::shared_ptr<const Map> current = slot_.load();

auto next = std::make_shared<Map>(*current);
(*next)[name] = plugin;
slot_.store(next);
```

规则：

- 读路径拿不可变快照，不加锁；
- 写路径复制 + 原子发布；
- 稳定槽位名与实现名分离；
- 旧插件在途调用期间不能销毁；
- 写-写用互斥锁串行化；
- 插件数量很大时评估复制成本；
- 跨版本热升级要考虑 ABI/数据格式兼容。

测试参照：`tests/test_registry.cpp`。

### 3.7 C 资源与 C ABI

C 资源：

```cpp
using Handle = std::unique_ptr<CResource, void (*)(CResource*)>;
```

规则：

- 删除器必须 `noexcept`；
- open 失败返回空时要检查；
- 不要把 `unique_ptr` 的 `release()` 结果再包一层而不处理异常；
- 需要共享时优先 `unique_ptr -> shared_ptr`，保留删除器；
- 不要假设 C 资源的释放顺序无关紧要。

跨 ABI：

- 不要把 `std::shared_ptr` 作为 C 接口参数；
- 不要假设不同编译器/标准库的 `shared_ptr` 布局一致；
- 使用不透明句柄 + 导出的 `retain/release`；
- 侵入式计数适合对象布局稳定的场景；
- Windows DLL 边界还要考虑 CRT/分配器一致性问题；
- 更稳妥的方案是 C 句柄 + 显式生命周期函数，内部再用 C++ 智能指针。

测试参照：`tests/test_resources.cpp`、`tests/test_intrusive.cpp`。

### 3.8 侵入式引用计数

适用场景：

- 对象需要跨 ABI 传递；
- 对象尺寸敏感；
- 已有对象内引用计数；
- 需要从裸指针直接增加引用；
- 高频创建/销毁，想避免单独控制块分配。

代价：

- 计数逻辑侵入类型；
- 容易误用 `add_ref=false`；
- 需要自己保证线程安全；
- 需要自己实现 cast；
- 与标准 `shared_ptr` 互操作要特别小心（不能同时用两套计数）。

规则：

- 构造时计数为 0，工厂 `make_intrusive` retain 到 1；
- `detach()` 后必须有人接管；
- `adopt(raw, add_ref=false)` 只能接管“已计入且唯一”的引用；
- `release()` 在计数归零时 `delete this`；
- 析构函数 protected，防止外部 delete；
- 多态基类析构必须 virtual。

## 4. 并发规则

### 4.1 标准保证

- 不同的 `shared_ptr` 实例可以并发修改，即使它们共享同一个被管理对象；
- 同一 `shared_ptr` 实例的并发读写不是线程安全的；
- 引用计数本身是原子的；
- `weak_ptr::lock()` 与最后一个 `shared_ptr` 释放竞争时，要么得到有效指针，要么得到空；
- 被管理对象的成员访问仍需业务锁。

### 4.2 同一实例的共享

C++11 方案：

```cpp
std::shared_ptr<T> load() const { return std::atomic_load(&slot_); }
void store(std::shared_ptr<T> value) { std::atomic_store(&slot_, std::move(value)); }
```

注意：

- 这些自由函数在 C++20 被弃用，应迁移到 `std::atomic<std::shared_ptr<T>>`；
- 读-改-写需要额外互斥；
- 不要 `atomic_load` 后直接修改对象；对象本身仍需同步或不可变。

### 4.3 回调与锁

- 不要在持锁时调用用户回调；
- 先复制强引用列表，再解锁回调；
- 回调可能重入注册/注销；
- 回调可能抛异常，需要隔离策略；
- 避免回调中销毁最后一个 `shared_ptr` 导致在错误线程析构。

### 4.4 析构线程

`shared_ptr` 的最后一个引用可能在任意线程释放，因此：

- 析构函数不要假设在创建线程执行；
- 需要线程亲和性的资源（GPU context、UI 对象）必须显式调度销毁；
- 不要在析构函数中调用可能阻塞的全局单例；
- 跨线程对象销毁要考虑日志/监控顺序。

## 5. 性能与分配

### 5.1 开销模型（x86-64 典型值）

| 类型 | 大小 | 说明 |
| --- | --- | --- |
| `unique_ptr<T>` | 8 | 默认删除器为空 |
| `unique_ptr<T, void(*)(T*)>` | 16 | 指针 + 函数指针 |
| `shared_ptr<T>` | 16 | 对象指针 + 控制块指针 |
| `weak_ptr<T>` | 16 | 对象指针 + 控制块指针 |
| `intrusive_ptr<T>` | 8 | 仅对象指针 |

具体值依赖 ABI 和实现，Demo 的 `test_sizes.cpp` 做了运行时断言，避免文档与平台脱节。

### 5.2 分配次数

- `shared_ptr<T>(new T)`：对象分配 + 控制块分配，2 次；
- `make_shared<T>`：合并分配，1 次；
- `allocate_shared<T>`：合并分配 + 自定义分配器；
- `weak_ptr` 存活会延迟合并块释放；
- 对象很大且弱引用长期存在时，合并分配可能不划算。

### 5.3 优化顺序

1. 先减少不必要的共享所有权；
2. 用 `unique_ptr` 表达独占；
3. 高频路径用 `make_shared`/对象池；
4. 避免在热路径复制 `shared_ptr`；
5. 需要跨线程传值时按值移动，而不是复制；
6. 最后才考虑自定义分配器和侵入式计数。

不要为了“省一次原子操作”引入复杂的自定义所有权，除非有测量数据。

## 6. 异常安全

### 6.1 基本规则

- 资源获取即初始化（RAII）；
- 一个资源只由一个智能指针拥有；
- 构造失败时已构造的成员会自动释放；
- 删除器不抛异常；
- 析构函数不抛异常；
- 多资源获取时注意顺序，避免部分成功泄漏。

### 6.2 函数实参求值顺序

```cpp
consume(std::unique_ptr<Foo>(new Foo), may_throw());
```

C++11/14 中实参求值顺序未指定，可能先执行 `may_throw()`。使用 `make_unique` 把分配和接管绑定在一个函数调用内。

### 6.3 `reset` 与自赋值

```cpp
p.reset(p.get()); // 错误：先释放再接管同一指针
```

`intrusive_ptr::reset` 中有 `p == p_` 保护；标准 `shared_ptr::reset(p)` 对同一指针也有特殊处理，但不要把这种边界当成常规写法。

### 6.4 构造期 `shared_from_this`

对象构造期间 `weak_this` 还没建立，调用 `shared_from_this()` 会抛 `std::bad_weak_ptr`。不要把 `shared_from_this()` 放进构造函数、初始化列表或构造期间启动的线程。

## 7. 代码评审清单

### 7.1 所有权

- [ ] 每个动态对象是否有明确所有者？
- [ ] 是否优先使用 `unique_ptr`？
- [ ] `shared_ptr` 是否真的需要共享，而不是“图省事”？
- [ ] 是否存在 `shared_ptr` 环？
- [ ] 父指针、观察者、缓存是否使用弱引用？
- [ ] 工厂返回类型是否表达所有权？
- [ ] 是否存在裸 `new`/`delete`？
- [ ] 是否存在 `release()` 后无人接管？

### 7.2 生命周期

- [ ] 异步任务是否捕获裸 `this`？
- [ ] 回调是否可能比对象活得更久？
- [ ] 缓存/注册表是否意外保活？
- [ ] `enable_shared_from_this` 是否只用于 shared_ptr 管理的对象？
- [ ] 构造/析构期间是否调用了 `shared_from_this()`？
- [ ] 最后一个 `shared_ptr` 是否可能在错误线程释放？
- [ ] 是否存在“对象已销毁但原始指针还在用”？

### 7.3 接口

- [ ] 是否用 `const shared_ptr<T>&` 表达了不持有的语义？
- [ ] 是否返回了内部 `shared_ptr` 的引用？
- [ ] 是否在 C ABI 中暴露了 C++ 智能指针？
- [ ] 删除器是否 `noexcept`？
- [ ] 多态基类是否有虚析构？
- [ ] pimpl 的析构/移动是否在 cpp 中定义？

### 7.4 并发

- [ ] 同一 `shared_ptr` 实例是否被多线程读写？
- [ ] 是否误以为“引用计数原子”等于“对象线程安全”？
- [ ] 回调是否在持锁时执行？
- [ ] 观察者列表是否在遍历时被修改？
- [ ] 任务队列关闭后是否还接受任务？
- [ ] 是否存在 worker 内调用 `shutdown()`？

### 7.5 性能

- [ ] 热路径是否频繁复制 `shared_ptr`？
- [ ] 是否可以用 `unique_ptr` 替代？
- [ ] 是否每次都用 `shared_ptr(new T)` 而不是 `make_shared`？
- [ ] 是否存在长期 `weak_ptr` 导致合并块延迟释放？
- [ ] 是否在无测量数据的情况下引入侵入式计数？

### 7.6 测试

- [ ] 是否有生命周期断言（alive/weak expired）？
- [ ] 是否覆盖移动后源对象状态？
- [ ] 是否覆盖异常路径？
- [ ] 是否覆盖并发关闭/析构竞争？
- [ ] 是否用 ASan/TSan 跑过？
- [ ] 是否只在 Debug 下通过？

## 8. 常见反模式

### 8.1 用 `shared_ptr` 包一切

症状：

- 所有对象都 `shared_ptr`；
- 函数参数全是 `shared_ptr`；
- 生命周期仍然靠“感觉”；
- 出现无法解释的内存增长。

修复：

- 从 `unique_ptr` 开始；
- 只在真正共享所有权的地方使用 `shared_ptr`；
- 观察关系改弱引用/裸指针；
- 用所有权图梳理环。

### 8.2 缓存保活

症状：

- 缓存命中率很高，但内存一直上涨；
- 对象销毁后缓存里仍有强引用；
- `weak_ptr` 改成 `shared_ptr` 后“问题消失”。

修复：

- 缓存默认 `weak_ptr`；
- 需要保活时设置容量上限和淘汰策略；
- 元数据表不要用强 `shared_ptr` 作为 key，除非明确需要保活。

### 8.3 异步捕获裸 `this`

症状：

- 偶发崩溃，尤其在关闭窗口/销毁对象时；
- ASan 报告 heap-use-after-free；
- 任务执行时间越长，崩溃概率越高。

修复：

- 保活：捕获 `shared_from_this()`；
- 可取消：捕获 `weak_ptr` 并 `lock()`；
- 关闭时等待任务或设置取消标志。

### 8.4 循环引用

症状：

- 对象 alive 计数不归零；
- 内存缓步增长；
- 断开某个链接后突然释放。

修复：

- 明确强边方向；
- 反向边用 `weak_ptr`；
- 加环检测；
- 在生命周期结束时显式断链；
- 用测试断言 alive 归零。

### 8.5 把 `owner_before` 当值比较

症状：

- 两个空 aliasing 指针被 `==` 判为相等；
- 不同控制块被误认为同一对象；
- map 查找结果错误。

修复：

- 身份比较用 `owner_before`/`owner_less`；
- 值比较用 `*p == *q`；
- 指针比较用 `p.get() == q.get()`，但要理解 aliasing 语义。

### 8.6 在 C ABI 传 `shared_ptr`

症状：

- 不同编译器/标准库版本崩溃；
- Windows DLL 边界释放崩溃；
- 调试版/发布版混用崩溃。

修复：

- 用不透明句柄；
- 导出 `create/destroy/retain/release`；
- 内部实现随便用智能指针，接口只暴露 C 类型；
- 分配和释放必须在同一模块/CRT。

## 9. C++11 到 C++17/20 的迁移

| 旧写法 | 新写法 | 标准 |
| --- | --- | --- |
| `spl::make_unique<T>(...)` | `std::make_unique<T>(...)` | C++14 |
| `weak_ptr(shared_from_this())` | `weak_from_this()` | C++17 |
| `shared_ptr<T[]>` 不可用 | `std::shared_ptr<T[]>` | C++17 |
| `atomic_load(&sp)` | `std::atomic<std::shared_ptr<T>>` | C++20 |
| `make_shared<T[]>` 不可用 | `std::make_shared<T[]>(n)` | C++20 |
| 初始化捕获不可用 | `[self = shared_from_this()]` | C++14 |
| `owner_less<shared_ptr<T>>` | `owner_less<void>` | C++17 起更通用 |

迁移建议：

1. 先替换机械 API（`make_unique`、`weak_from_this`）；
2. 再替换并发共享槽（`atomic<shared_ptr>`）；
3. 最后评估是否使用数组 `shared_ptr`；
4. 不要因为升级标准就全面改写所有权模型；
5. 升级后重新跑生命周期测试和 sanitizer。

## 10. 场景决策速查

| 业务问题 | 首选方案 | 关键测试 |
| --- | --- | --- |
| 图片 ROI 零拷贝 | aliasing `shared_ptr` + `Frame` 视图 | ROI 地址、owner 生命周期 |
| 帧缓存 | `weak_ptr` + 惰性清理 | 过期后 miss、size 归零 |
| 场景树 | 强子 + 弱父 | 销毁后 alive 归零 |
| 事件订阅 | `weak_ptr` 观察者 | 观察者销毁后不再回调 |
| 后台任务必须完成 | 强捕获 `shared_ptr` | 任务期间 alive=1 |
| 后台任务可取消 | 弱捕获 + `lock()` | 对象销毁后跳过 |
| 插件热升级 | 不可变快照 + 原子发布 | 在途调用持有旧插件 |
| C 资源 | `unique_ptr` + 自定义删除器 | close 次数 |
| 跨 ABI GPU 资源 | 侵入式计数 + C 句柄 | retain/release 平衡 |
| pimpl | `unique_ptr<Impl>` + cpp 析构 | 移动、moved-from |
| 共享配置 | `shared_ptr<const Config>` | 发布后不可变 |
| 高频小对象 | 对象池/`make_shared` | 分配次数 |

## 11. 最后的原则

1. **所有权是设计问题，不是语法问题。** 先画清楚谁拥有谁，再选智能指针。
2. **默认独占，按需共享。** `unique_ptr` 应该是大多数动态对象的起点。
3. **弱引用是打破环和观察关系的默认工具。** 但弱引用不是“自动安全”，调用方必须处理过期。
4. **不要用智能指针掩盖生命周期。** 如果一段代码需要靠 `shared_ptr` 才能不崩溃，先检查设计。
5. **让测试证明生命周期。** alive 计数、`weak_ptr::expired()`、close 次数、分配次数，比注释可靠。
6. **让失败路径显式。** 空指针、越界、环、关闭后投递、moved-from 调用，都应该有明确行为。
7. **跨边界时优先稳定 ABI。** C 句柄、显式 retain/release、不可变快照，通常比直接传 `shared_ptr` 更可靠。
