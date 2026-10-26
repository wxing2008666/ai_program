# C++ 虚函数、多态与虚表：支付风控规则引擎 Demo

> 知识点：`virtual` 虚函数、纯虚函数、抽象类、虚析构、`override`/`final`、协变返回、vptr/vtable、多继承、虚继承、RTTI、`dynamic_cast`、去虚化、ABI 稳定性。
>
> 目标不是写一个“动物叫”的示例，而是用接近生产形态的**支付风控规则引擎**，把这些机制放进真实的业务约束里：插件化、组合、审计、类型安全、性能和二进制兼容。

---

## 1. 项目定位

### 1.1 业务场景

交易进入风控引擎后，需要按规则做决策：

```text
交易 Context
   ├─ 金额、国家、商户、1 分钟交易次数、设备是否可信 ...
   v
RuleEngine
   ├─ MaxAmountRule       金额上限
   ├─ CountryBlockRule    国家/地区黑名单
   ├─ VelocityRule        速度检查
   └─ CompositeRule       把多条规则组合成一个“支付守卫”
   v
ALLOW / REVIEW / REJECT + 命中原因 + 审计信息
```

规则会不断增加，引擎不应每加一条规则就改一次 `if/else`。因此：

- 引擎只依赖 `IRule` 接口；
- 具体规则独立实现、独立注册；
- 规则可以组合成树；
- 需要审计的规则额外实现 `IAuditable`；
- 规则通过 `clone()` 复制，支持插件式原型管理；
- 规则树通过 Visitor 导出，不把“导出格式”塞进规则类。

### 1.2 代码规模与依赖

- C++11（不使用 C++14/17 特性；没有 `std::make_unique`、没有 `std::variant`）；
- Linux + GCC/Clang（Itanium ABI）；
- 无第三方依赖；
- 核心源码约 **970 行**（不含 CMake、文档和 compile_fail 实验）；
- 构建：CMake 3.10+，或直接 `g++`。

### 1.3 目录结构

```text
01_virtual_polymorphism/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── risk/
│   │   └── risk.hpp          # 接口、NVI、规则、注册表、引擎
│   └── demo/
│       └── abi_probe.hpp     # ABI 探针声明
├── src/
│   ├── risk.cpp              # 具体规则、组合、clone、注册、引擎实现
│   ├── main.cpp              # 7 组运行时演示 + 行为检查
│   └── abi_probe.cpp         # 多继承/虚继承/vtable/RTTI 原始观察
├── docs/
│   ├── abi-deep-dive.md      # Itanium ABI 深度解析
│   └── business-design.md    # 架构决策、模式取舍、生产化清单
└── compile_fail/
    ├── README.md
    ├── override_typo.cpp
    ├── final_class.cpp
    ├── final_method.cpp
    ├── bad_covariant.cpp
    ├── virtual_base_downcast.cpp
    └── hiding.cpp
```

---

## 2. 构建与运行

### 2.1 CMake（推荐）

```bash
cd 01_virtual_polymorphism
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j
./vtable_demo
```

运行测试：

```bash
ctest --output-on-failure
```

`main` 中的 `require` 失败会抛异常，进程返回非 0，CTest 因此失败。

### 2.2 直接 g++

在项目根目录执行：

```bash
g++ -std=c++11 -Wall -Wextra -Wpedantic -Wnon-virtual-dtor \
    -fno-strict-aliasing -Iinclude \
    src/risk.cpp src/main.cpp src/abi_probe.cpp \
    -o vtable_demo

./vtable_demo
```

### 2.3 Sanitizer

在项目根目录执行：

```bash
mkdir -p build-asan && cd build-asan
cmake .. -DENABLE_SANITIZERS=ON
make -j
./vtable_demo
```

或直接：

```bash
g++ -std=c++11 -g -O1 -fsanitize=address,undefined \
    -fno-omit-frame-pointer -fno-strict-aliasing -Iinclude \
    src/risk.cpp src/main.cpp src/abi_probe.cpp \
    -o vtable_demo_asan
```

### 2.4 期望输出（节选）

不同编译器/版本的地址、偏移、`typeid` 名称可能不同，但决策和断言应一致：

```text
=== 1. rule engine: prototype registry + composite + audit ===
registered: country_block max_amount velocity
Composite(payment_guard, children=3)
  MaxAmount(limit=5000)
  CountryBlock(blocked=2)
  Velocity(velocity)
engine rules=1

case 1: country=SG amount=120 tx_1m=2
  result=ALLOW score=0
  - payment_guard => ALLOW (+0): all rules passed
  audit: composite(payment_guard, children=3)

case 2: country=SG amount=8000 tx_1m=2
  result=REJECT score=60
  - payment_guard => REJECT (+60): max_amount: amount 8000.000000 exceeds limit 5000.000000
...
```

