# Demo 代码逐模块解析

本文按数据流和依赖顺序拆解 Demo。每一节包含：

- 业务问题；
- 关键代码与文件位置；
- 智能指针知识点；
- 对应测试以及它为什么能证明结论；
- 业务落地时的注意事项。

文件位置使用 `path:line` 形式，行号以当前代码为准。

## 0. 全局视角

Demo 的数据流：

```text
make_frame_buffer
      |
      v
Frame::wrap ---- aliasing ----> Frame::slice (ROI)
      |                              |
      v                              v
FrameCache (weak)             OwnerRegistry (owner_less)
      |
      v
Pipeline (Node, weak upstream, weak observers)
      |
      v
TaskQueue (strong/weak capture)
      |
      v
PluginRegistry (atomic snapshot, hot swap)
      |
      v
C resource / intrusive GPU buffer / pimpl decoder
```

核心原则：

- 创建和销毁资源的地方用 `unique_ptr` 或 `make_shared`；
- 需要共享生命周期的地方用 `shared_ptr`；
- 所有“观察/缓存/反向边/异步取消”默认 `weak_ptr`；
- 需要身份比较时用 `owner_before`/`owner_less`，不用 `==`；
- 跨 C ABI 或高频对象使用侵入式计数。

## 1. 生命周期探针：`Tracked`

文件：`include/spl/tracked.h`

### 1.1 代码

```cpp
template <typename Tag>
class Tracked {
public:
    static std::size_t alive() { ... }
    static std::size_t created() { ... }
    static std::size_t destroyed() { ... }
protected:
    Tracked() { ++alive_; ++created_; }
    ~Tracked() { --alive_; ++destroyed_; }
private:
    static std::atomic<std::size_t> alive_;
    ...
};
```

### 1.2 原理与设计

- 每个业务类通过 `Tracked<SomeTag>` 获得独立的静态计数器；`Tag` 是空结构体，仅用于区分类型。
- 构造函数/析构函数递增/递减 `alive`，测试可以断言“对象确实被销毁”，而不是只看弱引用是否过期。
- 空基类不增加对象大小（EBO），不会干扰尺寸实验。
- 计数器是 `std::atomic`，可在多线程测试中观察；但测试中的 `reset_counters()` 只能在确定没有存活对象时调用，否则计数会失真。
- 析构函数是 `protected` 且非虚：防止通过 `Tracked<Tag>*` 删除派生对象；派生类自己决定是否虚析构。

### 1.3 测试

- `test_intrusive.cpp:24`：复制/移动/释放后 alive 从 1 到 0。
- `test_frame.cpp:30`：ROI 释放后缓冲区 alive 归零。
- `test_graph.cpp:51`：循环引用时 alive=2，断链后 alive=0。

这种“计数探针”比在析构函数里打印日志更可靠，因为测试可以对它做断言，也避免了输出顺序干扰。

## 2. C++11 的 `make_unique`

文件：`include/spl/memory_utils.h:12`

### 2.1 代码

```cpp
template <typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

template <typename T>
std::unique_ptr<T[]> make_unique_array(std::size_t count) {
    return std::unique_ptr<T[]>(new T[count]());
}
```

### 2.2 为什么需要自己实现

C++11 没有 `std::make_unique`，C++14 才进入标准。直接写：

```cpp
std::unique_ptr<Foo> p(new Foo);
```

在简单场景没问题，但在函数实参中可能因为求值顺序泄漏：

```cpp
consume(std::unique_ptr<Foo>(new Foo), may_throw());
```

`may_throw()` 可能先执行并抛出，`new Foo` 已经分配但还没被 `unique_ptr` 接管。`make_unique` 把 `new` 和接管封装进函数体内部，消除了这个窗口。

### 2.3 测试

`tests/test_make_unique.cpp:7` 和 `:17` 验证：

- 返回的 `unique_ptr` 非空且值正确；
- 移动后源为空；
- 数组版本对元素做值初始化（全 0）。

### 2.4 业务注意

- `make_unique_array<T>(n)` 对数组做值初始化；如果 T 是大型结构体，考虑是否真的需要清零。
- 不要用 `unique_ptr<T[]>` 管理需要 size 的缓冲区而不保存 size；Demo 的 `FrameBuffer` 同时保存 `size_`。
- 升级到 C++14 后直接用 `std::make_unique`，删除这个头文件。

## 3. 手写侵入式指针：`intrusive_ptr`

文件：`include/spl/intrusive_ptr.h`

### 3.1 `RefCounted`：对象内计数

`include/spl/intrusive_ptr.h:11`

```cpp
class RefCounted {
public:
    void retain() const {
        refs_.fetch_add(1, std::memory_order_relaxed);
    }

    bool release() const {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
            return true;
        }
        return false;
    }
protected:
    virtual ~RefCounted() {}
private:
    mutable std::atomic<long> refs_;
};
```

