//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2019
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/actor/impl/Actor-decl.h"
#include "td/actor/impl/ActorId-decl.h"
#include "td/actor/impl/EventFull-decl.h"

#include "td/utils/Closure.h"
#include "td/utils/Heap.h"
#include "td/utils/List.h"
#include "td/utils/MovableValue.h"
#include "td/utils/MpscPollableQueue.h"
#include "td/utils/ObjectPool.h"
#include "td/utils/port/detail/PollableFd.h"
#include "td/utils/port/Poll.h"
#include "td/utils/port/PollFlags.h"
#include "td/utils/port/thread_local.h"
#include "td/utils/Slice.h"
#include "td/utils/Time.h"
#include "td/utils/type_traits.h"

#include <functional>
#include <map>
#include <memory>
#include <type_traits>
#include <utility>

namespace td {

class ActorInfo;

enum class ActorSendType { Immediate, Later, LaterWeak };

class Scheduler;
class SchedulerGuard {
 public:
  explicit SchedulerGuard(Scheduler *scheduler, bool lock = true);
  ~SchedulerGuard();
  SchedulerGuard(const SchedulerGuard &other) = delete;
  SchedulerGuard &operator=(const SchedulerGuard &other) = delete;
  SchedulerGuard(SchedulerGuard &&other) = default;
  SchedulerGuard &operator=(SchedulerGuard &&other) = delete;

 private:
  MovableValue<bool> is_valid_ = true;
  bool is_locked_;
  Scheduler *scheduler_;
  ActorContext *save_context_;
  Scheduler *save_scheduler_;
  const char *save_tag_;
};

class Scheduler {
 public:
  class Callback {
   public:
    Callback() = default;
    Callback(const Callback &) = delete;
    Callback &operator=(const Callback &) = delete;
    virtual ~Callback() = default;
    virtual void on_finish() = 0;
    virtual void register_at_finish(std::function<void()>) = 0;
  };
  Scheduler() = default;
  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler(Scheduler &&) = delete;
  Scheduler &operator=(Scheduler &&) = delete;
  ~Scheduler();

  void init();
  void init(int32 id, std::vector<std::shared_ptr<MpscPollableQueue<EventFull>>> outbound, Callback *callback);
  void clear();

  int32 sched_id() const;
  int32 sched_count() const;

  template <class ActorT, class... Args>
  TD_WARN_UNUSED_RESULT ActorOwn<ActorT> create_actor(Slice name, Args &&... args);
  template <class ActorT, class... Args>
  TD_WARN_UNUSED_RESULT ActorOwn<ActorT> create_actor_on_scheduler(Slice name, int32 sched_id, Args &&... args);
  template <class ActorT>
  TD_WARN_UNUSED_RESULT ActorOwn<ActorT> register_actor(Slice name, ActorT *actor_ptr, int32 sched_id = -1);
  template <class ActorT>
  TD_WARN_UNUSED_RESULT ActorOwn<ActorT> register_actor(Slice name, unique_ptr<ActorT> actor_ptr, int32 sched_id = -1);

  template <class ActorT>
  TD_WARN_UNUSED_RESULT ActorOwn<ActorT> register_existing_actor(unique_ptr<ActorT> actor_ptr);

  void send_to_scheduler(int32 sched_id, const ActorId<> &actor_id, Event &&event);
  void send_to_other_scheduler(int32 sched_id, const ActorId<> &actor_id, Event &&event);

  template <ActorSendType send_type, class EventT>
  void send_lambda(ActorRef actor_ref, EventT &&lambda);

  template <ActorSendType send_type, class EventT>
  void send_closure(ActorRef actor_ref, EventT &&closure);

  template <ActorSendType send_type>
  void send(ActorRef actor_ref, Event &&event);

  void hack(const ActorId<> &actor_id, Event &&event) {
    actor_id.get_actor_unsafe()->raw_event(event.data);
  }
  void before_tail_send(const ActorId<> &actor_id);

