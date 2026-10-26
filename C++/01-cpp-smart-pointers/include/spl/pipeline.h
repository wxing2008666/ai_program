#ifndef SPL_PIPELINE_H
#define SPL_PIPELINE_H

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "spl/frame.h"
#include "spl/tracked.h"

namespace spl {

struct ObserverTag {};

class Observer : public Tracked<ObserverTag> {
public:
    virtual ~Observer();
    virtual void on_frame(const std::string& node_name, const Frame& frame) = 0;
};

struct NodeTag {};

class Node : public std::enable_shared_from_this<Node>,
             public Tracked<NodeTag> {
public:
    explicit Node(const std::string& name);
    ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    const std::string& name() const {
        return name_;
    }

    void subscribe(const std::shared_ptr<Observer>& observer);
    std::size_t observer_count() const;
    void emit(const Frame& frame);
    void connect_to(const std::shared_ptr<Node>& downstream);
    std::shared_ptr<Node> upstream() const;
    std::weak_ptr<Node> self_weak();

private:
    std::string name_;
    std::weak_ptr<Node> upstream_;
    mutable std::mutex observers_mutex_;
    mutable std::vector<std::weak_ptr<Observer> > observers_;
};

std::shared_ptr<Node> make_node(const std::string& name);

class Pipeline {
public:
    void add(const std::shared_ptr<Node>& node);
    void link(const std::shared_ptr<Node>& upstream,
              const std::shared_ptr<Node>& downstream);
    void push(const Frame& frame) const;
    std::size_t size() const {
        return nodes_.size();
    }

private:
    bool contains(const Node* node) const;

    std::vector<std::shared_ptr<Node> > nodes_;
};

}

#endif
