#pragma once

#include "FreeRTOS.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

// Blocking event group so tests observe whether waiters are woken or time out.
struct HostEventGroup {
    std::mutex mutex;
    std::condition_variable cv;
    uint32_t bits = 0;
};

using EventBits_t = uint32_t;
using EventGroupHandle_t = HostEventGroup*;

inline EventGroupHandle_t xEventGroupCreate() { return new HostEventGroup(); }

inline void vEventGroupDelete(EventGroupHandle_t group) { delete group; }

inline EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) {
    std::lock_guard<std::mutex> lock(group->mutex);
    group->bits |= bits;
    group->cv.notify_all();
    return group->bits;
}

inline EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
    std::lock_guard<std::mutex> lock(group->mutex);
    const EventBits_t previous = group->bits;
    group->bits &= ~bits;
    return previous;
}

inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                       BaseType_t clear_on_exit, BaseType_t wait_for_all,
                                       TickType_t ticks) {
    std::unique_lock<std::mutex> lock(group->mutex);
    auto satisfied = [&] {
        return wait_for_all ? (group->bits & bits) == bits : (group->bits & bits) != 0;
    };
    group->cv.wait_for(lock, std::chrono::milliseconds(ticks), satisfied);
    const EventBits_t result = group->bits;
    if (clear_on_exit && satisfied()) {
        group->bits &= ~bits;
    }
    return result;
}
