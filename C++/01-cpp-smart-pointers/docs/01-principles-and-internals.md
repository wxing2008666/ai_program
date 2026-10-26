# 智能指针原理与内部模型

本文建立“为什么”的模型。Demo 中的每个 API 选择，都可以在本文找到对应的生命周期、内存布局或并发语义依据。代码以 C++11 为基线，升级到更新标准时的差异在最后一节说明。

## 1. 智能指针解决的不是“自动 delete”

智能指针解决的是 **所有权（ownership）的表达、转移和释放时机**：

- 谁负责释放资源？
- 所有权能否复制？能否转移？
- 最后一个所有者何时消失？
- 观察者如何在不延长生命周期的情况下访问对象？
- 资源不是 `new` 出来的（文件、fd、GPU handle、C 结构体）时，如何统一管理？

`delete` 只是最后一步。真正的 bug 通常来自所有权语义不清：两个组件都以为自己拥有对象；观察者比被观察者活得久；异步任务捕获了裸 `this`；父子对象互相强引用。Demo 的所有模块都围绕这些边界展开。

| 类型 | 所有权 | 可复制 | 主要成本 | 典型场景 |
| --- | --- | --- | --- | --- |
| `T*` / `T&` | 无 | 是 | 无 | 参数、观察者、短生命周期借用 |
| `std::unique_ptr<T>` | 独占 | 否，可移动 | 1 个指针（无状态删除器） | 工厂返回值、pimpl、资源句柄 |
| `std::shared_ptr<T>` | 共享 | 是 | 2 个指针 + 控制块 + 原子计数 | 真正共享所有权、跨模块生命周期 |
| `std::weak_ptr<T>` | 不拥有 | 是 | 2 个指针 | 缓存、观察者、反向边、取消令牌 |
| `intrusive_ptr<T>` | 共享（计数在对象内） | 是 | 1 个指针 + 对象内计数 | ABI 边界、高频对象、已有引用计数 |

一个常见误区是“用了智能指针就不会泄漏”。循环引用、`release()` 后忘记接管、把同一裸指针交给两个 `shared_ptr`，都会泄漏甚至双重释放。智能指针只是把所有权规则变成类型系统的一部分，规则仍然要人来设计。

## 2. `unique_ptr`：零开销的独占所有权

### 2.1 对象布局

`unique_ptr` 通常由两部分组成：

```text
unique_ptr<T, D>
+----------------+------------------+
| T* 指针        | D 删除器          |
+----------------+------------------+
```

当 `D` 是空类（例如无捕获 lambda、`std::default_delete<T>`）时，标准库通过空基类优化/压缩对把删除器压到零大小，于是 `unique_ptr` 只占一个指针。Demo 的实测：

```text
unique_ptr<int>                    : 8 bytes
unique_ptr<int, function pointer>  : 16 bytes
```

函数指针删除器本身要存一个地址，所以变成两个机器字。捕获了状态的 lambda 删除器还会更大。这个差异是选型时要意识到的：

- 删除器类型是 `unique_ptr` 类型的一部分，编译期确定，不做类型擦除。
- 不同删除器的 `unique_ptr` 是不同类型，不能互相赋值。
- 没有控制块，没有原子操作，没有额外分配。

`shared_ptr` 的删除器则被擦除到控制块里，所以 `sizeof(shared_ptr<T>)` 不随删除器变化。这是“独占优化”和“共享灵活”的典型取舍。

### 2.2 移动语义与所有权转移

`unique_ptr` 删除拷贝构造/拷贝赋值，只保留移动：

```cpp
std::unique_ptr<Thing> make_thing();
std::unique_ptr<Thing> owner = make_thing();  // 从返回值转移
std::unique_ptr<Thing> sink = std::move(owner); // 显式转移
// owner 现在为空，但对象仍然有效
```

关键点：

- `std::move(owner)` 不移动对象，只把 `owner` 转成右值引用，触发 `unique_ptr` 的移动构造。
- 移动后源指针变为 `nullptr`；对源解引用是未定义行为。
- 函数按值接收 `unique_ptr` 表示“接管所有权”（sink）；按 `const unique_ptr&` 接收通常只应观察，不应释放或转移。
- `release()` 放弃所有权并返回裸指针，必须有人接管，否则泄漏；`reset()` 释放当前对象并接管新对象。

Demo 中 `UniqueResource` 的移动测试验证了源对象变空、目标对象持有资源、关闭次数恰好一次：

```cpp
spl::UniqueResource first = spl::make_unique_resource(2);
spl::UniqueResource second(std::move(first));
SPL_CHECK(!first);
SPL_CHECK_EQ(second->id, 2);
```

### 2.3 自定义删除器与 C 资源

C API 通常成对出现：

```c
Resource* resource_open(int id);
void resource_close(Resource*);
```

直接把裸指针放进 `unique_ptr` 的默认删除器会调用 `delete`，这是错的。正确做法是把删除器作为类型的一部分：

```cpp
typedef std::unique_ptr<CResource, void (*)(CResource*)> UniqueResource;
UniqueResource owner(c_resource_open(7), &c_resource_close);
```

这样：

- 离开作用域自动调用 `c_resource_close`；
- 异常路径也能释放；
- 所有权可以移动，但不能复制；
- 删除器函数指针让 `unique_ptr` 变成两个机器字，这是为正确性付出的可接受成本。

`std::unique_ptr<T, D>` 的删除器要求可调用、可移动；删除器在析构时不应抛异常（否则通常触发 `std::terminate`）。`make_unique` 不支持自定义删除器，所以 C 资源通常直接构造 `unique_ptr`，或写一个返回 `unique_ptr` 的工厂。

### 2.4 数组与 `make_unique`

C++11 没有 `std::make_unique`，C++14 才加入。Demo 在 `memory_utils.h` 中实现：

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

为什么要工厂函数？除了少写 `new`，更重要的是异常安全和求值顺序。C++11 中函数实参的求值顺序未指定，下面这种写法可能泄漏：

```cpp
consume(std::unique_ptr<Foo>(new Foo), may_throw());
```

如果 `may_throw()` 先执行并抛出，`Foo` 还没被 `unique_ptr` 接管，`new Foo` 就泄漏了。改成：

