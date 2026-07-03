#include "wss/affinity.hpp"

#if defined(__linux__)

#include <pthread.h>
#include <sched.h>

namespace wss::affinity {

bool pin_to_core(std::thread::native_handle_type handle, unsigned core_index) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core_index, &cpu_set);
    return pthread_setaffinity_np(handle, sizeof(cpu_set_t), &cpu_set) == 0;
}

} // namespace wss::affinity

#endif // __linux__
