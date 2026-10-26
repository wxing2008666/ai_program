# Itanium C++ ABI 深度解析：vtable、vptr、thunk 与 RTTI

> 本文解释 `src/abi_probe.cpp` 打印出来的东西，以及 `src/risk.cpp` 里多继承、RTTI、协变返回背后的机器级机制。
>
> 先记住一句话：**C++ 标准规定“多态的行为”，但没有规定“用 vtable 实现”。** Linux 上的 GCC/Clang 采用 Itanium C++ ABI，所以本文的布局在 Linux 上成立；MSVC 的布局不同，标准也没有承诺任何偏移。

---

## 1. 为什么需要 vtable

考虑调用：

```cpp
risk::IRule* rule = ...;       // 静态类型 IRule*
risk::Result r = rule->evaluate(ctx);
```

编译 `evaluate` 调用点时，编译器只知道：

- `rule` 指向某个 `IRule` 子对象；
- `IRule::evaluate` 在接口中的“槽位”；
- 具体是 `MaxAmountRule` 还是 `CountryBlockRule`，要运行到这一行才知道。

如果每个对象都保存一份“函数地址数组”，对象会很大；如果调用点做 `if/else` 判断类型，每加一个规则都要改调用点，违反开闭原则。

工业实现采用两层结构：

```text
每个“类”共享一张函数地址表        每个“对象”保存一个指向表的指针
+--------------------------+       +----------------------+
| MaxAmountRule 的 vtable  | <---- | vptr                 |
| CountryBlock 的 vtable   |       | limit_ = 5000        |
| ...                      |       +----------------------+
+--------------------------+
```

- **vtable（虚函数表）**：类级别，编译/链接期生成，通常只读。
- **vptr（虚表指针）**：对象级别，隐藏在对象布局中，构造时由编译器写入。
- 同一个类的多个对象共享同一张 vtable，但每个对象有自己的 vptr。

一个直观类比：同一型号的机器共用一本说明书（vtable），每台机器身上贴一个“说明书入口”的标签（vptr）。换型号时换标签，不需要把整本说明书塞进机器。

---

## 2. 一个多态对象长什么样

### 2.1 没有虚函数

```cpp
struct Plain {
    int x;
    double y;
};
```

布局通常只有 `x`、`y`（可能有填充），没有 vptr。

### 2.2 有虚函数

```cpp
struct Base {
    virtual ~Base() {}
    virtual int f() const;
    int x;
};
```

Itanium ABI 下，典型布局是：

```text
偏移 0    vptr ──────────────► Base 的 vtable
偏移 8    x
```

vptr 通常是对象的第一个机器字。**但这不是 C++ 标准保证的**：标准只要求通过基类指针调用虚函数能得到正确行为。不要把“vptr 在偏移 0”写进跨平台业务代码。

---

## 3. vtable 的结构

Itanium ABI 中，vptr 指向的不是“表头”，而是**地址点（address point）**，也就是第一个虚函数槽。

```text
                    更负的偏移（如果有虚基类/虚调用）
                  ┌──────────────────────────────┐
                  │ vbase offset / vcall offset   │  vtable[-3], ...
                  ├──────────────────────────────┤
                  │ offset_to_top                 │  vtable[-2]
                  ├──────────────────────────────┤
                  │ type_info*                    │  vtable[-1]
                  ├──────────────────────────────┤  <── vptr 指向这里
                  │ 完整对象析构函数 D1            │  vtable[0]
                  ├──────────────────────────────┤
                  │ 删除析构函数 D0                │  vtable[1]
                  ├──────────────────────────────┤
                  │ 第 1 个虚函数                  │  vtable[2]
                  ├──────────────────────────────┤
                  │ 第 2 个虚函数                  │  vtable[3]
                  └──────────────────────────────┘
```

三个关键点：

1. **`vtable[-1]` 是 `std::type_info*`**，`typeid` 和多态对象的 RTTI 靠它。
2. **`vtable[-2]` 是 `offset_to_top`**，表示“当前子对象指针 + 这个偏移 = 最派生对象指针”。主 vtable 通常是 0，次 vtable 通常是负数。
3. **析构函数占两个槽**：
   - 完整对象析构函数（complete object destructor，Itanium 记作 D1）：只析构，不释放内存。
   - 删除析构函数（deleting destructor，D0）：先析构，再调用 `operator delete`。
   - `delete basePtr` 实际调用的是 D0；D0 再调用 D1，并释放内存。

