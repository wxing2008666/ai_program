#include "spl/frame_cache.h"

namespace spl {

void FrameCache::put(std::uint64_t id, const FrameBufferPtr& buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[id] = std::weak_ptr<FrameBuffer>(buffer);
}

FrameBufferPtr FrameCache::get(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::uint64_t, std::weak_ptr<FrameBuffer> >::iterator it =
        entries_.find(id);
    if (it == entries_.end()) {
        return FrameBufferPtr();
    }
    FrameBufferPtr buffer = it->second.lock();
    if (!buffer) {
        entries_.erase(it);
    }
    return buffer;
}

std::size_t FrameCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::size_t FrameCache::purge_expired() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t removed = 0;
    for (std::unordered_map<std::uint64_t, std::weak_ptr<FrameBuffer> >::iterator it =
             entries_.begin();
         it != entries_.end();) {
        if (it->second.expired()) {
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void OwnerRegistry::annotate(const Key& key, const std::string& label) {
    std::lock_guard<std::mutex> lock(mutex_);
    labels_[key] = label;
}

std::string OwnerRegistry::label_of(const Key& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<Key, std::string, KeyLess>::const_iterator it = labels_.find(key);
    if (it == labels_.end()) {
        return std::string();
    }
    return it->second;
}

bool OwnerRegistry::erase(const Key& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return labels_.erase(key) != 0;
}

void OwnerRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    labels_.clear();
}

std::size_t OwnerRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return labels_.size();
}

}
