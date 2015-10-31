#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/thread.hpp>
#include "dispatcher_exec.hpp"

boost::asio::io_service ioService;
boost::asio::io_service::work work(ioService);
boost::thread_group threadpool;
void dispatcher_exec_startup()
{
  threadpool.create_thread
    (boost::bind(&boost::asio::io_service::run, &ioService));
}

void exec_rpc(rpc_info_t *rpc)
{
  ioService.post(boost::bind(exec_rpc_internal, rpc));
}

