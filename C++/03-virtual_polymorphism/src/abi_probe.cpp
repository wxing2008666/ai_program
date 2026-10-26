#include "demo/abi_probe.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <typeinfo>

#if defined(__GNUG__) && !defined(_WIN32)
#include <cxxabi.h>
#endif

namespace demo {

std::string demangle(const char* name) {
#if defined(__GNUG__) && !defined(_WIN32)
    int status = 0;
    char* readable = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status == 0 && readable != nullptr) {
        std::string result(readable);
        std::free(readable);
        return result;
    }
    std::free(readable);
#endif
    return name;
}

}  // namespace demo

#if defined(__GNUG__) && !defined(_WIN32)

namespace demo {
namespace {

using std::cout;

std::ptrdiff_t byte_offset(const void* base, const void* part) {
    return static_cast<std::ptrdiff_t>(
        reinterpret_cast<std::intptr_t>(part) -
        reinterpret_cast<std::intptr_t>(base));
}

const void* const* vptr_of(const void* object) {
    // 用 memcpy 读取对象首字，避免把对象内存直接当指针解引用（严格别名）。
    const void* const* vptr = nullptr;
    std::memcpy(&vptr, object, sizeof(vptr));
    return vptr;
}

void dump_vtable(const char* label, const void* object, std::size_t slots) {
    // 仅用于本 demo 的 Itanium 布局：槽位数按各演示类硬编码。
    const void* const* vptr = vptr_of(object);

    // Itanium C++ ABI: vptr[-2] 是 offset-to-top，vptr[-1] 是 type_info*。
    std::ptrdiff_t offset_to_top = 0;
    std::memcpy(&offset_to_top, vptr - 2, sizeof(offset_to_top));
    const std::type_info* rtti =
        reinterpret_cast<const std::type_info*>(vptr[-1]);

    cout << label << "\n"
         << "  this          = " << object << "\n"
         << "  vptr          = " << static_cast<const void*>(vptr) << "\n"
         << "  offset_to_top = " << offset_to_top << "\n"
         << "  rtti          = "
         << (rtti != nullptr ? demo::demangle(rtti->name()) : "?") << "\n";
    for (std::size_t i = 0; i < slots; ++i) {
        cout << "  vtable[" << i << "]   = " << vptr[i] << "\n";
    }
}

struct MA {
    virtual ~MA() {}
    virtual const char* name() const { return "MA"; }
    int a = 1;
};

struct MB {
    virtual ~MB() {}
    virtual const char* name() const { return "MB"; }
    int b = 2;
};

struct MC : MA, MB {
    ~MC() override {}
    const char* name() const override { return "MC"; }
    int c = 3;
};

void multiple_inheritance_probe() {
    cout << "\n--- multiple inheritance ---\n";

    MC mc;
    MA* ma = &mc;
    MB* mb = &mc;

    cout << "sizeof(MC)=" << sizeof(MC) << " alignof(MC)=" << alignof(MC)
         << "\n";
    cout << "MA subobject offset = " << byte_offset(&mc, ma) << "\n";
    cout << "MB subobject offset = " << byte_offset(&mc, mb) << "\n";
    cout << "mb->name() = " << mb->name() << "\n";
    cout << "dynamic_cast<MC*>(mb) == &mc: "
         << (dynamic_cast<MC*>(mb) == &mc) << "\n";
    cout << "dynamic_cast<void*>(mb) == most-derived: "
         << (dynamic_cast<void*>(mb) == static_cast<void*>(&mc)) << "\n";

    MC second;
    cout << "two MC objects share primary vptr: "
         << (vptr_of(&mc) == vptr_of(&second)) << "\n";
    cout << "primary vptr != secondary vptr: "
         << (vptr_of(&mc) != vptr_of(mb)) << "\n";

    dump_vtable("MC primary vtable", ma, 3);
    dump_vtable("MC secondary vtable (MB subobject)", mb, 3);

    const std::type_info* rtti =
        reinterpret_cast<const std::type_info*>(vptr_of(&mc)[-1]);
    // 单可执行文件内成立；跨 DSO 或隐藏可见性时不保证是同一个 type_info。
    cout << "vtable[-1] == &typeid(MC): " << (rtti == &typeid(MC)) << "\n";
}

struct VBase {
    virtual ~VBase() {}
    virtual const char* kind() const { return "VBase"; }
    int seq = 7;
};

struct VLeft : virtual VBase {
    virtual const char* left() const { return "VLeft::left"; }
    int l = 1;
};

struct VRight : virtual VBase {
    virtual const char* right() const { return "VRight::right"; }
    int r = 2;
};

struct Diamond : VLeft, VRight {
    ~Diamond() override {}
    const char* kind() const override { return "Diamond"; }
    int d = 3;
};

void virtual_inheritance_probe() {
    cout << "\n--- virtual inheritance (diamond) ---\n";

    Diamond d;
    VLeft* left = &d;
    VRight* right = &d;
    VBase* base_from_left = left;
    VBase* base_from_right = right;

    cout << "sizeof(VBase)=" << sizeof(VBase)
         << " sizeof(VLeft)=" << sizeof(VLeft)
         << " sizeof(VRight)=" << sizeof(VRight)
         << " sizeof(Diamond)=" << sizeof(Diamond) << "\n";
    cout << "VLeft subobject offset  = " << byte_offset(&d, left) << "\n";
    cout << "VRight subobject offset = " << byte_offset(&d, right) << "\n";
    cout << "VBase offset via left   = " << byte_offset(&d, base_from_left)
         << "\n";
    cout << "VBase offset via right  = " << byte_offset(&d, base_from_right)
         << "\n";
    cout << "one shared VBase        = "
         << (base_from_left == base_from_right) << "\n";
    cout << "kind() through VBase    = " << base_from_left->kind() << "\n";
    cout << "typeid(*base)           = "
         << demo::demangle(typeid(*base_from_left).name()) << "\n";
    cout << "dynamic_cast<Diamond*>  = "
         << (dynamic_cast<Diamond*>(base_from_left) == &d) << "\n";
    cout << "cross-cast VRight*      = "
         << (dynamic_cast<VRight*>(base_from_left) == right) << "\n";
    cout << "most-derived identity   = "
         << (dynamic_cast<void*>(base_from_left) ==
             static_cast<void*>(&d))
         << "\n";
    cout << "VBase subobject offset from object = "
         << byte_offset(&d, static_cast<void*>(base_from_left)) << "\n";

    dump_vtable("Diamond primary vtable", left, 3);
    dump_vtable("Diamond shared VBase vtable", base_from_left, 3);
}

}  // namespace

void run_abi_probe() {
    std::cout << "\n=== Itanium C++ ABI probe (GCC/Clang, Linux) ===\n";
    multiple_inheritance_probe();
    virtual_inheritance_probe();
    std::cout << "\n提示：vtable[-2] 是 offset-to-top，vtable[-1] 是 type_info*；\n"
                 "地址与偏移随编译器和版本变化，不属于 C++ 标准。\n";
}

}  // namespace demo

#else

namespace demo {

void run_abi_probe() {
    std::cout << "ABI probe skipped: this demo targets GCC/Clang on Linux.\n";
}

}  // namespace demo

#endif
