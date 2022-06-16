/*
    Copyright 2015 Adobe
    Distributed under the Boost Software License, Version 1.0.
    (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
*/

/**************************************************************************************************/

#ifndef STLAB_CONCURRENCY_DEFAULT_EXECUTOR_HPP
#define STLAB_CONCURRENCY_DEFAULT_EXECUTOR_HPP

#include <stlab/concurrency/config.hpp>
#include <stlab/concurrency/set_current_thread_name.hpp>
#include <stlab/concurrency/task.hpp>

#include <cassert>
#include <chrono>
#include <functional>

#if STLAB_TASK_SYSTEM(LIBDISPATCH)
#include <dispatch/dispatch.h>
#elif STLAB_TASK_SYSTEM(EMSCRIPTEN)
#include <emscripten.h>
#elif STLAB_TASK_SYSTEM(PNACL)
#include <ppapi/cpp/completion_callback.h>
#include <ppapi/cpp/core.h>
#include <ppapi/cpp/module.h>
#elif STLAB_TASK_SYSTEM(WINDOWS)
#include <Windows.h>
#include <memory>
#elif STLAB_TASK_SYSTEM(PORTABLE)

#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <thread>
#include <vector>

#endif

/**************************************************************************************************/

namespace stlab {

/**************************************************************************************************/

inline namespace v1 {

/**************************************************************************************************/

namespace detail {

/**************************************************************************************************/

enum class executor_priority
{
    high,
    medium,
    low
};

/**************************************************************************************************/

#if STLAB_TASK_SYSTEM(LIBDISPATCH)

constexpr auto platform_priority(executor_priority p)
{
    switch (p)
    {
        case executor_priority::high:
            return DISPATCH_QUEUE_PRIORITY_HIGH;
        case executor_priority::medium:
          return DISPATCH_QUEUE_PRIORITY_DEFAULT;
        case executor_priority::low:
            return DISPATCH_QUEUE_PRIORITY_LOW;
        default:
            assert(!"Unknown value!");
    }
    return DISPATCH_QUEUE_PRIORITY_DEFAULT;
}

struct group_t {
    dispatch_group_t _group = dispatch_group_create();
    ~group_t() {
        dispatch_group_wait(_group, DISPATCH_TIME_FOREVER);
#if !STLAB_FEATURE(OBJC_ARC)
        dispatch_release(_group);
#endif
    }
};

inline group_t& group() {
    static group_t g;
    return g;
}

template <executor_priority P = executor_priority::medium>
struct executor_type {
    using result_type = void;

    template <typename F>
    void operator()(F f) const {
        using f_t = decltype(f);

        dispatch_group_async_f(detail::group()._group,
                               dispatch_get_global_queue(platform_priority(P), 0),
                               new f_t(std::move(f)), [](void* f_) {
                                   auto f = static_cast<f_t*>(f_);
                                   (*f)();
                                   delete f;
                               });
    }
};

/**************************************************************************************************/

#elif STLAB_TASK_SYSTEM(EMSCRIPTEN)

template <executor_priority P = executor_priority::medium>
struct executor_type {
    using result_type = void;

    template <typename F>
    void operator()(F f) const {
        // REVISIT (sparent) : Using a negative timeout may give better performance. Need to test.
        using f_t = decltype(f);

        emscripten_async_call(
            [](void* f_) {
                auto f = static_cast<f_t*>(f_);
                (*f)();
                delete f;
            },
            new f_t(std::move(f)), 0);
    }
};

/**************************************************************************************************/

#elif STLAB_TASK_SYSTEM(PNACL)

template <executor_priority P = executor_priority::medium>
struct executor_type {
    using result_type = void;

    template <typename F>
    void operator()(F f) const {
        using f_t = decltype(f);

        pp::Module::Get()->core()->CallOnMainThread(0,
                                                    pp::CompletionCallback(
                                                        [](void* f_, int32_t) {
                                                            auto f = static_cast<f_t*>(f_);
                                                            (*f)();
                                                            delete f;
                                                        },
                                                        new f_t(std::move(f))),
                                                    0);
    }
};

/**************************************************************************************************/

#elif STLAB_TASK_SYSTEM(WINDOWS)

constexpr auto platform_priority(executor_priority p)
{
    switch (p)
    {
        case executor_priority::high:
            return TP_CALLBACK_PRIORITY_HIGH;
        case executor_priority::medium:
            return TP_CALLBACK_PRIORITY_NORMAL;
        case executor_priority::low:
            return TP_CALLBACK_PRIORITY_LOW;
        default:
            assert(!"Unknown value!");

    }
    return TP_CALLBACK_PRIORITY_NORMAL;
}

template <executor_priority P = executor_priority::medium>
class task_system {
    PTP_POOL _pool = nullptr;
    TP_CALLBACK_ENVIRON _callBackEnvironment;
    PTP_CLEANUP_GROUP _cleanupgroup = nullptr;

public:
    task_system() {
        InitializeThreadpoolEnvironment(&_callBackEnvironment);
        _pool = CreateThreadpool(nullptr);
        if (_pool == nullptr) throw std::bad_alloc();

        _cleanupgroup = CreateThreadpoolCleanupGroup();
        if (_cleanupgroup == nullptr) {
            CloseThreadpool(_pool);
            throw std::bad_alloc();
        }

        SetThreadpoolCallbackPriority(&_callBackEnvironment, platform_priority(P));
        SetThreadpoolCallbackPool(&_callBackEnvironment, _pool);
        SetThreadpoolCallbackCleanupGroup(&_callBackEnvironment, _cleanupgroup, nullptr);
    }

    ~task_system() {
        CloseThreadpoolCleanupGroupMembers(_cleanupgroup, FALSE, nullptr);
        CloseThreadpoolCleanupGroup(_cleanupgroup);
        CloseThreadpool(_pool);
    }

    template <typename F>
    void operator()(F&& f) {
        auto work = CreateThreadpoolWork(&callback_impl<F>, new F(std::forward<F>(f)),
                                         &_callBackEnvironment);

        if (work == nullptr) {
            throw std::bad_alloc();
        }
        SubmitThreadpoolWork(work);
    }

private:
    template <typename F>
    static void CALLBACK callback_impl(PTP_CALLBACK_INSTANCE /*instance*/,
                                       PVOID parameter,
                                       PTP_WORK work) {
        std::unique_ptr<F> f(static_cast<F*>(parameter));
        (*f)();
        CloseThreadpoolWork(work);
    }
};

/**************************************************************************************************/

#elif STLAB_TASK_SYSTEM(PORTABLE)

class waiter {
    std::mutex _mutex;
    using lock_t = std::unique_lock<std::mutex>;
    std::condition_variable _ready;

    bool _waiting{false};
    bool _done{false};

public:
    void done() {
        {
            lock_t lock{_mutex};
            _done = true;
        }
        _ready.notify_one();
    }

    // If wait() is waiting, wake and return true, otherwise return false
    bool wake() {
        {
            lock_t lock{_mutex, std::try_to_lock};
            if (!lock || !_waiting) return false;
            _waiting = false;
        }
        _ready.notify_one();
        return true;
    }

    // Will wait until `wake()` or `done()` returns true if done
    bool wait() {
        lock_t lock{_mutex};
        _waiting = true;
        while (_waiting && !_done) _ready.wait(lock);
        _waiting = false;
        return _done;
    }
};

class notification_queue {

    struct element_t {
        std::size_t _priority;
        task<void()> _task;

        template <class F>
        element_t(F&& f, std::size_t priority) : _priority{priority}, _task{std::forward<F>(f)} { }

        struct greater {
            bool operator()(const element_t& a, const element_t& b) const {
                return b._priority < a._priority;
            }
        };
    };

    std::mutex _mutex;
    using lock_t = std::unique_lock<std::mutex>;
    std::condition_variable _ready;
    std::vector<element_t> _q; // can't use priority queue because top() is const
    std::size_t _count{0};
    bool _done{false};
    bool _waiting{false};

    static constexpr std::size_t merge_priority_count(std::size_t priority, std::size_t count) {
        assert((priority < 4) && "Priority must be in the range [0, 4).");
        return (priority << (sizeof(std::size_t) * CHAR_BIT - 2)) | count;
    }

    // Must be called under a lock with a non-empty _q, always returns a valid task
    auto pop_not_empty() -> task<void()>  {
        auto result = std::move(_q.front()._task);
        std::pop_heap(begin(_q), end(_q), element_t::greater());
        _q.pop_back();
        return result;
    }

public:
    auto try_pop() -> task<void()> {
        lock_t lock{_mutex, std::try_to_lock};
        if (!lock || _q.empty()) return nullptr;
        return pop_not_empty();
    }

    // If waiting in `pop()`, wakes and returns true. Otherwise returns false.
    bool wake() {
        {
            lock_t lock{_mutex, std::try_to_lock};
            if (!lock || !_waiting) return false;
            _waiting = false; // triggers wake
        }
        _ready.notify_one();
        return true;
    }


    auto pop() -> std::pair<bool, task<void()>> {
        lock_t lock{_mutex};
        _waiting = true;
        while (_q.empty() && !_done && _waiting) _ready.wait(lock);
        _waiting = false;
        if (_q.empty()) return { _done, nullptr };
        return { false, pop_not_empty() };
    }

    void done() {
        {
            lock_t lock{_mutex};
            _done = true;
        }
        _ready.notify_one();
    }

    template <typename F>
    bool try_push(F&& f, std::size_t priority) {
        {
            lock_t lock{_mutex, std::try_to_lock};
            if (!lock) return false;
            _q.emplace_back(std::forward<F>(f), merge_priority_count(priority, _count++));
            std::push_heap(begin(_q), end(_q), element_t::greater());
        }
        _ready.notify_one();
        return true;
    }

    template <typename F>
    void push(F&& f, std::size_t priority) {
        {
            lock_t lock{_mutex};
            _q.emplace_back(std::forward<F>(f), merge_priority_count(priority, _count++));
            std::push_heap(begin(_q), end(_q), element_t::greater());
        }
        _ready.notify_one();
    }
};

/**************************************************************************************************/

class priority_task_system {
    // _count is the number of threads in the thread pool
    // it is at least 1 but usually number of cores - 1 reserved for the main thread
    const unsigned _count{std::max(2u, std::thread::hardware_concurrency()) - 1};
    // thread limit is the total number of threads, including expansion threads for waiting calls
    // It is odd number because a usual pattern is to fan out based on the number of cores, we want
    // one additional thread so if we fan out the limit number of times we have one additional thread
    const unsigned _thread_limit{std::max(9U, std::thread::hardware_concurrency() * 4 + 1)};

    std::vector<notification_queue> _q{_count};
    std::atomic<unsigned> _index{0};

    std::mutex _mutex;
    using lock_t = std::unique_lock<std::mutex>;
    std::vector<std::thread> _threads;
    std::vector<waiter> _waiters{_thread_limit - _count};

    void run(unsigned i) {
        stlab::set_current_thread_name("cc.stlab.default_executor");
        while (true) {
            task<void()> f;

            for (unsigned n = 0; n != _count && !f; ++n) {
                f = _q[(i + n) % _count].try_pop();
            }
            if (!f) {
                bool done;
                std::tie(done, f) = _q[i].pop();
                if (done) break;
            }
            if (f) f(); // we can wake with no task.
        }
    }

    std::size_t waiters_size() {
        lock_t lock{_mutex};
        return _threads.size() - _count;
    }

public:
    // Initialize the task system with
    priority_task_system() {
        _threads.reserve(_thread_limit);
        for (unsigned n = 0; n != _count; ++n) {
            _threads.emplace_back([&, n]{ run(n); });
        }
    }

    ~priority_task_system() {
        for (auto& e : _q) e.done();
        for (auto& e : _waiters) e.done();
        for (auto& e : _threads) e.join();
    }

    template <std::size_t P, typename F>
    void execute(F&& f) {
        static_assert(P < 3, "More than 3 priorities are not known!");
        auto i = _index++;

        for (unsigned n = 0; n != _count; ++n) {
            if (_q[(i + n) % _count].try_push(std::forward<F>(f), P)) return;
        }

        _q[i % _count].push(std::forward<F>(f), P);
    }



    void add_thread() {
        lock_t lock{_mutex};
        if (_threads.size() == _thread_limit) return; // log with cerr
        _threads.emplace_back([&, i = _threads.size()] {
            stlab::set_current_thread_name("cc.stlab.default_executor.expansion");

            while(true) {
                task<void()> f;

                for (unsigned n = 0; n != _count && !f; ++n) {
                    f = _q[(i + n) % _count].try_pop();
                }

                if (f) {
                    f(); // we can wake with no task.
                    continue;
                }
                if (_waiters[i - _count].wait()) break;
            };
        });
    }

    // returns true if a thread was woken
    bool wake() {
        for (auto& e : _q) {
            if (e.wake()) return true;
        }
        for (std::size_t n = 0, l = waiters_size(); n != l; ++n) {
            if (_waiters[n].wake()) return true;
        }
        return false;
    }
};

inline priority_task_system& pts() {
    static priority_task_system only_task_system;
    return only_task_system;
}

template <executor_priority P = executor_priority::medium>
struct task_system
{
    using result_type = void;

    void operator()(task<void()> f) const {
        pts().execute<static_cast<std::size_t>(P)>(std::move(f));
    }
};

#endif

/**************************************************************************************************/

#if STLAB_TASK_SYSTEM(WINDOWS) || STLAB_TASK_SYSTEM(PORTABLE)

template <executor_priority P = executor_priority::medium>
struct executor_type {
    using result_type = void;

    void operator()(task<void()> f) const {
        static task_system<P> only_task_system;
        only_task_system(std::move(f));
    }
};

#endif

/**************************************************************************************************/

} // namespace detail

/**************************************************************************************************/

constexpr auto low_executor = detail::executor_type<detail::executor_priority::low>{};
constexpr auto default_executor = detail::executor_type<detail::executor_priority::medium>{};
constexpr auto high_executor = detail::executor_type<detail::executor_priority::high>{};

/**************************************************************************************************/

} // namespace v1

/**************************************************************************************************/

} // namespace stlab

/**************************************************************************************************/

#endif // STLAB_CONCURRENCY_DEFAULT_EXECUTOR_HPP

/**************************************************************************************************/