> 上面的 `slot 0/1` 假设类有虚析构函数。若类没有虚析构，slot 0 直接是第一个虚函数；本 demo 的多态基类都有虚析构，所以按 D1/D0 排列。

纯虚函数在抽象类的 vtable 里通常填一个 `__cxa_pure_virtual` 之类的占位函数；如果运行期真的调到它（例如在构造函数里调用纯虚函数），程序通常会终止。

### 3.1 槽位顺序由声明顺序决定

基类声明：

```cpp
class Base {
public:
    virtual ~Base();
    virtual int f() const;
    virtual int g() const;
};
```

派生类：

```cpp
class Derived : public Base {
public:
    ~Derived() override;
    int f() const override;
    virtual int h() const;   // 新增虚函数
};
```

Itanium 下，`Derived` 的 vtable 通常保持基类槽位顺序：

```text
slot 0: Derived 完整对象析构
slot 1: Derived 删除析构
slot 2: Derived::f        // 覆盖 Base::f
slot 3: Base::g           // 未覆盖，沿用基类实现
slot 4: Derived::h        // 新虚函数追加到末尾
```

这也解释了 ABI 兼容规则：**在已有虚函数中间插入新函数，会改变后面所有槽位的索引**，旧插件调用就会跳错函数。新增虚函数应追加到末尾（详见第 12 节）。

---

## 4. 一次虚调用发生了什么

源码：

```cpp
rule->evaluate(ctx);
```

伪汇编（x86-64 System V，示意）：

```asm
mov  (%rdi), %rax      ; rdi = rule，取对象第一个字：vptr
mov  16(%rax), %rax    ; 取 vtable 中固定槽位的函数地址
call *%rax             ; 间接调用；rdi 仍是 this
```

三个动作：

1. 从对象取 vptr；
2. 从 vtable 的固定偏移取函数地址；
3. 间接调用。

因此一次虚调用通常比直接调用多一次（或两次）内存访问和一个间接分支。现代 CPU 的分支预测器对“同一调用点总是同一类型”很擅长，所以不要凭感觉认为虚函数一定慢；但在超热路径、类型频繁变化、或无法内联时，成本是真实的。

`this` 指针在调用前可能需要调整：

- 单继承、主基类：通常不需要；
- 多继承、次基类：vtable 槽里放的是 **thunk**，先调整 `this`，再跳到真实函数；
- 虚继承：可能还要通过 vtable 中的 vbase offset 计算虚基类地址。

---

## 5. 多继承：主 vtable、次 vtable 与 thunk

Demo 中的类：

```cpp
struct MA {
    virtual ~MA() {}
    virtual const char* name() const;
    int a;
};

struct MB {
    virtual ~MB() {}
    virtual const char* name() const;
    int b;
};

struct MC : MA, MB {
    ~MC() override {}
    const char* name() const override;
    int c;
};
```

### 5.1 对象布局

x86-64 GCC 的典型布局：

```text
MC 对象
0    MA 子对象
     +0   vptr  ──────► MC 的主 vtable
     +8   a
16   MB 子对象
     +0   vptr  ──────► MC 的次 vtable
     +8   b
32   c
```

所以：

```cpp
MC mc;
MA* ma = &mc;   // 偏移 0
MB* mb = &mc;   // 偏移 16（典型值）
```

`MC* -> MB*` 的加法是编译期确定的常量偏移；反过来 `MB* -> MC*` 需要减 16。`static_cast` 会做这个调整但不检查真实类型，`dynamic_cast` 会先检查再调整。

### 5.2 两张 vtable

`MC` 有两组 vtable 入口：

- **主 vtable**：给 `MA` 子对象用，槽位对应 `MA` 的虚函数；覆盖后的 `name` 直接指向 `MC::name`。
- **次 vtable**：给 `MB` 子对象用，槽位对应 `MB` 的虚函数；里面的函数通常不是 `MC::name` 本体，而是 **thunk**：