设计解释：

- `refs_` 初始为 0；对象刚构造时还没有所有者。
- `retain` 用 `relaxed`：增加引用不会发布对象内容，只需要原子性。
- `release` 用 `acq_rel`：最后一个释放者需要看到其他线程对对象的写入，并保证析构前的同步。
- `delete this` 发生在计数从 1 减到 0 时；析构函数是 `protected virtual`，既允许通过基类指针正确删除派生对象，又禁止外部 `delete`。
- `mutable` 让 `const` 成员函数也能修改计数；计数是对象元数据，不是逻辑状态。

### 3.2 `intrusive_ptr` 的构造/复制/移动

`include/spl/intrusive_ptr.h:41`

```cpp
explicit intrusive_ptr(T* p, bool add_ref = true) : p_(p) {
    if (p_ && add_ref) p_->retain();
}

intrusive_ptr(const intrusive_ptr& other) : p_(other.p_) {
    if (p_) p_->retain();
}

intrusive_ptr(intrusive_ptr&& other) : p_(other.p_) {
    other.p_ = 0;
}
```

- 默认构造为空；
- 从裸指针构造默认 `add_ref=true`，适合接管一个“尚未计数”的新对象；
- 复制增加计数；
- 移动直接偷指针，源置空，不增加计数；
- 转换构造/赋值支持 `Derived -> Base`，通过 `std::is_convertible` 做 SFINAE。

### 3.3 `detach` / `adopt` 与 ABI 交接

`include/spl/intrusive_ptr.h:120`、`:127`

```cpp
void reset(T* p, bool add_ref = true) {
    if (p == p_) return;
    intrusive_ptr(p, add_ref).swap(*this);
}

T* detach() {
    T* result = p_;
    p_ = 0;
    return result;
}
```

业务场景：一个 C ABI 函数需要把 GPU 缓冲区所有权传出去：

```cpp
GpuBuffer* raw = owner.detach();       // 计数不变，但 owner 不再持有
intrusive_ptr<GpuBuffer> adopted(raw, false); // 接管这 1 个计数
```

关键规则：

- `detach()` 只是放弃指针，不改变计数；调用者必须接管。
- `add_ref=false` 表示“接管一个已经计入的引用”，不能用于一个还有别的所有者的对象，否则会偷走别人的引用。
- `reset(p, false)` 中 `p == p_` 的保护避免“先释放再接管同一指针”的双重释放。
- Demo 的 main 先 `copy.reset()`，确保 detach 的是唯一引用，再 adopt。

### 3.4 `make_intrusive` 与初始计数

`include/spl/intrusive_ptr.h:169`

```cpp
template <typename T, typename... Args>
intrusive_ptr<T> make_intrusive(Args&&... args) {
    return intrusive_ptr<T>(new T(std::forward<Args>(args)...));
}
```

`RefCounted` 初始计数为 0，所以这里使用默认 `add_ref=true`，把计数从 0 增到 1。另一种设计是对象构造时计数为 1，工厂用 `add_ref=false` 接管，可以少一次原子操作，但要求所有创建路径严格一致。Demo 选择“显式增加一次、语义更不容易错”的版本。

### 3.5 转换与 cast

`include/spl/intrusive_ptr.h` 末尾提供：

```cpp
intrusive_ptr<T> static_pointer_cast(const intrusive_ptr<U>&);
intrusive_ptr<T> dynamic_pointer_cast(const intrusive_ptr<U>&);
intrusive_ptr<T> const_pointer_cast(const intrusive_ptr<U>&);
```

它们返回新的 `intrusive_ptr`，会增加计数；源对象仍然持有自己的引用。`dynamic_pointer_cast` 失败返回空指针，不抛异常。

### 3.6 业务对象 `GpuBuffer`

`include/spl/gpu_buffer.h`

```cpp
class GpuBuffer : public RefCounted, public Tracked<GpuBufferTag> {
    std::unique_ptr<std::uint8_t[]> data_;
    std::size_t bytes_;
    int device_;
};
```

- `RefCounted` 提供侵入式计数；
- `Tracked` 用于测试存活数量；
- `unique_ptr<uint8_t[]>` 管理实际内存；
- `make_gpu_buffer` 返回 `intrusive_ptr<GpuBuffer>`。

为什么不用 `shared_ptr`？这个对象模拟 GPU/驱动资源，需要跨 C ABI 传裸句柄，并且尺寸敏感。侵入式计数让句柄就是一个指针，C 侧只要调用 `retain/release`（或通过导出的函数）即可参与所有权。

### 3.7 测试

`tests/test_intrusive.cpp`：

- `:24` 复制、作用域、reset、move 的计数变化；
- `:51` detach/adopt 模拟 ABI 交接；
- `:68` `Derived -> Base` 转换和动态 cast。

