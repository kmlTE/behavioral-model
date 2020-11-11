/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Antonin Bas (antonin@barefootnetworks.com)
 *
 */

//! @file queueing.h
//! This file contains convenience classes that can be useful for targets that
//! wish to queue packets at some point during processing (for example, between
//! an ingress pipeline and an egress pipeline, as is the case for the standard
//! simple switch target). We realized that if one decided to use the bm::Queue
//! class (in queue.h) to achieve this, quite a lot of work was required, even
//! for the standard, basic case: one queue per egress port, with a limited
//! number of threads processing all the queues.

#ifndef BM_BM_SIM_QUEUEING_H_
#define BM_BM_SIM_QUEUEING_H_

#include <deque>
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>  // for std::max

#include "logger.h"

namespace bm {

//! One of the most basic queueing block possible. Lets you choose (at runtime)
//! the desired number of logical queues and the number of worker threads that
//! will be reading from these queues. I write "logical queues" because the
//! implementation actually uses as many physical queues as there are worker
//! threads. However, each logical queue still has its own maximum capacity.
//! As of now, the behavior is blocking for both read (pop_back()) and write
//! (push_front()), but we may offer additional options if there is interest
//! expressed in the future.
//!
//! Template parameter `T` is the type (has to be movable) of the objects that
//! will be stored in the queues. Template parameter `FMap` is a callable object
//! that has to be able to map every logical queue id to a worker id. The
//! following is a good example of functor that meets the requirements:
//! @code
//! struct WorkerMapper {
//!   WorkerMapper(size_t nb_workers)
//!       : nb_workers(nb_workers) { }
//!
//!   size_t operator()(size_t queue_id) const {
//!     return queue_id % nb_workers;
//!   }
//!
//!   size_t nb_workers;
//! };
//! @endcode
template <typename T, typename FMap>
class QueueingLogic {
 public:
  //! \p nb_queues is the number of logical queues; each queue is identified by
  //! an id in the range `[0, nb_queues)` when pushing to the queue. \p
  //! nb_workers is the number of threads that will be consuming from the
  //! queues; they will be identified by an id in the range `[0,
  //! nb_workers)`. \p capacity is the number of objects that each logical queue
  //! can hold. Because we need to be able to map each queue id to a worker id,
  //! the user has to provide a callable object of type `FMap`, \p
  //! map_to_worker, that can do this mapping. See the QueueingLogic class
  //! description for more information about the `FMap` template parameter.
  QueueingLogic(size_t nb_queues, size_t nb_workers, size_t capacity,
                FMap map_to_worker)
      : nb_queues(nb_queues), nb_workers(nb_workers),
        queues_info(nb_queues), workers_info(nb_workers),
        map_to_worker(std::move(map_to_worker)) {
    for (auto &q_info : queues_info)
      q_info.capacity = capacity;
  }

  //! Makes a copy of \p item and pushes it to the front of the logical queue
  //! with id \p queue_id.
  void push_front(size_t queue_id, const T &item) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    std::unique_lock<std::mutex> lock(w_info.q_mutex);
    while (q_info.size >= q_info.capacity) {
      q_info.q_not_full.wait(lock);
    }
    w_info.queue.emplace_front(item, queue_id);
    q_info.size++;
    w_info.q_not_empty.notify_one();
  }

  //! Moves \p item to the front of the logical queue with id \p queue_id.
  void push_front(size_t queue_id, T &&item) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    std::unique_lock<std::mutex> lock(w_info.q_mutex);
    while (q_info.size >= q_info.capacity) {
      q_info.q_not_full.wait(lock);
    }
    w_info.queue.emplace_front(std::move(item), queue_id);
    q_info.size++;
    w_info.q_not_empty.notify_one();
  }

  //! Retrieves the oldest element for the worker thread indentified by \p
  //! worker_id and moves it to \p pItem. The id of the logical queue which
  //! contained this element is copied to \p queue_id. As a remainder, the
  //! `map_to_worker` argument provided when constructing the class is used to
  //! map every queue id to the corresponding worker id. Therefore, if an
  //! element `E` was pushed to queue `queue_id`, you need to use the worker id
  //! `map_to_worker(queue_id)` to retrieve it with this function.
  void pop_back(size_t worker_id, size_t *queue_id, T *pItem) {
    auto &w_info = workers_info.at(worker_id);
    auto &queue = w_info.queue;
    std::unique_lock<std::mutex> lock(w_info.q_mutex);
    while (queue.size() == 0) {
      w_info.q_not_empty.wait(lock);
    }
    *queue_id = queue.back().queue_id;
    *pItem = std::move(queue.back().e);
    queue.pop_back();
    auto &q_info = queues_info.at(*queue_id);
    q_info.size--;
    q_info.q_not_full.notify_one();
  }

  //! Get the occupancy of the logical queue with id \p queue_id.
  size_t size(size_t queue_id) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    std::unique_lock<std::mutex> lock(w_info.q_mutex);
    return q_info.size;
  }

  //! Set the capacity of the logical queue with id \p queue_id to \p c
  //! elements.
  void set_capacity(size_t queue_id, size_t c) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    std::unique_lock<std::mutex> lock(w_info.q_mutex);
    q_info.capacity = c;
  }

  //! Deleted copy constructor
  QueueingLogic(const QueueingLogic &) = delete;
  //! Deleted copy assignment operator
  QueueingLogic &operator =(const QueueingLogic &) = delete;

  //! Deleted move constructor
  QueueingLogic(QueueingLogic &&) = delete;
  //! Deleted move assignment operator
  QueueingLogic &&operator =(QueueingLogic &&) = delete;

 private:
  struct QE {
    QE(T e, size_t queue_id)
        : e(std::move(e)), queue_id(queue_id) { }

    T e;
    size_t queue_id;
  };

  using MyQ = std::deque<QE>;

  struct QueueInfo {
    size_t size{0};
    size_t capacity{0};
    mutable std::condition_variable q_not_full{};
  };

  struct WorkerInfo {
    MyQ queue{};
    mutable std::mutex q_mutex{};
    mutable std::condition_variable q_not_empty{};
  };

  size_t nb_queues;
  size_t nb_workers;
  std::vector<QueueInfo> queues_info;
  std::vector<WorkerInfo> workers_info;
  FMap map_to_worker;
};


//! This class is slightly more advanced than QueueingLogic. The difference
//! between the 2 is that this one offers the ability to rate-limit every
//! logical queue, by providing a maximum number of elements consumed per
//! second. If the rate is too small compared to the incoming packet rate, or if
//! the worker thread cannot sustain the desired rate, elements are buffered in
//! the queue. However, the write behavior (push_front()) for this class is
//! different than the one for QueueingLogic. It is not blocking: if the queue
//! is full, the function will return immediately and the element will not be
//! queued. Look at the documentation for QueueingLogic for more information
//! about the template parameters (they are the same).
//! This is the queueing logic used by the standard simple_switch target.
template <typename T, typename FMap>
class QueueingLogicRL {
 public:
  //! @copydoc QueueingLogic::QueueingLogic()
  //!
  //! Initially, none of the logical queues will be rate-limited, i.e. the
  //! instance will behave as an instance of QueueingLogic.
  QueueingLogicRL(size_t nb_queues, size_t nb_workers, size_t capacity,
                  FMap map_to_worker)
      : nb_queues(nb_queues), nb_workers(nb_workers),
        queues_info(nb_queues), workers_info(nb_workers),
        map_to_worker(std::move(map_to_worker)) {
    auto now = clock::now();
    for (auto &q_info : queues_info) {
      q_info.capacity = capacity;
      q_info.last_sent = now;
    }
  }

  //! If the logical queue with id \p queue_id is full, the function will return
  //! `0` immediately. Otherwise, \p item will be copied to the front of the
  //! logical queue and the function will return `1`.
  int push_front(size_t queue_id, const T &item) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    std::unique_lock<std::mutex> lock(w_info.q_mutex);
    if (q_info.size >= q_info.capacity) return 0;
    q_info.last_sent = get_next_tp(q_info);
    // w_info.queue.emplace(item, queue_id, q_info.last_sent, id++);
    w_info.queue.emplace(item, queue_id, q_info.last_sent);
    q_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  //! Same as push_front(size_t queue_id, const T &item), but \p item is moved
  //! instead of copied.
  int push_front(size_t queue_id, T &&item) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    std::unique_lock<std::mutex> lock(w_info.q_mutex);
    if (q_info.size >= q_info.capacity) return 0;
    q_info.last_sent = get_next_tp(q_info);
    // w_info.queue.emplace(std::move(item), queue_id, q_info.last_sent, id++);
    w_info.queue.emplace(std::move(item), queue_id, q_info.last_sent);
    q_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  //! Retrieves the oldest element for the worker thread indentified by \p
  //! worker_id and moves it to \p pItem. The id of the logical queue which
  //! contained this element is copied to \p queue_id. Note that this function
  //! will block until 1) an element is available 2) this element is free to
  //! leave the queue according to the rate limiter.
  void pop_back(size_t worker_id, size_t *queue_id, T *pItem) {
    auto &w_info = workers_info.at(worker_id);
    auto &queue = w_info.queue;
    std::unique_lock<std::mutex> lock(w_info.q_mutex);
    while (true) {
      if (queue.size() == 0) {
        w_info.q_not_empty.wait(lock);
      } else {
        if (queue.top().send <= clock::now()) break;
        w_info.q_not_empty.wait_until(lock, queue.top().send);
      }
    }
    *queue_id = queue.top().queue_id;
    // TODO(antonin): improve / document this
    // http://stackoverflow.com/questions/20149471/move-out-element-of-std-priority-queue-in-c11
    *pItem = std::move(const_cast<QE &>(queue.top()).e);
    queue.pop();
    auto &q_info = queues_info.at(*queue_id);
    q_info.size--;
  }

