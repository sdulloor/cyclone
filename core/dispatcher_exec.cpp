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

static void print(const char *prefix,
		  const void *data,
		  const int size)
{
  rtc_clock timer;
  const rpc_t *rpc = (const rpc_t *)data;
  if(rpc->code == RPC_REQ_MARKER) {
    BOOST_LOG_TRIVIAL(info)
      << "KICKER";
    return;
  }
  BOOST_LOG_TRIVIAL(info)
    << prefix << " "
    << rpc->client_id  << " "
    << rpc->client_txid << " "
    << (timer.current_time() - rpc->timestamp);
}

void trace_send_cmd(void *data, const int size)
{
  print("SERVER: SEND_CMD", data, size);
}

void trace_recv_cmd(void *data, const int size)
{
  print("SERVER: RECV_CMD", data, size);
}

void trace_pre_append(void *data, const int size)
{
  print("SERVER: PRE_APPEND", data, size);
}

void trace_post_append(void *data, const int size)
{
  print("SERVER: POST_APPEND", data, size); 
}

void trace_send_entry(void *data, const int size)
{
  print("SERVER: SEND_ENTRY", data, size);
}

void trace_recv_entry(void *data, const int size)
{
  print("SERVER: RECV_ENTRY", data, size);
}


