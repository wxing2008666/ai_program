#include <memory>
#include <stdexcept>
#include <string>

#include "spl/pipeline.h"
#include "test_framework.h"

namespace {

class CountingObserver : public spl::Observer {
public:
    CountingObserver() : calls(0) {}

    void on_frame(const std::string& node_name, const spl::Frame& frame) {
        ++calls;
        last_node = node_name;
        last_width = frame.width();
    }

    int calls;
    std::string last_node;
    int last_width;
};

spl::Frame make_test_frame() {
    spl::FrameBufferPtr buffer = spl::make_frame_buffer(4 * 4);
    return spl::Frame::wrap(buffer, 4, 4);
}

}

SPL_TEST(node_observers_are_weak_and_pruned) {
    spl::Tracked<spl::ObserverTag>::reset_counters();
    std::shared_ptr<spl::Node> node = spl::make_node("source");
    spl::Frame frame = make_test_frame();
    {
        std::shared_ptr<CountingObserver> observer =
            std::make_shared<CountingObserver>();
        node->subscribe(observer);
        SPL_CHECK_EQ(node->observer_count(), 1);
        node->emit(frame);
        SPL_CHECK_EQ(observer->calls, 1);
        SPL_CHECK_EQ(observer->last_node, std::string("source"));
        SPL_CHECK_EQ(observer->last_width, 4);
    }
    SPL_CHECK_EQ(node->observer_count(), 0);
    node->emit(frame);
    SPL_CHECK_EQ(spl::Tracked<spl::ObserverTag>::alive(), 0);
}

SPL_TEST(node_upstream_link_is_weak) {
    std::shared_ptr<spl::Node> upstream = spl::make_node("upstream");
    std::shared_ptr<spl::Node> downstream = spl::make_node("downstream");
    upstream->connect_to(downstream);
    SPL_CHECK(downstream->upstream().get() == upstream.get());

    upstream.reset();
    SPL_CHECK(!downstream->upstream());
}

SPL_TEST(node_rejects_upstream_cycles_and_stacked_objects) {
    std::shared_ptr<spl::Node> first = spl::make_node("first");
    std::shared_ptr<spl::Node> second = spl::make_node("second");
    first->connect_to(second);
    SPL_CHECK_THROW(second->connect_to(first), std::logic_error);
    SPL_CHECK_THROW(first->connect_to(first), std::logic_error);

    spl::Node stacked("stacked");
    std::shared_ptr<spl::Node> other = spl::make_node("other");
    SPL_CHECK_THROW(stacked.connect_to(other), std::bad_weak_ptr);
}

SPL_TEST(pipeline_owns_nodes_and_emits_in_order) {
    spl::Tracked<spl::NodeTag>::reset_counters();
    std::weak_ptr<spl::Node> second_observer;
    spl::Frame frame = make_test_frame();
    {
        spl::Pipeline pipeline;
        std::shared_ptr<spl::Node> first = spl::make_node("first");
        std::shared_ptr<spl::Node> second = spl::make_node("second");
        std::shared_ptr<CountingObserver> observer = std::make_shared<CountingObserver>();
        first->subscribe(observer);
        second->subscribe(observer);

        pipeline.add(first);
        pipeline.add(second);
        pipeline.link(first, second);
        second_observer = second;
        first.reset();
        second.reset();
        SPL_CHECK(!second_observer.expired());
        SPL_CHECK_EQ(pipeline.size(), 2);

        pipeline.push(frame);
        SPL_CHECK_EQ(observer->calls, 2);
    }
    SPL_CHECK(second_observer.expired());
    SPL_CHECK_EQ(spl::Tracked<spl::NodeTag>::alive(), 0);
}

SPL_TEST(node_self_weak_observes_lifetime) {
    std::weak_ptr<spl::Node> observer;
    {
        std::shared_ptr<spl::Node> node = spl::make_node("ephemeral");
        observer = node->self_weak();
        SPL_CHECK(!observer.expired());
    }
    SPL_CHECK(observer.expired());
}