## 4. C 资源与自定义删除器

文件：`include/spl/resource.h`、`src/resource.cpp`

### 4.1 资源类型

```cpp
struct CResource { explicit CResource(int id) : id(id) {} int id; };

CResource* c_resource_open(int id);
void c_resource_close(CResource* resource) noexcept;

typedef std::unique_ptr<CResource, void (*)(CResource*)> UniqueResource;
typedef std::shared_ptr<CResource> SharedResource;
```

`UniqueResource` 的删除器是函数指针：

- 类型安全，删除器是 `unique_ptr` 类型的一部分；
- 大小为 2 个机器字（指针 + 函数指针）；
- 不能误用 `delete`。

### 4.2 `unique_ptr` 关闭路径

`src/resource.cpp:53`

```cpp
UniqueResource make_unique_resource(int resource_id) {
    return UniqueResource(c_resource_open(resource_id), &c_resource_close);
}
```

测试 `tests/test_resources.cpp:8`：

- 作用域结束关闭一次；
- open/close 计数一致；
- 移动后源为空、目标继续持有；
- `adopt_resource` 接管裸 C 句柄。

### 4.3 `unique_ptr` 提升为 `shared_ptr`

`src/resource.cpp:61`

```cpp
SharedResource share_resource(UniqueResource resource) {
    return SharedResource(std::move(resource));
}
```

C++11 的 `shared_ptr` 有接收 `unique_ptr&&` 的构造函数；它会把 `unique_ptr` 的删除器移动进控制块。测试 `tests/test_resources.cpp:58` 验证：

- 转换后 `unique_ptr` 为空；
- 多个 `shared_ptr` 共享同一资源；
- 最后一个释放时才关闭；
- 控制块正确保存了函数指针删除器。

### 4.4 为什么不用 `make_shared`

`make_shared` 不支持自定义删除器。C 资源必须用 `shared_ptr(raw, deleter)` 或 `unique_ptr -> shared_ptr`。后者在 Demo 中更自然：资源先由 `unique_ptr` 独占创建，确认需要共享时再提升。

### 4.5 业务注意

- C API 的 `open` 返回值可能为 `nullptr`；`unique_ptr` 不会对空指针调用删除器，但业务上应检查并抛错。
- 删除器必须不抛异常；`c_resource_close` 标记 `noexcept`。
- 不要先 `release()` 再包 `shared_ptr`；异常路径会泄漏。
- 如果 C API 自带引用计数，考虑侵入式指针或自定义 deleter，而不是强行套 `shared_ptr`。

## 5. 零拷贝帧视图与 aliasing constructor

文件：`include/spl/frame.h`、`src/frame.cpp`

### 5.1 `FrameBuffer`

```cpp
class FrameBuffer {
    std::unique_ptr<std::uint8_t[]> data_;
    std::size_t size_;
};
```

`FrameBuffer` 本身独占像素内存，但可以被多个 `Frame` 视图共享。`make_frame_buffer` 用 `make_shared<FrameBuffer>` 创建它。

### 5.2 `Frame::wrap`

`src/frame.cpp:26`

```cpp
Frame Frame::wrap(FrameBufferPtr buffer, int width, int height, int stride) {
    // 校验 geometry 和 buffer->size()
    const std::uint8_t* base = buffer->data();
    std::shared_ptr<const std::uint8_t> pixels(buffer, base);
    return Frame(std::move(pixels), width, height, stride);
}
```

`pixels` 是 aliasing `shared_ptr`：

- 存储指针是 `base`（像素首地址）；
- 控制块来自 `buffer`（`FrameBuffer`）；
- 即使 `buffer` 局部变量析构，`Frame` 仍让 `FrameBuffer` 活着；
- 像素通过 `shared_ptr<const uint8_t>` 暴露为只读，避免多个视图意外修改同一缓冲区。

### 5.3 `Frame::slice`

`src/frame.cpp:55`

```cpp
const std::uint8_t* base =
    pixels_.get() + y * stride_ + x;
std::shared_ptr<const std::uint8_t> pixels(pixels_, base);
return Frame(std::move(pixels), width, height, stride_);
```

- `pixels_` 已经是 aliasing 指针；再次 aliasing 仍然共享同一个控制块。
- `base` 是原缓冲区中的偏移，`stride_` 保留原始行跨度。
- 没有像素拷贝，切片成本是两次原子计数（构造/析构）和对象构造。
- 边界检查在 `Frame::slice` 和 `Frame::at` 中完成，越界抛 `std::out_of_range`。

测试 `tests/test_frame.cpp:9` 断言：

```cpp
roi.data() == full.data() + 2 * 16 + 4;
roi.at(0, 0) == 36;
roi.at(7, 3) == 91;
roi.shares_owner_with(full);
```

