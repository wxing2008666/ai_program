# C++ 智能指针工程实验室

这是一个以 **C++11** 为基线、面向 Linux 编译环境的智能指针深度 Demo。它不是 API 速查表，而是一个可运行的“帧处理运行时”切片：把 `unique_ptr`、`shared_ptr`、`weak_ptr`、`intrusive_ptr` 放进真实的生命周期、并发、缓存、插件热升级、异步取消和 C ABI 边界中。

项目目标：

- 覆盖智能指针约 90% 的工程知识点，而不是只演示 `make_shared`。
- 每个结论都有可执行测试或可观察输出，不靠注释和口头说明。
- 代码可直接用 `g++ -std=c++11` 在 Linux 上构建，不依赖第三方库。
- 文档解释“为什么这样设计”，以及业务中什么时候不该用智能指针。

## 快速开始

```bash
cd 01-cpp-smart-pointers

make -j
./build/spl_demo
./build/spl_tests
```

也可以使用 CMake：

```bash
mkdir -p build-cmake
cd build-cmake
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
ctest --output-on-failure
./spl_demo
cd ..
```

要求：

- Linux + GCC/Clang，支持 C++11。
- 链接 pthread（Makefile 和 CMake 已处理）。
- 不需要 Boost、gtest 等第三方依赖；CMake 只是可选的构建方式。

## Demo 主线

程序模拟一条最小但完整的媒体处理链路：

1. 生产者创建 `FrameBuffer`，通过 **aliasing constructor** 切出零拷贝 ROI 视图。
2. 弱引用缓存按帧 ID 查找缓冲区，不延长生命周期；`owner_less` 身份表按控制块而不是裸地址索引元数据。
3. 场景树用强子节点 + 弱父节点表达双向关系，显式演示循环引用泄漏与断链。
4. Pipeline 用 `enable_shared_from_this` 建立弱上行边，事件观察者全部使用 `weak_ptr`。
5. 线程池任务分别用强捕获和弱捕获，演示“保活”与“取消”两种异步生命周期策略。
6. 插件注册表用原子共享快照实现无锁读、写时复制和热升级；旧调用方手中的插件不会被卸载。
7. C 资源通过自定义删除器进入 `unique_ptr`，再提升为 `shared_ptr`；GPU 资源使用侵入式引用计数跨越 ABI 边界。
8. 最后用计数分配器观察 `allocate_shared` 的合并分配，以及 `weak_ptr` 如何延迟释放整块内存。

## 目录结构

```text
01-cpp-smart-pointers/
├── include/spl/
│   ├── allocation_probe.h      # 计数分配器、合并分配实验
│   ├── codec.h                 # unique_ptr + pimpl
│   ├── frame.h                 # aliasing constructor、零拷贝视图
│   ├── frame_cache.h           # weak_ptr 缓存、owner_less 身份表
│   ├── gpu_buffer.h            # intrusive_ptr 业务对象
│   ├── intrusive_ptr.h         # 手写侵入式引用计数指针
│   ├── memory_utils.h          # C++11 make_unique、same_owner
│   ├── object_graph.h          # 弱父节点、循环引用
│   ├── pipeline.h              # enable_shared_from_this、弱观察者
│   ├── plugin.h                # 多态插件接口
│   ├── plugin_registry.h       # 原子快照、热升级
│   ├── resource.h              # C 资源自定义删除器
│   ├── shared_slot.h           # atomic_load/atomic_store 共享槽
│   ├── task_queue.h            # 异步任务生命周期
│   └── tracked.h               # 实例计数探针
├── src/                        # 对应实现与 demo 入口
├── tests/                      # 47 个测试，自带轻量测试框架
├── docs/
│   ├── 01-principles-and-internals.md
│   ├── 02-demo-walkthrough.md
│   └── 03-business-playbook.md
├── CMakeLists.txt
├── Makefile
└── README.md
```

## 关键运行结果

`./build/spl_demo` 的实测输出（地址和具体字节数随平台变化）：

```text
unique_ptr<int>                    : 8 bytes
unique_ptr<int, function pointer>  : 16 bytes
shared_ptr<int>                    : 16 bytes
weak_ptr<int>                      : 16 bytes
intrusive_ptr<GpuBuffer>           : 8 bytes

pointer delta=520 bytes, stride=64, roi owner_use_count=3
same control block=true, after owner_key use_count=5
after releasing producer: cache_hit=true, buffer_alive=true
after releasing last view: cache_hit=false, buffer_alive=false, cache_size=0

lookup through slice=camera-0, registry_size=1

leaf depth=2, parent=child, alive=3
after tree destruction alive=0

after external reset alive=2
after break_link alive=0

sink upstream=source, observer_count=1
observer calls=2
after pipeline scope observers_alive=0

queued while strong task exists: alive=1
  weak task: node=cancelled
  strong task: node=async-strong
after queue drain: alive=0, observer_expired=true

in_flight plugin=invert old_first_pixel=255, current plugin=passthrough new_first_pixel=0, passthrough_shared=true

unique id=7, open=1
shared use_count=2, open=1
after scope open=0, opened=1, closed=1

use_count=2, alive=1
detached raw, adopted use_count=1
after scope alive=0

allocate_shared allocations=1, object_alive=1
after strong refs: object_alive=0, block_outstanding=1
after weak reset: block_outstanding=0

decoder=h264/q80, moved_from_valid=false, output_shared=true
```

测试结果：

```text
[==========] 47 tests, 228 checks, 0 failed
```

## 知识点覆盖矩阵

