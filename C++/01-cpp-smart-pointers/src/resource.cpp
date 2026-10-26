#include "spl/resource.h"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace spl {
namespace {

std::atomic<long> g_open_count(0);
std::atomic<long> g_total_opened(0);
std::atomic<long> g_total_closed(0);

}

CResource* c_resource_open(int resource_id) {
    if (resource_id < 0) {
        throw std::invalid_argument("resource id must be non-negative");
    }
    CResource* resource = new CResource(resource_id);
    g_open_count.fetch_add(1, std::memory_order_relaxed);
    g_total_opened.fetch_add(1, std::memory_order_relaxed);
    return resource;
}

void c_resource_close(CResource* resource) noexcept {
    if (!resource) {
        return;
    }
    g_open_count.fetch_sub(1, std::memory_order_relaxed);
    g_total_closed.fetch_add(1, std::memory_order_relaxed);
    delete resource;
}

long c_resource_open_count() noexcept {
    return g_open_count.load(std::memory_order_relaxed);
}

long c_resource_total_opened() noexcept {
    return g_total_opened.load(std::memory_order_relaxed);
}

long c_resource_total_closed() noexcept {
    return g_total_closed.load(std::memory_order_relaxed);
}

void c_resource_reset_stats() noexcept {
    g_open_count.store(0, std::memory_order_relaxed);
    g_total_opened.store(0, std::memory_order_relaxed);
    g_total_closed.store(0, std::memory_order_relaxed);
}

UniqueResource make_unique_resource(int resource_id) {
    return UniqueResource(c_resource_open(resource_id), &c_resource_close);
}

UniqueResource adopt_resource(CResource* raw) noexcept {
    return UniqueResource(raw, &c_resource_close);
}

SharedResource share_resource(UniqueResource resource) {
    return SharedResource(std::move(resource));
}

SharedResource make_shared_resource(int resource_id) {
    return share_resource(make_unique_resource(resource_id));
}

}