最后应看到：

```text
All checks passed.
```

---

## 3. 七组运行时演示

`src/main.cpp` 按知识点组织，建议从输出反推原理：

| # | 演示 | 关键知识点 |
| --- | --- | --- |
| 1 | 规则引擎 | 接口多态、组合、原型注册、审计、`dynamic_cast` 边界 |
| 2 | RTTI | `typeid`、`dynamic_cast` 向下/交叉转换、`dynamic_cast<void*>`、`bad_cast`、`bad_typeid` |
| 3 | 默认参数 | 虚函数默认参数按静态类型绑定 |
| 4 | 构造/析构中的虚调用 | vptr 在生命周期中如何切换 |
| 5 | 对象切片 | 按值传递丢失动态类型 |
| 6 | 重载隐藏 | 派生类同名函数隐藏基类重载，`using` 恢复 |
| 7 | 虚析构/纯虚析构 | 通过基类指针安全删除、纯虚析构必须有定义 |

### 3.1 规则引擎的决策表

Demo 的默认参数：

- `MaxAmountRule` 限额 5000；
- `CountryBlockRule` 黑名单 `KP`、`IR`；
- `VelocityRule`：1 分钟 ≥ 20 次 → REVIEW；≥ 50 次且金额 ≥ 1000 → REJECT。

| 输入 | 预期决策 | 触发点 |
| --- | --- | --- |
| `SG, 120, 2` | ALLOW | 全部通过 |
| `SG, 8000, 2` | REJECT | 金额超限 |
| `IR, 120, 2` | REJECT | 黑名单国家 |
| `US, 2000, 60` | REJECT | 高频 + 高金额 |
| `US, 500, 25` | REVIEW | 高频但金额不高 |

组合规则取“最严决策”：任意子规则 REJECT → REJECT；否则任意 REVIEW → REVIEW；否则 ALLOW。评分累加，便于后续排序。

### 3.2 RTTI 演示

重点看：

```cpp
risk::IAuditable* auditable = dynamic_cast<risk::IAuditable*>(base);
```

`base` 是 `IRule*`，`IAuditable` 是同对象的另一个基类；这是一次 **cross-cast**。`CountryBlockRule` 没有实现 `IAuditable`，所以同样的转换返回 `nullptr`。

```cpp
void* most = dynamic_cast<void*>(base);
```

多继承下，`static_cast<void*>(base)` 可能只得到某个子对象地址；`dynamic_cast<void*>` 回到最派生对象开头。

### 3.3 ABI 演示

`src/abi_probe.cpp` 会打印：

- `sizeof`、各基类子对象偏移；
- 主 vtable / 次 vtable 的 `vptr`；
- `offset_to_top`；
- `vtable[-1]` 是否为 `typeid`；
- 虚继承下共享 `VBase` 的地址；
- `dynamic_cast<void*>` 的对象身份。

它直接读取 `vptr`，属于 **Itanium ABI 教学探针**，不是可移植的生产代码。详细解释见 [`docs/abi-deep-dive.md`](docs/abi-deep-dive.md)。

---

## 4. 原理主线：一张图看懂动态多态

### 4.1 静态类型 vs 动态类型

```cpp
risk::IRule* rule = new risk::MaxAmountRule(5000.0);

rule->evaluate(ctx);
// 静态类型：IRule*
// 动态类型：MaxAmountRule
```

- 编译器按 `IRule` 找到 `evaluate` 的槽位；
- 运行时根据对象的 vptr 找到 `MaxAmountRule` 的 vtable；
- 从对应槽位取出函数地址并调用。

### 4.2 vtable 与 vptr

```text
对象（MaxAmountRule）                  类共享的 vtable
+----------------------+              +-----------------------------+
| vptr  ---------------+------------> | &MaxAmountRule::~D1         | slot 0
| limit_ = 5000        |              | &MaxAmountRule::~D0         | slot 1
| ...                  |              | &MaxAmountRule::evaluate    | slot 2
+----------------------+              | ...                         |
                                      | type_info* (在 vptr[-1])    |
                                      | offset_to_top (vptr[-2])    |
                                      +-----------------------------+
```

- **vtable 是类级别的**：同类的所有对象共享；
- **vptr 是对象级别的**：每个多态对象（更准确地说，每个需要独立 vtable 的基类子对象）有一个；
- 对象里通常只多一个隐藏指针，不是一整张函数表；
- 多继承时可能有多个 vptr（主/次 vtable）。

### 4.3 一次虚调用的三步

```text
1. load vptr from object
2. load function address from vtable[fixed_slot]
3. indirect call with this
```