| 知识点 | 主要代码 | 验证 |
| --- | --- | --- |
| `unique_ptr` 独占所有权与移动 | `resource.h/.cpp`、`codec.h/.cpp` | `test_resources`、`test_codec` |
| `unique_ptr` 无状态/函数指针删除器开销 | `test_sizes.cpp` | 8/16 字节对比 |
| `unique_ptr<T[]>` 与 C++11 `make_unique` | `memory_utils.h` | `test_make_unique` |
| 自定义删除器封装 C API | `resource.h/.cpp` | `test_resources` |
| `unique_ptr` 多态与虚析构约束 | `test_casts.cpp` | `has_virtual_destructor` |
| pimpl + 不完整类型 | `codec.h/.cpp` | 移动、moved-from |
| `unique_ptr` 提升为 `shared_ptr` | `resource.cpp` | `test_resources` |
| `shared_ptr` 控制块与引用计数 | `frame.h`、`frame_cache` | 生命周期断言 |
| `make_shared` / `allocate_shared` 合并分配 | `allocation_probe.h` | 1 次 vs 2 次分配 |
| `weak_ptr` 延迟释放合并块 | `allocation_probe` | `weak_ptr_delays...` |
| `shared_ptr` 自定义删除器类型擦除 | `resource.cpp` | 尺寸与关闭次数 |
| aliasing constructor 零拷贝子视图 | `frame.h/.cpp` | ROI 地址/生命周期 |
| `owner_before` / `owner_less` 身份比较 | `frame_cache.*` | 同一/不同控制块 |
| null aliasing 指针陷阱 | `frame.h`、`test_frame` | `==` 与 `owner_before` 差异 |
| `enable_shared_from_this` | `object_graph`、`pipeline` | 构造期/栈对象异常 |
| 弱父节点与循环引用 | `object_graph.*` | 泄漏与显式断链 |
| 弱观察者自动摘除 | `pipeline.*` | observer 过期后计数 |
| 弱缓存不延长生命周期 | `frame_cache.*` | hit/miss/purge |
| 异步强捕获保活 | `task_queue`、`test_async` | 任务期间 alive=1 |
| 异步弱捕获取消 | `task_queue`、`test_async` | owner 销毁后跳过 |
| 线程池异常隔离 | `task_queue.*` | failed_tasks |
| `shared_ptr` 控制块线程安全 | `shared_slot.h` | 并发读写烟测 |
| 同一 `shared_ptr` 实例并发读写风险 | 文档 + `SharedSlot` | 原子自由函数方案 |
| RCU/COW 插件热升级 | `plugin_registry.*` | 旧快照/在途调用 |
| 侵入式引用计数 | `intrusive_ptr.h`、`gpu_buffer` | detach/adopt、ABI |
| 引用计数转移与 `reset` 自赋值 | `intrusive_ptr.h` | `test_intrusive` |
| 派生到基类转换的删除器语义 | `test_casts.cpp` | 非虚析构仍安全 |
| `shared_ptr` 三种 cast | `test_casts.cpp` | dynamic cast |
| 异常安全与所有权表达 | 全局 | RAII + 测试 |
| 对象大小/开销模型 | `test_sizes`、文档 | x86-64 实测 |

## C++11 基线说明

项目刻意使用 C++11，因为很多生产环境仍停留在 C++11/14。由此产生了一些“标准缺口”，Demo 中给出了工程替代：

| C++11 缺口 | Demo 方案 | 更新标准 |
| --- | --- | --- |
| 没有 `std::make_unique` | `spl::make_unique` / `make_unique_array` | C++14 |
| 没有 `weak_from_this()` | `Node::self_weak()` 内部 `weak_ptr(shared_from_this())` | C++17 |
| 没有 `std::atomic<std::shared_ptr<T>>` | `atomic_load/atomic_store` 自由函数 | C++20 |
| 没有 `shared_ptr<T[]>` | 用 `unique_ptr<T[]>` 持有数组，`shared_ptr` 持有对象 | C++17 |
| 没有初始化捕获 | 显式构造 `weak_ptr` 变量再按值捕获 | C++14 |
| 没有 `owner_less<void>` 异构比较 | 固定 `shared_ptr<const void>` key 类型 | 较新标准 |

升级到 C++17/20 时，相关替换点集中在 `memory_utils.h`、`pipeline.h`、`shared_slot.h`，业务代码不需要改所有权模型。

## 阅读顺序

1. 先运行 `spl_demo`，对照本文“关键运行结果”观察生命周期。
2. 阅读 `docs/01-principles-and-internals.md`，建立控制块、引用计数、删除器和 aliasing 的心智模型。
3. 阅读 `docs/02-demo-walkthrough.md`，逐模块对照代码和测试。
4. 阅读 `docs/03-business-playbook.md`，把模式落到 API 设计和代码评审清单。
5. 修改测试中的生命周期顺序，观察断言如何失败，这是理解智能指针最快的方式。

## 设计取舍

- **不用 gtest**：减少依赖，测试框架只保留断言、注册和运行器；重点在生命周期断言，而不是测试框架本身。
- **不用 `shared_ptr` 包打天下**：`Frame` 的像素用 `shared_ptr<const uint8_t>`，但缓冲区创建、解码器实现、插件槽位都优先使用独占所有权或快照。
- **不隐藏失败路径**：越界、空帧、循环连接、栈对象调用 `shared_from_this`、关闭后投递任务都会抛出明确异常，并由测试覆盖。
- **不演示未定义行为**：`unique_ptr<Base>` 删除无虚析构派生对象只在文档中说明风险；测试使用 `shared_ptr` 的安全转换和带虚析构的 `unique_ptr` 对照。
