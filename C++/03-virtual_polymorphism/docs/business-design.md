# 业务架构解析：把多态用进一个规则引擎

> 这份文档回答两个问题：**为什么 demo 要这样设计**，以及**在真实业务里怎么避免“会写 virtual 但不会用多态”**。
>
> 代码入口：[`include/risk/risk.hpp`](../include/risk/risk.hpp)、[`src/risk.cpp`](../src/risk.cpp)、[`src/main.cpp`](../src/main.cpp)。

---

## 1. 业务问题

支付风控的典型需求：

- 交易进来后，按一组规则做实时决策：放行、人工审核、拒绝；
- 规则会不断增加：金额上限、国家/地区黑名单、速度检查、设备信誉、商户等级……
- 规则可以组合：一个“支付守卫”由多条规则组成；
- 每次决策要能审计：为什么拒绝？哪条规则触发？规则参数是什么？
- 未来可能以插件形式下发规则；
- 核心引擎不能每加一条规则就改一次 `if/else`。

这正是多态最擅长的场景：**调用方依赖稳定接口，变化被隔离到具体实现**。

如果写成：

```cpp
if (rule_name == "max_amount") { ... }
else if (rule_name == "country_block") { ... }
else if (rule_name == "velocity") { ... }
```

每加一条规则都要改引擎、重新编译、重新测试；规则组合和审计也会变成字符串判断的泥潭。

---

## 2. 结构总览

```text
                    +----------------------+
                    |      RuleEngine      |
                    |  run(Context)        |
                    +----------+-----------+
                               | owns 1..n
                               v
                    +----------------------+
                    |        IRule         |<---------------------+
                    | id()                 |                      |
                    | evaluate(Context)    |                      |
                    | clone()              |                      |
                    | accept(Visitor)      |                      |
                    +----------+-----------+                      |
                               ^                                  |
              +----------------+----------------+                 |
              |                |                |                 |
     +--------+------+  +------+-------+  +-----+------+   +------+-------+
     | MaxAmountRule |  | CountryBlock |  | Velocity  |   | CompositeRule|
     +--------+------+  +--------------+  +-----------+   +------+-------+
              |                                                   |
              | implements                                        | children
              v                                                   v
     +----------------+                                  +----------------+
     |  IAuditable    |                                  |   IRule ...    |
     |  audit()       |                                  +----------------+
     +----------------+
```

关键点：

- `IRule` 是稳定接口，引擎只依赖它；
- `RuleBase` 用 NVI 提供公共外壳，具体规则只实现 `doEvaluate`；
- `CompositeRule` 也是 `IRule`，所以“组合”对引擎透明；
- `IAuditable` 是独立能力接口，只有需要审计的规则才实现；
- `RuleRegistry` 保存原型，`create()` 通过 `clone()` 生产独立对象；
- `RuleVisitor` 把“对规则做某种操作”从规则类中分离出来。

---

## 3. 设计决策一：接口要窄，能力要组合

### 3.1 为什么不让 `IRule` 直接带 `audit()`

一种写法：

```cpp
class IRule {
public:
    virtual Result evaluate(...) const = 0;
    virtual std::string audit() const = 0;   // 所有规则都被迫实现
};
```

问题是：

- 有的规则没有审计信息；
- 审计格式变化会迫使所有规则重编译；
- 违反接口隔离原则（ISP）。

Demo 用两个接口：

```cpp
class IRule { ... };
class IAuditable { virtual std::string audit() const = 0; };
```

需要审计的规则多继承一个 `IAuditable`。这带来一个真实成本：多继承、次 vtable、`this` 调整、cross-cast。Demo 故意保留这个成本，因为业务中“能力接口 + 多继承”非常常见，架构师必须理解它的代价，而不是只会写单继承。

### 3.2 多继承的边界

适合多继承的场景：