```cpp
consume(spl::make_unique<Foo>(), may_throw());
```

`new Foo` 发生在 `make_unique` 函数体内部，一旦构造成功就立即被返回值接管；`may_throw()` 无论何时抛出，都不会留下未接管的裸指针。

`unique_ptr<T[]>` 使用 `delete[]`，但不知道数组长度；需要长度时用 `std::vector`、`std::array` 或自己保存 size。Demo 的 `FrameBuffer` 同时保存 `unique_ptr<uint8_t[]>` 和 `size`。

### 2.5 多态与虚析构

```cpp
std::unique_ptr<Base> p(new Derived);
```

销毁时执行 `delete Base*`。如果 `Base` 的析构函数不是虚函数，行为未定义，`Derived` 的析构函数可能不会执行。因此：

```cpp
static_assert(std::has_virtual_destructor<Base>::value,
              "Base must expose a virtual destructor");
```

Demo 在 `test_casts.cpp` 用 `VirtualBase` 验证安全转换，同时保留 `NonVirtualBase` 来说明 `shared_ptr` 的不同行为（见 3.6）。

### 2.6 pimpl 与不完整类型

`Decoder` 在头文件里只暴露：

```cpp
class Decoder {
public:
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    ...
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

`Impl` 在 `.cpp` 中定义。这样：

- 头文件不暴露实现细节，减少编译依赖；
- `unique_ptr<Impl>` 的析构需要完整类型，所以析构函数必须在 `.cpp` 中定义（不能 `= default` 在头文件里）；
- 移动构造/移动赋值也需要在 `Impl` 完整处定义；
- 如果忘记 out-of-line 析构，常见报错是 `invalid application of sizeof to incomplete type`。

Demo 的 `Decoder` 还演示了 moved-from 状态：移动后 `impl_` 为空，`valid()` 返回 false，`decode()` 抛出 `std::logic_error`，而不是解引用空指针。这是库作者应该定义的“有效但未指定”状态。

## 3. `shared_ptr`：控制块才是核心

### 3.1 两个指针

`shared_ptr<T>` 通常包含：

```text
shared_ptr<T>
+------------------+----------------------+
| T* 存储指针       | control_block* 控制块 |
+------------------+----------------------+
```

存储指针不一定等于控制块里管理的对象指针，这正是 aliasing constructor 的基础。控制块通常包含：

```text
control block
+----------------+----------------+----------------------+
| strong count   | weak count     | deleter / allocator  |
+----------------+----------------+----------------------+
| virtual dispose()  -> 销毁对象                         |
| virtual destroy()  -> 释放控制块/合并内存               |
+--------------------------------------------------------+
```

标准没有规定控制块的具体布局，但所有实现都必须满足同一套语义：

- 强引用计数归零时销毁被管理对象；
- 弱引用计数归零时释放控制块；
- 强引用计数和弱引用计数都必须是线程安全的；
- 删除器/分配器必须被保存，因为 `shared_ptr` 的类型擦除了它们。

解释：
- 强引用计数归零 → 销毁被管理对象：
控制块会调用删除器销毁被管理对象。这里说的是“销毁对象”(析构)，不一定立刻“释放对象内存”。如果用 make_shared，对象和控制块可能分配在同一块内存里，对象析构后，那块内存要等到控制块也释放时才能归还。
- 弱引用计数归零 → 释放控制块：
强引用计数归零且弱引用计数也归零，才释放控制块；
弱引用计数由 weak_ptr 参与，同时控制块自身也依赖它来判断是否还能被安全访问。
当强引用计数已经归零，且弱引用计数也归零时，说明既没有 shared_ptr 拥有对象，也没有 weak_ptr 在观察对象，控制块才可以被销毁和释放。
所以控制块的生命周期通常比对象长：对象可能已经析构，但只要还有 weak_ptr，控制块就必须活着，否则 weak_ptr::expired() 和 weak_ptr::lock() 就无法安全判断对象是否还存在。


### 3.2 强计数、弱计数与销毁顺序

简化后的生命周期：

```text
shared_ptr 构造        strong=1, weak=1（隐含弱引用）
复制 shared_ptr        strong++ 
复制 weak_ptr          weak++
shared_ptr 析构        strong--
最后一个 shared_ptr    strong=0 -> 销毁对象 -> 释放隐含弱引用 -> weak--
最后一个 weak_ptr      weak=0 -> 释放控制块（或 make_shared 的合并块）
```

“隐含弱引用”是理解 `weak_ptr::lock` 并发的关键：只要还有强引用，控制块就不会因为弱计数为 0 而被释放，`lock` 才能安全地尝试把强计数从 0 增到 1。`lock` 的实现是一个原子条件递增：如果强计数为 0，返回空；否则成功递增并返回新的 `shared_ptr`。

Demo 的 `weak_ptr_delays_combined_block_deallocation` 验证了：

```text
allocate_shared allocations=1, object_alive=1
after strong refs: object_alive=0, block_outstanding=1
after weak reset: block_outstanding=0
```

强引用归零时对象析构已经执行（`alive=0`），但 `make_shared`/`allocate_shared` 的合并内存要等弱引用也归零才释放。这是 `make_shared` + 长寿命 `weak_ptr` 的一个真实内存代价。

### 3.3 `make_shared` 与 `shared_ptr(new T)`

`std::shared_ptr<T> p(new T)` 通常发生两次分配：

1. `new T` 分配对象；
2. 控制块分配（保存计数、删除器）。

`std::make_shared<T>(args...)` 通常把对象和控制块放进同一块内存，只发生一次分配，并且缓存局部性更好。Demo 用计数分配器测得：

```text
allocate_shared: 1 次分配
shared_ptr(new T, deleter, allocator): 2 次分配（对象 + 控制块）
```

但 `make_shared` 不是无条件更优：

- 不能指定自定义删除器；
- 不能直接构造私有构造函数（工厂函数需要是友元或使用其他技巧）；
- 对象和控制块绑定，弱引用会延迟整块内存释放；如果对象很大且 `weak_ptr` 很长寿，可能造成明显内存滞留；
- 分配器版本是 `std::allocate_shared`，它把分配器保存进控制块，适合 arena/池化分配。

选择原则：

- 普通对象、没有自定义删除器、没有长期弱引用：优先 `make_shared`。
- C 资源、需要自定义释放：直接构造 `shared_ptr` 或先构造 `unique_ptr` 再提升。
- 高频分配、需要池化：`allocate_shared`。
- 大对象 + 长期 `weak_ptr`：评估是否接受合并块滞留，必要时分开分配。

### 3.4 从 `unique_ptr` 提升为 `shared_ptr`

C++11 提供：

```cpp
std::shared_ptr<CResource> share_resource(UniqueResource resource) {
    return std::shared_ptr<CResource>(std::move(resource));
}
```

这会把 `unique_ptr` 的删除器移动进控制块，所有权从独占变为共享。Demo 用它封装 C API：

```cpp
SharedResource shared = share_resource(make_unique_resource(5));
```

注意：

- 转换后源 `unique_ptr` 为空；
- 删除器类型被擦除，`shared_ptr` 大小仍是两个指针；
- 如果 `unique_ptr` 的删除器不可移动，转换会失败；
- 不要先 `release()` 再构造 `shared_ptr`，否则异常路径可能泄漏。

### 3.5 自定义删除器与类型擦除

```cpp
std::shared_ptr<CResource> p(raw, &c_resource_close);
```

控制块里保存了函数指针。对使用者来说，`shared_ptr` 的类型只有 `CResource`，删除器细节被擦除。这带来两个结果：

- 好：接口稳定，不同删除器可以放进同一个容器；
- 代价：控制块需要虚函数/函数指针分派，且 `make_shared` 不能用自定义删除器。

删除器可以是函数对象、lambda、状态ful 对象。删除器应满足：

- 可复制（`shared_ptr` 可能复制控制块中的删除器？实际控制块构造时移动/复制一次）；
- 不抛异常；
- 能正确处理空指针（标准保证不会对空存储指针调用删除器，但自定义逻辑仍应健壮）。

### 3.6 派生到基类转换：`shared_ptr` 的删除器救了非虚析构

这是面试和代码评审中的高频陷阱。

```cpp
struct Base { ~Base(); };              // 非虚析构
struct Derived : Base { ~Derived(); };