```text
MB 的 vtable 槽位
  -> thunk: sub $16, %rdi   ; 把 MB* 调回 MC*
            jmp MC::name
```

这样 `mb->name()` 时：

1. `mb` 指向 `MC` 对象 + 16；
2. 取次 vtable；
3. 调 thunk；
4. thunk 把 `this` 减 16，变成 `MC*`；
5. 跳到 `MC::name`。

`offset_to_top` 在次 vtable 中通常是 `-16`，表示“当前子对象距离最派生对象开头 -16 字节”。RTTI 和 `dynamic_cast<void*>` 会利用它。

### 5.3 `dynamic_cast<void*>` 的价值

```cpp
void* p1 = dynamic_cast<void*>(ma);  // MC 对象开头
void* p2 = dynamic_cast<void*>(mb);  // 也是 MC 对象开头
```

`static_cast<void*>(mb)` 得到的是 `MB` 子对象地址（MC 对象 + 16），不是最派生对象开头。二者在“对象身份”“缓存 key”“日志关联”场景中含义不同。

### 5.4 通过次基类删除

```cpp
MB* p = new MC;
delete p;
```

如果 `MB` 的析构函数是虚函数，`p` 的次 vtable 中放的是**删除析构 thunk**：先把 `this` 调回 `MC*`，再调用 `MC` 的 D0/D1，最后释放正确地址。若析构函数不是虚函数，这里就是未定义行为（UB）。

### 5.5 Demo 中的对应关系

```cpp
class MaxAmountRule final : public RuleBase, public IAuditable;
```

- `IRule` 是主基类，`IRule*` 通常指向对象偏移 0；
- `IAuditable` 是次基类，`IAuditable*` 通常指向对象偏移 8（`RuleBase` 没有数据成员时）；
- `dynamic_cast<IAuditable*>(IRule*)` 是一次 **cross-cast**，运行期检查 `MaxAmountRule` 是否真的实现了 `IAuditable`，并做指针调整；
- `CountryBlockRule` 没有实现 `IAuditable`，所以同一转换返回 `nullptr`。

---

## 6. 虚继承：共享基类与 vbase offset

### 6.1 菱形问题

```cpp
struct VBase { virtual ~VBase() {} int seq; };
struct VLeft  : virtual VBase { int l; };
struct VRight : virtual VBase { int r; };
struct Diamond : VLeft, VRight { int d; };
```

如果不用 `virtual` 继承，`Diamond` 会有两份 `VBase`，`diamond.seq` 产生歧义。`virtual` 继承让 `VLeft` 和 `VRight` **共享同一份 `VBase` 子对象**。

典型布局（x86-64 GCC，示意）：

```text
Diamond 对象
0    VLeft 子对象
     +0  vptr
     +8  l
16   VRight 子对象
     +0  vptr
     +8  r
32   d
40   VBase 共享子对象
     +0  vptr
     +8  seq
```

具体偏移会因编译器版本、数据成员对齐而变化，demo 会打印实际值。

### 6.2 为什么虚继承的指针转换更贵

普通继承：

```cpp
MC* p;
MB* q = p;   // 编译期常量加法
```

虚继承：

```cpp
VLeft* p;
VBase* q = p;  // VBase 的位置由运行期对象决定
```

`VLeft` 不知道自己在 `Diamond` 里的哪个位置，也不知道 `VBase` 最终被放在哪里；它必须从**自己的 vtable 中读取 vbase offset**，再做加法。因此：

- 虚继承的对象通常更大（多 vptr、vbase 偏移信息）；
- 指针转换可能多一次内存访问；
- 布局更复杂，跨编译器更不稳定。

### 6.3 `static_cast` 不能从虚基类向下转

```cpp
VBase* base = new Diamond;
Diamond* d = static_cast<Diamond*>(base);  // 编译错误
```

因为 `VBase` 是虚基类，`VBase*` 到 `Diamond*` 的偏移不是编译期常量，标准不允许 `static_cast` 做这个向下转换。必须：

```cpp
Diamond* d = dynamic_cast<Diamond*>(base);  // 运行期检查 + 调整
```

`compile_fail/virtual_base_downcast.cpp` 就是让编译器报这个错。