### 5.4 生命周期验证

`tests/test_frame.cpp:30`：

```cpp
std::weak_ptr<FrameBuffer> observer = buffer;
Frame full = Frame::wrap(buffer, 16, 8);
Frame roi = full.slice(0, 0, 16, 8);
buffer.reset();
full = Frame();
SPL_CHECK(!observer.expired());   // ROI 仍持有控制块
SPL_CHECK_EQ(roi.owner_use_count(), 1);
roi = Frame();
SPL_CHECK(observer.expired());
```

这证明 aliasing 的“共享所有权”语义：生产者释放后，视图仍有效；最后一个视图释放后，缓冲区才销毁。

### 5.5 `owner_key` 的身份语义

`include/spl/frame.h:81`

```cpp
OwnerKey owner_key() const {
    return OwnerKey(pixels_, static_cast<const void*>(0));
}
```

它产生一个空存储指针、非空控制块的 `shared_ptr<const void>`。测试 `tests/test_frame.cpp:49` 验证：

- 同一缓冲区的 full 和 ROI 的 owner key 共享控制块；
- `operator==` 比较存储指针，两个 null alias 会误判相等；
- `owner_before`/`same_owner` 能区分不同控制块。

Demo 输出中 `roi owner_use_count=3`（buffer + full + roi），创建两个 owner key 后变成 5，正是因为 owner key 也是强引用。

### 5.6 业务注意

- aliasing 不复制数据，但会改变 `use_count`；不要把 owner key 长期存放在全局表里而不清理，否则缓冲区无法释放。
- `shared_ptr<const uint8_t>` 只保证通过该指针不能写；如果底层还有非 const 别名，别处仍可修改。真正的不可变需要设计约束。
- `stride` 与 `width` 不同时，切片后的数据不是连续的；消费者必须按 stride 访问。

## 6. 弱缓存与 `owner_less` 身份表

文件：`include/spl/frame_cache.h`、`src/frame_cache.cpp`

### 6.1 `FrameCache`：弱引用缓存

`src/frame_cache.cpp:5`

```cpp
void FrameCache::put(std::uint64_t id, const FrameBufferPtr& buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[id] = std::weak_ptr<FrameBuffer>(buffer);
}

FrameBufferPtr FrameCache::get(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) return FrameBufferPtr();
    FrameBufferPtr buffer = it->second.lock();
    if (!buffer) entries_.erase(it);
    return buffer;
}
```

- 缓存不拥有对象；`put` 只保存弱引用。
- `get` 在锁内 `lock()`，失败即删除条目；返回的 `shared_ptr` 在调用方手中保持对象存活。
- `size()` 包含已过期但尚未清理的条目；`purge_expired()` 批量清理。这种“惰性删除 + 周期清理”适合缓存。
- 用 `unordered_map` + `mutex` 是 Demo 的可读选择；生产环境可考虑分片锁、RCU 快照或并发哈希表。

测试 `tests/test_cache.cpp:8`：

```cpp
cache.put(7, buffer);
buffer.reset();
SPL_CHECK(observer.expired());
SPL_CHECK_EQ(cache.size(), 1);  // 过期条目还在
SPL_CHECK(!cache.get(7));       // get 顺手删除
SPL_CHECK_EQ(cache.size(), 0);
```

### 6.2 `OwnerRegistry`：按控制块索引元数据

`include/spl/frame_cache.h:27`

```cpp
typedef std::shared_ptr<const void> Key;
typedef std::owner_less<Key> KeyLess;
std::map<Key, std::string, KeyLess> labels_;
```

`annotate` 保存 key；`label_of` 用另一个 aliasing key 查找。由于 `owner_less` 按控制块比较，full 和 ROI 的 key 等价。

测试 `tests/test_cache.cpp:44`：

```cpp
registry.annotate(full.owner_key(), "camera-0");
SPL_CHECK_EQ(registry.label_of(roi.owner_key()), "camera-0");
```

关键陷阱：`Key` 是 `shared_ptr`，map 会强引用控制块，从而延长缓冲区生命周期。测试在清理 registry 前断言 `alive=1`，`clear()` 后才归零。这是“元数据表意外保活”的典型问题。

如果业务不希望保活，有两种方案：

1. 使用 `std::weak_ptr<const void>` 作为 key，`owner_less` 也支持弱引用；但需要处理过期条目；
2. 使用整数 ID/代际号，把生命周期交给业务层管理，避免智能指针键。

Demo 选择强 key 是为了演示 `owner_less` 和保活副作用，不是推荐所有场景都这样做。

### 6.3 业务注意