  //! @copydoc QueueingLogic::size
  size_t size(size_t queue_id) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    std::unique_lock<std::mutex> lock(w_info.q_mutex);
    return q_info.size;
  }

  //! @copydoc QueueingLogic::set_capacity
  void set_capacity(size_t queue_id, size_t c) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    std::unique_lock<std::mutex> lock(w_info.q_mutex);
    q_info.capacity = c;
  }

  //! Set the maximum rate of the logical queue with id \p queue_id to \p
  //! pps. \p pps is expressed in "number of elements per second". Until this
  //! function is called, there will be no rate limit for the queue.
  void set_rate(size_t queue_id, uint64_t pps) {
    using std::chrono::duration;
    using std::chrono::duration_cast;
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    std::unique_lock<std::mutex> lock(w_info.q_mutex);
    q_info.queue_rate_pps = pps;
    q_info.pkt_delay_ticks = duration_cast<ticks>(duration<double>(1. / pps));
  }

  //! Deleted copy constructor
  QueueingLogicRL(const QueueingLogicRL &) = delete;
  //! Deleted copy assignment operator
  QueueingLogicRL &operator =(const QueueingLogicRL &) = delete;

  //! Deleted move constructor
  QueueingLogicRL(QueueingLogicRL &&) = delete;
  //! Deleted move assignment operator
  QueueingLogicRL &&operator =(QueueingLogicRL &&) = delete;

 private:
  using ticks = std::chrono::nanoseconds;
  // clock choice? switch to steady if observing re-ordering
  // using clock = std::chrono::steady_clock;
  using clock = std::chrono::high_resolution_clock;

  struct QE {
    // QE(T e, size_t queue_id, const clock::time_point &send, size_t id)
    //     : e(std::move(e)), queue_id(queue_id), send(send), id(id) { }
    QE(T e, size_t queue_id, const clock::time_point &send)
        : e(std::move(e)), queue_id(queue_id), send(send) { }

    T e;
    size_t queue_id;
    clock::time_point send;
    // size_t id;
  };

  struct QEComp {
    bool operator()(const QE &lhs, const QE &rhs) const {
      // return (lhs.send == rhs.send) ? lhs.id > rhs.id : lhs.send > rhs.send;
      return lhs.send > rhs.send;
    }
  };

  // performance seems to be roughly the same for deque vs vector
  using MyQ = std::priority_queue<QE, std::deque<QE>, QEComp>;
  // using MyQ = std::priority_queue<QE, std::vector<QE>, QEComp>;

  struct QueueInfo {
    size_t size{0};
    size_t capacity{0};
    uint64_t queue_rate_pps{};
    // interesting to note that {0} fails with g++4.8, but not with g++4.9
    // did not get to the root of it, but could be because of an explicit
    // constructor somewhere in the std lib implementation used
    ticks pkt_delay_ticks{ticks::zero()};
    clock::time_point last_sent{};
  };

  struct WorkerInfo {
    MyQ queue{};
    mutable std::mutex q_mutex{};
    mutable std::condition_variable q_not_empty{};
  };

  clock::time_point get_next_tp(const QueueInfo &q_info) {
    return std::max(clock::now(), q_info.last_sent + q_info.pkt_delay_ticks);
  }

  size_t nb_queues;
  size_t nb_workers;
  std::vector<QueueInfo> queues_info;
  std::vector<WorkerInfo> workers_info;
  FMap map_to_worker;
  // size_t id{0};
};


//! This class is slightly more advanced than QueueingLogicRL. The difference
//! between the 2 is that this one offers the ability to set several priority
//! queues for each logical queue. Priority queues are numbered from `0` to
//! `nb_priorities` (see QueueingLogicPriRL::QueueingLogicPriRL()). Priority `0`
//! is the highest priority queue. Each priority queue can have its own rate and
//! its own capacity. Queues will be served in order of priority, until their
//! respective maximum rate is reached. If no maximum rate is set, queues with a
//! high priority can starve lower-priority queues. For example, if the queue
//! with priority `0` always contains at least one element, the other queues
//! will never be served.
//! As for QueueingLogicRL, the write behavior (push_front()) is blocking: once
//! a logical queue is full, subsequent incoming elements will be dropped until
//! the queue starts draining again.
//! Look at the documentation for QueueingLogic for more information about the
//! template parameters (they are the same).
template <typename T, typename FMap>
class QueueingLogicPriRL {
  using MutexType = std::mutex;
  using LockType = std::unique_lock<MutexType>;

 public:
  //! See QueueingLogic::QueueingLogicRL() for an introduction. The difference
  //! here is that each logical queues can receive several priority queues (as
  //! determined by \p nb_priorities, which is set to `2` by default). Each of
  //! these priority queues will initially be able to hold \p capacity
  //! elements. The capacity of each priority queue can be changed later by
  //! using set_capacity(size_t queue_id, size_t priority, size_t c).
  QueueingLogicPriRL(size_t nb_queues, size_t nb_workers, size_t capacity,
                     FMap map_to_worker, size_t nb_priorities = 2)
      : nb_queues(nb_queues), nb_workers(nb_workers),
        workers_info(nb_workers),
        map_to_worker(std::move(map_to_worker)),
        nb_priorities(nb_priorities) {
    auto now = clock::now();
    for (size_t i = 0; i < nb_queues; i++) {
      QueueInfoPri v = {0, capacity, 0, ticks::zero(), now};
      queues_info.emplace_back(nb_priorities, v);
    }
  }

  //! If priority queue \p priority of logical queue \p queue_id is full, the
  //! function will return `0` immediately. Otherwise, \p item will be copied to
  //! the queue and the function will return `1`. If \p queue_id or \p priority
  //! are incorrect, an exception of type std::out_of_range will be thrown (same
  //! if the FMap object provided to the constructor does not behave correctly).
  int push_front(size_t queue_id, size_t priority, const T &item) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    if (q_info_pri.size >= q_info_pri.capacity) return 0;
    q_info_pri.last_sent = get_next_tp(q_info_pri);
    w_info.queues[priority].emplace(item, queue_id, q_info_pri.last_sent);
    q_info_pri.size++;
    q_info.size++;
    w_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  int push_front(size_t queue_id, const T &item) {
    return push_front(queue_id, 0, item);
  }

  //! Same as push_front(size_t queue_id, size_t priority, const T &item), but
  //! \p item is moved instead of copied.
  int push_front(size_t queue_id, size_t priority, T &&item) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    if (q_info_pri.size >= q_info_pri.capacity) return 0;
    q_info_pri.last_sent = get_next_tp(q_info_pri);
    w_info.queues[priority].emplace(std::move(item), queue_id,
                                    q_info_pri.last_sent);
    q_info_pri.size++;
    q_info.size++;
    w_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  int push_front(size_t queue_id, T &&item) {
    return push_front(queue_id, 0, std::move(item));
  }

  //! Retrieves an element for the worker thread indentified by \p worker_id and
  //! moves it to \p pItem. The id of the logical queue which contained this
  //! element is copied to \p queue_id and the priority value of the served
  //! queue is copied to \p priority.
  //! Elements are retrieved according to the priority queue they are in
  //! (highest priorities, i.e. lowest priority values, are served first). Once
  //! a given priority queue reaches its maximum rate, the next queue is served.
  //! If no elements are available (either the queues are empty or they have
  //! exceeded their rate already), the function will block.
  void pop_back(size_t worker_id, size_t *queue_id, size_t *priority,
                T *pItem) {
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    MyQ *queue = nullptr;
    size_t pri;
    while (true) {
      if (w_info.size == 0) {
        w_info.q_not_empty.wait(lock);
      } else {
        auto now = clock::now();
        auto next = clock::time_point::max();
        for (pri = 0; pri < nb_priorities; pri++) {
          auto &q = w_info.queues[pri];
          if (q.size() == 0) continue;
          if (q.top().send <= now) {
            queue = &q;
            break;
          }
          next = std::min(next, q.top().send);
        }
        if (queue) break;
        w_info.q_not_empty.wait_until(lock, next);
      }
    }
    *queue_id = queue->top().queue_id;
    *priority = pri;
    // TODO(antonin): improve / document this
    // http://stackoverflow.com/questions/20149471/move-out-element-of-std-priority-queue-in-c11
    *pItem = std::move(const_cast<QE &>(queue->top()).e);
    queue->pop();
    auto &q_info = queues_info.at(*queue_id);
    auto &q_info_pri = q_info.at(*priority);
    q_info_pri.size--;
    q_info.size--;
    w_info.size--;
  }

