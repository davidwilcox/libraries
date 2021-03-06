/*
    Copyright 2015 Adobe
    Distributed under the Boost Software License, Version 1.0.
    (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
*/

/**************************************************************************************************/

#ifndef STLAB_CHANNEL_HPP
#define STLAB_CHANNEL_HPP

#include <deque>
#include <memory>
#include <thread>
#include <utility>

#include <stlab/future.hpp>

/**************************************************************************************************/

namespace stlab {

/**************************************************************************************************/

template <typename> class sender;
template <typename> class receiver;

/**************************************************************************************************/

enum class process_state {
    await,
    await_try,
    yield,
};

/**************************************************************************************************/

template <typename I, // I models ForwardIterator
          typename N, // N models PositiveInteger
          typename F> // F models UnaryFunction
I for_each_n(I p, N n, F f) {
    for (N i = 0; i != n; ++i, ++p) {
        f(*p);
    }
    return p;
}

struct identity {
    template <typename T>
    T operator()(T x) { return x; }
};

/**************************************************************************************************/

template <typename>
struct result_of_;

template <typename R, typename... Args>
struct result_of_<R(Args...)> { using type = R; };

template <typename F>
using result_of_t_ = typename result_of_<F>::type;

/**************************************************************************************************/

template <typename> struct argument_of;
template <typename R, typename Arg>
struct argument_of<R(Arg)> { using type = Arg; };

template <typename T>
using argument_of_t = typename argument_of<T>::type;

/**************************************************************************************************/

namespace detail {

/**************************************************************************************************/

class avoid_ { };

template <typename T>
using avoid = std::conditional_t<std::is_same<void, T>::value, avoid_, T>;

/**************************************************************************************************/

template <typename F, typename... Args>
auto avoid_invoke(F&& f, Args&&... args)
        -> std::enable_if_t<!std::is_same<void,
            decltype(std::forward<F>(f)(std::forward<Args>(args)...))>::value,
            decltype(std::forward<F>(f)(std::forward<Args>(args)...))>
{
    return std::forward<F>(f)(std::forward<Args>(args)...);
}

template <typename F, typename... Args>
auto avoid_invoke(F&& f, Args&&... args)
        -> std::enable_if_t<std::is_same<void,
            decltype(std::forward<F>(f)(std::forward<Args>(args)...))>::value,
            avoid_>
{
    std::forward<F>(f)(std::forward<Args>(args)...);
    return avoid_();
}


/**************************************************************************************************/

// REVISIT (sparent) : I have a make_weak_ptr() someplace already. Should be in memory.hpp

template <typename T>
auto make_weak_ptr(const std::shared_ptr<T>& x) { return std::weak_ptr<T>(x); }

/**************************************************************************************************/

template <typename T>
struct shared_process_receiver {
    virtual ~shared_process_receiver() = default;

    virtual void map(sender<T>) = 0;
    virtual void cts() = 0;
    virtual void add_receiver() = 0;
    virtual void remove_receiver() = 0;
};

/**************************************************************************************************/

template <typename T>
struct shared_process_sender {
    virtual ~shared_process_sender() = default;