### 6.4 构造与 VTT

虚继承下，最派生类负责构造虚基类：

```text
Diamond 构造顺序：
1. 虚基类 VBase
2. 直接基类 VLeft / VRight（按声明顺序）
3. Diamond 自身
```

编译器可能生成 **VTT（Virtual Table Table）** 和 **construction vtable**，在构造/析构的不同阶段给 vptr 填入不同阶段的表。你在构造函数里看到的虚函数行为，正是这些阶段切换的结果。

> 跨平台提示：MSVC 处理虚继承时会使用 `vtordisp` 字段；Itanium ABI 不使用 `vtordisp`，而是使用 vbase offset / VTT。不要把某一家的布局当成标准。

---

## 7. RTTI：`typeid` 与 `dynamic_cast` 的底层

### 7.1 `type_info` 在哪里

多态对象的 vtable `[-1]` 槽指向 `std::type_info`：

```cpp
const std::type_info* ti =
    reinterpret_cast<const std::type_info*>(vptr[-1]);
```

因此：

```cpp
typeid(*basePtr)   // 多态左值：运行期取 vptr[-1]，得到动态类型
typeid(basePtr)    // 指针本身：静态类型 IRule*，不取动态类型
```

`typeid` 的结果是 `const std::type_info&`，生命周期到程序结束；`name()` 返回的字符串是实现定义的（GCC 下是 mangled name），demo 用 `abi::__cxa_demangle` 转成可读形式只是为了展示。

### 7.2 `typeid` 的边界

- 对**多态类型的左值**取 `typeid`，会运行期求动态类型；
- 对**指针**取 `typeid`，得到指针的静态类型；
- 对**非多态类型**取 `typeid`，编译期决定，不求值；
- 对**空指针解引用后的多态左值**取 `typeid`，抛 `std::bad_typeid`；
- `typeid` 要求 RTTI；`-fno-rtti` 下无法使用（但虚函数仍然工作）。

### 7.3 `dynamic_cast` 做了什么

简化流程：

1. 从源指针找到对象，读取其动态类型（RTTI）；
2. 在类型层次结构中查找目标类型；
3. 如果目标可达，计算指针调整量（可能用到 `offset_to_top` 和基类偏移）；
4. 返回调整后的指针；失败时指针返回 `nullptr`，引用抛 `std::bad_cast`。

Itanium ABI 用不同的 `type_info` 子类描述继承关系：

- `__class_type_info`：普通类；
- `__si_class_type_info`：单继承；
- `__vmi_class_type_info`：多继承/虚继承，包含每个基类的偏移和虚基类标志。

因此 `dynamic_cast` 不是“查一个字符串”，而是按 RTTI 描述的继承图做搜索和偏移计算。层次越深、多继承越复杂，成本越高；它也不适合放在极热路径里反复调用。

### 7.4 四种常见转换

```cpp
IRule* r = ...;

// 1. 向下转型：失败返回 nullptr
MaxAmountRule* p = dynamic_cast<MaxAmountRule*>(r);

// 2. 交叉转型：从 IRule* 转到同对象的 IAuditable*
IAuditable* a = dynamic_cast<IAuditable*>(r);

// 3. 最派生对象身份：任何基类指针都能回到对象开头
void* most = dynamic_cast<void*>(r);

// 4. 引用版本：失败抛 std::bad_cast
IAuditable& ref = dynamic_cast<IAuditable&>(*r);
```

`dynamic_cast<void*>` 在多继承/虚继承下特别有用：它直接返回最派生对象地址，常用于对象身份比较、缓存 key、日志追踪。

### 7.5 跨 DSO 的 RTTI

`type_info` 的相等性在同一个进程、同一套 ABI 下通常没问题，但跨共享库时要注意：

- 隐藏符号（`-fvisibility=hidden`）可能让同一个类型在不同 DSO 中有不同 `type_info` 实例；
- 某些平台/链接方式下 `typeid` 比较可能退化；
- 插件边界不要依赖 RTTI 做协议，改用稳定的枚举/ID/版本化接口。

---

## 8. 构造与析构期间的 vptr

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

