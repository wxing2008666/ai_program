#ifndef SPL_RESOURCE_H
#define SPL_RESOURCE_H

#include <memory>

namespace spl {

struct CResource {
    explicit CResource(int resource_id) : id(resource_id) {}
    int id;
};

CResource* c_resource_open(int resource_id);
void c_resource_close(CResource* resource) noexcept;

long c_resource_open_count() noexcept;
long c_resource_total_opened() noexcept;
long c_resource_total_closed() noexcept;
void c_resource_reset_stats() noexcept;

typedef std::unique_ptr<CResource, void (*)(CResource*)> UniqueResource;
typedef std::shared_ptr<CResource> SharedResource;

UniqueResource make_unique_resource(int resource_id);
UniqueResource adopt_resource(CResource* raw) noexcept;
SharedResource make_shared_resource(int resource_id);
SharedResource share_resource(UniqueResource resource);

}

#endif