- 弱缓存适合“失效可接受”的加速；如果缓存需要保活，使用 `shared_ptr` + LRU 容量控制。
- 不要用 `expired()` + `lock()` 两步判断；直接 `lock()` 并检查结果。
- `owner_less` 比较的是控制块，不是对象内容；不要用它做值排序。
- 关联容器的 key 生命周期语义要写进接口文档，否则调用方会误以为缓存不保活。

## 7. 对象树、弱父边与循环引用

文件：`include/spl/object_graph.h`、`src/object_graph.cpp`

### 7.1 结构

```cpp
class Element : public std::enable_shared_from_this<Element>,
                public Tracked<ElementTag> {
    std::weak_ptr<Element> parent_;
    std::vector<std::shared_ptr<Element>> children_;
};
```

- 父 -> 子：强引用，树拥有子树；
- 子 -> 父：弱引用，避免环；
- `enable_shared_from_this` 让 `add_child` 能从 `this` 取得父对象的 `shared_ptr`，而不会创建第二个控制块。

### 7.2 `add_child` 的三重校验

`src/object_graph.cpp:24`

```cpp
if (!child) throw std::invalid_argument(...);
if (is_ancestor_of(child.get())) throw std::logic_error("cycle");
if (child->parent_.lock()) throw std::logic_error("already has a parent");
std::shared_ptr<Element> self = shared_from_this();
children_.push_back(child);
child->parent_ = self;
```

- `is_ancestor_of` 从 `this` 沿 `parent_` 向上走，检查 child 是否是祖先；防止 `leaf->add_child(root)` 形成环。
- `child->parent_.lock()` 检查是否已有父节点；避免一个节点被两个父节点强引用。
- `shared_from_this()` 在栈对象上会抛 `std::bad_weak_ptr`，测试 `tests/test_graph.cpp:45` 覆盖。
- 先 `push_back` 再设置 `parent_`，避免 `push_back` 抛异常后留下“有父但不在子列表”的不一致。

### 7.3 深度与遍历

`depth()` 沿弱父边向上计数。每次 `lock()` 得到一个临时 `shared_ptr`，保证遍历期间祖先不被销毁；循环结束后临时引用释放。

测试 `tests/test_graph.cpp:8`：

- 三层树 depth=2；
- parent 指针正确；
- 作用域结束后所有元素 alive=0。

### 7.4 循环泄漏演示

`CyclicNode` 的 `peer_` 是 `shared_ptr`：

```cpp
first->link_to(second);
second->link_to(first);
first.reset();
second.reset();
// alive=2
```

测试 `tests/test_graph.cpp:51` 随后：

```cpp
first_observer.lock()->break_link();
// alive=0
```

这直观说明：循环引用不是“智能指针自动解决”的问题；必须有一个边是弱引用，或者在生命周期结束时显式断开。

### 7.5 业务注意

- 树/图的所有权方向必须在设计文档中明确；不要靠“应该不会成环”。
- 父指针用 `weak_ptr` 后，父对象销毁时子对象可以通过 `parent()` 得到空，业务要处理这种状态。
- `enable_shared_from_this` 只适用于“对象一定由 shared_ptr 管理”的类；栈对象/静态对象要禁止调用相关接口。

## 8. Pipeline：`enable_shared_from_this` 与弱观察者

文件：`include/spl/pipeline.h`、`src/pipeline.cpp`

### 8.1 Node 的双向关系

```cpp
class Node : public std::enable_shared_from_this<Node> {
    std::weak_ptr<Node> upstream_;
    std::vector<std::weak_ptr<Observer>> observers_;
};
```

- 上游边弱：下游不拥有上游；上游销毁后 `upstream()` 返回空。
- 观察者弱：节点不拥有观察者；观察者销毁后自动失效。
- Pipeline 用 `vector<shared_ptr<Node>>` 拥有节点。

### 8.2 `connect_to` 的环检测

`src/pipeline.cpp:60`

```cpp
std::shared_ptr<Node> self = shared_from_this();

std::shared_ptr<Node> cursor = self;
while (cursor) {
    if (cursor.get() == downstream.get()) throw std::logic_error("cycle");
    cursor = cursor->upstream_.lock();
}

cursor = downstream;
while (cursor) {
    if (cursor.get() == self.get()) throw std::logic_error("cycle");
    cursor = cursor->upstream_.lock();
}

downstream->upstream_ = self;
```

两段遍历分别检查：

- `downstream` 是否已经在 `self` 的上游链中；
- `self` 是否已经在 `downstream` 的上游链中。

任意一个成立，新增边都会形成逻辑环。虽然边是弱引用，不会造成引用计数泄漏，但会造成处理顺序/通知环，业务上仍然要拒绝。

`shared_from_this()` 在 `connect_to` 开头调用：如果 Node 不是由 `shared_ptr` 管理，立即抛 `std::bad_weak_ptr`，不会留下部分修改。测试 `tests/test_pipeline.cpp:61` 覆盖栈对象和环。