1. 构造 `Derived` 时，先构造 `Base`；
2. `Base` 构造期间，vptr 指向 `Base` 的 vtable，所以 `who()` 调到 `Base::who`；
3. `Base` 构造完成后，进入 `Derived` 构造，vptr 被改为 `Derived` 的 vtable，所以调到 `Derived::who`；
4. 析构顺序相反：先 `Derived` 析构（vptr 仍是 Derived），再 `Base` 析构（vptr 切回 Base）。

这不是“编译器故意阻止多态”，而是**对象还没构造完/已经开始拆解**：如果基类构造时调用派生类 override，派生类成员还没初始化，行为会非常危险。

### 8.1 纯虚函数调用会终止

如果基类构造函数直接或间接调用纯虚函数：

```cpp
struct Base {
    Base() { f(); }       // 危险
    virtual void f() = 0;
};
```

`Base` 构造期间动态类型是 `Base`，`f` 没有实现，通常落到 `__cxa_pure_virtual`，程序终止。标准将构造函数/析构函数中调用纯虚函数视为未定义行为。业务代码中不要在基类构造/析构里调用可被派生的虚函数；需要初始化钩子时，用两阶段初始化或工厂。

---

## 9. 协变返回与返回指针调整

```cpp
class IRule {
public:
    virtual IRule* clone() const = 0;
};

class MaxAmountRule : public RuleBase, public IAuditable {
public:
    MaxAmountRule* clone() const override;
};
```

`MaxAmountRule*` 可以协变地覆盖 `IRule*`，因为 `MaxAmountRule` 派生自 `IRule`。但机器层面，返回类型从 `MaxAmountRule*` 转成 `IRule*` 可能需要调整指针：

- 如果 `IRule` 子对象在偏移 0，通常不需要；
- 如果基类在对象中间，编译器会生成 **return-adjusting thunk**：调用真实函数后，把返回指针加上/减去偏移。

协变返回不只是“类型系统的小把戏”，它也可能引入 thunk 和额外的指针调整。Demo 中 `IRule` 是主基类，所以偏移通常是 0；但设计多继承接口时要有这个意识。

---

## 10. 析构函数的 vtable 槽位

一个虚析构函数通常占两个槽：

```text
D1：complete object destructor
    只调用析构函数，不释放内存
D0：deleting destructor
    调用 D1，然后 operator delete(this)
```

为什么需要两个？

- 栈对象/成员对象析构：只需要 D1；
- `delete p`：需要 D1 + 释放内存，但 `p` 可能指向基类子对象，释放前必须调整回真正的对象地址；
- 多继承下，`delete` 通过次基类指针时，D0 thunk 负责把 `this` 调回最派生对象，再释放正确地址。

`PureDtorBase` 的纯虚析构函数仍然必须有定义：

```cpp
struct PureDtorBase {
    virtual ~PureDtorBase() = 0;
};

PureDtorBase::~PureDtorBase() {}   // 必须定义
```

因为派生类析构时一定会调用基类析构函数；只声明不定义会在链接期报错。

### 10.1 `unique_ptr` 与 `shared_ptr` 的差异

```cpp
std::unique_ptr<Base> p(new Derived);   // default_delete<Base>，要求 Base 虚析构
std::shared_ptr<Base> q(new Derived);   // 控制块按 Derived* 生成删除器，通常能正确删除
```

`shared_ptr` 的构造函数模板会保留实参的静态类型 `Derived*`，控制块里的删除器按 `Derived` 删除；即使 `Base` 析构不是虚函数，这种写法通常也能正确释放（但仍然是坏设计）。

但如果你先转成 `Base*` 再交给 `shared_ptr`：

```cpp
Base* raw = new Derived;
std::shared_ptr<Base> q(raw);   // 删除器按 Base*，非虚析构 => UB
```

所以结论不变：**多态基类一律虚析构**，不要依赖 `shared_ptr` 的“碰巧正确”。

---

## 11. 性能：虚调用、去虚化与 RTTI

### 11.1 成本来源

- 一次 vptr 加载；
- 一次 vtable 槽加载；
- 一次间接调用（分支预测压力）；
- 阻止内联和跨函数优化；
- 可能存在的 this 调整/thunk；
- 对象多一个 vptr（多继承多个）。

### 11.2 去虚化（devirtualization）

