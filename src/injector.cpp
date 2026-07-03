#include "wss/injector.hpp"

namespace wss {

void Injector::push(Job job) {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push_back(std::move(job));
}

std::optional<Job> Injector::pop_batch_into(ChaseLevDeque<Job>& dest, std::size_t max_batch) {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.empty()) {
        return std::nullopt;
    }

    Job first = std::move(queue_.front());
    queue_.pop_front();

    std::size_t moved = 1;
    while (moved < max_batch && !queue_.empty()) {
        dest.push(std::move(queue_.front()));
        queue_.pop_front();
        ++moved;
    }
    return first;
}

} // namespace wss