### 8.3 `emit`：弱观察者的安全回调

`src/pipeline.cpp:36`

```cpp
std::vector<std::shared_ptr<Observer>> active;
{
    std::lock_guard<std::mutex> lock(observers_mutex_);
    for (auto it = observers_.begin(); it != observers_.end();) {
        std::shared_ptr<Observer> observer = it->lock();
        if (observer) {
            active.push_back(observer);
            ++it;
        } else {
            it = observers_.erase(it);
        }
    }
}
for (auto& observer : active) observer->on_frame(name_, frame);
```

三个设计点：

1. `weak_ptr::lock()` 在锁内完成，保证观察者不会在提升过程中被销毁；
2. 提升后复制到局部 `active` 列表，回调在锁外执行，避免重入死锁和长临界区；
3. 过期观察者顺手删除，避免列表无限增长。

测试 `tests/test_pipeline.cpp:32`：

- 观察者在作用域内时 `observer_count=1`，emit 收到通知；
- 观察者销毁后 `observer_count=0`，emit 不崩溃；
- `ObserverTag::alive()==0`，节点没有保活观察者。

### 8.4 Pipeline 的所有权

`Pipeline::add` 保存 `shared_ptr`；`Pipeline::link` 只允许本 Pipeline 拥有的节点，避免跨 Pipeline 建立悬挂弱边。

测试 `tests/test_pipeline.cpp:73`：

- Pipeline 作用域内，外部释放节点后节点仍活着（Pipeline 拥有）；
- Pipeline 销毁后节点全部销毁；
- 观察者收到 source 和 sink 两次通知。

### 8.5 业务注意

- 观察者列表默认用弱引用；如果业务需要“订阅即保活”，必须显式文档化，并防止循环。
- 回调中不要长期保存 `Frame&`，如果需要异步使用，复制 `Frame`（它本身只是共享视图，复制成本低）。
- 如果观察者回调可能抛异常，`emit` 目前会向上传播；生产环境可决定是否隔离每个观察者，并记录错误。
- 节点名、连接关系、观察者列表的并发访问要统一锁策略；Demo 只对观察者列表加锁，节点图结构假定在启动阶段构建。

## 9. TaskQueue：异步生命周期

文件：`include/spl/task_queue.h`、`src/task_queue.cpp`

### 9.1 队列与工作线程

`src/task_queue.cpp:8` 构造函数创建 N 个 worker；`post` 入队并通知；`run` 等待任务；`shutdown` 设置停止标志并 join。

关键实现点：

- `post` 在锁内检查 `stopping_`，拒绝关闭后的新任务；
- `run` 使用 `condition_variable` 等待“停止或队列非空”；
- 停止时先把队列中已有任务执行完（`stopping_ && tasks_.empty()` 才退出），保证已接收任务不被静默丢弃；
- 任务异常被捕获并计数，worker 不会因为一个任务抛异常而退出；
- `shutdown` 用 `lifecycle_mutex_` 串行化，避免并发 join 同一线程。

### 9.2 强捕获与弱捕获

测试 `tests/test_async.cpp:9`：

```cpp
queue.post([gate_future]() { gate_future.wait(); });
queue.post([node, &done]() { done.set_value(); });
node.reset();
SPL_CHECK(!observer.expired());  // 任务里的 shared_ptr 保活
```

测试 `tests/test_async.cpp:38`：

```cpp
std::weak_ptr<Node> weak_node = node;
queue.post([weak_node, &cancelled, &ran]() {
    std::shared_ptr<Node> locked = weak_node.lock();
    if (!locked) {
        ran = true;
        cancelled.set_value();
    }
});
node.reset();
SPL_CHECK(observer.expired());
```

两种语义：

- 强捕获：任务执行前对象不会被销毁，适合“必须完成”的后台工作；代价是任务队列可能延长对象生命周期。
- 弱捕获：对象销毁后任务自动取消，适合 UI 回调、周期刷新、可丢弃的计算；需要在任务内部 `lock()` 并检查。

### 9.3 为什么测试用 gate

直接“先 reset 再 post”无法验证竞态；测试用一个阻塞任务占住唯一 worker，确保后续任务在对象释放后才执行，从而确定性地验证捕获策略。这是并发测试中常用的“闸门”技巧。

### 9.4 业务注意

- 不要在任务里捕获裸 `this`；用 `shared_from_this()`（保活）或 `weak_ptr`（可取消）。
- `shutdown` 会等待已入队任务执行完；如果任务可能长时间阻塞，需要设计取消标志或超时。
- 不要在 worker 任务里调用同一个队列的 `shutdown()`，会 join 自己导致死锁。
- 任务异常目前被吞掉并计数；生产环境应接入日志/监控，或提供错误回调。
- 队列析构会调用 `shutdown()`，已接收的任务会执行完；如果任务可能长时间阻塞，析构会一直等待，需要在任务内部设计取消标志或超时。

