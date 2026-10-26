#ifndef SPL_FRAME_CACHE_H
#define SPL_FRAME_CACHE_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "spl/frame.h"

namespace spl {

class FrameCache {
public:
    void put(std::uint64_t id, const FrameBufferPtr& buffer);
    FrameBufferPtr get(std::uint64_t id) const;
    std::size_t size() const;
    std::size_t purge_expired();

private:
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::uint64_t, std::weak_ptr<FrameBuffer> > entries_;
};

class OwnerRegistry {
public:
    typedef std::shared_ptr<const void> Key;
    typedef std::owner_less<Key> KeyLess;

    void annotate(const Key& key, const std::string& label);
    std::string label_of(const Key& key) const;
    bool erase(const Key& key);
    void clear();
    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::map<Key, std::string, KeyLess> labels_;
};

}

#endif
