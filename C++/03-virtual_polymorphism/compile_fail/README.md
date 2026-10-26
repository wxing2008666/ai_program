# 编译期失败用例

这些文件**不参与正常构建**，故意制造编译错误，用来证明 `override`、`final`、协变返回、虚基类向下转型和重载隐藏的编译期约束。

在项目根目录执行：

```bash
for f in compile_fail/*.cpp; do
    echo "== $f"
    g++ -std=c++11 -Iinclude -c "$f" -o /dev/null 2>&1 | head -n 4
done
```

预期结果（不同 GCC 版本措辞可能略有差异）：

| 文件 | 触发的规则 | 典型错误 |
| --- | --- | --- |
| `override_typo.cpp` | 签名不匹配时 `override` 必须报错 | `marked 'override', but does not override` |
| `final_class.cpp` | `final` 类不能被继承 | `cannot derive from 'final' base` |
| `final_method.cpp` | `final` 虚函数不能被重写 | `overriding final function` |
| `bad_covariant.cpp` | 协变返回类型必须是派生类指针/引用 | `invalid covariant return type` |
| `virtual_base_downcast.cpp` | 虚基类不能 `static_cast` 向下转型 | `cannot convert ... because base is virtual` |
| `hiding.cpp` | 派生类同名函数隐藏基类重载 | `no matching function for call to ... handle(int)` |

最后一个 `hiding.cpp` 展示的是“名字隐藏”，不是虚函数失败；把 `using Base::handle;` 加回派生类即可通过，`src/main.cpp` 中有正确写法。
