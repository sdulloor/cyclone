#ifndef _RUNQ_
#define _RUNQ_

static void lock(volatile unsigned long *lockp)
{
  // TEST + TEST&SET
  do {
    while((*lockp) != 0);
  } while(!__sync_bool_compare_and_swap(lockp, 0, 1));
  __sync_synchronize();
}

static void unlock(volatile unsigned long *lockp)
{
  __sync_synchronize();
  __sync_bool_compare_and_swap(lockp, 1, 0);
  __sync_synchronize();
}


template<typename T>
struct runq_t {
  T* volatile run_queue_head;
  T* volatile run_queue_tail;
  volatile unsigned long runqueue_lock;
  volatile unsigned long ticket;

  runq_t()
  {
    run_queue_head = run_queue_tail = NULL;
    runqueue_lock = 0;
  }

  void add_to_runqueue(T * work)
  {
    work->next_issue = NULL;
    lock(&runqueue_lock);
    if(run_queue_head == NULL) {
      run_queue_head = run_queue_tail = work;
    }
    else {
      run_queue_tail->next_issue = work;
      run_queue_tail = work;
    }
    unlock(&runqueue_lock);
  }

  T * get_from_runqueue()
  {
    T *work;
    if(run_queue_head == NULL) {
      return NULL;
    }
    lock(&runqueue_lock);
    if(run_queue_head == NULL) {
      unlock(&runqueue_lock);
      return NULL;
    }
    work = run_queue_head;
    run_queue_head = work->next_issue;
    if(run_queue_head == NULL) {
      run_queue_tail = NULL;
    }
    unlock(&runqueue_lock);
    return work;
  }

  T* get_from_runqueue(unsigned long *ticketp)
  {
    T *work;
    if(run_queue_head == NULL) {
      return NULL;
    }
    lock(&runqueue_lock);
    if(run_queue_head == NULL) {
      unlock(&runqueue_lock);
      return NULL;
    }
    work = run_queue_head;
    *ticketp = ticket++;
    run_queue_head = work->next_issue;
    if(run_queue_head == NULL) {
      run_queue_tail = NULL;
    }
    unlock(&runqueue_lock);
    return work;
  }

};

#endif
