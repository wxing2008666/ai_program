#include "spl/object_graph.h"

#include <stdexcept>
#include <utility>

namespace spl {

Element::Element(const std::string& name) : name_(name) {}

Element::~Element() {}

bool Element::is_ancestor_of(const Element* node) const {
    const Element* cursor = this;
    while (cursor) {
        if (cursor == node) {
            return true;
        }
        std::shared_ptr<Element> parent = cursor->parent_.lock();
        cursor = parent.get();
    }
    return false;
}

void Element::add_child(const std::shared_ptr<Element>& child) {
    if (!child) {
        throw std::invalid_argument("Element::add_child requires a child");
    }
    if (is_ancestor_of(child.get())) {
        throw std::logic_error("Element::add_child would create a cycle");
    }
    if (child->parent_.lock()) {
        throw std::logic_error("Element already has a parent");
    }
    std::shared_ptr<Element> self = shared_from_this();
    children_.push_back(child);
    child->parent_ = self;
}

std::shared_ptr<Element> Element::parent() const {
    return parent_.lock();
}

std::size_t Element::depth() const {
    std::size_t result = 0;
    std::shared_ptr<Element> cursor = parent_.lock();
    while (cursor) {
        ++result;
        cursor = cursor->parent_.lock();
    }
    return result;
}

std::shared_ptr<Element> make_element(const std::string& name) {
    return std::make_shared<Element>(name);
}

void CyclicNode::link_to(const std::shared_ptr<CyclicNode>& other) {
    if (!other) {
        throw std::invalid_argument("CyclicNode::link_to requires a peer");
    }
    peer_ = other;
}

void CyclicNode::break_link() {
    peer_.reset();
}

}
