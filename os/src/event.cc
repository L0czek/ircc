#include "event.hpp"
#include "cmsis_os2.h"

namespace os {

event::event(std::unique_ptr<osEventFlagsAttr_t> attrs) :attrs(std::move(attrs)) {
    handle = osEventFlagsNew(this->attrs.get());
}

event::event() : event(nullptr) {}

event::~event() {
    osEventFlagsDelete(handle);
}

uint32_t event::get() const {
    return osEventFlagsGet(handle);
}

const char *event::name() const {
    return osEventFlagsGetName(handle);
}

}
