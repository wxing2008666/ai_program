#ifndef SPL_PLUGIN_H
#define SPL_PLUGIN_H

#include <string>

#include "spl/frame.h"
#include "spl/tracked.h"

namespace spl {

struct PluginTag {};

class Plugin : public Tracked<PluginTag> {
public:
    virtual ~Plugin();
    virtual std::string name() const = 0;
    virtual Frame apply(const Frame& input) const = 0;
};

class InvertPlugin : public Plugin {
public:
    std::string name() const;
    Frame apply(const Frame& input) const;
};

class ThresholdPlugin : public Plugin {
public:
    explicit ThresholdPlugin(int threshold);
    std::string name() const;
    Frame apply(const Frame& input) const;

private:
    int threshold_;
};

class PassthroughPlugin : public Plugin {
public:
    std::string name() const;
    Frame apply(const Frame& input) const;
};

}

#endif