这解释了：

- 为什么虚函数通常不能内联；
- 为什么多继承会有 thunk（先调整 `this` 再跳）；
- 为什么 `final` + LTO/PGO 可能去虚化；
- 为什么读 vtable 是 ABI 相关、不可移植。

### 4.4 C++ 标准没有规定 vtable

标准只规定“通过基类指针/引用调用虚函数，应执行动态类型的最终覆盖者”。vtable/vptr 是实现技术。

- Linux GCC/Clang：Itanium C++ ABI；
- MSVC：不同的 vftable 布局，虚继承还会用 `vtordisp`；
- 所以 demo 的 `abi_probe` 明确限定 GCC/Clang + Linux。

---

## 5. 代码解析：接口与多态

### 5.1 `IRule`：稳定接口

```cpp
class IRule {
public:
    virtual ~IRule() = default;
    virtual const char* id() const = 0;
    virtual Result evaluate(const Context& ctx) const = 0;
    virtual IRule* clone() const = 0;
    virtual void accept(RuleVisitor& visitor) const = 0;
};
```

四个设计点：

1. **虚析构**：允许 `delete IRule*` / `unique_ptr<IRule>` 正确调用派生析构；
2. **纯虚函数**：`IRule` 是抽象类，不能实例化；
3. **`const` 成员函数**：决策不修改规则状态；
4. **`clone` + `accept`**：分别支持原型复制和 Visitor 双分派。

### 5.2 `RuleBase`：NVI

```cpp
class RuleBase : public IRule {
public:
    Result evaluate(const Context& ctx) const final;

private:
    virtual Result doEvaluate(const Context& ctx) const = 0;
};
```

- `evaluate` 是 public + final 的“外壳”；
- `doEvaluate` 是 private 的定制点；
- 派生类不能覆盖 `evaluate`，只能实现 `doEvaluate`；
- 公共逻辑（校验、日志、指标、结果规范化）可以集中在 `evaluate`；
- `private` 不影响虚分派：访问控制是编译期规则，动态分派是运行期机制。

这是**模板方法模式**的 C++ 多态版本，也是业务代码中最值得掌握的多态技巧之一。

### 5.3 具体规则

```cpp
class MaxAmountRule final : public RuleBase, public IAuditable {
public:
    const char* id() const override;
    MaxAmountRule* clone() const override;      // 协变返回
    void accept(RuleVisitor&) const override;
    std::string audit() const override;

private:
    Result doEvaluate(const Context& ctx) const override;
    double limit_;
};
```

`MaxAmountRule` 同时实现：

- `IRule`（通过 `RuleBase`）：主接口；
- `IAuditable`：独立能力接口。

因此它有两组 vtable 入口，`IRule*` 和 `IAuditable*` 之间需要指针调整。这不是“为了演示而演示”：真实业务中“主接口 + 可审计/可序列化/可观测能力”非常常见。

### 5.4 协变返回与 `clone`

基类：

```cpp
virtual IRule* clone() const = 0;
```

派生类：

```cpp
MaxAmountRule* clone() const override;
```

返回类型从 `IRule*` 协变为 `MaxAmountRule*`，调用方可以拿到更具体的类型，同时仍然满足基类契约。机器层面，如果基类子对象不在偏移 0，编译器可能生成返回指针调整 thunk。

`clone` 返回裸指针是经典原型写法，调用方负责释放；demo 提供：

```cpp
inline std::unique_ptr<IRule> clone_rule(const IRule& rule);
```

把所有权收进 `unique_ptr`。生产中也可以设计 `virtual std::unique_ptr<IRule> cloneUnique() const`，但那就没有协变返回的演示了。

### 5.5 组合模式

```cpp
class CompositeRule final : public RuleBase, public IAuditable {
public:
    void add(std::unique_ptr<IRule> rule);
    const std::vector<std::unique_ptr<IRule>>& children() const;
private:
    Result doEvaluate(const Context& ctx) const override;
};
```

`CompositeRule` 本身也是 `IRule`，所以引擎可以把它当普通规则；它内部递归调用子规则的 `evaluate`。这体现了“组合优于继承”的另一种形态：**对象树 + 统一接口**。

`clone()` 必须深拷贝：

```cpp
CompositeRule* copy = new CompositeRule(id_);
for (const auto& child : children_) {
    copy->add(clone_rule(*child));
}
```

如果只复制 `unique_ptr`，代码根本编译不过（`unique_ptr` 不可拷贝），这相当于编译器帮你阻止浅拷贝。

### 5.6 原型注册表

```cpp
Registrar<MaxAmountRule, double> g_max_amount("max_amount", 5000.0);
```