## 10. SharedSlot 与插件热升级

文件：`include/spl/shared_slot.h`、`include/spl/plugin_registry.h`、`src/plugin_registry.cpp`

### 10.1 `SharedSlot`

`include/spl/shared_slot.h:19`

```cpp
void store(std::shared_ptr<T> value) {
    std::atomic_store(&value_, std::move(value));
}

std::shared_ptr<T> load() const {
    return std::atomic_load(&value_);
}
```

- 解决“同一个 `shared_ptr` 实例被多线程读写”的数据竞争；
- 读线程拿到一个强引用快照，旧值不会在使用中被销毁；
- C++20 后应迁移到 `std::atomic<std::shared_ptr<T>>`；
- 写者之间的读-改-写仍需外部互斥。

### 10.2 插件接口与稳定槽位

`Plugin` 是抽象基类，`InvertPlugin`、`ThresholdPlugin`、`PassthroughPlugin` 是具体实现。注册表支持：

```cpp
void install(const std::shared_ptr<Plugin>& plugin);              // 用 plugin->name() 作为槽位
void install(const std::string& slot, const std::shared_ptr<Plugin>& plugin);
```

`install(slot, plugin)` 是业务关键：升级时槽位名保持稳定（例如 "invert"），实现可以替换（invert -> passthrough）。如果只用 `plugin->name()` 作为 key，替换实现会新增一个 key，旧 key 仍然指向旧插件，热升级语义就不成立。

### 10.3 写时复制与在途调用

`src/plugin_registry.cpp:18`

```cpp
PluginSnapshot current = slot_.load();
auto next = std::make_shared<PluginMap>(*current);
(*next)[slot] = plugin;
slot_.store(next);
```

- 读者拿到的旧 map 快照不会被修改；
- 在途调用方持有旧插件 `shared_ptr`，热升级不会让旧插件析构；
- 只有最后一个持有旧快照/旧插件的调用方释放后，旧插件才销毁；
- 写者用 `write_mutex_` 串行化，避免丢失更新。

测试 `tests/test_registry.cpp:45`：

```cpp
auto in_flight = registry.find("invert");
auto old_snapshot = registry.snapshot();
registry.install("invert", replacement);
SPL_CHECK(registry.find("invert") == replacement);
SPL_CHECK(!observer.expired());       // 旧插件仍被 in_flight/snapshot 持有
old_snapshot.reset();
in_flight.reset();
SPL_CHECK(observer.expired());        // 所有旧引用释放后才销毁
```

### 10.4 业务注意

- 原子快照适合“读多写少”的配置/插件表；每次写都复制整个 map，插件数量很大时成本高，可改用持久化数据结构或分片。
- 旧插件可能仍在执行，热升级必须保证新旧版本的 ABI/数据格式兼容，或等待在途调用结束。
- `snapshot()` 返回 `shared_ptr<const PluginMap>`，调用方不能修改；这是不可变快照的关键。
- 插件对象本身的线程安全性由插件实现负责；注册表只保证生命周期和发布安全。

## 11. 分配次数与弱引用滞留

文件：`include/spl/allocation_probe.h`、`src/allocation_probe.cpp`

### 11.1 计数分配器

```cpp
template <typename T>
class CountingAllocator {
    T* allocate(std::size_t count) {
        AllocationStats::note_allocation();
        return static_cast<T*>(::operator new(count * sizeof(T)));
    }
    void deallocate(T* pointer, std::size_t) {
        AllocationStats::note_deallocation();
        ::operator delete(pointer);
    }
};
```

`AllocProbe` 还重载了类专属 `operator new/delete`，用来分别观察：

- `allocate_shared` 走分配器，1 次；
- `shared_ptr(new AllocProbe, deleter, allocator)` 走类 `new` + 分配器，2 次。

### 11.2 测试解释

`tests/test_allocation.cpp:6`：

```cpp
auto probe = std::allocate_shared<AllocProbe>(CountingAllocator<AllocProbe>(), 7);
SPL_CHECK_EQ(AllocationStats::allocations(), 1);
```

`tests/test_allocation.cpp:21`：

```cpp
std::shared_ptr<AllocProbe> probe(
    new AllocProbe(9),
    std::default_delete<AllocProbe>(),
    CountingAllocator<AllocProbe>());
SPL_CHECK_EQ(AllocationStats::allocations(), 2);
```

`tests/test_allocation.cpp:34`：

```cpp
std::weak_ptr<AllocProbe> weak;
{
    auto probe = std::allocate_shared<AllocProbe>(..., 11);
    weak = probe;
}
SPL_CHECK_EQ(AllocProbeTag::alive(), 0);       // 对象已析构
SPL_CHECK_EQ(AllocationStats::outstanding(), 1); // 合并块还在
weak.reset();
SPL_CHECK_EQ(AllocationStats::outstanding(), 0); // 弱引用归零才释放
```

