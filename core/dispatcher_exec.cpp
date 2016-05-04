#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include "dispatcher_exec.hpp"
#include "checkpoint.hpp"

boost::asio::io_service ioService;
boost::asio::io_service::work work(ioService);
boost::asio::io_service ioService2;
boost::asio::io_service::work work2(ioService2);
boost::thread_group threadpool;
void dispatcher_exec_startup()
{
  threadpool.create_thread
    (boost::bind(&boost::asio::io_service::run, &ioService));
  threadpool.create_thread
    (boost::bind(&boost::asio::io_service::run, &ioService2));
}

void exec_rpc(rpc_info_t *rpc)
{
  if(rpc->rpc->flags & RPC_FLAG_RO) {
    ioService.post(boost::bind(exec_rpc_internal_ro, rpc));
  }
  else if(rpc->rpc->flags & RPC_FLAG_SYNCHRONOUS) {
    ioService.post(boost::bind(exec_rpc_internal_synchronous, rpc));
  }
  else {
    ioService.post(boost::bind(exec_rpc_internal, rpc));
  }
}


void exec_send_checkpoint(void *socket, void *handle)
{
  ioService2.post(boost::bind(send_checkpoint, socket, handle));
}