`Registrar` 在静态初始化阶段把原型放进 `RuleRegistry`。`create()` 返回 `prototype->clone()`，所以：

- 原型集中配置；
- 每个引擎/请求拿到独立对象；
- 新增规则只需注册，不改工厂 `switch`；
- 运行期注册表只读，避免并发修改。

### 5.7 Visitor 双分派

```cpp
class IRule {
public:
    virtual void accept(RuleVisitor& visitor) const = 0;
};

void MaxAmountRule::accept(RuleVisitor& visitor) const {
    visitor.visit(*this);   // *this 静态类型是 MaxAmountRule
}
```

- `accept` 虚分派：根据规则动态类型进入具体类；
- `visitor.visit(*this)` 重载决议：根据 `*this` 的静态类型选择 `visit(const MaxAmountRule&)`；
- `visit` 本身也是虚函数：根据访问者动态类型选择实现。

两跳合起来完成“规则类型 × 操作类型”的双分派。它把“导出/校验/优化”等操作从规则类中分离，符合开闭原则的“对扩展开放”。

---

## 6. 代码解析：生命周期与陷阱

### 6.1 虚析构

```cpp
class IRule {
public:
    virtual ~IRule() = default;
};
```

没有虚析构时：

```cpp
IRule* p = new MaxAmountRule();
delete p;   // 未定义行为：只调用 IRule 的析构
```

有虚析构时，`delete p` 通过 vtable 的删除析构槽（D0）调到最派生类析构，再逐层析构基类。

`unique_ptr<IRule>` 的默认删除器是 `default_delete<IRule>`，同样依赖虚析构。

`shared_ptr` 有一个容易误解的点：

```cpp
std::shared_ptr<IRule> p(new MaxAmountRule());  // 控制块按 MaxAmountRule* 删除，通常正确
IRule* raw = new MaxAmountRule();
std::shared_ptr<IRule> q(raw);                    // 控制块按 IRule* 删除，非虚析构 => UB
```

不要依赖这种“碰巧正确”；多态基类一律虚析构。

### 6.2 纯虚析构

```cpp
struct PureDtorBase {
    virtual ~PureDtorBase() = 0;
};
PureDtorBase::~PureDtorBase() {}
```

纯虚析构让类成为抽象类，但**仍然必须提供定义**：派生类析构时一定会调用基类析构，链接器需要这个函数体。

### 6.3 构造/析构中的虚调用

Demo 输出：

```text
constructing:
  Base ctor -> Base
  Derived ctor -> Derived
destroying:
  Derived dtor -> Derived
  Base dtor -> Base
```

原因：

- 构造 `Derived` 时先构造 `Base`，此时 vptr 指向 `Base` vtable；
- `Base` 构造完成后，进入 `Derived` 构造，vptr 切换到 `Derived` vtable；
- 析构顺序相反，vptr 逐层切回。

如果在基类构造/析构中调用纯虚函数，通常落到 `__cxa_pure_virtual`，程序终止；标准将这种调用视为未定义行为。不要在基类构造/析构里调用“指望派生类实现”的虚函数。

### 6.4 对象切片

```cpp
void consume(SliceBase value);   // 按值传递
SliceDerived d;
consume(d);                      // 切片：只复制 SliceBase 子对象
```

切片后：

- 动态类型信息（vptr）变成 `SliceBase`；
- 派生类数据成员丢失；
- 虚调用调到 `SliceBase::label()`。

避免方式：按引用/指针传递，或使用 `unique_ptr<Base>` / `clone()`。

### 6.5 默认参数

```cpp
struct ArgBase {
    virtual int adjust(int x = 10) const;
};
struct ArgDerived : ArgBase {
    int adjust(int x = 20) const override;
};

ArgBase& ref = derived;
ref.adjust();   // 默认参数取 ArgBase 的 10，函数体取 ArgDerived，结果 20
```

默认参数在调用点按**静态类型**替换，虚函数只决定“调用哪个函数体”。因此：

- 不要把关键默认值放在虚函数上；
- 用 NVI 在非虚外壳中给默认值，或使用重载；
- 默认参数与虚函数组合是常见面试题，也是实际 bug 来源。

### 6.6 重载隐藏

```cpp
struct HideBase {
    virtual void handle(int) const;
};
struct HideDerived : HideBase {
    void handle(const std::string&) const;   // 隐藏 handle(int)
};
```

派生类声明同名函数后，基类的所有同名重载在**名字查找**阶段被隐藏，除非：

```cpp
using HideBase::handle;
```

`override` 不会解决这个问题；它只检查“是否真的覆盖了某个虚函数”。`compile_fail/hiding.cpp` 展示了不写 `using` 的编译错误。

