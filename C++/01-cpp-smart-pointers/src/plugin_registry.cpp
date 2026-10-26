#include "spl/plugin_registry.h"

#include <stdexcept>
#include <utility>

namespace spl {

PluginRegistry::PluginRegistry()
    : slot_(std::shared_ptr<const PluginMap>(std::make_shared<PluginMap>())) {}

void PluginRegistry::install(const std::shared_ptr<Plugin>& plugin) {
    if (!plugin) {
        throw std::invalid_argument("PluginRegistry::install requires a plugin");
    }
    install(plugin->name(), plugin);
}

void PluginRegistry::install(const std::string& slot,
                             const std::shared_ptr<Plugin>& plugin) {
    if (slot.empty()) {
        throw std::invalid_argument("PluginRegistry slot name must not be empty");
    }
    if (!plugin) {
        throw std::invalid_argument("PluginRegistry::install requires a plugin");
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    PluginSnapshot current = slot_.load();
    std::shared_ptr<PluginMap> next = std::make_shared<PluginMap>(*current);
    (*next)[slot] = plugin;
    slot_.store(next);
}

bool PluginRegistry::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    PluginSnapshot current = slot_.load();
    PluginMap::const_iterator found = current->find(name);
    if (found == current->end()) {
        return false;
    }
    std::shared_ptr<PluginMap> next = std::make_shared<PluginMap>(*current);
    next->erase(name);
    slot_.store(next);
    return true;
}

std::shared_ptr<Plugin> PluginRegistry::find(const std::string& name) const {
    PluginSnapshot current = slot_.load();
    PluginMap::const_iterator found = current->find(name);
    if (found == current->end()) {
        return std::shared_ptr<Plugin>();
    }
    return found->second;
}

PluginSnapshot PluginRegistry::snapshot() const {
    return slot_.load();
}

std::size_t PluginRegistry::size() const {
    return slot_.load()->size();
}

}
