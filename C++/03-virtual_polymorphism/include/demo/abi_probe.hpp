#ifndef DEMO_ABI_PROBE_HPP
#define DEMO_ABI_PROBE_HPP

#include <string>

namespace demo {

std::string demangle(const char* name);
void run_abi_probe();

}  // namespace demo

#endif  // DEMO_ABI_PROBE_HPP