  static void subscribe(PollableFd fd, PollFlags flags = PollFlags::ReadWrite());
  static void unsubscribe(PollableFdRef fd);
  static void unsubscribe_before_close(PollableFdRef fd);

  void yield_actor(Actor *actor);
  void stop_actor(Actor *actor);
  void do_stop_actor(Actor *actor);
  uint64 get_link_token(Actor *actor);
  void migrate_actor(Actor *actor, int32 dest_sched_id);
  void do_migrate_actor(Actor *actor, int32 dest_sched_id);
  void start_migrate_actor(Actor *actor, int32 dest_sched_id);
  void finish_migrate_actor(Actor *actor);

  bool has_actor_timeout(const Actor *actor) const;
  void set_actor_timeout_in(Actor *actor, double timeout);
  void set_actor_timeout_at(Actor *actor, double timeout_at);
  void cancel_actor_timeout(Actor *actor);

  void finish();
  void yield();
  void run(Timestamp timeout);
  void run_no_guard(Timestamp timeout);

  void wakeup();

  static Scheduler *instance();
  static ActorContext *&context();
  static void on_context_updated();

  SchedulerGuard get_guard();
  SchedulerGuard get_const_guard();

  Timestamp get_timeout();

 private:
  static void set_scheduler(Scheduler *scheduler);
  /*** ServiceActor ***/
  class ServiceActor final : public Actor {
   public:
    void set_queue(std::shared_ptr<MpscPollableQueue<EventFull>> queues);
    void start_up() override;

   private:
    std::shared_ptr<MpscPollableQueue<EventFull>> inbound_;
    bool subscribed_{false};
    void loop() override;
    void tear_down() override;
  };
  friend class ServiceActor;

  void do_custom_event(ActorInfo *actor, CustomEvent &event);
  void do_event(ActorInfo *actor, Event &&event);

  void enter_actor(ActorInfo *actor_info);
  void exit_actor(ActorInfo *actor_info);

  void yield_actor(ActorInfo *actor_info);
  void stop_actor(ActorInfo *actor_info);
  void do_stop_actor(ActorInfo *actor_info);
  uint64 get_link_token(ActorInfo *actor_info);
  void migrate_actor(ActorInfo *actor_info, int32 dest_sched_id);
  void do_migrate_actor(ActorInfo *actor_info, int32 dest_sched_id);
  void start_migrate_actor(ActorInfo *actor_info, int32 dest_sched_id);

  bool has_actor_timeout(const ActorInfo *actor_info) const;
  void set_actor_timeout_in(ActorInfo *actor_info, double timeout);
  void set_actor_timeout_at(ActorInfo *actor_info, double timeout_at);
  void cancel_actor_timeout(ActorInfo *actor_info);

  void register_migrated_actor(ActorInfo *actor_info);
  void add_to_mailbox(ActorInfo *actor_info, Event &&event);
  void clear_mailbox(ActorInfo *actor_info);

  template <class RunFuncT, class EventFuncT>
  void flush_mailbox(ActorInfo *actor_info, const RunFuncT &run_func, const EventFuncT &event_func);

  template <ActorSendType send_type, class RunFuncT, class EventFuncT>
  void send_impl(const ActorId<> &actor_id, const RunFuncT &run_func, const EventFuncT &event_func);

  void inc_wait_generation();

  Timestamp run_timeout();
  void run_mailbox();
  Timestamp run_events();
  void run_poll(Timestamp timeout);

  template <class ActorT>
  ActorOwn<ActorT> register_actor_impl(Slice name, ActorT *actor_ptr, Actor::Deleter deleter, int32 sched_id);
  void destroy_actor(ActorInfo *actor_info);

  static TD_THREAD_LOCAL Scheduler *scheduler_;
  static TD_THREAD_LOCAL ActorContext *context_;

  Callback *callback_ = nullptr;
  unique_ptr<ObjectPool<ActorInfo>> actor_info_pool_;

