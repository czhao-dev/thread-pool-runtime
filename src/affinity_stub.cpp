#include "wss/affinity.hpp"

#if !defined(__linux__) && !defined(__APPLE__)

namespace wss::affinity {

bool pin_to_core(std::thread::native_handle_type /*handle*/, unsigned /*core_index*/) { return false; }

} // namespace wss::affinity

#endif