  //! Same as
  //! pop_back(size_t worker_id, size_t *queue_id, size_t *priority, T *pItem),
  //! but the priority of the popped element is discarded.
  void pop_back(size_t worker_id, size_t *queue_id, T *pItem) {
    size_t priority;
    return pop_back(worker_id, queue_id, &priority, pItem);
  }

  //! @copydoc QueueingLogic::size
  //! The occupancies of all the priority queues for this logical queue are
  //! added.
  size_t size(size_t queue_id) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    return q_info.size;
  }

  //! Get the occupancy of priority queue \p priority for logical queue with id
  //! \p queue_id.
  size_t size(size_t queue_id, size_t priority) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &q_info_pri = q_info.at(priority);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    return q_info_pri.size;
  }

  //! Set the capacity of all the priority queues for logical queue \p queue_id
  //! to \p c elements.
  void set_capacity(size_t queue_id, size_t c) {
    for_each_q(queue_id, SetCapacityFn(c));
  }

  //! Set the capacity of priority queue \p priority for logical queue \p
  //! queue_id to \p c elements.
  void set_capacity(size_t queue_id, size_t priority, size_t c) {
    for_one_q(queue_id, priority, SetCapacityFn(c));
  }

  //! Set the maximum rate of all the priority queues for logical queue \p
  //! queue_id to \p pps. \p pps is expressed in "number of elements per
  //! second". Until this function is called, there will be no rate limit for
  //! the queue.
  void set_rate(size_t queue_id, uint64_t pps) {
    for_each_q(queue_id, SetRateFn(pps));
  }

  //! Same as set_rate(size_t queue_id, uint64_t pps) but only applies to the
  //! given priority queue.
  void set_rate(size_t queue_id, size_t priority, uint64_t pps) {
    for_one_q(queue_id, priority, SetRateFn(pps));
  }

  //! Deleted copy constructor
  QueueingLogicPriRL(const QueueingLogicPriRL &) = delete;
  //! Deleted copy assignment operator
  QueueingLogicPriRL &operator =(const QueueingLogicPriRL &) = delete;

  //! Deleted move constructor
  QueueingLogicPriRL(QueueingLogicPriRL &&) = delete;
  //! Deleted move assignment operator
  QueueingLogicPriRL &&operator =(QueueingLogicPriRL &&) = delete;

 private:
  using ticks = std::chrono::nanoseconds;
  // clock choice? switch to steady if observing re-ordering
  // using clock = std::chrono::steady_clock;
  using clock = std::chrono::high_resolution_clock;

  struct QE {
    QE(T e, size_t queue_id, const clock::time_point &send)
        : e(std::move(e)), queue_id(queue_id), send(send) { }

    T e;
    size_t queue_id;
    clock::time_point send;
  };

  struct QEComp {
    bool operator()(const QE &lhs, const QE &rhs) const {
      return lhs.send > rhs.send;
    }
  };

  using MyQ = std::priority_queue<QE, std::deque<QE>, QEComp>;

  struct QueueInfoPri {
    size_t size;
    size_t capacity;
    uint64_t queue_rate_pps;
    ticks pkt_delay_ticks;
    clock::time_point last_sent;
  };

  struct QueueInfo : public std::vector<QueueInfoPri> {
    QueueInfo(size_t nb_priorities, const QueueInfoPri &v)
        : std::vector<QueueInfoPri>(nb_priorities, v) { }

    size_t size{0};
  };

  struct WorkerInfo {
    mutable std::mutex q_mutex{};
    mutable std::condition_variable q_not_empty{};
    size_t size{0};
    std::array<MyQ, 32> queues;
  };

  clock::time_point get_next_tp(const QueueInfoPri &q_info_pri) {
    return std::max(clock::now(),
                    q_info_pri.last_sent + q_info_pri.pkt_delay_ticks);
  }

  template <typename Function>
  Function for_each_q(size_t queue_id, Function fn) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    for (auto &q_info_pri : q_info) {
      fn(q_info_pri);
    }
    return std::move(fn);
  }

  template <typename Function>
  Function for_one_q(size_t queue_id, size_t priority, Function fn) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    fn(q_info_pri);
    return std::move(fn);
  }

  struct SetCapacityFn {
    explicit SetCapacityFn(size_t c)
        : c(c) { }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.capacity = c;
    }

    size_t c;
  };

  struct SetRateFn {
    explicit SetRateFn(uint64_t pps)
        : pps(pps) {
      using std::chrono::duration;
      using std::chrono::duration_cast;
      pkt_delay_ticks = (pps == 0)
                        ? duration_cast<ticks>(duration<double>(0.0))
                        : duration_cast<ticks>(duration<double>(1. / pps));
    }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.queue_rate_pps = pps;
      info.pkt_delay_ticks = pkt_delay_ticks;
    }

    uint64_t pps;
    ticks pkt_delay_ticks;
  };

  size_t nb_queues;
  size_t nb_workers;
  std::vector<QueueInfo> queues_info{};
  std::vector<WorkerInfo> workers_info{};
  std::vector<MyQ> queues{};
  FMap map_to_worker;
  size_t nb_priorities;
};


// same as QueueingLogicPriRL but rate is Bytes per second
template <typename T, typename FMap>
class QueueingLogicPriBytesRL {
  using MutexType = std::mutex;
  using LockType = std::unique_lock<MutexType>;

 public:
  //! See QueueingLogic::QueueingLogicRL() for an introduction. The difference
  //! here is that each logical queues can receive several priority queues (as
  //! determined by \p nb_priorities, which is set to `2` by default). Each of
  //! these priority queues will initially be able to hold \p capacity
  //! elements. The capacity of each priority queue can be changed later by
  //! using set_capacity(size_t queue_id, size_t priority, size_t c).
  QueueingLogicPriBytesRL(size_t nb_queues, size_t nb_workers, size_t capacity,
                     FMap map_to_worker, size_t nb_priorities = 2)
      : nb_queues(nb_queues), nb_workers(nb_workers),
        workers_info(nb_workers),
        map_to_worker(std::move(map_to_worker)),
        nb_priorities(nb_priorities) {
    auto now = clock::now();
    for (size_t i = 0; i < nb_queues; i++) {
      QueueInfoPri v = {0, capacity, 0, ticks::zero(), now};
      queues_info.emplace_back(nb_priorities, v);
    }
  }

  //! If priority queue \p priority of logical queue \p queue_id is full, the
  //! function will return `0` immediately. Otherwise, \p item will be copied to
  //! the queue and the function will return `1`. If \p queue_id or \p priority
  //! are incorrect, an exception of type std::out_of_range will be thrown (same
  //! if the FMap object provided to the constructor does not behave correctly).
  int push_front(size_t queue_id, size_t priority, const T &item, int pkt_size) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    if (q_info_pri.size >= q_info_pri.capacity) return 0;
    q_info_pri.last_sent = get_next_tp(q_info_pri, pkt_size);
    w_info.queues[priority].emplace(item, queue_id, q_info_pri.last_sent);
    q_info_pri.size++;
    q_info.size++;
    w_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  int push_front(size_t queue_id, const T &item, int pkt_size) {
    return push_front(queue_id, 0, item, pkt_size);
  }

  //! Same as push_front(size_t queue_id, size_t priority, const T &item), but
  //! \p item is moved instead of copied.
  int push_front(size_t queue_id, size_t priority, T &&item, int pkt_size) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    if (q_info_pri.size >= q_info_pri.capacity) return 0;
    q_info_pri.last_sent = get_next_tp(q_info_pri, pkt_size);
    w_info.queues[priority].emplace(std::move(item), queue_id,
                                    q_info_pri.last_sent);
    q_info_pri.size++;
    q_info.size++;
    w_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  int push_front(size_t queue_id, T &&item, int pkt_size) {
    return push_front(queue_id, 0, std::move(item), pkt_size);
  }