### 6.7 `override` 与 `final`

- `override`：编译期检查签名是否真的覆盖基类虚函数；签名不匹配立即报错。
- `final`：
  - 修饰类：不能被继承；
  - 修饰虚函数：不能被派生类覆盖；
  - 可能帮助编译器去虚化，但不是性能保证。

Demo 中：

```cpp
class MaxAmountRule final : public RuleBase, public IAuditable;
Result evaluate(const Context&) const final;
```

`final` 既是设计约束（这些叶子规则不允许再派生），也是优化提示。

### 6.8 访问控制与虚函数

`RuleBase::doEvaluate` 是 private，但派生类可以 override 它；`evaluate` 通过虚分派调用到派生实现。这说明：

- 访问控制在编译期检查调用表达式；
- 虚分派在运行期选择函数体；
- `private virtual` 是 NVI 的常用组合，不是语法怪癖。

---

## 7. RTTI 核心用法

### 7.1 `typeid`

```cpp
typeid(*base)     // 多态左值：动态类型
typeid(base)      // 指针：静态类型 IRule*
typeid(IRule)     // 类型本身
```

- `name()` 是实现定义的 mangled 字符串；
- `hash_code()` 可用于进程内哈希，但不保证跨进程/跨版本稳定；
- `std::type_index` 可以把 `type_info` 放进 `map`/`unordered_map`；
- 对多态空指针解引用取 `typeid` 会抛 `std::bad_typeid`。

### 7.2 `dynamic_cast` 四种形态

```cpp
// 向下转型，失败返回 nullptr
MaxAmountRule* p = dynamic_cast<MaxAmountRule*>(base);

// 交叉转型，失败返回 nullptr
IAuditable* a = dynamic_cast<IAuditable*>(base);

// 最派生对象身份
void* most = dynamic_cast<void*>(base);

// 引用版，失败抛 std::bad_cast
IAuditable& ref = dynamic_cast<IAuditable&>(*base);
```

- 向上转换（`Derived* -> Base*`）不需要 RTTI，编译期即可完成；向下/交叉转换以及 `dynamic_cast<void*>` 要求源对象是多态类型；
- 指针版失败返回 `nullptr`，引用版失败抛 `bad_cast`；
- `dynamic_cast<void*>` 在多继承/虚继承下特别有用；
- 跨 DSO 使用 RTTI 要小心符号可见性和 `type_info` 合并。

### 7.3 RTTI 与 Visitor 的取舍

| 场景 | 推荐 |
| --- | --- |
| 偶尔判断具体类型 | `dynamic_cast` / `typeid` |
| 核心决策路径 | 虚函数，不用 RTTI |
| 多种操作、类型相对稳定 | Visitor |
| 类型集合封闭、性能敏感 | 标签 + `switch` / `std::variant`（C++17） |
| 插件边界 | 稳定 ID/版本化接口，不依赖 RTTI |

---

## 8. 多继承与虚继承（速览）

完整机器级解释见 [`docs/abi-deep-dive.md`](docs/abi-deep-dive.md)。

### 8.1 多继承

```cpp
class MaxAmountRule : public RuleBase, public IAuditable;
```

- `IRule` 主基类、`IAuditable` 次基类；
- `IRule*` 与 `IAuditable*` 通常相差 8 字节（demo 打印实际值）；
- `IRule* -> IAuditable*` 用 `dynamic_cast` 做 cross-cast；
- 次 vtable 中的函数可能是 thunk：先调整 `this`，再跳真实函数；
- `dynamic_cast<void*>` 回到对象开头；
- 通过次基类 `delete` 时，删除析构 thunk 负责调整地址。

### 8.2 虚继承

```cpp
struct VLeft  : virtual VBase;
struct VRight : virtual VBase;
struct Diamond : VLeft, VRight;
```

- `Diamond` 只有一份共享 `VBase`；
- `VLeft* -> VBase*` 的偏移不是编译期常量，要从 vtable 读 vbase offset；
- `static_cast<Diamond*>(VBase*)` 编译失败，必须 `dynamic_cast`；
- 对象更大、构造更复杂，虚继承不是“免费的去重”。

### 8.3 `vtable[-2]` / `vtable[-1]`

Itanium ABI 中：

```text
vptr[-2] = offset_to_top
vptr[-1] = type_info*
vptr[0]  = 完整对象析构 D1
vptr[1]  = 删除析构 D0
vptr[2]  = 第一个虚函数
...
```

`offset_to_top` 用于从子对象找回最派生对象；次 vtable 中通常为负数。`type_info*` 是 `typeid`/`dynamic_cast` 的入口。

---

## 9. 性能与 ABI：架构师视角