std::shared_ptr<Derived> d = std::make_shared<Derived>();
std::shared_ptr<Base> b = d;           // 安全
```

为什么安全？因为 `b` 和 `d` 共享同一个控制块，控制块里保存的销毁逻辑知道原始类型是 `Derived`。强引用归零时，控制块调用的是 `delete Derived*`，而不是 `delete Base*`。

对比：

```cpp
std::unique_ptr<Base> p(new Derived);  // 若 Base 析构非虚：未定义行为
```

`unique_ptr<Base>` 的删除器是 `default_delete<Base>`，它执行 `delete Base*`。因此 `unique_ptr` 的多态删除要求基类虚析构，或者自定义删除器知道真实类型。

重要限定：`shared_ptr` 的安全前提是对象一开始就由 `shared_ptr<Derived>` 管理。下面仍然是灾难：

```cpp
Base* raw = new Derived;
std::shared_ptr<Base> b(raw);  // 控制块按 Base* 删除，非虚析构 -> UB
```

Demo 在 `test_casts.cpp` 用 `NonVirtualBase`/`DerivedCast` 验证了安全路径，并在文档中明确不演示不安全路径。

### 3.7 `shared_ptr` 的 cast

- `std::static_pointer_cast<T>(p)`：静态转换存储指针，共享控制块；
- `std::dynamic_pointer_cast<T>(p)`：多态向下转换，失败返回空 `shared_ptr`；
- `std::const_pointer_cast<T>(p)`：移除 const，危险，只有确实需要时才用；
- `std::reinterpret_pointer_cast<T>(p)`：C++17 起，低层重解释，通常不应出现在业务代码。

Demo 用 `dynamic_pointer_cast<InvertPlugin>` 从 `shared_ptr<Plugin>` 取具体插件，失败时返回空。转换后的指针仍共享原控制块，强计数增加。

### 3.8 多线程安全

两部分：shared_ptr实例本身和被管理对象
- 控制块和多个不同shared_ptr实例的并发操作安全；
- 同一个shared_ptr实例被多线程写不安全；
- 被管理对象本身也不自带线程安全。

1. 控制块
- 引用计数本身线程安全;
- 强引用计数由shared_ptr维护，弱引用计数由weak_ptr和维护控制块生命周期的逻辑使用；增加计数常用 memory_order_relaxed，因为只要求原子性；减少计数常用 memory_order_acq_rel，因为最后一次把计数从 1 减到 0 时，需要保证对象析构前其他线程对对象的写入已经可见。
所以多个线程同时拷贝、析构、reset 不同的 shared_ptr 实例，即使它们共享同一个控制块，也不会产生数据竞争。换个说法：不同的 shared_ptr 实例即使共享同一个控制块，也可以在不同线程里同时拷贝、赋值、析构，计数不会出错，对象也只会被析构一次。
2. 被管理对象
- shared_ptr 只保证“对象只会被销毁一次”，不保证对象内部状态安全。
- shared_ptr 只负责对象的生命周期，不负责对象的访问。多个线程通过各自的 shared_ptr 副本访问同一个 T，和多个线程拿着同一个裸指针没有任何区别
```cpp
std::shared_ptr<std::vector<int>> sp = std::make_shared<std::vector<int>>();
// 线程1
sp->push_back(1); // ❌ 数据竞争
// 线程2
sp->push_back(2); // ❌ 数据竞争
```
多个线程通过任意 shared_ptr 副本读写同一份对象时，仍需 mutex、atomic 或其他同步机制。

3. shared_ptr实例本身

- 读线程安全，写线程不安全；
- 同一个shared_ptr实例和不同shared_ptr实例：

| 场景 | 是否安全 | 说明 |
|---|---|---|
| 多线程只读同一个 `shared_ptr` | ✅ 安全 | 例如多个线程都调用 `get()`、`use_count()` |
| 多线程操作不同的 `shared_ptr` 实例 | ✅ 安全 | 即使它们共享同一控制块 |
| 多线程同时写同一个 `shared_ptr` | ❌ 不安全 | 例如 `reset()`、`operator=`、`swap()` |
| 一个线程写，另一个线程读同一个实例 | ❌ 不安全 | 可能读到半更新状态 |


4. weak_ptr
- weak_ptr 的弱引用计数也是原子的；多个线程并发操作不同的 weak_ptr 实例安全。但同一个 weak_ptr 实例被多线程写仍然不安全。weak_ptr::lock() 会原子地尝试提升强引用计数，从而在提升成功后保证对象不会被销毁。
weak_ptr::lock() 用 CAS 循环实现"计数不为 0 才加一"，所以它也是线程安全的。不会出现对象正在析构，又被 lock 出一个新 shared_ptr 的情况。

工程上这样记忆：
- 一句话：shared_ptr 的引用计数是线程安全的，但同一个 shared_ptr 实例和它管理的对象都不是线程安全的。
- 控制块安全：不同 shared_ptr 实例并发拷贝/析构没问题。
- 实例不安全：同一个 shared_ptr 变量被多线程写，必须加锁或用 atomic<shared_ptr>。
- 对象不安全：通过 shared_ptr 访问业务对象时，仍按普通并发规则处理。
- use_count() 只能调试：多线程下它返回的是近似值，不能用来做正确性判断。
- lambda 按引用捕获 shared_ptr 是错误的。[&sp] 捕获的是同一个实例，主线程一旦 reset 就构成竞争。应该按值捕获 [sp]。

## 4. Aliasing Constructor：共享所有权，指向子对象

### 4.1 语义

```cpp
template <typename Y>
shared_ptr(const shared_ptr<Y>& owner, element_type* ptr) noexcept;
```

新 `shared_ptr`：

- **共享** `owner` 的控制块，从而延长 `owner` 的生命周期；
- **存储** `ptr`，解引用得到 `ptr` 指向的对象；
- 不复制、不拥有 `ptr` 所指向的独立资源。

典型用途：

- 指向对象的成员；
- 指向大缓冲区中的一段；
- 类型擦除为 `shared_ptr<void>`；
- 生成不增加对象指针语义、但保持所有者存活的身份 key。

### 4.2 Frame 的零拷贝 ROI

`Frame::wrap`：

```cpp
const std::uint8_t* base = buffer->data();
std::shared_ptr<const std::uint8_t> pixels(buffer, base);
return Frame(std::move(pixels), width, height, stride);
```

这里 `pixels` 的存储指针是 `base`，控制块来自 `buffer`（`shared_ptr<FrameBuffer>`）。`Frame` 不再需要保存 `FrameBufferPtr`，但缓冲区会一直活到最后一个像素视图销毁。

`Frame::slice` 再对 `pixels_` 做 aliasing：

```cpp
const std::uint8_t* base =
    pixels_.get() + y * stride_ + x;
