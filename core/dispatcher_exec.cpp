#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include "dispatcher_exec.hpp"
#include "checkpoint.hpp"

boost::asio::io_service ioService2;
boost::asio::io_service::work work2(ioService2);

static rpc_info_t *volatile run_queue_head = NULL;
static rpc_info_t *volatile run_queue_tail = NULL;
static unsigned long runqueue_lock = 0;

void add_to_runqueue(rpc_info_t *work)
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

rpc_info_t * get_from_runqueue()
{
  rpc_info_t *work;
  if(run_queue_head == NULL) {
    return NULL;
  }
  lock(&runqueue_lock);
  work = run_queue_head;
  run_queue_head = work->next_issue;
  if(run_queue_head == NULL) {
    run_queue_tail = NULL;
  }
  unlock(&runqueue_lock);
  return work;
}

static struct executor_st {
  volatile bool terminate_now;
  void operator() ()
  {
    while(!terminate_now) {
      rpc_info_t *rpc = get_from_runqueue();
      if(rpc == NULL) {
	continue;
      }
      if(rpc->rpc->flags & RPC_FLAG_RO) {
	exec_rpc_internal_ro(rpc);
      }
      else if(rpc->rpc->flags & RPC_FLAG_SYNCHRONOUS) {
	exec_rpc_internal_synchronous(rpc);
      }
      else {
	exec_rpc_internal(rpc);
      }
    }
  }
} executor;

boost::thread_group threadpool;
boost::thread *executor_thread;
extern volatile bool bail_out;

void disp_exec_cleanup()
{
  bail_out = true;
  executor.terminate_now =  true;
  __sync_synchronize();
  executor_thread->join();
  ioService2.stop();
  threadpool.join_all();
}

void dispatcher_exec_startup()
{
  executor.terminate_now = false;
  executor_thread = new boost::thread(boost::ref(executor));
  threadpool.create_thread
    (boost::bind(&boost::asio::io_service::run, &ioService2));
  atexit(disp_exec_cleanup);
}

void exec_rpc(rpc_info_t *rpc)
{
  add_to_runqueue(rpc);
}

void exec_send_checkpoint(void *socket, void *handle)
{
  ioService2.post(boost::bind(send_checkpoint, socket, handle));
}