  //! Retrieves an element for the worker thread indentified by \p worker_id and
  //! moves it to \p pItem. The id of the logical queue which contained this
  //! element is copied to \p queue_id and the priority value of the served
  //! queue is copied to \p priority.
  //! Elements are retrieved according to the priority queue they are in
  //! (highest priorities, i.e. lowest priority values, are served first). Once
  //! a given priority queue reaches its maximum rate, the next queue is served.
  //! If no elements are available (either the queues are empty or they have
  //! exceeded their rate already), the function will block.
  void pop_back(size_t worker_id, size_t *queue_id, size_t *priority,
                T *pItem) {
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    MyQ *queue = nullptr;
    size_t pri;
    while (true) {
      if (w_info.size == 0) {
        w_info.q_not_empty.wait(lock);
      } else {
        auto now = clock::now();
        auto next = clock::time_point::max();
        for (pri = 0; pri < nb_priorities; pri++) {
          auto &q = w_info.queues[pri];
          if (q.size() == 0) continue;
          if (q.top().send <= now) {
            queue = &q;
            break;
          }
          next = std::min(next, q.top().send);
        }
        if (queue) break;
        w_info.q_not_empty.wait_until(lock, next);
      }
    }
    *queue_id = queue->top().queue_id;
    *priority = pri;
    // TODO(antonin): improve / document this
    // http://stackoverflow.com/questions/20149471/move-out-element-of-std-priority-queue-in-c11
    *pItem = std::move(const_cast<QE &>(queue->top()).e);
    queue->pop();
    auto &q_info = queues_info.at(*queue_id);
    auto &q_info_pri = q_info.at(*priority);
    q_info_pri.size--;
    q_info.size--;
    w_info.size--;
  }

  //! Same as
  //! pop_back(size_t worker_id, size_t *queue_id, size_t *priority, T *pItem),
  //! but the priority of the popped element is discarded.
  void pop_back(size_t worker_id, size_t *queue_id, T *pItem) {
    size_t priority;
    return pop_back(worker_id, queue_id, &priority, pItem);
  }

  //! @copydoc QueueingLogic::size
  //! The occupancies of all the priority queues for this logical queue are
  //! added.
  size_t size(size_t queue_id) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    return q_info.size;
  }

  //! Get the occupancy of priority queue \p priority for logical queue with id
  //! \p queue_id.
  size_t size(size_t queue_id, size_t priority) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &q_info_pri = q_info.at(priority);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    return q_info_pri.size;
  }

  //! Set the capacity of all the priority queues for logical queue \p queue_id
  //! to \p c elements.
  void set_capacity(size_t queue_id, size_t c) {
    for_each_q(queue_id, SetCapacityFn(c));
  }

  //! Set the capacity of priority queue \p priority for logical queue \p
  //! queue_id to \p c elements.
  void set_capacity(size_t queue_id, size_t priority, size_t c) {
    for_one_q(queue_id, priority, SetCapacityFn(c));
  }

  //! Set the maximum rate of all the priority queues for logical queue \p
  //! queue_id to \p Bps. \p Bps is expressed in "number of Bytes per
  //! second". Until this function is called, there will be no rate limit for
  //! the queue.
  void set_rate(size_t queue_id, uint64_t Bps) {
    for_each_q(queue_id, SetRateFn(Bps));
  }

  //! Same as set_rate(size_t queue_id, uint64_t Bps) but only applies to the
  //! given priority queue.
  void set_rate(size_t queue_id, size_t priority, uint64_t Bps) {
    for_one_q(queue_id, priority, SetRateFn(Bps));
  }

  //! Deleted copy constructor
  QueueingLogicPriBytesRL(const QueueingLogicPriBytesRL &) = delete;
  //! Deleted copy assignment operator
  QueueingLogicPriBytesRL &operator =(const QueueingLogicPriBytesRL &) = delete;

  //! Deleted move constructor
  QueueingLogicPriBytesRL(QueueingLogicPriBytesRL &&) = delete;
  //! Deleted move assignment operator
  QueueingLogicPriBytesRL &&operator =(QueueingLogicPriBytesRL &&) = delete;

 private:
  using ticks = std::chrono::nanoseconds;
  // clock choice? switch to steady if observing re-ordering
  // using clock = std::chrono::steady_clock;
  using clock = std::chrono::high_resolution_clock;

  struct QE {
    QE(T e, size_t queue_id, const clock::time_point &send)
        : e(std::move(e)), queue_id(queue_id), send(send) { }

    T e;
    size_t queue_id;
    clock::time_point send;
  };

  struct QEComp {
    bool operator()(const QE &lhs, const QE &rhs) const {
      return lhs.send > rhs.send;
    }
  };

  using MyQ = std::priority_queue<QE, std::deque<QE>, QEComp>;

  struct QueueInfoPri {
    size_t size;
    size_t capacity;
    uint64_t queue_rate_Bps;
    ticks pkt_delay_ticks;
    clock::time_point last_sent;
  };

  struct QueueInfo : public std::vector<QueueInfoPri> {
    QueueInfo(size_t nb_priorities, const QueueInfoPri &v)
        : std::vector<QueueInfoPri>(nb_priorities, v) { }

    size_t size{0};
  };

  struct WorkerInfo {
    mutable std::mutex q_mutex{};
    mutable std::condition_variable q_not_empty{};
    size_t size{0};
    std::array<MyQ, 32> queues;
  };

  clock::time_point get_next_tp(const QueueInfoPri &q_info_pri, int pkt_size) {
    return std::max(clock::now(),
                    q_info_pri.last_sent + q_info_pri.pkt_delay_ticks * pkt_size);
  }

  template <typename Function>
  Function for_each_q(size_t queue_id, Function fn) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    for (auto &q_info_pri : q_info) {
      fn(q_info_pri);
    }
    return std::move(fn);
  }

  template <typename Function>
  Function for_one_q(size_t queue_id, size_t priority, Function fn) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    fn(q_info_pri);
    return std::move(fn);
  }

  struct SetCapacityFn {
    explicit SetCapacityFn(size_t c)
        : c(c) { }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.capacity = c;
    }

    size_t c;
  };

  struct SetRateFn {
    explicit SetRateFn(uint64_t Bps)
        : Bps(Bps) {
      using std::chrono::duration;
      using std::chrono::duration_cast;
      pkt_delay_ticks = (Bps == 0)
                        ? duration_cast<ticks>(duration<double>(0.0))
                        : duration_cast<ticks>(duration<double>(1. / Bps));
    }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.queue_rate_Bps = Bps;
      info.pkt_delay_ticks = pkt_delay_ticks;
    }

    uint64_t Bps;
    ticks pkt_delay_ticks;
  };

  size_t nb_queues;
  size_t nb_workers;
  std::vector<QueueInfo> queues_info{};
  std::vector<WorkerInfo> workers_info{};
  std::vector<MyQ> queues{};
  FMap map_to_worker;
  size_t nb_priorities;
};


// if there is only one active queue , then it will have no rate limit,
// as soon as there is another active queue, rate limit
template <typename T, typename FMap>
class QueueingLogicPriBytesRL_AQ {
  using MutexType = std::mutex;
  using LockType = std::unique_lock<MutexType>;

 public:
  //! See QueueingLogic::QueueingLogicRL() for an introduction. The difference
  //! here is that each logical queues can receive several priority queues (as
  //! determined by \p nb_priorities, which is set to `2` by default). Each of
  //! these priority queues will initially be able to hold \p capacity
  //! elements. The capacity of each priority queue can be changed later by
  //! using set_capacity(size_t queue_id, size_t priority, size_t c).
  QueueingLogicPriBytesRL_AQ(size_t nb_queues, size_t nb_workers, size_t capacity,
                     FMap map_to_worker, size_t nb_priorities = 2)
      : nb_queues(nb_queues), nb_workers(nb_workers),
        workers_info(nb_workers),
        map_to_worker(std::move(map_to_worker)),
        nb_priorities(nb_priorities) {
    auto now = clock::now();
    for (size_t i = 0; i < nb_queues; i++) {
      QueueInfoPri v = {0, capacity, 0, ticks::zero(), now};
      queues_info.emplace_back(nb_priorities, v);
    }
  }

  //! If priority queue \p priority of logical queue \p queue_id is full, the
  //! function will return `0` immediately. Otherwise, \p item will be copied to
  //! the queue and the function will return `1`. If \p queue_id or \p priority
  //! are incorrect, an exception of type std::out_of_range will be thrown (same
  //! if the FMap object provided to the constructor does not behave correctly).
  int push_front(size_t queue_id, size_t priority, const T &item, int pkt_size) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    if (q_info_pri.size >= q_info_pri.capacity) return 0;

    // if there is only one queue active, give the full bandwidth, 
    // if not, follow the rate limit rules
    q_info_pri.last_sent = (q_info.size == q_info_pri.size) ? get_next_tp(q_info_pri, 0)
                                                            : get_next_tp(q_info_pri, pkt_size);