std::shared_ptr<const std::uint8_t> pixels(pixels_, base);
return Frame(std::move(pixels), width, height, stride_);
```

`pixels_` 本身已经是一个 aliasing `shared_ptr`，再次 aliasing 仍然指向同一个控制块。于是：

- 一个 4K 帧可以切出几十个 ROI，不发生像素拷贝；
- 每个 ROI 独立延长缓冲区生命周期；
- ROI 的 `data()` 是基址加偏移，`stride` 保留原始行跨度；
- 释放生产者/全帧后，ROI 仍然有效。Demo 断言了这一点。

这比“每个 ROI 拷贝一份 vector”在视频/图像/网络包处理里差别巨大。

### 4.3 `owner_key`：空存储指针但非空控制块

`Frame::owner_key()`：

```cpp
OwnerKey owner_key() const {
    return OwnerKey(pixels_, static_cast<const void*>(0));
}
```

这产生一个：

- `get() == nullptr`；
- `use_count() > 0`；
- 共享 `pixels_` 控制块；

的 `shared_ptr<const void>`。

它适合作为“控制块身份”的 key：不关心对象地址，只关心“是不是同一个所有者”。但必须注意：

```cpp
Frame::OwnerKey a = frame_a.owner_key();
Frame::OwnerKey b = frame_b.owner_key();
a == b;              // true！因为 operator== 比较的是存储指针，两个都是 nullptr
same_owner(a, b);    // false，owner_before 比较控制块
```

Demo 的 `frame_owner_identity_uses_control_block_not_value` 明确验证了这个反直觉行为。结论：

- 判断“同一个所有者”用 `owner_before`/`owner_less`，不要用 `==`；
- 不要把“空存储指针”误判为“空 `shared_ptr`”；
- `operator bool` 对 aliasing null 返回 false，但它仍然拥有控制块。

### 4.4 aliasing 的生命周期边界

aliasing 只保证 **owner 控制块** 活着，不保证 `ptr` 指向的内存在所有情况下都有效。你必须自己保证：

- `ptr` 指向的是 owner 管理的对象/缓冲区的一部分；
- 偏移和长度没有越界；
- 如果 owner 是自定义删除器管理的资源，`ptr` 指向的地址在删除器执行前有效。

Demo 的 `Frame::slice` 做了边界检查，并用 `stride * height` 验证缓冲区大小。

### 4.5 重新理解aliasing constructor

aliasing constructor 是 std::shared_ptr 的别名构造函数，核心原理是：共享 r 的控制块来管理生命周期，但 get() 返回另一个指针 ptr，实现“所有权和指向对象分离”。
原型：
```cpp
template<class Y>
shared_ptr(const shared_ptr<Y>& r, element_type* ptr) noexcept;
```
工作原理:
普通 shared_ptr 里，所有权指针和存储指针通常指向同一个对象。别名构造后，两者可以不同：
- 所有权：与 r 共享同一个控制块，use_count() 相同；
- 指向对象：get() 返回 ptr，而不是 r.get()；
- 析构行为：引用计数归零时，删除的是 r 原来管理的对象，而不是 ptr；
- 程序员责任：必须保证 ptr 在别名 shared_ptr 存活期间始终有效。

示例：
```cpp
struct Foo { Bar member; };