- 一个主接口 + 若干**无状态能力接口**（可审计、可序列化、可观测）；
- 接口没有数据成员，避免布局复杂化；
- 能接受次 vtable 和指针调整成本。

不适合：

- 多个基类都带复杂状态；
- 菱形继承；
- 跨编译器插件边界；
- 只是为了“代码复用”而继承。

Demo 的 `MaxAmountRule` / `CompositeRule` 都是“主接口 + 能力接口”，没有菱形；这是可控的多继承。

---

## 4. 设计决策二：NVI + private virtual

### 4.1 写法

```cpp
class RuleBase : public IRule {
public:
    Result evaluate(const Context& ctx) const final;

private:
    virtual Result doEvaluate(const Context& ctx) const = 0;
};
```

派生类：

```cpp
class MaxAmountRule final : public RuleBase {
private:
    Result doEvaluate(const Context& ctx) const override;
};
```

### 4.2 为什么

- `evaluate` 是 public、非虚（对派生类而言是 final），调用方看到稳定的入口；
- `doEvaluate` 是 private virtual，派生类只定制“核心判断”；
- 基类可以在 `evaluate` 里统一做参数校验、日志、指标、异常转换、结果规范化；
- 派生类不能绕过这些不变量；
- `private` 不影响虚 dispatch：访问控制是编译期规则，虚调用是运行期机制。

### 4.3 业务价值

假设未来要给所有规则加：

- 耗时统计；
- 决策埋点；
- 空指针/金额校验；
- 统一异常处理。

只需要改 `RuleBase::evaluate` 一处，不需要改每个规则。这是“模板方法”在业务中的真正价值。

### 4.4 注意

- 不要把 `evaluate` 再声明为 `virtual` 且让派生类覆盖；`final` 就是防止这件事；
- 如果确实需要派生类定制外壳，说明抽象层次可能设计错了；
- NVI 不意味着所有虚函数都要 private；接口方法（如 `id()`）仍可 public。

---

## 5. 设计决策三：原型 + 注册表

### 5.1 注册

```cpp
Registrar<MaxAmountRule, double> g_max_amount("max_amount", 5000.0);
Registrar<CountryBlockRule> g_country_block("country_block");
Registrar<VelocityRule> g_velocity("velocity");
```

`Registrar` 在静态初始化阶段把“原型对象”放进 `RuleRegistry`。注册表用函数内静态变量：

```cpp
RuleRegistry& RuleRegistry::instance() {
    static RuleRegistry registry;
    return registry;
}
```

这避免跨 TU 的静态初始化顺序问题：第一次调用时一定已经构造。

### 5.2 创建

```cpp
std::unique_ptr<IRule> rule = registry.create("max_amount");
```

`create` 不直接返回原型，而是：

```cpp
return std::unique_ptr<IRule>(prototype->clone());
```

### 5.3 为什么用 clone

- 原型可能带配置（`limit_`、阈值、黑名单）；
- 每个引擎/每个请求需要独立实例，避免共享可变状态；
- 原型可以集中配置，实例按需复制；
- 新增规则只要注册，不需要改工厂的 `switch`。

### 5.4 代价与注意

- `clone()` 返回裸指针，调用方必须 `delete`；demo 用 `clone_rule()` 包成 `unique_ptr`；
- 深拷贝必须正确：`CompositeRule::clone()` 递归复制子规则，不能只复制 `unique_ptr`（那会编译失败，正好防止浅拷贝）；
- 注册发生在启动阶段，运行期不要并发修改注册表；
- 静态初始化阶段抛异常会导致 `std::terminate`，注册代码要简单、可靠；
- 跨 DSO 注册时，注意静态对象是否真的被链接进来（有时需要 `--whole-archive` 或显式引用）。

---

## 6. 设计决策四：组合模式

```cpp
class CompositeRule final : public RuleBase, public IAuditable {
public:
    void add(std::unique_ptr<IRule> rule);
private:
    Result doEvaluate(const Context& ctx) const override;
    std::vector<std::unique_ptr<IRule>> children_;
};
```