    virtual void send(avoid<T> x) = 0;
    virtual void add_sender() = 0;
    virtual void remove_sender() = 0;
};

/**************************************************************************************************/

template <typename T>
auto test_process_close(decltype(&T::close)) -> std::true_type;

template <typename>
auto test_process_close(...) -> std::false_type;

template <typename T>
constexpr bool has_process_close = decltype(test_process_close<T>(0))::value;

template <typename T>
auto process_close(T& x) -> std::enable_if_t<has_process_close<T>> {
    x.close();
}

template <typename T>
auto process_close(T& x) -> std::enable_if_t<!has_process_close<T>> { }

/**************************************************************************************************/

template <typename T>
auto test_process_state(decltype(&T::state)) -> std::true_type;

template <typename>
auto test_process_state(...) -> std::false_type;

template <typename T>
constexpr bool has_process_state = decltype(test_process_state<T>(0))::value;

template <typename T>
auto get_process_state(const T& x) -> std::enable_if_t<has_process_state<T>, process_state> {
    return x.state();
}

template <typename T>
auto get_process_state(const T& x) -> std::enable_if_t<!has_process_state<T>, process_state> {
    return process_state::await;
}

/**************************************************************************************************/

template <typename T>
auto test_process_yield(decltype(&T::yield)) -> std::true_type;

template <typename>
auto test_process_yield(...) -> std::false_type;

template <typename T>
constexpr bool has_process_yield = decltype(test_process_yield<T>(0))::value;

/**************************************************************************************************/

template <typename T, typename>
auto yield_type_(decltype(&T::yield)) -> decltype(std::declval<T>().yield());

template <typename T, typename Arg>
auto yield_type_(...) -> decltype(std::declval<T>()(std::declval<Arg>()));

template <typename T, typename Arg>
using yield_type = decltype(yield_type_<T, Arg>(0));

/**************************************************************************************************/

template<typename T, typename Arg>
struct shared_process : shared_process_receiver<yield_type<T, Arg>>,
                        shared_process_sender<Arg>,
                        std::enable_shared_from_this<shared_process<T, Arg>> {

    /*
        the downstream continuations are stored in a deque so we don't get reallocations
        on push back - this allows us to make calls while additional inserts happen.
    */

    using argument = Arg;
    using result = yield_type<T, Arg>;

    std::mutex                    _downstream_mutex;
    std::deque<sender<result>>   _downstream;

    T                        _process;

    std::mutex               _process_mutex;
    std::deque<argument>     _process_message_queue;
    bool                     _process_running = false;
    std::size_t              _process_suspend_count = 0;
    bool                     _process_close_queue = false;
    // REVISIT (sparent) : I'm not certain final needs to be under the mutex
    bool                     _process_final = false;

    std::atomic_size_t       _sender_count;
    std::atomic_size_t       _receiver_count;

    /*
        Join is not yet implemented so single upstream for now. We don't just use
        a receiver for upstream because we need to be be able to kick the task
        chaining.
    */
    const std::shared_ptr<shared_process_receiver<argument>> _upstream;

    template <typename F>
    shared_process(F&& f) :
        _process(std::forward<F>(f))
    {
        _sender_count = 1;
        _receiver_count = !std::is_same<result, void>::value;
    }

    template <typename F>
    shared_process(F&& f, std::shared_ptr<shared_process_receiver<argument>> p) :
        _process(std::forward<F>(f)),
        _upstream(std::move(p))
    {
        _sender_count = 1;
        _receiver_count = !std::is_same<result, void>::value;
    }

    void add_sender() override {
        ++_sender_count;
    }
    void remove_sender() override {
        if (--_sender_count == 0) {
            bool do_run;
            {
                std::unique_lock<std::mutex> lock(_process_mutex);
                _process_close_queue = true;
                do_run = !_receiver_count && !_process_running;
                _process_running = _process_running || do_run;
            }
            if (do_run) run();
        }
    }

    void add_receiver() override {
        if (std::is_same<result, void>::value) return;
        ++_receiver_count;
    }

    void remove_receiver() override {
        if (std::is_same<result, void>::value) return;
        /*
            NOTE (sparent) : Decrementing the receiver count can allow this to start running on a
            send before we can get to the check - so we need to see if we are already running
            before starting again.
        */

        if (--_receiver_count == 0) {
            bool do_run;
            {
                std::unique_lock<std::mutex> lock(_process_mutex);
                do_run = (!_process_message_queue.empty() || _process_close_queue) && !_process_running;
                _process_running = _process_running || do_run;
            }
            if (do_run) run();
        }
    }

    void task_done() {
        bool do_run;
        bool do_final;
        {
        std::unique_lock<std::mutex> lock(_process_mutex);
        do_run = !_process_message_queue.empty() || _process_close_queue;
        _process_running = do_run;
        do_final = _process_final;
        }
        // The mutual exclusiveness of this assert implies too many variables. Should have a single
        // "get state" call.
        assert(!(do_run && do_final) && "ERROR (sparent) : can't run and close at the same time.");
        // I met him on a Monday and my heart stood still
        if (do_run) run();
        if (do_final) {
            std::unique_lock<std::mutex> lock(_downstream_mutex);
            _downstream.clear(); // This will propogate the close to anything downstream
        }
        // REVISIT (sparent) : process needs to be optional so it can be destructed here
        // _process~();
    }

    void cts() override {
        bool do_run = false;
        {
            std::unique_lock<std::mutex> lock(_process_mutex);
            --_process_suspend_count; // could be atomic?
            assert(_process_running && "ERROR (sparent) : cts but not running!");
            if (!_process_suspend_count) {
                // FIXME (sparent): This is calling the process state ender the lock.
                if (get_process_state(_process) == process_state::yield || !_process_message_queue.empty()
                        || _process_close_queue) {
                    do_run = true;
                } else {
                    _process_running = false;
                    do_run = false;
                }
            }
        }
        // Somebody told me that his name was Bill
        if (do_run) run();
    }

    bool dequeue() {
        boost::optional<argument> message; // TODO : make functional
        bool cts = false;
        bool do_close = false;
        {
            std::unique_lock<std::mutex> lock(_process_mutex);
            if (_process_message_queue.empty()) {
                std::swap(do_close, _process_close_queue);
                _process_final = do_close; // unravel after any yield
            } else {
                message = std::move(_process_message_queue.front());
                _process_message_queue.pop_front();
                cts = _process_message_queue.size() == 0; // TODO (sparent) : queue size - 1.
            }
        }
        if (cts && _upstream) _upstream->cts();
        if (message) _process.await(std::move(message.get()));
        else if (do_close) process_close(_process);
        return bool(message);
    }

    template <typename U>
    auto step() -> std::enable_if_t<has_process_yield<U>> {
        /*
            REVISIT (sparent) : Put a timer on this loop to limit it?
        */
        while (get_process_state(_process) != process_state::yield) {
            if (!dequeue()) break;
        }
        if (get_process_state(_process) == process_state::await) {
            task_done();
        } else {
            broadcast(_process.yield()); // after this point must call cts()
            cts();
        }
    }

    template <typename U>
    auto step() -> std::enable_if_t<!has_process_yield<U>> {
        boost::optional<argument> message; // TODO : make functional
        bool do_cts = false;
        bool do_close = false;
        {
            std::unique_lock<std::mutex> lock(_process_mutex);
            if (_process_message_queue.empty()) {
                std::swap(do_close, _process_close_queue);
                _process_final = do_close; // unravel after any yield
            } else {
                message = std::move(_process_message_queue.front());
                _process_message_queue.pop_front();
                do_cts = _process_message_queue.size() == 0; // TODO (sparent) : queue size - 1.
            }
        }
        if (do_cts && _upstream) _upstream->cts();
        if (message) {
            broadcast(avoid_invoke(_process, std::move(message.get())));
            cts();
        }
        else task_done();
    }

    void run() {
        default_scheduler()([_p = make_weak_ptr(this->shared_from_this())]{
            auto p = _p.lock();
            if (p) p->template step<T>();
        });
    }

    template <typename... A>
    void broadcast(const A&... args) {
        /*
            We broadcast the result to any processes currently attached to receiver
        */

        std::size_t n;
        {
        std::unique_lock<std::mutex> lock(_downstream_mutex);
        n = _downstream.size();
        }

        {
            /*
                There is no need for a lock here because the other processes that could change
                _process_suspend_count must all be ready (and not running).
                
                But we'll assert that to be sure!
            */
            // std::unique_lock<std::mutex> lock(_process_mutex);
            assert(_process_suspend_count == 0 && "broadcasting while suspended");
            _process_suspend_count = n + 1;
        }

        for_each_n(begin(_downstream), n, [&](const auto& e){
            e(args...);
        });
    }

    void send(argument arg) override {
        bool do_run;
        {
            std::unique_lock<std::mutex> lock(_process_mutex);
            _process_message_queue.emplace_back(std::move(arg)); // TODO (sparent) : overwrite here.
            
            do_run = !_receiver_count && !_process_running;
            _process_running = _process_running || do_run;

            #if 0
            running = running || _process_suspend_count;
            #endif
        }
        if (do_run) run();
    }

    void map(sender<result> f) override {
        /*
            REVISIT (sparent) : If we are in a final state then we should destruct the sender
            and not add it here.
        */
        {
        std::unique_lock<std::mutex> lock(_downstream_mutex);
        _downstream.emplace_back(f);
        }
    }
};

/**************************************************************************************************/

} // namespace detail

/**************************************************************************************************/

template <typename T>
auto channel() -> std::pair<sender<T>, receiver<T>> {
    auto p = std::make_shared<detail::shared_process<identity, T>>(identity());
    return std::make_pair(sender<T>(p), receiver<T>(p));
}

/**************************************************************************************************/

template <typename> class receiver;

template <typename T>
class receiver {
    using ptr_t = std::shared_ptr<detail::shared_process_receiver<T>>;