auto pf = std::make_shared<Foo>();
std::shared_ptr<Bar> pb(pf, &pf->member); // 别名构造
```
pb 指向 pf->member，但只要 pb 还活着，pf 管理的 Foo 就不会被销毁。
常见场景包括：指向成员对象、容器元素、static/dynamic/const_pointer_cast 的实现，以及需要“持有子对象但保证父对象不死”的接口设计。

常见坑：
```cpp
shared_ptr<X> sp2(sp1, new X); // ❌ 危险
```
new X 不会被这个 shared_ptr 管理，最后也不会被删除，容易泄漏。 所以 ptr 通常应该是被管理对象的成员、子对象或类型转换后的指针，而不是独立分配的新对象。

## 5. `weak_ptr`：不拥有，但能安全观察

### 5.1 基本语义

`weak_ptr` 从 `shared_ptr` 或另一个 `weak_ptr` 构造，不增加强计数，只增加弱计数：

```cpp
std::weak_ptr<FrameBuffer> observer = buffer;
if (std::shared_ptr<FrameBuffer> locked = observer.lock()) {
    // 这里 locked 保证 buffer 至少活到作用域结束
}
```

- `lock()` 是原子的“检查 + 增引用”；
- `expired()` 只是瞬时快照，不能代替 `lock()`；正确模式是先 `lock()`，再检查返回值；
- `weak_ptr` 本身不提供 `operator*`/`operator->`，必须 `lock()`；
- `weak_ptr::reset()` 放弃观察，减少弱计数；
- `use_count()` 在 `weak_ptr` 上不存在（C++11），只能通过 `lock()` 后看强计数。

### 5.2 打破循环引用

Demo 的场景树：

```cpp
class Element {
    std::weak_ptr<Element> parent_;                  // 反向边：弱
    std::vector<std::shared_ptr<Element>> children_; // 正向边：强
};
```

如果父子的两条边都是 `shared_ptr`：

```text
root <----> child <----> grandchild
```

外部引用释放后，每个对象的强计数都不为 0，整个环泄漏。Demo 的 `CyclicNode` 复现了这一点：

```cpp
first->link_to(second);
second->link_to(first);
first.reset();
second.reset();
// alive == 2，两个对象都还在
```

随后通过 `break_link()` 断开一条边，两个对象才被销毁：

```text
after external reset alive=2
after break_link alive=0
```

设计原则：

- 树/图选择一个方向作为强所有权（通常是父 -> 子、容器 -> 元素）；
- 反向边用 `weak_ptr`；
- 如果图可能任意连接，必须有显式的环检测或生命周期协议，不能假设“不会成环”；
- 第三方 API 返回 `shared_ptr` 时，注意它可能已经持有环。

### 5.3 弱缓存

`FrameCache`：

```cpp
std::unordered_map<uint64_t, std::weak_ptr<FrameBuffer>> entries_;
```

缓存不延长缓冲区生命周期：

- 生产者释放后，缓存项自动过期；
- `get()` 先 `lock()`，失败则删除条目；
- `purge_expired()` 批量清理。

这适合“缓存只是加速，不是所有权”的场景。反过来，如果缓存应该保活（例如热数据），就应该存 `shared_ptr`，并设计容量/淘汰策略（LRU）。不要让缓存的所有权语义模糊。

注意 `expired()` 和 `lock()` 之间的竞态：另一个线程可能刚好释放最后一个强引用。正确写法是直接 `lock()` 并检查结果，而不是先 `if (!weak.expired()) weak.lock()`。

### 5.4 弱观察者

Pipeline 的 `Node` 保存：

```cpp
std::vector<std::weak_ptr<Observer>> observers_;
```

好处：

- 观察者销毁后，节点不会把它保活；
- `emit` 时把仍存活的观察者 `lock()` 到局部 `shared_ptr` 列表，回调期间即使观察者被其他线程释放也安全；
- `observer_count()` 顺手清理过期项。

如果这里存 `shared_ptr`，一个 UI 观察者忘记退订就会导致整条 pipeline/节点泄漏。观察者模式的默认所有权应该是弱引用，除非业务明确要求“订阅即保活”。

### 5.5 异步取消与保活

异步任务捕获什么，决定了生命周期语义：

```cpp
// 保活：任务执行前对象不会被销毁
queue.post([node]() { node->do_work(); });

// 取消：对象销毁后任务自动跳过
std::weak_ptr<Node> weak = node;
queue.post([weak]() {
    if (auto locked = weak.lock()) {
        locked->do_work();
    }
});
```

Demo 的测试用 gate 把任务阻塞住，确保对象在任务执行前被释放，然后验证：

- 强捕获任务仍能看到对象，任务结束后对象才销毁；
- 弱捕获任务看到 `lock()` 失败，直接跳过。

这是“异步任务生命周期安全”的核心模式。不要捕获裸 `this`，也不要在 lambda 里保存裸指针后再跨线程使用。

### 5.6 弱引用不是“无成本”

`weak_ptr` 占两个指针，`lock()` 有一次原子操作；大量弱引用会增加控制块弱计数，影响 `make_shared` 的内存释放时机。缓存/观察者场景通常值得，但不要在高频路径上无脑创建 `weak_ptr`。

## 6. `enable_shared_from_this`

### 6.1 为什么不能 `shared_ptr<T>(this)`

如果对象已经由 `shared_ptr` 管理：

```cpp
std::shared_ptr<Node> node = std::make_shared<Node>();
```

在成员函数里写：

```cpp
std::shared_ptr<Node> self(this);  // 灾难
```

会创建 **第二个控制块**。两个控制块都认为自己是唯一所有者，强计数各自为 1，最终双重释放。

`enable_shared_from_this<T>` 的做法：

- 基类里放一个 `weak_ptr<T>`；
- 当 `shared_ptr<T>` 构造并检测到 `T` 继承自 `enable_shared_from_this<T>` 时，把该 `weak_ptr` 初始化为当前控制块；
- `shared_from_this()` 等价于 `weak_self.lock()`，失败抛 `std::bad_weak_ptr`。

一句话总结：
对象已经在被某个 shared_ptr 管理，但成员函数里只有裸指针 this，如何安全地再拿到一个共享所有权的 shared_ptr。
如果直接写 std::shared_ptr<T>(this)，会产生一个新的、独立的控制块，导致同一个对象被两个互不知情的 shared_ptr 管理，最终可能 double-free。
一句话原理：
它内部保存了一个 weak_ptr，通常叫 weak_this。
当对象第一次被 shared_ptr 管理时，shared_ptr 的构造函数会检测到它继承了 enable_shared_from_this，并把当前控制块绑定到这个 weak_this。
之后调用 shared_from_this()，本质上是把这个 weak_ptr 提升成 shared_ptr，所以新指针和原来的 shared_ptr 共享同一个控制块。

### 6.2 使用边界

Demo 中 `Element::add_child` 和 `Node::connect_to` 都调用 `shared_from_this()` 来建立反向弱边。

正确：

```cpp
std::shared_ptr<Node> node = std::make_shared<Node>("n");
node->connect_to(other);   // 可以
```

错误：

```cpp
Node stack_node("n");
stack_node.connect_to(other);  // throws std::bad_weak_ptr
```

`shared_from_this()` 不能在构造函数里调用：对象构造期间，外部 `shared_ptr` 还没完成控制块与 `weak_this` 的绑定。Demo 的 `ConstructorProbe` 在构造函数里调用并捕获 `bad_weak_ptr`，验证了这一点。

其他边界：

- 析构函数里也不要依赖 `shared_from_this()`；此时强计数通常已经归零；
- `enable_shared_from_this` 必须是可访问的基类，私有继承可能导致无法初始化；
- 多继承中若有两个 `enable_shared_from_this` 基类，`shared_from_this()` 可能歧义，需要显式限定或重新设计；
- C++17 提供 `weak_from_this()`，可以在不抛异常的情况下拿到弱引用；Demo 在 C++11 下用 `self_weak()` 封装 `weak_ptr(shared_from_this())`。

### 6.3 异步回调中的选择

- 如果回调必须让对象活着：在成员函数里 `shared_from_this()`，把 `shared_ptr` 捕获进任务；
- 如果对象可以被取消：捕获 `weak_ptr`，`lock()` 失败就返回；
- 如果回调可能比对象活得久，绝不能捕获裸 `this`。

## 7. 侵入式引用计数：`intrusive_ptr`

### 7.1 为什么需要它

`shared_ptr` 的控制块在对象外部：

```text
shared_ptr -> 对象
           -> 控制块（另一个分配）