业务价值：

- 引擎只看到“一个规则”，不关心内部有多少子规则；
- 可以嵌套：`payment_guard` 里再放一个 `high_risk_guard`；
- 决策聚合逻辑集中在一处：任意子规则 Reject → Reject；否则任意 Review → Review；否则 Allow；
- 评分累加，方便风控排序；
- `clone()` 深拷贝，支持按请求复制规则树。

复杂度：

- 递归调用有栈深度；规则树异常深时要限制深度；
- 组合语义要明确：是“全部通过”还是“任意通过”？Demo 选择“全部规则都执行，取最严决策”，因为审计需要完整结果；
- 如果业务需要短路，可以在 Reject 时提前返回，但会丢失后续审计信息；这是业务取舍，不是技术问题。

---

## 7. 设计决策五：Visitor 做双分派

### 7.1 需求

运营后台要导出规则树：

```text
Composite(payment_guard, children=3)
  MaxAmount(limit=5000)
  CountryBlock(blocked=2)
  Velocity(velocity)
```

### 7.2 不用 Visitor 的写法

```cpp
for (auto& rule : rules) {
    if (auto* p = dynamic_cast<MaxAmountRule*>(rule.get())) { ... }
    else if (auto* p = dynamic_cast<CountryBlockRule*>(rule.get())) { ... }
    else if (auto* p = dynamic_cast<VelocityRule*>(rule.get())) { ... }
}
```

问题：

- 每加一种规则都要改导出代码；
- `dynamic_cast` 链越来越长；
- 类型判断和业务格式耦合。

### 7.3 Visitor 写法

```cpp
class IRule {
public:
    virtual void accept(RuleVisitor& visitor) const = 0;
};

class MaxAmountRule {
public:
    void accept(RuleVisitor& visitor) const override {
        visitor.visit(*this);   // 静态类型是 MaxAmountRule&
    }
};
```

调用：

```cpp
rule->accept(reporter);
```

两跳：

1. `accept` 是虚函数，运行期分派到具体规则；
2. 具体规则里的 `*this` 静态类型已知，`visitor.visit(*this)` 在编译期选中正确的重载，再对 `visitor` 做虚分派。

这就是“双分派”：**规则类型 + 访问者类型**共同决定执行哪段代码。

### 7.4 代价

- 新增一种规则，要改 `RuleVisitor` 接口和所有 visitor；这是“类型维度”的扩展成本；
- 新增一种操作（导出、校验、优化），只需加一个 visitor，不改规则类；
- 适合“规则类型相对稳定、操作经常增加”的场景；如果类型经常增加，Visitor 会变得笨重。

### 7.5 另一种选择

如果只是偶尔取一下类型信息，`dynamic_cast`/`typeid` 更简单；如果是高频、多操作，Visitor 更好。架构师要根据变化方向选择，而不是背模式。

---

## 8. RTTI 在 demo 中的位置

Demo 用 RTTI 做三件事：

1. **审计能力发现**：`dynamic_cast<IAuditable*>(rule.get())`，只有实现能力接口的规则才采集审计；
2. **类型安全向下转型**：`dynamic_cast<MaxAmountRule*>(base)`，失败返回 `nullptr`；
3. **对象身份**：`dynamic_cast<void*>(base)`，多继承下回到最派生对象。

业务原则：

- **决策本身不依赖 RTTI**：规则执行靠虚函数，不靠 `typeid` 分支；
- RTTI 用在边界：审计采集、插件加载、导出、调试、兼容旧接口；`RuleEngine::run` 中的 `dynamic_cast<IAuditable*>` 就属于审计旁路，不影响决策结果；
- 热路径中避免大层次 `dynamic_cast`；
- 如果发现大量 `dynamic_cast` 链，优先考虑 Visitor、能力接口、标签枚举或类型擦除。