### 9.1 虚调用成本

- 取 vptr、取槽、间接调用；
- 阻止内联；
- 间接分支预测压力；
- 多继承 thunk/指针调整；
- 对象多 vptr（多继承）。

现代 CPU 对稳定类型的虚调用预测很好，不要凭“虚函数一定慢”做过度设计。但在每秒百万/千万次的超热路径，应该测量。

### 9.2 去虚化

可能帮助去虚化的手段：

- `final` 类/函数；
- LTO；
- PGO；
- 调用点静态类型已知；
- 把热路径内层改成模板/CRTP/`variant`。

`final` 不是保证；用 `-O2 -S`、`-fdump-tree-devirt` 和 benchmark 验证。

### 9.3 ABI 稳定性

插件架构必须遵守：

1. 新虚函数追加到末尾；
2. 不删除/重排已有虚函数；
3. 不修改已有签名；
4. 不改变基类顺序；
5. 跨编译器边界用 C ABI/不透明句柄；
6. 不跨 DSO 随意传 STL/异常；
7. 加版本号/能力查询，别靠 `dynamic_cast` 猜版本。

一个在中间插入虚函数的“小改动”，会让旧插件的所有后续槽位错位，直接调用到错误函数。

---

## 10. 知识点覆盖表

> 说明：覆盖业务常用与 ABI 认知的绝大多数核心点。未覆盖的极端细节：虚函数 `volatile` 限定、多重虚继承的完整 VTT 构造序列、MSVC `vtordisp` 实测、跨 DSO RTTI 合并实验、C++20 协程/概念等。

| # | 知识点 | Demo 位置 | 观察点 |
| --- | --- | --- | --- |
| 1 | 虚函数声明/调用 | `IRule`、所有规则 | 通过 `IRule*` 调用动态实现 |
| 2 | 纯虚函数 | `IRule`、`RuleBase::doEvaluate` | 抽象类不能实例化 |
| 3 | 抽象类 | `IRule`、`RuleBase` | `std::is_abstract` 静态断言 |
| 4 | 虚析构 | `IRule::~IRule` | `unique_ptr<IRule>` 安全删除 |
| 5 | 纯虚析构 | `PureDtorBase` | 纯虚但必须有定义 |
| 6 | `override` | 所有具体规则 | 签名错误编译失败 |
| 7 | `final` 类 | `MaxAmountRule`、`CompositeRule` | 不能继续派生 |
| 8 | `final` 方法 | `RuleBase::evaluate` | 派生类不能覆盖 |
| 9 | 协变返回 | `MaxAmountRule::clone` | 返回更具体的指针 |
| 10 | NVI / private virtual | `RuleBase` | 外壳 final，定制点 private |
| 11 | 访问控制与虚分派 | `doEvaluate` | private 也能 override |
| 12 | 组合模式 | `CompositeRule` | 递归虚调用、深拷贝 |
| 13 | 原型模式 | `RuleRegistry` | `clone` 生产独立实例 |
| 14 | 插件注册 | `Registrar` | 静态初始化、函数内静态单例 |
| 15 | Visitor 双分派 | `RuleVisitor`、`RuleReporter` | `accept` + `visit` |
| 16 | `typeid` 动态类型 | `run_rtti_demo` | 对象 vs 指针 |
| 17 | `type_index` | `run_rtti_demo` | `hash_code` |
| 18 | `dynamic_cast` 向下 | `run_rtti_demo` | 失败返回 `nullptr` |
| 19 | `dynamic_cast` 交叉 | `IRule* -> IAuditable*` | cross-cast |
| 20 | `dynamic_cast<void*>` | `run_rtti_demo` | 最派生对象身份 |
| 21 | 引用版 `bad_cast` | `run_rtti_demo` | 失败抛异常 |
| 22 | 空指针 `bad_typeid` | `run_rtti_demo` | `typeid(*nullptr)` |
| 23 | 多继承布局 | `abi_probe.cpp` | 主/次 vptr、偏移 |
| 24 | thunk / this 调整 | `abi_probe.cpp` | 次 vtable 槽 |
| 25 | `offset_to_top` | `abi_probe.cpp` | `vptr[-2]` |
| 26 | RTTI 指针位置 | `abi_probe.cpp` | `vptr[-1] == &typeid` |
| 27 | 虚继承菱形 | `abi_probe.cpp` | 共享 `VBase` |
| 28 | vbase offset | `abi_probe.cpp` | 左/右路转换同址 |
| 29 | 虚基类向下转型 | `compile_fail/virtual_base_downcast.cpp` | `static_cast` 编译失败 |
| 30 | 构造/析构 vptr 切换 | `run_ctor_dtor_demo` | Base/Derived 输出 |
| 31 | 对象切片 | `run_slicing_demo` | 按值传参 |
| 32 | 默认参数静态绑定 | `run_default_arg_demo` | 10/20 的差异 |
| 33 | 重载隐藏 | `run_hiding_demo` | `using` 恢复 |
| 34 | 删除基类指针 | `run_destructor_demo` | D0/D1 概念 |
| 35 | `shared_ptr` 所有权细节 | `docs/abi-deep-dive.md` | 先转基类指针的坑 |
| 36 | 去虚化 | 文档 + `final` | `-O2`/LTO/PGO |
| 37 | ABI 版本兼容 | `docs/abi-deep-dive.md` | 新虚函数追加末尾 |
| 38 | `-fno-rtti` | 文档 | 虚函数仍可用，RTTI 不可用 |
| 39 | `noexcept` 与虚函数 | 文档 | C++17 起进入函数类型 |
| 40 | 编译期契约测试 | `compile_fail/` | 6 个反例 |

