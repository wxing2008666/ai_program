#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "spl/allocation_probe.h"
#include "spl/codec.h"
#include "spl/frame.h"
#include "spl/frame_cache.h"
#include "spl/gpu_buffer.h"
#include "spl/intrusive_ptr.h"
#include "spl/memory_utils.h"
#include "spl/object_graph.h"
#include "spl/pipeline.h"
#include "spl/plugin.h"
#include "spl/plugin_registry.h"
#include "spl/resource.h"
#include "spl/task_queue.h"

namespace {

class ConsoleObserver : public spl::Observer {
public:
    ConsoleObserver() : calls(0) {}

    void on_frame(const std::string& node_name, const spl::Frame& frame) {
        ++calls;
        std::cout << "  observer <- " << node_name << " " << frame.width() << "x"
                  << frame.height() << " first_pixel="
                  << static_cast<int>(frame.at(0, 0)) << std::endl;
    }

    int calls;
};

spl::Frame make_gradient_frame() {
    spl::FrameBufferPtr buffer = spl::make_frame_buffer(8 * 8);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            buffer->data()[y * 8 + x] = static_cast<std::uint8_t>(y * 8 + x);
        }
    }
    return spl::Frame::wrap(buffer, 8, 8);
}

void section(const char* title) {
    std::cout << "\n=== " << title << " ===" << std::endl;
}

}