---

## 9. 所有权与生命周期

- 引擎持有 `vector<unique_ptr<IRule>>`，独占所有权；
- `CompositeRule` 持有子规则的 `unique_ptr`，形成树；
- `clone()` 返回裸指针，调用方负责释放；`clone_rule()` 提供 `unique_ptr` 包装；
- 接口析构函数必须是 `virtual`，否则 `delete IRule*` 是 UB；
- `shared_ptr` 不是“万能解药”：如果先转成基类指针再构造，非虚析构仍然 UB；
- 不要让规则持有裸指针指向引擎/上下文，除非生命周期非常明确；
- 规则对象最好不可变，或每个请求独立 clone，避免并发写。

---

## 10. 并发与线程安全

Demo 的注册表是启动期只读的：`main` 之前注册，运行期只 `create`/`clone`。这是有意的简化。

生产环境要考虑：

- 注册表初始化完成后冻结（freeze），运行期只读；
- 如果必须运行期注册，用读写锁/并发 map，并明确内存序；
- 原型对象不可变；`clone` 时复制配置；
- 规则实例是否线程安全，取决于规则内部状态；无状态规则天然安全，有状态规则要加锁或按线程复制；
- 避免在规则里写全局可变状态；
- `RuleEngine` 本身可以被多个线程共享（只读），但 `run` 里不要修改成员。

---

## 11. 异常安全

- `RuleEngine::run` 不捕获规则异常：异常向上抛，由调用方决定降级策略（拒绝？审核？放行？）；
- 如果业务要求“单条规则失败不能影响整体”，在引擎层捕获并转成 `Review`/`Reject`，同时记录审计；
- `clone` 可能抛 `bad_alloc`；注册/加载阶段要处理；
- 析构函数不要抛异常（C++11 析构默认 `noexcept`，抛出会 `terminate`）；
- `dynamic_cast` 引用版会抛 `bad_cast`，在边界捕获；指针版不抛。

---

## 12. 性能与架构取舍

| 方案 | 动态分派 | 扩展新类型 | 扩展新操作 | 典型成本 | 适用 |
| --- | --- | --- | --- | --- | --- |
| 虚函数 + 接口 | 运行期 | 容易 | 改接口/Visitor | vptr + 间接调用 | 插件、规则、驱动 |
| `std::function` | 运行期 | 容易 | 容易 | 类型擦除 + 可能堆分配 | 回调、命令 |
| 标签 + `switch` | 编译期/跳转 | 要改 switch | 要改 switch | 低 | 类型少且稳定 |
| CRTP/模板 | 编译期 | 要重编译 | 容易 | 代码膨胀 | 热路径、静态组合 |
| `std::variant`（C++17） | 编译期访问 | 要改类型列表 | 容易 | 无 vptr，但类型封闭 | 封闭集合 |
| 类型擦除 | 运行期 | 容易 | 容易 | 间接层 | 跨库 API |

Demo 选虚函数，因为：

- 规则类型开放，插件化；
- 引擎不应知道具体类型；
- 规则数量可控，性能不是瓶颈；
- 需要运行期组合和 RTTI 边界能力。

如果是每秒千万次的超热路径，且类型集合封闭，可以考虑模板/`variant`/CRTP，或把虚调用移出内层循环。

---

## 13. ABI 与插件边界

C++ 插件最容易踩的坑不是语法，而是 ABI：

- 宿主和插件必须用兼容的编译器/标准库/异常模型；
- `std::string`、`std::vector`、异常跨 DSO 传递可能不兼容；
- RTTI 在隐藏符号下可能不合并；
- 虚表布局随编译器版本变化；
- 新增虚函数要追加到末尾；
- 不要跨边界传 `unique_ptr` 到不同 allocator/CRT；
- 最稳的方案是 C ABI：`extern "C"` 工厂 + 不透明句柄 + 版本号 + 显式 `destroy` 函数。