    w_info.queues[priority].emplace(item, queue_id, q_info_pri.last_sent);
    q_info_pri.size++;
    q_info.size++;
    w_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  int push_front(size_t queue_id, const T &item, int pkt_size) {
    return push_front(queue_id, 0, item, pkt_size);
  }

  //! Same as push_front(size_t queue_id, size_t priority, const T &item), but
  //! \p item is moved instead of copied.
  int push_front(size_t queue_id, size_t priority, T &&item, int pkt_size) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    if (q_info_pri.size >= q_info_pri.capacity) return 0;

    // if there is only one queue active, give the full bandwidth, 
    // if not, follow the rate limit rules
    q_info_pri.last_sent = (q_info.size == q_info_pri.size) ? get_next_tp(q_info_pri, 0)
                                                            : get_next_tp(q_info_pri, pkt_size);
    
    w_info.queues[priority].emplace(std::move(item), queue_id,
                                    q_info_pri.last_sent);
    q_info_pri.size++;
    q_info.size++;
    w_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  int push_front(size_t queue_id, T &&item, int pkt_size) {
    return push_front(queue_id, 0, std::move(item), pkt_size);
  }

  //! Retrieves an element for the worker thread indentified by \p worker_id and
  //! moves it to \p pItem. The id of the logical queue which contained this
  //! element is copied to \p queue_id and the priority value of the served
  //! queue is copied to \p priority.
  //! Elements are retrieved according to the priority queue they are in
  //! (highest priorities, i.e. lowest priority values, are served first). Once
  //! a given priority queue reaches its maximum rate, the next queue is served.
  //! If no elements are available (either the queues are empty or they have
  //! exceeded their rate already), the function will block.
  void pop_back(size_t worker_id, size_t *queue_id, size_t *priority,
                T *pItem) {
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    MyQ *queue = nullptr;
    size_t pri;
    while (true) {
      if (w_info.size == 0) {
        w_info.q_not_empty.wait(lock);
      } else {
        auto now = clock::now();
        auto next = clock::time_point::max();

        for (pri = 0; pri < nb_priorities; pri++) {
          auto &q = w_info.queues[pri];
          if (q.size() == 0) continue;
          if (q.top().send <= now) {
            queue = &q;
            break;
          }
          next = std::min(next, q.top().send);
        }
        if (queue) break;
        w_info.q_not_empty.wait_until(lock, next);
      }
    }
    *queue_id = queue->top().queue_id;
    *priority = pri;
    // TODO(antonin): improve / document this
    // http://stackoverflow.com/questions/20149471/move-out-element-of-std-priority-queue-in-c11
    *pItem = std::move(const_cast<QE &>(queue->top()).e);
    queue->pop();
    auto &q_info = queues_info.at(*queue_id);
    auto &q_info_pri = q_info.at(*priority);
    q_info_pri.size--;
    q_info.size--;
    w_info.size--;
  }

  //! Same as
  //! pop_back(size_t worker_id, size_t *queue_id, size_t *priority, T *pItem),
  //! but the priority of the popped element is discarded.
  void pop_back(size_t worker_id, size_t *queue_id, T *pItem) {
    size_t priority;
    return pop_back(worker_id, queue_id, &priority, pItem);
  }

  //! @copydoc QueueingLogic::size
  //! The occupancies of all the priority queues for this logical queue are
  //! added.
  size_t size(size_t queue_id) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    return q_info.size;
  }

  //! Get the occupancy of priority queue \p priority for logical queue with id
  //! \p queue_id.
  size_t size(size_t queue_id, size_t priority) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &q_info_pri = q_info.at(priority);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    return q_info_pri.size;
  }

  //! Set the capacity of all the priority queues for logical queue \p queue_id
  //! to \p c elements.
  void set_capacity(size_t queue_id, size_t c) {
    for_each_q(queue_id, SetCapacityFn(c));
  }

  //! Set the capacity of priority queue \p priority for logical queue \p
  //! queue_id to \p c elements.
  void set_capacity(size_t queue_id, size_t priority, size_t c) {
    for_one_q(queue_id, priority, SetCapacityFn(c));
  }

  //! Set the maximum rate of all the priority queues for logical queue \p
  //! queue_id to \p Bps. \p Bps is expressed in "number of Bytes per
  //! second". Until this function is called, there will be no rate limit for
  //! the queue.
  void set_rate(size_t queue_id, uint64_t Bps) {
    for_each_q(queue_id, SetRateFn(Bps));
  }

  //! Same as set_rate(size_t queue_id, uint64_t Bps) but only applies to the
  //! given priority queue.
  void set_rate(size_t queue_id, size_t priority, uint64_t Bps) {
    for_one_q(queue_id, priority, SetRateFn(Bps));
  }

  //! Deleted copy constructor
  QueueingLogicPriBytesRL_AQ(const QueueingLogicPriBytesRL_AQ &) = delete;
  //! Deleted copy assignment operator
  QueueingLogicPriBytesRL_AQ &operator =(const QueueingLogicPriBytesRL_AQ &) = delete;

  //! Deleted move constructor
  QueueingLogicPriBytesRL_AQ(QueueingLogicPriBytesRL_AQ &&) = delete;
  //! Deleted move assignment operator
  QueueingLogicPriBytesRL_AQ &&operator =(QueueingLogicPriBytesRL_AQ &&) = delete;

 private:
  using ticks = std::chrono::nanoseconds;
  // clock choice? switch to steady if observing re-ordering
  // using clock = std::chrono::steady_clock;
  using clock = std::chrono::high_resolution_clock;

  struct QE {
    QE(T e, size_t queue_id, const clock::time_point &send)
        : e(std::move(e)), queue_id(queue_id), send(send) { }

    T e;
    size_t queue_id;
    clock::time_point send;
  };

  struct QEComp {
    bool operator()(const QE &lhs, const QE &rhs) const {
      return lhs.send > rhs.send;
    }
  };

  using MyQ = std::priority_queue<QE, std::deque<QE>, QEComp>;

  struct QueueInfoPri {
    size_t size;
    size_t capacity;
    uint64_t queue_rate_Bps;
    ticks pkt_delay_ticks;
    clock::time_point last_sent;
  };

  struct QueueInfo : public std::vector<QueueInfoPri> {
    QueueInfo(size_t nb_priorities, const QueueInfoPri &v)
        : std::vector<QueueInfoPri>(nb_priorities, v) { }

    size_t size{0};
  };

  struct WorkerInfo {
    mutable std::mutex q_mutex{};
    mutable std::condition_variable q_not_empty{};
    size_t size{0};
    std::array<MyQ, 32> queues;
  };

  clock::time_point get_next_tp(const QueueInfoPri &q_info_pri, int pkt_size) {
    return std::max(clock::now(),
                    q_info_pri.last_sent + q_info_pri.pkt_delay_ticks * pkt_size);
  }

  template <typename Function>
  Function for_each_q(size_t queue_id, Function fn) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    for (auto &q_info_pri : q_info) {
      fn(q_info_pri);
    }
    return std::move(fn);
  }

  template <typename Function>
  Function for_one_q(size_t queue_id, size_t priority, Function fn) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    fn(q_info_pri);
    return std::move(fn);
  }

  struct SetCapacityFn {
    explicit SetCapacityFn(size_t c)
        : c(c) { }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.capacity = c;
    }

    size_t c;
  };

  struct SetRateFn {
    explicit SetRateFn(uint64_t Bps)
        : Bps(Bps) {
      using std::chrono::duration;
      using std::chrono::duration_cast;
      pkt_delay_ticks = (Bps == 0)
                        ? duration_cast<ticks>(duration<double>(0.0))
                        : duration_cast<ticks>(duration<double>(1. / Bps));
    }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.queue_rate_Bps = Bps;
      info.pkt_delay_ticks = pkt_delay_ticks;
    }

    uint64_t Bps;
    ticks pkt_delay_ticks;
  };

  size_t nb_queues;
  size_t nb_workers;
  std::vector<QueueInfo> queues_info{};
  std::vector<WorkerInfo> workers_info{};
  std::vector<MyQ> queues{};
  FMap map_to_worker;
  size_t nb_priorities;
};






















template <typename T, typename FMap>
class QueueingLogic_RLSP_DRR {
  using MutexType = std::mutex;
  using LockType = std::unique_lock<MutexType>;