    ptr_t _p;
    bool _ready = false;

    template <typename U>
    friend class sender;

    template <typename U>
    friend class receiver; // huh?

    template <typename U>
    friend auto channel() -> std::pair<sender<U>, receiver<U>>;

    receiver(ptr_t p) : _p(std::move(p)) { }

  public:
    receiver() = default;

    ~receiver() {
        if (!_ready && _p) _p->remove_receiver();
    }

    receiver(const receiver& x) : _p(x._p) {
        if (_p) _p->add_sender();
    }
    receiver(receiver&&) noexcept = default;

    receiver& operator=(const receiver& x) {
        auto tmp = x; *this = std::move(tmp); return *this;
    }
    receiver& operator=(receiver&& x) noexcept = default;

    void set_ready() {
        if (!_ready && _p) _p->remove_receiver();
        _ready = true;
    }

    bool ready() { return _ready; }

    template <typename F>
    auto operator|(F&& f) const {
        // TODO - report error if not constructed or _ready.
        auto p = std::make_shared<detail::shared_process<F, T>>(std::forward<F>(f), _p);
        _p->map(sender<T>(p));
        return receiver<detail::yield_type<F, T>>(std::move(p));
    }
};

/**************************************************************************************************/

template <typename T>
class sender {
    using ptr_t = std::weak_ptr<detail::shared_process_sender<T>>;
    ptr_t _p;