Demo 是同进程、同编译器、同 STL 的教学项目，所以可以直接用 C++ 接口；生产插件要额外设计版本协商和能力查询。

---

## 14. 测试策略

1. **运行时检查**：`src/main.cpp` 用 `require` 验证决策、RTTI、切片、默认参数等行为；失败抛异常，进程返回非 0。
2. **编译期检查**：`compile_fail/` 用故意错误的代码证明 `override`、`final`、协变返回、虚基类向下转型、名字隐藏的约束。
3. **ABI 观察**：`src/abi_probe.cpp` 打印 vptr、vtable、offset_to_top、RTTI 指针；不把具体偏移写成断言，避免编译器差异导致假失败。
4. **Sanitizer**：

   ```bash
   mkdir -p build-asan && cd build-asan
   cmake .. -DENABLE_SANITIZERS=ON
   make -j
   ./vtable_demo
   ```

   ASan 查越界/释放错误，UBSan 查未定义行为。ABI 探针本身是“故意越界读实现细节”，已用 `-fno-strict-aliasing` 降低风险，但仍建议把探针视为教学代码。
5. **CTest**：`add_test(NAME vtable_demo COMMAND vtable_demo)`，任何 `require` 失败都会让测试失败。

---

## 15. 代码走读路线

建议按这个顺序读：

1. `include/risk/risk.hpp`：接口、NVI、多继承、注册表、引擎的数据结构；
2. `src/risk.cpp`：具体规则的 `doEvaluate`、组合递归、clone、注册；
3. `src/main.cpp`：规则引擎场景 → RTTI → 默认参数 → 构造/析构 → 切片 → 隐藏 → 虚析构；
4. `src/abi_probe.cpp`：多继承/虚继承布局与 vtable 原始读取；
5. `docs/abi-deep-dive.md`：把探针输出和 ABI 原理对上；
6. `compile_fail/`：看编译器如何阻止错误设计。

---

## 16. 生产化清单

- [ ] 所有多态基类都有 `virtual` 析构；
- [ ] 所有 override 都写 `override`；
- [ ] 不需要再派生的叶子类写 `final`；
- [ ] 接口方法尽量少，能力用独立接口组合；
- [ ] 核心路径不依赖 RTTI；
- [ ] 规则 clone 是深拷贝；
- [ ] 注册表启动期冻结；
- [ ] 规则实例不可变或按请求隔离；
- [ ] 插件边界有版本号/能力查询；
- [ ] 不跨 ABI 传 STL/异常；
- [ ] 有编译期失败用例、运行时用例、Sanitizer；
- [ ] 热路径有 benchmark，必要时去虚化。

---

## 17. 练习题

1. 增加 `OrRule`（任意子规则通过即通过），思考它和 `CompositeRule` 的决策聚合差异。
2. 增加 `NotRule` 装饰器，观察 `clone()` 深拷贝如何递归。
3. 给 `RuleVisitor` 增加一个 `JsonExportVisitor`，输出规则树 JSON。
4. 给 `MaxAmountRule` 增加一个“按币种配置限额”的 map，确保 `clone()` 复制配置。
5. 用 `-fdump-lang-class` 观察 `MaxAmountRule` 的 vtable 数量，解释为什么是两张。
6. 写一个 benchmark，比较 `IRule*` 虚调用、`MaxAmountRule` 直接调用、`final` + `-O2` 的差异。
7. 把 `IRule` 改成 C ABI 风格的不透明句柄，思考需要放弃哪些 C++ 特性。
8. 用 `dynamic_cast` 链实现 `RuleReporter`，再和 Visitor 版本比较可维护性。

---

## 18. 一句话总结

**多态不是“写一个 virtual 就完了”，而是：用稳定接口隔离变化，用 NVI 保护不变量，用组合/Visitor/原型组织复杂行为，用 RTTI 处理边界，用 ABI 意识保护插件生态。**