---

## 11. 常见问题（FAQ）

### Q1：vtable 是每个对象一份吗？

不是。vtable 是类共享的；对象里是 vptr。多继承时一个对象可能有多个 vptr（指向不同 vtable 组），但表本身仍是类级别的。

### Q2：虚函数一定慢吗？

有一次间接调用和阻止内联的成本，但现代 CPU 预测很好；`final`、LTO、PGO、静态类型已知都可能去虚化。要测量，不要教条。

### Q3：构造函数可以是虚函数吗？

不可以。构造时对象动态类型尚未建立。析构函数可以是、而且多态基类应该是虚函数。

### Q4：为什么基类析构必须虚？

`delete Base*` 指向 `Derived` 时，非虚析构只调用 `Base::~Base`，派生类资源不释放，行为未定义。`unique_ptr<Base>` 同理。

### Q5：`shared_ptr<Base>` 能救非虚析构吗？

只有在构造时保留 `Derived*` 静态类型（`shared_ptr<Base> p(new Derived)`）时，控制块才按 `Derived*` 删除；如果先转成 `Base*` 再交给 `shared_ptr`，仍然 UB。不要依赖这个细节。

### Q6：纯虚函数可以有实现吗？

可以。纯虚只表示“派生类必须覆盖”，不禁止基类提供函数体。纯虚析构函数**必须**有定义。

### Q7：`override` 是关键字吗？

它是上下文关键字（contextual keyword），只在特定位置有特殊含义；不需要把它当保留字。它提供编译期检查，没有运行期开销。

### Q8：`final` 一定能让虚函数变快吗？

不一定。它给编译器“没有更派生 override”的信息，可能帮助去虚化；是否优化取决于调用点、优化级别、LTO/PGO。

### Q9：虚函数默认参数会动态绑定吗？

不会。默认参数在调用点按静态类型替换；虚分派只决定函数体。

### Q10：派生类同名函数会自动重载基类吗？

不会。派生类声明同名函数会隐藏基类所有同名重载；需要 `using Base::name;`。

### Q11：`dynamic_cast` 失败会怎样？

指针版返回 `nullptr`；引用版抛 `std::bad_cast`。向上转换不要求源多态；向下/交叉转换要求源对象是多态类型。

### Q12：`typeid(*p)` 和 `typeid(p)` 有什么区别？

`typeid(*p)` 对多态左值取动态类型；`typeid(p)` 取指针的静态类型。`typeid(*nullptr)` 对多态指针抛 `bad_typeid`。

### Q13：`dynamic_cast<void*>` 有什么用？

返回最派生对象地址。多继承下 `static_cast<void*>(base)` 可能只是子对象地址；`dynamic_cast<void*>` 用于对象身份/缓存 key/日志关联。

### Q14：虚继承为什么更贵？

共享虚基类需要运行期偏移（vbase offset）、更复杂的构造（VTT/construction vtable）、更大的对象。它不是简单的“去重语法”。

### Q15：可以直接读 vtable 吗？

可以用于教学/调试，但这是 ABI 细节，不可移植。生产代码不要依赖 `vptr` 偏移或槽位顺序。

### Q16：`-fno-rtti` 会怎样？

`typeid`、`dynamic_cast` 不可用（编译失败），但虚函数、虚析构、多态调用仍然工作。

### Q17：虚函数与 `noexcept` 有什么关系？

C++11 的异常规格不是函数类型的一部分；C++17 起 `noexcept` 进入函数类型，override 的异常规格不兼容会编译失败。跨标准升级时要检查。

### Q18：为什么新增虚函数要加在末尾？

