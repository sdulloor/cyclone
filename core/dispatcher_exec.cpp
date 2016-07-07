#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include "dispatcher_exec.hpp"
#include "checkpoint.hpp"
#if defined(DPDK_STACK)
#include <rte_launch.h>
#endif

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
  void operator() ()
  {
    while(true) {
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
      else if(rpc->rpc->flags & RPC_FLAG_SEQ) {
	exec_rpc_internal_seq(rpc);
      }
      else {
	exec_rpc_internal(rpc);
      }
    }
  }
} executor;

int dpdk_executor(void *arg)
{
  executor();
  return 0;
}

boost::thread_group threadpool;
boost::thread *executor_thread;

void dispatcher_exec_startup()
{
#if defined(DPDK_STACK)
  int e = rte_eal_remote_launch(dpdk_executor, NULL, 2);
  if(e != 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to launch executor on remote lcore";
    exit(-1);
  }
#else
  executor_thread = new boost::thread(boost::ref(executor));
  threadpool.create_thread
    (boost::bind(&boost::asio::io_service::run, &ioService2));
#endif
}

void exec_rpc(rpc_info_t *rpc)
{
  add_to_runqueue(rpc);
}

void exec_send_checkpoint(void *socket, void *handle)
{
  ioService2.post(boost::bind(send_checkpoint, socket, handle));
}