编译器在能看到“确切动态类型”时，可以把虚调用改成直接调用，甚至内联：

```cpp
MaxAmountRule rule;        // 静态类型就是最终类型
rule.evaluate(ctx);        // 可能直接调用
```

`final` 能提供额外信息：

```cpp
class MaxAmountRule final : public RuleBase { ... };
```

如果编译器知道指针/引用的静态类型是 `MaxAmountRule`，且类是 `final`，就可以确定没有更派生的 override，从而去虚化。

常见手段：

- `final` 类/函数；
- LTO（链接时优化）；
- PGO（按实际类型分布做 speculative devirtualization）；
- 把热路径上的小函数做成非虚模板/CRTP（静态多态）；
- 减少不必要的接口层次。

但注意：**`final` 不是性能保证**。是否去虚化取决于优化级别、编译器、调用点上下文。用 benchmark 和汇编验证，不要凭教条。

### 11.3 RTTI 成本

- `typeid` 通常很快（读 vptr[-1]）；
- `dynamic_cast` 需要遍历继承图、计算偏移，明显更贵；
- 热路径中频繁 `dynamic_cast` 往往意味着设计可以用 visitor、标签枚举、类型擦除或虚函数本身替代。

### 11.4 观察编译器输出

```bash
# 查看类布局（GCC 8+，输出 .class 文件）
g++ -std=c++11 -c -Iinclude -fdump-lang-class src/abi_probe.cpp -o /tmp/abi.o

# 查看汇编中的间接调用
g++ -std=c++11 -O2 -Iinclude -S src/risk.cpp -o /tmp/risk.s
grep -n "call.*\*" /tmp/risk.s

# 查看去虚化决策（GCC）
g++ -std=c++11 -O2 -Iinclude -fdump-tree-devirt src/main.cpp -c -o /tmp/main.o
```

不同 GCC 版本支持情况不同；命令失败就跳过，不影响 demo 构建。

---

## 12. ABI 稳定性：插件架构必须知道的规则

C++ 的 vtable 布局不是标准，跨编译器/跨版本没有二进制兼容保证。即使是同一个 GCC，也要遵守：

1. **新增虚函数追加到末尾**，不要插在中间；
2. **不要删除/重排已有虚函数**；
3. **不要修改已有虚函数的签名**（参数、const、返回类型、异常规格）；
4. **不要改变基类顺序**；
5. 不要在插件边界传 STL 容器/异常/`std::string`，除非确定 ABI 完全一致；
6. 跨编译器/跨语言插件优先用 C ABI（`extern "C"` + 不透明句柄 + 版本号）；
7. 用 `pImpl` 隐藏实现，减少头文件暴露的布局；
8. 给接口加版本号/能力查询，不要靠 `dynamic_cast` 猜版本。

一个典型的“破坏 ABI”的例子：

```cpp
class IRule {
public:
    virtual ~IRule();
    virtual Result evaluate(const Context&) const;
    virtual IRule* clone() const;
};

// 新版本在中间插入：
class IRule {
public:
    virtual ~IRule();
    virtual Result evaluate(const Context&) const;
    virtual void warmup();              // 新插入
    virtual IRule* clone() const;       // 槽位后移！
};
```

旧插件的 `clone` 槽位和新宿主对不上，调用会跳到错误函数。正确做法是把 `warmup` 放到最后，或者升级接口版本。

---

## 13. `abi_probe.cpp` 逐行说明

核心读取：

```cpp
const void* const* vptr = nullptr;
std::memcpy(&vptr, object, sizeof(vptr));
```

拆开看：

- `object` 是对象地址；
- `memcpy` 把对象开头的第一个机器字复制到 `vptr`；
- 这里仍然假设“vptr 位于子对象偏移 0”，这是 Itanium 布局，不是标准保证；
- 用 `memcpy` 而不是 `*reinterpret_cast<...>`，是为了避免把对象内存直接当成指针对象解引用带来的严格别名问题。

然后：

```cpp
std::ptrdiff_t offset_to_top = 0;
std::memcpy(&offset_to_top, vptr - 2, sizeof(offset_to_top));

const std::type_info* rtti =
    reinterpret_cast<const std::type_info*>(vptr[-1]);
```