vtable 槽位按声明顺序排列；在中间插入会让后续槽位索引整体后移，旧插件的调用会跳到错误函数，破坏二进制兼容。

### Q19：什么时候不该用虚函数？

类型集合封闭、超热路径、需要值语义、跨 ABI 边界时，考虑模板/CRTP、`std::variant`（C++17）、标签枚举、C ABI。虚函数不是“面向对象”的唯一答案。

### Q20：为什么 demo 用 `clone()` 返回裸指针？

这是经典原型写法，便于展示协变返回。生产中建议用 `unique_ptr` 包装（demo 的 `clone_rule`）或设计 `cloneUnique()`，避免所有权泄漏。

---

## 12. 编译期失败实验

这些文件故意编译失败，用来证明约束不是“靠自觉”：

```bash
cd 01_virtual_polymorphism

for f in compile_fail/*.cpp; do
    echo "== $f"
    g++ -std=c++11 -Iinclude -c "$f" -o /dev/null 2>&1 | head -n 4
done
```

| 文件 | 预期错误 |
| --- | --- |
| `override_typo.cpp` | `marked 'override', but does not override` |
| `final_class.cpp` | `cannot derive from 'final' base` |
| `final_method.cpp` | `overriding final function` |
| `bad_covariant.cpp` | `invalid covariant return type` |
| `virtual_base_downcast.cpp` | `cannot convert ... because base is virtual` |
| `hiding.cpp` | `no matching function for call to ... handle(int)` |

详细说明见 [`compile_fail/README.md`](compile_fail/README.md)。

---

## 13. 建议的阅读/实验顺序

1. 运行 demo，先看输出，不急着读代码；
2. 读 `include/risk/risk.hpp`，画出 `IRule` / `RuleBase` / 具体规则的关系；
3. 读 `src/risk.cpp`，重点看 `clone`、组合递归、注册表；
4. 读 `src/main.cpp` 的 7 组演示，理解每个陷阱；
5. 读 `src/abi_probe.cpp`，对照 `docs/abi-deep-dive.md` 看 vtable 原始布局；
6. 跑 `compile_fail`，确认编译器如何阻止错误；
7. 做 `docs/business-design.md` 末尾的练习题。

---

## 14. 延伸实验

### 14.1 看类布局

```bash
g++ -std=c++11 -c -Iinclude -fdump-lang-class src/abi_probe.cpp -o /tmp/abi.o
```

GCC 8+ 会生成 `.class` 文件，能看到 vtable 数量、槽位、虚基类偏移。不同版本参数名可能不同。

### 14.2 看虚调用汇编

```bash
g++ -std=c++11 -O2 -Iinclude -S src/risk.cpp -o /tmp/risk.s
grep -n "call.*\*" /tmp/risk.s
```

### 14.3 看去虚化

```bash
g++ -std=c++11 -O2 -Iinclude -fdump-tree-devirt src/main.cpp -c -o /tmp/main.o
```

### 14.4 关掉 RTTI

```bash
g++ -std=c++11 -fno-rtti -Iinclude src/risk.cpp -c -o /tmp/risk.o
```

`RuleEngine::run` 里的审计 cross-cast 使用了 `dynamic_cast`，因此这个命令会失败。这恰好说明：**虚函数多态本身不依赖 RTTI，但 `dynamic_cast`/`typeid` 依赖 RTTI**。

如果把审计改成 Visitor 或显式的“可审计”能力查询，就可以在 `-fno-rtti` 下保留虚函数多态，只放弃运行期类型识别。生产中使用 `-fno-rtti` 前，要确认代码和依赖库都不再使用 `typeid`/`dynamic_cast`。

---

## 15. 参考资料

- [cppreference: virtual function](https://en.cppreference.com/w/cpp/language/virtual)
- [cppreference: dynamic_cast](https://en.cppreference.com/w/cpp/language/dynamic_cast)
- [cppreference: type_info](https://en.cppreference.com/w/cpp/types/type_info)
- [Itanium C++ ABI](https://itanium-cxx-abi.github.io/cxx-abi/abi.html)
- [GCC Developer Options（-fdump-lang-class 等）](https://gcc.gnu.org/onlinedocs/gcc/Developer-Options.html)
- [Agner Fog: Optimizing software in C++](https://www.agner.org/optimize/)
- Stanley Lippman, *Inside the C++ Object Model*（对象模型经典）
- ISO/IEC 14882（C++11 及后续标准）

---

## 16. 一句话总结

**多态的核心不是 `virtual` 关键字，而是“用稳定接口隔离变化”；vtable 只是这个契约在 Itanium ABI 下的实现方式。会用接口、会设计所有权、会处理 RTTI 边界、会保护 ABI，才是业务中的高级用法。**
