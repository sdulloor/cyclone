#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include "dispatcher_exec.hpp"
#include "checkpoint.hpp"
#include "cyclone_comm.hpp"
#include "tuning.hpp"
#if defined(DPDK_STACK)
#include <rte_launch.h>
#endif

boost::asio::io_service ioService2;
boost::asio::io_service::work work2(ioService2);

static runq_t<rpc_info_t> issue_q;

ticket_t ticket_window;

extern int dpdk_executor(void *arg);

boost::thread_group threadpool;
boost::thread *executor_thread;


void exec_rpc(rpc_info_t *rpc)
{
  issue_q.add_to_runqueue(rpc);
}

void exec_send_checkpoint(void *socket, void *handle)
{
  ioService2.post(boost::bind(send_checkpoint, socket, handle));
}


