#include "dream/runtime/sched/scheduler.h"

#include <utility>

namespace dream::sched {

int Scheduler::add(std::string name, Callback cb) {
    events_.push_back(Event{std::move(name), std::move(cb), kNever});
    return static_cast<int>(events_.size() - 1);
}

void Scheduler::request(int id, std::uint64_t cycles) {
    auto& e = events_[static_cast<std::size_t>(id)];
    e.deadline = cycles == kNever ? kNever : now_ + cycles;
}

std::uint64_t Scheduler::next_deadline() const noexcept {
    std::uint64_t best = kNever;
    for (const auto& e : events_)
        if (e.deadline < best)
            best = e.deadline;
    return best;
}

void Scheduler::advance_to(std::uint64_t target) {
    for (;;) {
        std::size_t idx = events_.size();
        std::uint64_t best = kNever;
        for (std::size_t i = 0; i < events_.size(); ++i) {
            if (events_[i].deadline < best) {
                best = events_[i].deadline;
                idx = i;
            }
        }
        if (idx == events_.size() || best > target)
            break;
        const std::uint64_t late = now_ > best ? now_ - best : 0;
        if (now_ < best)
            now_ = best;
        events_[idx].deadline = kNever;  // the callback may re-arm
        events_[idx].cb(now_, late);
    }
    if (target > now_)
        now_ = target;
}

}  // namespace dream::sched