 public:
  //! See QueueingLogic::QueueingLogicRL() for an introduction. The difference
  //! here is that each logical queues can receive several priority queues (as
  //! determined by \p nb_priorities, which is set to `2` by default). Each of
  //! these priority queues will initially be able to hold \p capacity
  //! elements. The capacity of each priority queue can be changed later by
  //! using set_capacity(size_t queue_id, size_t priority, size_t c).
  QueueingLogic_RLSP_DRR(size_t nb_queues, size_t nb_workers, size_t capacity,
                     FMap map_to_worker, size_t nb_priorities = 2)
      : nb_queues(nb_queues), nb_workers(nb_workers),
        workers_info(nb_workers),
        map_to_worker(std::move(map_to_worker)),
        nb_priorities(nb_priorities) {
    auto now = clock::now();
    for (size_t i = 0; i < nb_queues; i++) {
      QueueInfoPri v = {0, capacity, 0, 0, ticks::zero(), now};
      queues_info.emplace_back(nb_priorities, v);
    }
  }

  //! If priority queue \p priority of logical queue \p queue_id is full, the
  //! function will return `0` immediately. Otherwise, \p item will be copied to
  //! the queue and the function will return `1`. If \p queue_id or \p priority
  //! are incorrect, an exception of type std::out_of_range will be thrown (same
  //! if the FMap object provided to the constructor does not behave correctly).
  int push_front(size_t queue_id, size_t priority, const T &item, int pkt_size) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    if (q_info_pri.size >= q_info_pri.capacity) return 0;

    // if current queue is the DRR queues
    // then send immediate without delay
    // if not, follow the rate limit rules
    q_info_pri.last_sent = (priority != 0) 
                           ? get_next_tp(q_info_pri, 0)
                           : get_next_tp(q_info_pri, pkt_size);
    
    w_info.queues[priority].queue.emplace(item, queue_id, pkt_size, q_info_pri.last_sent);
    q_info_pri.size++;
    q_info.size++;
    w_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  int push_front(size_t queue_id, const T &item, int pkt_size) {
    return push_front(queue_id, 0, item, pkt_size);
  }

  //! Same as push_front(size_t queue_id, size_t priority, const T &item), but
  //! \p item is moved instead of copied.
  int push_front(size_t queue_id, size_t priority, T &&item, int pkt_size) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    if (q_info_pri.size >= q_info_pri.capacity) return 0;

    // if current queue is the DRR queues
    // then send immediate without delay
    // if not, follow the rate limit rules
    q_info_pri.last_sent = (priority != 0) 
                           ? get_next_tp(q_info_pri, 0)
                           : get_next_tp(q_info_pri, pkt_size);
    
    w_info.queues[priority].queue.emplace(std::move(item), queue_id, pkt_size, q_info_pri.last_sent);
    q_info_pri.size++;
    q_info.size++;
    w_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  int push_front(size_t queue_id, T &&item, int pkt_size) {
    return push_front(queue_id, 0, std::move(item), pkt_size);
  }

  //! Retrieves an element for the worker thread indentified by \p worker_id and
  //! moves it to \p pItem. The id of the logical queue which contained this
  //! element is copied to \p queue_id and the priority value of the served
  //! queue is copied to \p priority.
  //! Elements are retrieved according to the priority queue they are in
  //! (highest priorities, i.e. lowest priority values, are served first). Once
  //! a given priority queue reaches its maximum rate, the next queue is served.
  //! If no elements are available (either the queues are empty or they have
  //! exceeded their rate already), the function will block.
  void pop_back(size_t worker_id, size_t *queue_id, size_t *priority,
                T *pItem) {
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    PriQueue *pqueue = nullptr;
    size_t pri;
    while (true) {
      if (w_info.size == 0) {
        w_info.q_not_empty.wait(lock);
      } else {
        auto now = clock::now();
        auto next = clock::time_point::max();

        auto &pq = w_info.queues[0];
        if (pq.queue.size() > 0) {
          if (pq.queue.top().send <= now) {
            pqueue = &pq;
            pri = 0;
            BMLOG_DEBUG("WORKER-{}, PRI-0 is selected", worker_id);
            break;
          } else {
            next = std::min(next, pq.queue.top().send);
            BMLOG_DEBUG("WORKER-{}, PRI-0 is suspended", worker_id);
          }
        }

        BMLOG_DEBUG("Entering DRR");

        // to have a complete cicle, put exit condition inside the body of the loop 
        for (pri = w_info.lsq; ; pri = (pri + 1) % nb_priorities) {

          auto &pq = w_info.queues[pri];

          if (pri == 0 || pq.queue.size() == 0 || pq.d_counter < 0) {
            BMLOG_DEBUG("WORKER-{}, PRI-{} is restored, QSIZE-{}", worker_id, pri, pq.queue.size());

            pq.d_counter = pq.quantum;
            if (pri == w_info.lsq - 1) break; // complete cycle condition
            else continue;
          }

          BMLOG_DEBUG("WORKER-{}, PRI-{} is selected: dc = {}, qt = {}, pkt = {}, qs = {}", 
                      worker_id, pri, pq.d_counter, pq.quantum, pq.queue.top().pkt_size, pq.queue.size());

          pq.d_counter -= pq.queue.top().pkt_size;

          BMLOG_DEBUG("WORKER-{}, PRI-{} DRR is performed: dc = {}", 
                      worker_id, pri, pq.d_counter);

          w_info.lsq = pri;
          pqueue = &pq;
          break;
        }

        if (pqueue) break;
        w_info.q_not_empty.wait_until(lock, next);
      }
    }
    *queue_id = pqueue->queue.top().queue_id;
    *priority = pri;
    // TODO(antonin): improve / document this
    // http://stackoverflow.com/questions/20149471/move-out-element-of-std-priority-queue-in-c11
    *pItem = std::move(const_cast<QE &>(pqueue->queue.top()).e);
    pqueue->queue.pop();
    auto &q_info = queues_info.at(*queue_id);
    auto &q_info_pri = q_info.at(*priority);
    q_info_pri.size--;
    q_info.size--;
    w_info.size--;
  }

  //! Same as
  //! pop_back(size_t worker_id, size_t *queue_id, size_t *priority, T *pItem),
  //! but the priority of the popped element is discarded.
  void pop_back(size_t worker_id, size_t *queue_id, T *pItem) {
    size_t priority;
    return pop_back(worker_id, queue_id, &priority, pItem);
  }

