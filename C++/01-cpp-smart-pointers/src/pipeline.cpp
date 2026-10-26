#include "spl/pipeline.h"

#include <stdexcept>

namespace spl {

Observer::~Observer() {}

Node::Node(const std::string& name) : name_(name) {}

Node::~Node() {}

void Node::subscribe(const std::shared_ptr<Observer>& observer) {
    if (!observer) {
        throw std::invalid_argument("Node::subscribe requires an observer");
    }
    std::lock_guard<std::mutex> lock(observers_mutex_);
    observers_.push_back(observer);
}

std::size_t Node::observer_count() const {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    std::size_t count = 0;
    for (std::vector<std::weak_ptr<Observer> >::iterator it = observers_.begin();
         it != observers_.end();) {
        if (it->expired()) {
            it = observers_.erase(it);
        } else {
            ++count;
            ++it;
        }
    }
    return count;
}

void Node::emit(const Frame& frame) {
    if (!frame) {
        throw std::invalid_argument("Node::emit requires a valid frame");
    }
    std::vector<std::shared_ptr<Observer> > active;
    {
        std::lock_guard<std::mutex> lock(observers_mutex_);
        for (std::vector<std::weak_ptr<Observer> >::iterator it = observers_.begin();
             it != observers_.end();) {
            std::shared_ptr<Observer> observer = it->lock();
            if (observer) {
                active.push_back(observer);
                ++it;
            } else {
                it = observers_.erase(it);
            }
        }
    }
    for (std::vector<std::shared_ptr<Observer> >::iterator it = active.begin();
         it != active.end(); ++it) {
        (*it)->on_frame(name_, frame);
    }
}

void Node::connect_to(const std::shared_ptr<Node>& downstream) {
    if (!downstream) {
        throw std::invalid_argument("Node::connect_to requires a downstream node");
    }
    std::shared_ptr<Node> self = shared_from_this();

    std::shared_ptr<Node> cursor = self;
    while (cursor) {
        if (cursor.get() == downstream.get()) {
            throw std::logic_error("Node::connect_to would create a cycle");
        }
        cursor = cursor->upstream_.lock();
    }

    cursor = downstream;
    while (cursor) {
        if (cursor.get() == self.get()) {
            throw std::logic_error("Node::connect_to would create a cycle");
        }
        cursor = cursor->upstream_.lock();
    }

    downstream->upstream_ = self;
}

std::shared_ptr<Node> Node::upstream() const {
    return upstream_.lock();
}

std::weak_ptr<Node> Node::self_weak() {
    return std::weak_ptr<Node>(shared_from_this());
}

std::shared_ptr<Node> make_node(const std::string& name) {
    return std::make_shared<Node>(name);
}

bool Pipeline::contains(const Node* node) const {
    for (std::vector<std::shared_ptr<Node> >::const_iterator it = nodes_.begin();
         it != nodes_.end(); ++it) {
        if (it->get() == node) {
            return true;
        }
    }
    return false;
}

void Pipeline::add(const std::shared_ptr<Node>& node) {
    if (!node) {
        throw std::invalid_argument("Pipeline::add requires a node");
    }
    if (contains(node.get())) {
        throw std::logic_error("Pipeline already contains the node");
    }
    nodes_.push_back(node);
}

void Pipeline::link(const std::shared_ptr<Node>& upstream,
                    const std::shared_ptr<Node>& downstream) {
    if (!upstream || !downstream) {
        throw std::invalid_argument("Pipeline::link requires two nodes");
    }
    if (!contains(upstream.get()) || !contains(downstream.get())) {
        throw std::logic_error("Pipeline::link requires nodes owned by the pipeline");
    }
    upstream->connect_to(downstream);
}

void Pipeline::push(const Frame& frame) const {
    for (std::vector<std::shared_ptr<Node> >::const_iterator it = nodes_.begin();
         it != nodes_.end(); ++it) {
        (*it)->emit(frame);
    }
}

}
