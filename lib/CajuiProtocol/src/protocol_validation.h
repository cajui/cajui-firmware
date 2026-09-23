#pragma once
#include "cajui_protocol.h"

// Internal shared validation; not an additional public protocol API.
namespace cajui {
namespace detail {
bool validData(const Data&);
bool usable(const Binding&);
}
}