  //! @copydoc QueueingLogic::size
  //! The occupancies of all the priority queues for this logical queue are
  //! added.
  size_t size(size_t queue_id) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    return q_info.size;
  }

  //! Get the occupancy of priority queue \p priority for logical queue with id
  //! \p queue_id.
  size_t size(size_t queue_id, size_t priority) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &q_info_pri = q_info.at(priority);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    return q_info_pri.size;
  }

  //! Set the capacity of all the priority queues for logical queue \p queue_id
  //! to \p c elements.
  void set_capacity(size_t queue_id, size_t c) {
    for_each_q(queue_id, SetCapacityFn(c));
  }

  //! Set the capacity of priority queue \p priority for logical queue \p
  //! queue_id to \p c elements.
  void set_capacity(size_t queue_id, size_t priority, size_t c) {
    for_one_q(queue_id, priority, SetCapacityFn(c));
  }

  //! Set the maximum rate of all the priority queues for logical queue \p
  //! queue_id to \p Bps. \p Bps is expressed in "number of Bytes per
  //! second". Until this function is called, there will be no rate limit for
  //! the queue.
  void set_rate(size_t queue_id, uint64_t Bps) {
    for_each_q(queue_id, SetRateFn(Bps));
  }

  //! Same as set_rate(size_t queue_id, uint64_t Bps) but only applies to the
  //! given priority queue.
  void set_rate(size_t queue_id, size_t priority, uint64_t Bps) {
    if (priority == 0)
      for_one_q(queue_id, priority, SetRateFn(Bps));
  }

  void set_quantum(size_t queue_id, size_t priority, uint64_t quantum) {
    if (priority != 0) {
      size_t worker_id = map_to_worker(queue_id);
      auto &q_info = queues_info.at(queue_id);
      auto &w_info = workers_info.at(worker_id);
      auto &q_info_pri = q_info.at(priority);
      LockType lock(w_info.q_mutex);

      q_info_pri.quantum = quantum;
      w_info.queues[priority].quantum = quantum;
    }
  }

  //! Deleted copy constructor
  QueueingLogic_RLSP_DRR(const QueueingLogic_RLSP_DRR &) = delete;
  //! Deleted copy assignment operator
  QueueingLogic_RLSP_DRR &operator =(const QueueingLogic_RLSP_DRR &) = delete;

  //! Deleted move constructor
  QueueingLogic_RLSP_DRR(QueueingLogic_RLSP_DRR &&) = delete;
  //! Deleted move assignment operator
  QueueingLogic_RLSP_DRR &&operator =(QueueingLogic_RLSP_DRR &&) = delete;

 private:
  using ticks = std::chrono::nanoseconds;
  // clock choice? switch to steady if observing re-ordering
  // using clock = std::chrono::steady_clock;
  using clock = std::chrono::high_resolution_clock;

  struct QE {
    QE(T e, size_t queue_id, int pkt_size, const clock::time_point &send)
        : e(std::move(e)), queue_id(queue_id), pkt_size(pkt_size), send(send) { }

    T e;
    size_t queue_id;
    int pkt_size;
    clock::time_point send;
  };

  struct QEComp {
    bool operator()(const QE &lhs, const QE &rhs) const {
      return lhs.send > rhs.send;
    }
  };

  using MyQ = std::priority_queue<QE, std::deque<QE>, QEComp>;

  struct PriQueue {
    MyQ queue;
    uint64_t quantum{0};
    int64_t d_counter{0};
  };

  struct QueueInfoPri {
    size_t size;
    size_t capacity;
    uint64_t queue_rate_Bps;
    uint64_t quantum;
    ticks pkt_delay_ticks;
    clock::time_point last_sent;
  };

  struct QueueInfo : public std::vector<QueueInfoPri> {
    QueueInfo(size_t nb_priorities, const QueueInfoPri &v)
        : std::vector<QueueInfoPri>(nb_priorities, v) { }

    size_t size{0};
  };

  struct WorkerInfo {
    mutable std::mutex q_mutex{};
    mutable std::condition_variable q_not_empty{};
    size_t size{0};
    std::array<PriQueue, 32> queues;
    size_t lsq{1};
  };

  clock::time_point get_next_tp(const QueueInfoPri &q_info_pri, int pkt_size) {
    return std::max(clock::now(),
                    q_info_pri.last_sent + q_info_pri.pkt_delay_ticks * pkt_size);
  }

  template <typename Function>
  Function for_each_q(size_t queue_id, Function fn) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    for (auto &q_info_pri : q_info) {
      fn(q_info_pri);
    }
    return std::move(fn);
  }

  template <typename Function>
  Function for_one_q(size_t queue_id, size_t priority, Function fn) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    fn(q_info_pri);
    return std::move(fn);
  }

  struct SetCapacityFn {
    explicit SetCapacityFn(size_t c)
        : c(c) { }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.capacity = c;
    }

    size_t c;
  };

  struct SetRateFn {
    explicit SetRateFn(uint64_t Bps)
        : Bps(Bps) {
      using std::chrono::duration;
      using std::chrono::duration_cast;
      pkt_delay_ticks = duration_cast<ticks>(duration<double>(1. / Bps));
    }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.queue_rate_Bps = Bps;
      info.pkt_delay_ticks = pkt_delay_ticks;
    }

    uint64_t Bps;
    ticks pkt_delay_ticks;
  };

  struct SetQuantumFn {
    explicit SetQuantumFn(uint64_t q)
        : q(q) { }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.quantum = q;
    }

    uint64_t q;
  };

  size_t nb_queues;
  size_t nb_workers;
  std::vector<QueueInfo> queues_info{};
  std::vector<WorkerInfo> workers_info{};
  FMap map_to_worker;
  size_t nb_priorities;
};

















template <typename T, typename FMap>
class QueueingLogic_RLSP_DRR_ORIG {
  using MutexType = std::mutex;
  using LockType = std::unique_lock<MutexType>;

 public:
  //! See QueueingLogic::QueueingLogicRL() for an introduction. The difference
  //! here is that each logical queues can receive several priority queues (as
  //! determined by \p nb_priorities, which is set to `2` by default). Each of
  //! these priority queues will initially be able to hold \p capacity
  //! elements. The capacity of each priority queue can be changed later by
  //! using set_capacity(size_t queue_id, size_t priority, size_t c).
  QueueingLogic_RLSP_DRR_ORIG(size_t nb_queues, size_t nb_workers, size_t capacity,
                     FMap map_to_worker, size_t nb_priorities = 2)
      : nb_queues(nb_queues), nb_workers(nb_workers),
        workers_info(nb_workers),
        map_to_worker(std::move(map_to_worker)),
        nb_priorities(nb_priorities) {
    auto now = clock::now();
    for (size_t i = 0; i < nb_queues; i++) {
      QueueInfoPri v = {0, capacity, 0, 0, ticks::zero(), now};
      queues_info.emplace_back(nb_priorities, v);
    }
  }

  //! If priority queue \p priority of logical queue \p queue_id is full, the
  //! function will return `0` immediately. Otherwise, \p item will be copied to
  //! the queue and the function will return `1`. If \p queue_id or \p priority
  //! are incorrect, an exception of type std::out_of_range will be thrown (same
  //! if the FMap object provided to the constructor does not behave correctly).
  int push_front(size_t queue_id, size_t priority, const T &item, int pkt_size) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    if (q_info_pri.size >= q_info_pri.capacity) return 0;

    // if current queue is the DRR queues
    // then send immediate without delay
    // if not, follow the rate limit rules
    q_info_pri.last_sent = (priority != 0) 
                           ? get_next_tp(q_info_pri, 0)
                           : get_next_tp(q_info_pri, pkt_size);
    
    if (priority != 0 && q_info_pri.size == 0)
      w_info.active_list.push_back(priority);

    w_info.queues[priority].queue.emplace(item, queue_id, pkt_size, q_info_pri.last_sent);
    q_info_pri.size++;
    q_info.size++;
    w_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  int push_front(size_t queue_id, const T &item, int pkt_size) {
    return push_front(queue_id, 0, item, pkt_size);
  }

  //! Same as push_front(size_t queue_id, size_t priority, const T &item), but
  //! \p item is moved instead of copied.
  int push_front(size_t queue_id, size_t priority, T &&item, int pkt_size) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    if (q_info_pri.size >= q_info_pri.capacity) return 0;

    // if current queue is the DRR queues
    // then send immediate without delay
    // if not, follow the rate limit rules
    q_info_pri.last_sent = (priority != 0) 
                           ? get_next_tp(q_info_pri, 0)
                           : get_next_tp(q_info_pri, pkt_size);

    if (priority != 0 && q_info_pri.size == 0)
      w_info.active_list.push_back(priority);
    