```

侵入式引用计数把计数放进对象内部：

```text
intrusive_ptr -> 对象 [atomic refcount | data...]
```

结果：

- 指针只有一个机器字（Demo 实测 8 字节）；
- 没有控制块，少一次分配，缓存局部性更好；
- 可以和 C ABI/插件边界共享所有权，只要双方遵守同一套 `retain/release` 协议；
- 可以复用对象已有的引用计数（例如某些 C 库、GPU 资源）；
- 代价：必须修改对象类型；没有 `weak_ptr`；没有类型擦除删除器；计数逻辑由你维护。

### 7.2 Demo 实现

`RefCounted`：

```cpp
class RefCounted {
public:
    void retain() const { refs_.fetch_add(1, relaxed); }
    bool release() const {
        if (refs_.fetch_sub(1, acq_rel) == 1) {
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

设计点：

- `refs_` 初始为 0；
- `retain()` 用 `relaxed`，因为增加引用不需要同步对象内容；
- `release()` 用 `acq_rel`，保证最后一个释放者能看到其他线程对对象的写入；
- 析构函数 `protected virtual`：允许 `intrusive_ptr<Base>` 正确删除派生对象，同时禁止外部裸 `delete`；
- `delete this` 发生在计数从 1 减到 0 时。

`make_intrusive`：

```cpp
template <typename T, typename... Args>
intrusive_ptr<T> make_intrusive(Args&&... args) {
    return intrusive_ptr<T>(new T(std::forward<Args>(args)...));
}
```

它构造对象后通过 `intrusive_ptr(T*)` 默认 `add_ref=true` 把计数从 0 增到 1。`intrusive_ptr(T*, false)` 表示“接管一个已经计入的引用”，只应在所有权转移时使用：

```cpp
GpuBuffer* raw = owner.detach();          // 计数 1 被转移给 raw
intrusive_ptr<GpuBuffer> adopted(raw, false); // 接管这 1，不增加
```

如果 `raw` 同时还有别的所有者，`add_ref=false` 会偷走别人的引用，最终双重释放。Demo 在 main 中先 `copy.reset()`，确保 detach 的是唯一计数，再做 adopt。

`reset(T* p, bool add_ref)` 里的同指针保护：

```cpp
if (p == p_) return;
```

避免 `reset(p_, false)` 先释放当前对象、再让临时对象释放同一指针导致双重释放。

### 7.3 与 `shared_ptr` 的取舍

| 维度 | `shared_ptr` | `intrusive_ptr` |
| --- | --- | --- |
| 指针大小 | 2 字 | 1 字 |
| 控制块 | 有，额外分配 | 无 |
| 对象大小 | 不变 | 增加计数 |
| 弱引用 | 支持 | 不支持（需另实现） |
| 自定义删除器 | 支持，类型擦除 | 不支持（析构固定） |
| 复用已有计数 | 困难 | 容易 |
| 跨 ABI | 控制块布局不保证 | 只要协议一致即可 |
| 适用 | 通用共享所有权 | 高频、ABI、已有引用计数对象 |

不要因为“更快”就默认选侵入式。它要求对象从设计之初就参与所有权协议，并且没有 `weak_ptr` 带来的观察能力。Demo 只把它用于 GPU 缓冲区这种“高频、需要跨 C 边界、对象本身可改造”的场景。

## 8. 线程安全模型

### 8.1 控制块安全 ≠ 对象安全

标准保证：

- 不同线程可以同时复制/销毁 **不同的** `shared_ptr` 实例，只要它们共享同一个控制块；
- 引用计数操作是原子的；
- `weak_ptr::lock` 与最后一个 `shared_ptr` 的释放是安全的。

标准不保证：

- 多个线程同时读写 **同一个** `shared_ptr` 对象（例如一个线程 `reset`，另一个线程 `get`）是安全的；这是数据竞争；
- `shared_ptr` 指向的对象本身线程安全；
- `use_count()` 的返回值在并发下是精确的；它只是瞬时近似。

### 8.2 同一实例的并发读写

错误：

```cpp
std::shared_ptr<Config> g_config;

void reader() { auto c = g_config; }  // 与 writer 的赋值竞争
void writer() { g_config = new_config; }
```

C++11 方案：

```cpp
std::shared_ptr<Config> g_config;

void reader() { auto c = std::atomic_load(&g_config); }
void writer() { std::atomic_store(&g_config, new_config); }
```

`std::atomic_load/atomic_store` 对 `shared_ptr` 提供原子快照。Demo 的 `SharedSlot` 就是这个模式：

```cpp
void store(std::shared_ptr<T> value) {
    std::atomic_store(&value_, std::move(value));
}

std::shared_ptr<T> load() const {
    return std::atomic_load(&value_);
}
```

注意：

- 这两个自由函数在 C++20 被弃用，C++20 应使用 `std::atomic<std::shared_ptr<T>>`；
- 原子快照解决的是“指针替换”，不是“对象内容修改”；
- 读多写少的配置/插件表适合这种 RCU 风格；写者仍需互斥来串行化读-改-写，否则会丢更新。

### 8.3 写时复制快照

`PluginRegistry` 的写路径：

```cpp
PluginSnapshot current = slot_.load();
auto next = std::make_shared<PluginMap>(*current);
(*next)[slot] = plugin;
slot_.store(next);
```

- 读者拿到的旧快照不会被修改；
- 在途调用方持有旧插件，热升级不会让它悬空；
- 写者用 `write_mutex_` 串行化，避免两个 `install` 同时基于同一旧快照写入而丢更新；
- 读者完全无锁。

这是“不可变快照 + 原子发布”的经典模式，比“读也加锁”有更好的读扩展性。

### 8.4 观察者回调的锁边界

`Node::emit`：

1. 加锁，把 `weak_ptr` 提升为 `shared_ptr` 列表，顺手清理过期项；
2. 解锁；
3. 遍历强引用列表回调。

为什么不在锁内回调？观察者回调可能：

- 订阅/退订，导致重入死锁；
- 执行耗时操作；
- 触发另一个节点的通知。

先复制强引用列表，既保证回调期间观察者不被销毁，又避免持锁回调。

## 9. `owner_before` 与身份比较

`shared_ptr` 的 `operator==` 比较的是 `get()`，不是控制块：

```cpp
std::shared_ptr<Base> a = ...;
std::shared_ptr<Member> b(a, &a->member);
a == b;  // false，存储指针不同
```

如果关心“是否由同一个控制块管理”，用：

```cpp
!a.owner_before(b) && !b.owner_before(a)
```

或者把 `std::owner_less<std::shared_ptr<T>>` 作为关联容器比较器：

```cpp
std::map<Key, std::string, std::owner_less<Key>> labels_;
```

`owner_less` 提供严格弱序，使：

- 同一控制块的不同 aliasing 指针等价；
- 不同控制块即使存储地址相同（例如都为 null）也不等价；
- 可用于 `shared_ptr` 和 `weak_ptr` 的异构比较（标准提供相应重载）。

`OwnerRegistry` 用 `shared_ptr<const void>` 作为 key，正是为了把“控制块身份”从对象类型中剥离。注意：

- key 是 `shared_ptr`，会 **强引用** 控制块，从而延长对象生命周期；Demo 中 `registry.clear()` 后对象才销毁。业务中如果元数据不应保活，应使用 `weak_ptr` key 并定期清理；
- `operator==` 对两个 null aliasing 指针返回 true，但 `owner_less` 能区分；
- `owner_before` 不是值比较，不要用它判断对象内容。

owner_before 不是用来比较指针地址大小的，而是用来比较“是否属于同一个控制块 / 同一组所有权”。它的核心价值是让 shared_ptr 和 weak_ptr 在关联容器中能按所有权正确排序和判等。

## 10. 异常安全与求值顺序

RAII 的核心收益是异常路径也能释放资源：

```cpp
void process() {
    UniqueResource resource = make_unique_resource(1);
    may_throw();
    // 即使抛异常，resource 析构仍会关闭资源
}
```

但智能指针本身也有陷阱：

### 10.1 裸指针交给两个所有者

```cpp
Thing* raw = new Thing;
std::shared_ptr<Thing> a(raw);
std::shared_ptr<Thing> b(raw);  // 两个控制块，双重释放
```

正确做法：`auto a = std::make_shared<Thing>(); auto b = a;` 或从 `unique_ptr` 提升。

### 10.2 函数实参求值顺序

C++11 中函数实参求值顺序未指定，`f(std::unique_ptr<T>(new T), g())` 可能先执行 `g()` 并抛出，导致 `new T` 泄漏。使用 `make_unique` 把 `new` 和所有权接管封装到同一个函数调用内部。

### 10.3 构造函数抛出

`std::make_shared<T>(args...)` 在构造 `T` 抛异常时会释放合并块；`std::shared_ptr<T>(new T)` 如果在控制块分配时抛异常，标准要求删除 `T`。但前提是 `new T` 和 `shared_ptr` 构造之间没有其他可能抛出的操作（见求值顺序）。

### 10.4 删除器与析构不能抛

- `unique_ptr` 的删除器、`shared_ptr` 控制块的销毁函数在析构路径上执行；抛出会导致 `std::terminate`（析构函数默认 `noexcept`）。
- 自定义删除器应标记 `noexcept` 或保证不抛。
- 不要在析构函数里调用可能抛异常的清理逻辑。

### 10.5 `reset` 与自赋值

`unique_ptr::reset(p)` 先删除旧对象，再接管新对象。如果 `p` 与当前指针相同，会先删除再接管悬空指针；业务代码不应这样写。`intrusive_ptr::reset` 中我们加了同指针保护，避免更隐蔽的双重释放。

## 11. 尺寸与性能模型

### 11.1 实测尺寸

x86-64、libstdc++/MinGW 下：

| 类型 | 大小 |
| --- | --- |
| `unique_ptr<int>` | 8 |
| `unique_ptr<int, void(*)(int*)>` | 16 |
| `shared_ptr<int>` | 16 |
| `weak_ptr<int>` | 16 |
| `intrusive_ptr<GpuBuffer>` | 8 |

具体数值依 ABI 和标准库实现可能不同，但相对关系稳定：

- `unique_ptr` 无状态删除器 = 一个指针；
- `unique_ptr` 函数指针删除器 = 两个指针；
- `shared_ptr`/`weak_ptr` = 两个指针，与删除器无关；
- 侵入式指针 = 一个指针，代价转移到对象。

### 11.2 分配次数

- `make_shared`/`allocate_shared`：通常 1 次分配（对象 + 控制块）；
- `shared_ptr(new T)`：通常 2 次分配；
- `unique_ptr(new T)`：1 次分配，无控制块；
- 从 `unique_ptr` 提升为 `shared_ptr`：额外 1 次控制块分配（对象已存在）；
- `weak_ptr` 本身不分配，但增加控制块弱计数。

### 11.3 原子操作成本

每次复制/销毁 `shared_ptr` 都涉及原子增减；`weak_ptr::lock` 还涉及条件原子操作。高频路径上：

- 能不复制就不复制，传 `const shared_ptr<T>&` 或 `T&`；
- 需要长期持有时再复制；
- 单线程/明确生命周期的地方优先 `unique_ptr`；
- 极端热路径考虑侵入式计数或对象池；
- 不要用 `shared_ptr` 作为“线程安全”的万能替代品。

### 11.4 缓存局部性

`make_shared` 把对象和控制块放在一起，访问对象和计数时缓存局部性更好；`shared_ptr(new T)` 两次分配可能落在不同缓存行。侵入式计数把计数和对象放在同一块内存，通常最好，但会增加对象大小并影响对齐。

### 11.5 `weak_ptr` 的内存滞留

`make_shared` + 长寿命 `weak_ptr` 会阻止整块内存释放。若对象很大，考虑：

- 用 `shared_ptr(new T)` 让对象内存在强引用归零时释放，只保留小控制块；
- 缩短 `weak_ptr` 生命周期；
- 用弱缓存而不是长期弱观察。

## 12. C++ 标准演进

Demo 以 C++11 为基线，以下特性在更新标准中可用：

| 特性 | 标准 | Demo 的 C++11 替代 |
| --- | --- | --- |
| `std::make_unique` | C++14 | `spl::make_unique` |
| 泛型 lambda / 初始化捕获 | C++14 | 显式变量 + 按值捕获 |
| `weak_from_this()` | C++17 | `weak_ptr(shared_from_this())` |
| `std::shared_ptr<T[]>` | C++17 | `unique_ptr<T[]>` + 自定义封装 |
| `std::atomic<std::shared_ptr<T>>` | C++20 | `atomic_load/atomic_store` |
| `std::atomic_load(shared_ptr*)` | C++11，C++20 弃用 | 保留用于 C++11 |
| `std::make_shared<T[]>` | C++20 | 手动 `shared_ptr` + 数组删除器 |
| `std::out_ptr` / `inout_ptr` | C++23 | 显式 `unique_ptr` 构造 |
| `owner_less<void>` 异构比较 | 较新标准 | 固定 key 类型 |

升级建议：

- 升级到 C++14：用 `std::make_unique` 替换自定义实现，删除 `memory_utils.h` 中的工厂；
- 升级到 C++17：用 `weak_from_this` 替换 `self_weak`；如果不需要数组共享，`shared_ptr<T[]>` 可简化缓冲区；
- 升级到 C++20：用 `std::atomic<std::shared_ptr<T>>` 替换自由函数，注意 `atomic_load/store` 已弃用；
- 升级不会改变所有权设计，反而会让代码更短。

## 13. 高频陷阱清单

1. **同一裸指针构造两个 `shared_ptr`**：两个控制块，双重释放。
2. **`shared_ptr<T>(this)`**：同上；用 `enable_shared_from_this`。
3. **构造函数里 `shared_from_this()`**：抛 `bad_weak_ptr`。
4. **循环引用**：强计数永不归零；用弱反向边并测试。
5. **用 `use_count()` 做业务判断**：并发下只是近似值。
6. **同一个 `shared_ptr` 实例并发读写**：数据竞争；用锁或原子自由函数。
7. **`weak_ptr::expired()` 后直接解引用**：`weak_ptr` 没有解引用；先 `lock()` 并检查。
8. **aliasing null 指针当空指针**：`operator bool` 为 false，但可能仍持有控制块。
9. **用 `==` 判断 owner 身份**：应使用 `owner_before`/`owner_less`。
10. **自定义删除器抛异常**：析构路径 `terminate`。
11. **`unique_ptr` 指向不完整类型**：析构/移动需在完整类型处定义。
12. **`unique_ptr<Base>` 删除非虚析构派生对象**：未定义行为。
13. **`std::move(unique_ptr)` 后继续解引用源**：源为空。
14. **`release()` 后忘记接管**：泄漏。
15. **`make_shared` + 长寿命 `weak_ptr` 持有大对象**：整块内存延迟释放。
16. **观察者/缓存存 `shared_ptr`**：无意中延长生命周期，甚至形成环。
17. **异步任务捕获裸 `this`**：对象销毁后悬空；用 `shared_from_this` 或 `weak_ptr`。
18. **`shared_ptr<const T>` 以为对象不可变**：只限制通过该指针修改，其他别名仍可改。
19. **把 `shared_ptr` 当线程安全对象**：控制块安全，对象和实例本身不保证。
20. **在锁内回调观察者**：重入/死锁/长临界区；先复制强引用再回调。

## 14. 小结

- `unique_ptr` 是默认选择：表达独占、零开销、可移动、可定制删除器。
- `shared_ptr` 只在真正共享所有权时使用；理解控制块、两次分配、类型擦除删除器和 `make_shared` 的弱引用滞留。
- `weak_ptr` 是观察者、缓存、反向边、异步取消的默认工具；始终 `lock()` 后使用。
- aliasing constructor 是零拷贝视图和 owner 身份工具，但要区分存储指针与控制块。
- `enable_shared_from_this` 解决“从 this 安全获得 shared_ptr”，但有构造期、栈对象、继承等边界。
- `intrusive_ptr` 用对象内计数换取尺寸/ABI/局部性，代价是没有弱引用和类型擦除删除器。
- 线程安全要区分“控制块”“shared_ptr 实例”“被管理对象”三个层次，不能混为一谈。

接下来的 `02-demo-walkthrough.md` 会把这些原理逐一落到 Demo 的具体文件、函数和测试断言上。
