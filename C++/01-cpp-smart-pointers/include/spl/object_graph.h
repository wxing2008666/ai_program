#ifndef SPL_OBJECT_GRAPH_H
#define SPL_OBJECT_GRAPH_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "spl/tracked.h"

namespace spl {

struct ElementTag {};

class Element : public std::enable_shared_from_this<Element>,
                public Tracked<ElementTag> {
public:
    explicit Element(const std::string& name);
    ~Element();

    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;

    const std::string& name() const {
        return name_;
    }

    void add_child(const std::shared_ptr<Element>& child);
    std::shared_ptr<Element> parent() const;
    const std::vector<std::shared_ptr<Element> >& children() const {
        return children_;
    }
    std::size_t depth() const;

private:
    bool is_ancestor_of(const Element* node) const;

    std::string name_;
    std::weak_ptr<Element> parent_;
    std::vector<std::shared_ptr<Element> > children_;
};

std::shared_ptr<Element> make_element(const std::string& name);

struct CyclicNodeTag {};

class CyclicNode : public Tracked<CyclicNodeTag> {
public:
    void link_to(const std::shared_ptr<CyclicNode>& other);
    void break_link();

private:
    std::shared_ptr<CyclicNode> peer_;
};

}

#endif