    w_info.queues[priority].queue.emplace(std::move(item), queue_id, pkt_size, q_info_pri.last_sent);
    q_info_pri.size++;
    q_info.size++;
    w_info.size++;
    w_info.q_not_empty.notify_one();
    return 1;
  }

  int push_front(size_t queue_id, T &&item, int pkt_size) {
    return push_front(queue_id, 0, std::move(item), pkt_size);
  }

  //! Retrieves an element for the worker thread indentified by \p worker_id and
  //! moves it to \p pItem. The id of the logical queue which contained this
  //! element is copied to \p queue_id and the priority value of the served
  //! queue is copied to \p priority.
  //! Elements are retrieved according to the priority queue they are in
  //! (highest priorities, i.e. lowest priority values, are served first). Once
  //! a given priority queue reaches its maximum rate, the next queue is served.
  //! If no elements are available (either the queues are empty or they have
  //! exceeded their rate already), the function will block.
  void pop_back(size_t worker_id, size_t *queue_id, size_t *priority,
                T *pItem) {
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    PriQueue *pqueue = nullptr;
    size_t pri;
    while (true) {
      if (w_info.size == 0) {
        w_info.q_not_empty.wait(lock);
      } else {
        auto now = clock::now();
        auto next = clock::time_point::max();

        auto &pq = w_info.queues[0];
        if (pq.queue.size() > 0) {
          if (pq.queue.top().send <= now) {
            pqueue = &pq;
            pri = 0;
            BMLOG_DEBUG("WORKER-{}, PRI-0 is selected", worker_id);
            break;
          } else {
            next = std::min(next, pq.queue.top().send);
            BMLOG_DEBUG("WORKER-{}, PRI-0 is suspended", worker_id);
          }
        }

        BMLOG_DEBUG("WORKER-{}, Entering DRR", worker_id);

        if (perform_DRR(w_info, pri)) {
          auto &pq = w_info.queues[pri];
          pqueue = &pq;
          BMLOG_DEBUG("WORKER-{}, DRR_PERFORMED: PRI-{}, QUEUE-{}", worker_id, pri, pqueue->queue.top().queue_id);
          break;
        }

        BMLOG_DEBUG("WORKER-{}, NO DRR QUEUES. Waiting...", worker_id);

        w_info.q_not_empty.wait_until(lock, next);
      }
    }
    *queue_id = pqueue->queue.top().queue_id;
    *priority = pri;
    // TODO(antonin): improve / document this
    // http://stackoverflow.com/questions/20149471/move-out-element-of-std-priority-queue-in-c11
    *pItem = std::move(const_cast<QE &>(pqueue->queue.top()).e);
    pqueue->queue.pop();
    auto &q_info = queues_info.at(*queue_id);
    auto &q_info_pri = q_info.at(*priority);
    q_info_pri.size--;
    q_info.size--;
    w_info.size--;

    if (pri != 0 && q_info_pri.size == 0) {
      w_info.queues[pri].d_counter = 0;
      w_info.active_list.pop_front();
      w_info.lsq = 0;
      BMLOG_DEBUG("WORKER-{}, NO PACKETS ARE LEFT FOR DRR QUEUE {}", worker_id, pri);
    }
  }

    //! Same as
  //! pop_back(size_t worker_id, size_t *queue_id, size_t *priority, T *pItem),
  //! but the priority of the popped element is discarded.
  void pop_back(size_t worker_id, size_t *queue_id, T *pItem) {
    size_t priority;
    return pop_back(worker_id, queue_id, &priority, pItem);
  }

  //! @copydoc QueueingLogic::size
  //! The occupancies of all the priority queues for this logical queue are
  //! added.
  size_t size(size_t queue_id) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    return q_info.size;
  }

  //! Get the occupancy of priority queue \p priority for logical queue with id
  //! \p queue_id.
  size_t size(size_t queue_id, size_t priority) const {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &q_info_pri = q_info.at(priority);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    return q_info_pri.size;
  }

  //! Set the capacity of all the priority queues for logical queue \p queue_id
  //! to \p c elements.
  void set_capacity(size_t queue_id, size_t c) {
    for_each_q(queue_id, SetCapacityFn(c));
  }

  //! Set the capacity of priority queue \p priority for logical queue \p
  //! queue_id to \p c elements.
  void set_capacity(size_t queue_id, size_t priority, size_t c) {
    for_one_q(queue_id, priority, SetCapacityFn(c));
  }

  //! Set the maximum rate of all the priority queues for logical queue \p
  //! queue_id to \p Bps. \p Bps is expressed in "number of Bytes per
  //! second". Until this function is called, there will be no rate limit for
  //! the queue.
  void set_rate(size_t queue_id, uint64_t Bps) {
    for_each_q(queue_id, SetRateFn(Bps));
  }

  //! Same as set_rate(size_t queue_id, uint64_t Bps) but only applies to the
  //! given priority queue.
  void set_rate(size_t queue_id, size_t priority, uint64_t Bps) {
    if (priority == 0)
      for_one_q(queue_id, priority, SetRateFn(Bps));
  }

  void set_quantum(size_t queue_id, size_t priority, uint64_t quantum) {
    if (priority != 0) {
      size_t worker_id = map_to_worker(queue_id);
      auto &q_info = queues_info.at(queue_id);
      auto &w_info = workers_info.at(worker_id);
      auto &q_info_pri = q_info.at(priority);
      LockType lock(w_info.q_mutex);

      q_info_pri.quantum = quantum;
      w_info.queues[priority].quantum = quantum;
    }
  }

  //! Deleted copy constructor
  QueueingLogic_RLSP_DRR_ORIG(const QueueingLogic_RLSP_DRR_ORIG &) = delete;
  //! Deleted copy assignment operator
  QueueingLogic_RLSP_DRR_ORIG &operator =(const QueueingLogic_RLSP_DRR_ORIG &) = delete;

  //! Deleted move constructor
  QueueingLogic_RLSP_DRR_ORIG(QueueingLogic_RLSP_DRR_ORIG &&) = delete;
  //! Deleted move assignment operator
  QueueingLogic_RLSP_DRR_ORIG &&operator =(QueueingLogic_RLSP_DRR_ORIG &&) = delete;

 private:
  using ticks = std::chrono::nanoseconds;
  // clock choice? switch to steady if observing re-ordering
  // using clock = std::chrono::steady_clock;
  using clock = std::chrono::high_resolution_clock;

  struct QE {
    QE(T e, size_t queue_id, int pkt_size, const clock::time_point &send)
        : e(std::move(e)), queue_id(queue_id), pkt_size(pkt_size), send(send) { }

    T e;
    size_t queue_id;
    int pkt_size;
    clock::time_point send;
  };

  struct QEComp {
    bool operator()(const QE &lhs, const QE &rhs) const {
      return lhs.send > rhs.send;
    }
  };

  using MyQ = std::priority_queue<QE, std::deque<QE>, QEComp>;

  struct PriQueue {
    MyQ queue;
    uint64_t quantum{0};
    int64_t d_counter{0};
  };

  struct QueueInfoPri {
    size_t size;
    size_t capacity;
    uint64_t queue_rate_Bps;
    uint64_t quantum;
    ticks pkt_delay_ticks;
    clock::time_point last_sent;
  };

  struct QueueInfo : public std::vector<QueueInfoPri> {
    QueueInfo(size_t nb_priorities, const QueueInfoPri &v)
        : std::vector<QueueInfoPri>(nb_priorities, v) { }

    size_t size{0};
  };

  struct WorkerInfo {
    mutable std::mutex q_mutex{};
    mutable std::condition_variable q_not_empty{};
    size_t size{0};
    size_t lsq{0};
    std::array<PriQueue, 32> queues;
    std::deque<size_t> active_list{};
  };

  clock::time_point get_next_tp(const QueueInfoPri &q_info_pri, int pkt_size) {
    return std::max(clock::now(),
                    q_info_pri.last_sent + q_info_pri.pkt_delay_ticks * pkt_size);
  }

  template <typename Function>
  Function for_each_q(size_t queue_id, Function fn) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    LockType lock(w_info.q_mutex);
    for (auto &q_info_pri : q_info) {
      fn(q_info_pri);
    }
    return std::move(fn);
  }

  template <typename Function>
  Function for_one_q(size_t queue_id, size_t priority, Function fn) {
    size_t worker_id = map_to_worker(queue_id);
    auto &q_info = queues_info.at(queue_id);
    auto &w_info = workers_info.at(worker_id);
    auto &q_info_pri = q_info.at(priority);
    LockType lock(w_info.q_mutex);
    fn(q_info_pri);
    return std::move(fn);
  }

  struct SetCapacityFn {
    explicit SetCapacityFn(size_t c)
        : c(c) { }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.capacity = c;
    }

    size_t c;
  };

  struct SetRateFn {
    explicit SetRateFn(uint64_t Bps)
        : Bps(Bps) {
      using std::chrono::duration;
      using std::chrono::duration_cast;
      pkt_delay_ticks = duration_cast<ticks>(duration<double>(1. / Bps));
    }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.queue_rate_Bps = Bps;
      info.pkt_delay_ticks = pkt_delay_ticks;
    }

    uint64_t Bps;
    ticks pkt_delay_ticks;
  };

  struct SetQuantumFn {
    explicit SetQuantumFn(uint64_t q)
        : q(q) { }

    void operator ()(QueueInfoPri &info) const {  // NOLINT(runtime/references)
      info.quantum = q;
    }

    uint64_t q;
  };

  int perform_DRR(WorkerInfo &w_info, size_t &pri) {

    if (w_info.active_list.size() > 0) {

      auto &aq_idx = w_info.active_list.front();
      auto &pq = w_info.queues[aq_idx];

      BMLOG_DEBUG("ACTIVE QUEUE - {}, D_COUNTER - {}, PKT_SIZE - {}, ACT_LIST - {}, PKT_NUM - {}", 
        aq_idx, pq.d_counter, pq.queue.top().pkt_size, w_info.active_list.size(), pq.queue.size());

      if (aq_idx != w_info.lsq) {
        pq.d_counter += pq.quantum;
        BMLOG_DEBUG("INCREMENT D_COUNTER - {}", pq.d_counter);
      }

      if (pq.d_counter >= pq.queue.top().pkt_size) {

        BMLOG_DEBUG("SERVICING QUEUE {}", aq_idx);

        pq.d_counter -= pq.queue.top().pkt_size;
        pri = aq_idx;
        w_info.lsq = aq_idx;

        BMLOG_DEBUG("D_COUNTER LEFT - {}, QUANTUM - {}", pq.d_counter, pq.quantum);

        return 1;

      } else {

        BMLOG_DEBUG("SKIPPING QUEUE - {}, NOT ENOUGH D_COUNTER", aq_idx);

        w_info.active_list.pop_front();
        w_info.active_list.push_back(aq_idx);

        // that means that only one queue is active,
        // therefore to increment d_counter:
        //    set last served queue of worker to 0
        if (w_info.active_list.front() == aq_idx) {
          w_info.lsq = 0;
          BMLOG_DEBUG("ONLY ONE QUEUE IS ACTIVE {}", aq_idx);
        }

        return perform_DRR(w_info, pri);

      }

    }

    BMLOG_DEBUG("NO DRR QUEUES ARE LEFT TO SERVE");

    w_info.lsq = 0;

    return 0;
  }

  size_t nb_queues;
  size_t nb_workers;
  std::vector<QueueInfo> queues_info{};
  std::vector<WorkerInfo> workers_info{};
  FMap map_to_worker;
  size_t nb_priorities;
};


}  // namespace bm

#endif  // BM_BM_SIM_QUEUEING_H_
