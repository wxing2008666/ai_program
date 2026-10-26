#ifndef SPL_PLUGIN_REGISTRY_H
#define SPL_PLUGIN_REGISTRY_H

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "spl/plugin.h"
#include "spl/shared_slot.h"

namespace spl {

typedef std::unordered_map<std::string, std::shared_ptr<Plugin> > PluginMap;
typedef std::shared_ptr<const PluginMap> PluginSnapshot;

class PluginRegistry {
public:
    PluginRegistry();

    PluginRegistry(const PluginRegistry&) = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

    void install(const std::shared_ptr<Plugin>& plugin);
    void install(const std::string& slot, const std::shared_ptr<Plugin>& plugin);
    bool remove(const std::string& name);
    std::shared_ptr<Plugin> find(const std::string& name) const;
    PluginSnapshot snapshot() const;
    std::size_t size() const;

private:
    SharedSlot<const PluginMap> slot_;
    mutable std::mutex write_mutex_;
};

}

#endif