### 11.3 业务含义

- 高频创建的对象优先 `make_shared`/`allocate_shared`，减少分配次数和缓存未命中；
- 如果对象很大且 `weak_ptr` 长期存在，合并块会延迟释放；此时评估“两次分配换及时释放”；
- 计数分配器只用于实验，生产环境应使用成熟的内存池/arena，并注意对齐和线程安全。

## 12. pimpl：`unique_ptr` 的编译防火墙

文件：`include/spl/codec.h`、`src/codec.cpp`

### 12.1 接口

```cpp
class Decoder {
public:
    Decoder(const std::string& codec, int quality);
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    ...
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

### 12.2 为什么析构/移动必须在 cpp 中定义

`std::unique_ptr<Impl>` 的析构函数需要 `Impl` 完整类型。头文件只声明 `Impl`，如果析构函数在头文件里内联，编译器会在每个包含者处实例化 `delete Impl`，报 `incomplete type`。因此：

- 析构函数在 `src/codec.cpp` 定义；
- 移动构造/赋值也在 cpp 定义；
- `Impl` 的定义只在 cpp 中可见，改实现不需要重新编译所有包含者。

### 12.3 moved-from 状态

`src/codec.cpp:37`：

```cpp
std::string Decoder::describe() const {
    if (!impl_) return "<moved-from>";
    ...
}
```

`src/codec.cpp:46`：

```cpp
Frame Decoder::decode(const Frame& input) const {
    if (!impl_) throw std::logic_error("Decoder::decode called on a moved-from decoder");
    ...
}
```

移动后 `unique_ptr` 为空，`valid()` 为 false；`describe()` 返回可识别字符串，`decode()` 抛异常，而不是解引用空指针。这是库作者应该定义的契约。测试 `tests/test_codec.cpp:17` 覆盖移动构造、移动赋值和 moved-from 调用。

## 13. 测试矩阵与运行方式

测试框架：`tests/test_framework.h`

- 自注册 `SPL_TEST(name)`；
- `SPL_CHECK`、`SPL_CHECK_EQ`、`SPL_CHECK_THROW`；
- 每个测试独立捕获异常，失败不终止整个进程；
- 断言计数统一输出。

运行：

```bash
make test
# 或
./build/spl_tests
```

测试覆盖：

| 测试文件 | 覆盖点 |
| --- | --- |
| `test_sizes.cpp` | 对象尺寸、删除器开销 |
| `test_make_unique.cpp` | C++11 `make_unique` |
| `test_intrusive.cpp` | 侵入式计数、detach/adopt、cast |
| `test_resources.cpp` | 自定义删除器、unique->shared |
| `test_frame.cpp` | aliasing、ROI、owner key |
| `test_cache.cpp` | 弱缓存、owner_less、保活副作用 |
| `test_graph.cpp` | 弱父边、环检测、循环泄漏 |
| `test_pipeline.cpp` | enable_shared_from_this、弱观察者 |
| `test_async.cpp` | 强/弱捕获、任务异常、关闭 |
| `test_registry.cpp` | 热升级、快照、SharedSlot |
| `test_casts.cpp` | shared/unique 多态、shared_from_this 构造期 |
| `test_allocation.cpp` | 分配次数、weak 延迟释放 |
| `test_codec.cpp` | pimpl、移动、moved-from |
| `test_shared_slot_threads.cpp` | 原子共享槽并发读写烟测 |

当前结果：47 个测试、228 个断言、0 失败。

注意：并发测试只是功能烟测，不能替代 ThreadSanitizer。生产代码建议用：

```bash
g++ -std=c++11 -fsanitize=thread -g ...
# 或
g++ -std=c++11 -fsanitize=address,undefined -g ...
```

## 14. 建议的动手实验

1. 把 `Frame::slice` 改成拷贝像素，观察 `owner_use_count`、内存和地址变化。
2. 把 `Element::parent_` 改成 `shared_ptr`，运行 `test_graph`，观察泄漏和 `alive` 断言失败。
3. 把 `Node::observers_` 改成 `shared_ptr`，在测试作用域结束时观察观察者无法销毁。
4. 把 `PluginRegistry::install` 改成直接修改当前 map（不加锁/不复制），观察在途 `snapshot` 被破坏。
5. 把 `TaskQueue` 的强捕获改成裸 `this`（用一个成员函数测试），用 ASan 观察悬空。
6. 把 `Frame::owner_key` 的 `operator==` 改为 `same_owner`，验证不同 control block 的区分。
7. 升级到 C++17，使用 `weak_from_this`、`std::make_unique`、`std::atomic<std::shared_ptr>`，对比代码量。