- `vptr[-2]`：`offset_to_top`；
- `vptr[-1]`：`type_info*`；
- `vptr[0]`、`vptr[1]`、...：函数地址槽。

为什么用 `memcpy` 读 `offset_to_top`？因为把 `void*` 直接当成 `ptrdiff_t*` 解引用违反严格别名规则；`memcpy` 是合法的按字节复制。整个探针还加了 `-fno-strict-aliasing`，降低优化器重排风险。

> 这段代码是教学探针，不是生产代码。不同 ABI、不同编译器、不同 `-fno-rtti` 设置都可能让它失效。生产环境不要读 vtable。

`src/abi_probe.cpp` 还做了这些观察：

- 两个同类型对象的 vptr 相同 → vtable 是类共享的；
- 主/次 vptr 不同 → 多继承有多张表；
- 次 vtable 的 `offset_to_top` 为负 → 次子对象在主对象中间；
- `vtable[-1] == &typeid(MC)` → RTTI 指针就在 vtable 前面；
- `Diamond` 的 `VBase` 无论从左路还是右路转换，地址相同 → 共享虚基类；
- `dynamic_cast<VRight*>(VBase*)` 成功 → 运行期 cross-cast；
- `dynamic_cast<void*>` 回到对象开头 → 身份识别。

---

## 14. 常见误解

| 误解 | 事实 |
| --- | --- |
| 每个对象有一张 vtable | 每个对象有 vptr；vtable 是类共享的 |
| 对象里存了一组函数指针 | 对象里通常只有一个 vptr（多继承多个），函数指针在 vtable |
| vtable 是 C++ 标准规定的 | 标准规定行为，布局由 ABI 决定；Itanium 和 MSVC 不同 |
| 虚函数一定慢 | 有间接调用成本，但可去虚化；要测量 |
| 虚析构会让对象变大 | 对象已经有 vptr；虚析构主要增加 vtable 槽和删除逻辑 |
| 构造函数可以是虚函数 | 不可以；构造时动态类型尚未建立 |
| 纯虚函数不能有函数体 | 可以有；纯虚析构必须有定义 |
| `override` 是运行期检查 | 编译期检查，无运行期开销 |
| `final` 一定更快 | 可能帮助去虚化，但不是保证 |
| 虚函数默认参数会动态绑定 | 默认参数按静态类型在调用点替换 |
| 派生类同名函数会自动重载基类 | 会隐藏；需要 `using` |
| `dynamic_cast` 对非多态类型也能用 | 向上转换可以；向下/交叉转换要求源对象是多态类型 |
| `typeid(ptr)` 得到动态类型 | 得到指针的静态类型；`typeid(*ptr)` 才是动态类型 |
| `dynamic_cast` 失败一定抛异常 | 指针返回 `nullptr`，引用才抛 `bad_cast` |
| 虚继承只是语法糖 | 有运行期偏移、对象变大、构造复杂等真实成本 |
| 读 vtable 可以跨平台 | 不可以；这是 ABI 细节，不是标准 |

---

## 15. 自测清单

读完本文，应该能回答：

1. 为什么同一个类的两个对象 vptr 相同？
2. 多继承下，`Derived*` 转 `Base2*` 为什么要调整？谁来做调整？
3. `offset_to_top` 在次 vtable 中为什么是负数？
4. `dynamic_cast<void*>` 和 `static_cast<void*>` 有什么区别？
5. 虚继承下，为什么 `static_cast<Diamond*>(VBase*)` 编译不过？
6. 构造 `Derived` 时，`Base` 构造函数里调用虚函数会调到谁？为什么？
7. 纯虚析构函数为什么必须定义？
8. `delete Base*` 时，D0 和 D1 分别做什么？
9. 为什么新增虚函数要追加到 vtable 末尾？
10. `final` 与去虚化是什么关系？
11. `typeid(*p)` 和 `typeid(p)` 的区别？
12. `dynamic_cast` 的指针版和引用版失败行为有何不同？

如果这些问题都能用自己的话解释，并能在 demo 里指出对应代码，那么你已经掌握了虚函数与虚表的核心机制，而不只是会写 `virtual` 关键字。
