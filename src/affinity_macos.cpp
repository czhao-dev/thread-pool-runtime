#include "wss/affinity.hpp"

#if defined(__APPLE__)

#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>

namespace wss::affinity {

bool pin_to_core(std::thread::native_handle_type handle, unsigned core_index) {
    thread_affinity_policy_data_t policy{static_cast<integer_t>(core_index)};
    mach_port_t mach_thread = pthread_mach_thread_np(handle);
    kern_return_t result =
        thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, reinterpret_cast<thread_policy_t>(&policy),
                           THREAD_AFFINITY_POLICY_COUNT);
    return result == KERN_SUCCESS;
}

} // namespace wss::affinity

#endif // __APPLE__