  int32 actor_count_ = 0;
  ListNode pending_actors_list_;
  ListNode ready_actors_list_;
  KHeap<double> timeout_queue_;

  std::map<ActorInfo *, std::vector<Event>> pending_events_;

  ServiceActor service_actor_;
  Poll poll_;

  bool yield_flag_ = false;
  bool has_guard_ = false;
  bool close_flag_ = false;

  uint32 wait_generation_ = 0;
  int32 sched_id_ = 0;
  int32 sched_n_ = 0;
  std::shared_ptr<MpscPollableQueue<EventFull>> inbound_queue_;
  std::vector<std::shared_ptr<MpscPollableQueue<EventFull>>> outbound_queues_;

  std::shared_ptr<ActorContext> save_context_;

  struct EventContext {
    int32 dest_sched_id{0};
    enum Flags { Stop = 1, Migrate = 2 };
    int32 flags{0};
    uint64 link_token{0};

    ActorInfo *actor_info{nullptr};
  };
  EventContext *event_context_ptr_{nullptr};

  friend class GlobalScheduler;
  friend class SchedulerGuard;
  friend class EventGuard;
};

/*** Interface to current scheduler ***/
template <class ActorT, class... Args>
TD_WARN_UNUSED_RESULT ActorOwn<ActorT> create_actor(Slice name, Args &&... args);
template <class ActorT, class... Args>
TD_WARN_UNUSED_RESULT ActorOwn<ActorT> create_actor_on_scheduler(Slice name, int32 sched_id, Args &&... args);
template <class ActorT>
TD_WARN_UNUSED_RESULT ActorOwn<ActorT> register_actor(Slice name, ActorT *actor_ptr, int32 sched_id = -1);
template <class ActorT>
TD_WARN_UNUSED_RESULT ActorOwn<ActorT> register_actor(Slice name, unique_ptr<ActorT> actor_ptr, int32 sched_id = -1);

template <class ActorT>
TD_WARN_UNUSED_RESULT ActorOwn<ActorT> register_existing_actor(unique_ptr<ActorT> actor_ptr);

template <class ActorIdT, class FunctionT, class... ArgsT>
void send_closure(ActorIdT &&actor_id, FunctionT function, ArgsT &&... args) {
  using ActorT = typename std::decay_t<ActorIdT>::ActorT;
  using FunctionClassT = member_function_class_t<FunctionT>;
  static_assert(std::is_base_of<FunctionClassT, ActorT>::value, "unsafe send_closure");

  Scheduler::instance()->send_closure<ActorSendType::Immediate>(
      std::forward<ActorIdT>(actor_id), create_immediate_closure(function, std::forward<ArgsT>(args)...));
}

template <class ActorIdT, class FunctionT, class... ArgsT>
void send_closure_later(ActorIdT &&actor_id, FunctionT function, ArgsT &&... args) {
  using ActorT = typename std::decay_t<ActorIdT>::ActorT;
  using FunctionClassT = member_function_class_t<FunctionT>;
  static_assert(std::is_base_of<FunctionClassT, ActorT>::value, "unsafe send_closure");

  Scheduler::instance()->send<ActorSendType::Later>(std::forward<ActorIdT>(actor_id),
                                                    Event::delayed_closure(function, std::forward<ArgsT>(args)...));
}

template <class... ArgsT>
void send_lambda(ActorRef actor_ref, ArgsT &&... args) {
  Scheduler::instance()->send_lambda<ActorSendType::Immediate>(actor_ref, std::forward<ArgsT>(args)...);
}

template <class... ArgsT>
void send_event(ActorRef actor_ref, ArgsT &&... args) {
  Scheduler::instance()->send<ActorSendType::Immediate>(actor_ref, std::forward<ArgsT>(args)...);
}

template <class... ArgsT>
void send_event_later(ActorRef actor_ref, ArgsT &&... args) {
  Scheduler::instance()->send<ActorSendType::Later>(actor_ref, std::forward<ArgsT>(args)...);
}

void yield_scheduler();
}  // namespace td