    template <typename U>
    friend class receiver;

    template <typename U>
    friend auto channel() -> std::pair<sender<U>, receiver<U>>;

    sender(ptr_t p) : _p(std::move(p)) { }

  public:
    sender() = default;

    ~sender() {
        auto p = _p.lock();
        if (p) p->remove_sender();
    }

    sender(const sender& x) : _p(x._p) {
        auto p = _p.lock();
        if (p) p->add_sender();
    }

    sender(sender&&) noexcept = default;
    sender& operator=(const sender& x) {
        auto tmp = x; *this = std::move(tmp); return *this;
    }
    sender& operator=(sender&& x) noexcept = default;

    void close() {
        auto p = _p.lock();
        if (p) p->remove_sender();
        _p.reset();
    }

    template <typename... A>
    void operator()(A&&... args) const {
        auto p = _p.lock();
        if (p) p->send(std::forward<A>(args)...);
    }
};

/**************************************************************************************************/

template <typename F> struct function_process;

template <typename R, typename... Args>
struct function_process<R (Args...)> {
    std::function<R (Args...)> _f;
    std::function<R ()> _bound;
    bool _done = true;

    using signature = R(Args...);

    template <typename F>
    function_process(F&& f) : _f(std::forward<F>(f)) { }

    template <typename... A>
    void await(A&&... args) {
        _bound = std::bind(_f, std::forward<A>(args)...);
        _done = false;
    }

    R yield() { _done = true; return _bound(); }
    process_state state() const { return _done ? process_state::await : process_state::yield; }
};

/**************************************************************************************************/

} // namespace stlab

/**************************************************************************************************/

#endif

/**************************************************************************************************/