int main() {
    std::cout << std::boolalpha;

    section("object sizes");
    std::cout << "unique_ptr<int>                    : "
              << sizeof(std::unique_ptr<int>) << " bytes" << std::endl;
    std::cout << "unique_ptr<int, function pointer>  : "
              << sizeof(std::unique_ptr<int, void (*)(int*)>) << " bytes" << std::endl;
    std::cout << "shared_ptr<int>                    : "
              << sizeof(std::shared_ptr<int>) << " bytes" << std::endl;
    std::cout << "weak_ptr<int>                      : "
              << sizeof(std::weak_ptr<int>) << " bytes" << std::endl;
    std::cout << "intrusive_ptr<GpuBuffer>           : "
              << sizeof(spl::intrusive_ptr<spl::GpuBuffer>) << " bytes" << std::endl;

    section("aliasing constructor and zero-copy frame views");
    spl::Tracked<spl::FrameBufferTag>::reset_counters();
    spl::FrameBufferPtr buffer = spl::make_frame_buffer(64 * 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            buffer->data()[y * 64 + x] = static_cast<std::uint8_t>((x + y) % 256);
        }
    }
    spl::Frame full = spl::Frame::wrap(buffer, 64, 64);
    spl::Frame roi = full.slice(8, 8, 16, 16);

    std::cout << "full.data=" << static_cast<const void*>(full.data())
              << " roi.data=" << static_cast<const void*>(roi.data()) << std::endl;
    std::cout << "pointer delta=" << (roi.data() - full.data())
              << " bytes, stride=" << roi.stride()
              << ", roi owner_use_count=" << roi.owner_use_count() << std::endl;

    spl::Frame::OwnerKey full_key = full.owner_key();
    spl::Frame::OwnerKey roi_key = roi.owner_key();
    std::cout << "same control block=" << spl::same_owner(full_key, roi_key)
              << ", after owner_key use_count=" << roi.owner_use_count() << std::endl;

    std::weak_ptr<spl::FrameBuffer> buffer_observer = buffer;
    spl::FrameCache cache;
    cache.put(42, buffer);
    full_key.reset();
    roi_key.reset();
    buffer.reset();
    full = spl::Frame();
    std::cout << "after releasing producer: cache_hit="
              << static_cast<bool>(cache.get(42))
              << ", buffer_alive=" << !buffer_observer.expired() << std::endl;
    roi = spl::Frame();
    std::cout << "after releasing last view: cache_hit="
              << static_cast<bool>(cache.get(42))
              << ", buffer_alive=" << !buffer_observer.expired()
              << ", cache_size=" << cache.size() << std::endl;

    section("owner_less identity map");
    spl::OwnerRegistry registry;
    spl::FrameBufferPtr annotated_buffer = spl::make_frame_buffer(8 * 8);
    spl::Frame annotated_full = spl::Frame::wrap(annotated_buffer, 8, 8);
    spl::Frame annotated_roi = annotated_full.slice(2, 2, 4, 4);
    registry.annotate(annotated_full.owner_key(), "camera-0");
    std::cout << "lookup through slice=" << registry.label_of(annotated_roi.owner_key())
              << ", registry_size=" << registry.size() << std::endl;
    registry.clear();

    section("tree ownership with weak parent");
    spl::Tracked<spl::ElementTag>::reset_counters();
    {
        std::shared_ptr<spl::Element> root = spl::make_element("root");
        std::shared_ptr<spl::Element> child = spl::make_element("child");
        std::shared_ptr<spl::Element> leaf = spl::make_element("leaf");
        root->add_child(child);
        child->add_child(leaf);
        std::cout << "leaf depth=" << leaf->depth()
                  << ", parent=" << leaf->parent()->name()
                  << ", alive=" << spl::Tracked<spl::ElementTag>::alive() << std::endl;
    }
    std::cout << "after tree destruction alive="
              << spl::Tracked<spl::ElementTag>::alive() << std::endl;

    section("shared_ptr cycle and explicit break");
    spl::Tracked<spl::CyclicNodeTag>::reset_counters();
    std::weak_ptr<spl::CyclicNode> cycle_first;
    {
        std::shared_ptr<spl::CyclicNode> first = std::make_shared<spl::CyclicNode>();
        std::shared_ptr<spl::CyclicNode> second = std::make_shared<spl::CyclicNode>();
        cycle_first = first;
        first->link_to(second);
        second->link_to(first);
        first.reset();
        second.reset();
        std::cout << "after external reset alive="
                  << spl::Tracked<spl::CyclicNodeTag>::alive() << std::endl;
        cycle_first.lock()->break_link();
    }
    std::cout << "after break_link alive="
              << spl::Tracked<spl::CyclicNodeTag>::alive() << std::endl;

    section("pipeline, enable_shared_from_this and weak observers");
    {
        spl::Pipeline pipeline;
        std::shared_ptr<spl::Node> source = spl::make_node("source");
        std::shared_ptr<spl::Node> sink = spl::make_node("sink");
        std::shared_ptr<ConsoleObserver> observer = std::make_shared<ConsoleObserver>();
        source->subscribe(observer);
        sink->subscribe(observer);
        pipeline.add(source);
        pipeline.add(sink);
        pipeline.link(source, sink);
        std::cout << "sink upstream=" << sink->upstream()->name()
                  << ", observer_count=" << source->observer_count() << std::endl;
        pipeline.push(make_gradient_frame());
        std::cout << "observer calls=" << observer->calls << std::endl;
    }
    std::cout << "after pipeline scope observers_alive="
              << spl::Tracked<spl::ObserverTag>::alive() << std::endl;

    section("async lifetime: strong capture vs weak cancellation");
    spl::Tracked<spl::NodeTag>::reset_counters();
    {
        spl::TaskQueue queue(1);
        std::shared_ptr<spl::Node> weak_node_object = spl::make_node("async-weak");
        std::weak_ptr<spl::Node> weak_node = weak_node_object;
        weak_node_object.reset();
        queue.post([weak_node]() {
            std::shared_ptr<spl::Node> locked = weak_node.lock();
            std::cout << "  weak task: node="
                      << (locked ? locked->name() : std::string("cancelled"))
                      << std::endl;
        });

        std::shared_ptr<spl::Node> strong_node = spl::make_node("async-strong");
        std::weak_ptr<spl::Node> strong_observer = strong_node;
        queue.post([strong_node]() {
            std::cout << "  strong task: node=" << strong_node->name() << std::endl;
        });
        strong_node.reset();
        std::cout << "queued while strong task exists: alive="
                  << spl::Tracked<spl::NodeTag>::alive() << std::endl;
        queue.shutdown();
        std::cout << "after queue drain: alive="
                  << spl::Tracked<spl::NodeTag>::alive()
                  << ", observer_expired=" << strong_observer.expired() << std::endl;
    }

    section("plugin registry hot swap");
    spl::PluginRegistry plugins;
    std::shared_ptr<spl::Plugin> old_plugin = std::make_shared<spl::InvertPlugin>();
    plugins.install(old_plugin);
    std::shared_ptr<spl::Plugin> in_flight = plugins.find("invert");
    std::shared_ptr<spl::Plugin> new_plugin = std::make_shared<spl::PassthroughPlugin>();
    plugins.install("invert", new_plugin);

    spl::Frame plugin_input = make_gradient_frame();
    spl::Frame old_result = in_flight->apply(plugin_input);
    spl::Frame new_result = plugins.find("invert")->apply(plugin_input);
    std::cout << "in_flight plugin=" << in_flight->name()
              << " old_first_pixel=" << static_cast<int>(old_result.at(0, 0))
              << ", current plugin=" << plugins.find("invert")->name()
              << " new_first_pixel=" << static_cast<int>(new_result.at(0, 0))
              << ", passthrough_shared=" << new_result.shares_owner_with(plugin_input)
              << std::endl;

    section("custom deleters and C resource interop");
    spl::c_resource_reset_stats();
    {
        spl::UniqueResource unique = spl::make_unique_resource(7);
        std::cout << "unique id=" << unique->id
                  << ", open=" << spl::c_resource_open_count() << std::endl;
        spl::SharedResource shared = spl::share_resource(std::move(unique));
        spl::SharedResource copy = shared;
        std::cout << "shared use_count=" << shared.use_count()
                  << ", open=" << spl::c_resource_open_count() << std::endl;
        copy.reset();
        shared.reset();
    }
    std::cout << "after scope open=" << spl::c_resource_open_count()
              << ", opened=" << spl::c_resource_total_opened()
              << ", closed=" << spl::c_resource_total_closed() << std::endl;

    section("intrusive reference counting across an ABI style call");
    spl::Tracked<spl::GpuBufferTag>::reset_counters();
    {
        spl::intrusive_ptr<spl::GpuBuffer> gpu = spl::make_gpu_buffer(1024, 0);
        spl::intrusive_ptr<spl::GpuBuffer> copy = gpu;
        std::cout << "use_count=" << gpu.use_count()
                  << ", alive=" << spl::Tracked<spl::GpuBufferTag>::alive() << std::endl;
        copy.reset();
        spl::GpuBuffer* raw = gpu.detach();
        spl::intrusive_ptr<spl::GpuBuffer> adopted(raw, false);
        std::cout << "detached raw, adopted use_count=" << adopted.use_count()
                  << std::endl;
        adopted.reset();
    }
    std::cout << "after scope alive=" << spl::Tracked<spl::GpuBufferTag>::alive()
              << std::endl;

    section("allocation combining and weak_ptr delayed deallocation");
    spl::Tracked<spl::AllocProbeTag>::reset_counters();
    spl::AllocationStats::reset();
    std::weak_ptr<spl::AllocProbe> probe_observer;
    {
        std::shared_ptr<spl::AllocProbe> probe =
            std::allocate_shared<spl::AllocProbe>(
                spl::CountingAllocator<spl::AllocProbe>(), 21);
        probe_observer = probe;
        std::cout << "allocate_shared allocations="
                  << spl::AllocationStats::allocations()
                  << ", object_alive=" << spl::Tracked<spl::AllocProbeTag>::alive()
                  << std::endl;
    }
    std::cout << "after strong refs: object_alive="
              << spl::Tracked<spl::AllocProbeTag>::alive()
              << ", block_outstanding=" << spl::AllocationStats::outstanding()
              << std::endl;
    probe_observer.reset();
    std::cout << "after weak reset: block_outstanding="
              << spl::AllocationStats::outstanding() << std::endl;

    section("pimpl with unique_ptr");
    spl::Decoder decoder("h264", 80);
    spl::Decoder moved_decoder(std::move(decoder));
    std::cout << "decoder=" << moved_decoder.describe()
              << ", moved_from_valid=" << decoder.valid()
              << ", output_shared="
              << moved_decoder.decode(plugin_input).shares_owner_with(plugin_input)
              << std::endl;

    section("summary");
    std::cout << "all modules executed" << std::endl;
    return 0;
}
