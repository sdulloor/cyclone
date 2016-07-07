#ifndef _CYCLONE_COMM_DPDK_
#define _CYCLONE_COMM_DPDK_


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/time.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_byteorder.h>

static const int q_raft       = 0;
static const int q_dispatcher = 1;
static const int q_control    = 2;
static const int num_queues   = 3;
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
static const uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static const uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;


static struct rte_eth_conf port_conf;

// Because C++ did not see it fit to include struct inits
static void init_port_conf()
{
  port_conf.rxmode.mq_mode        = ETH_MQ_RX_RSS;
  port_conf.rxmode.max_rx_pkt_len = ETHER_MAX_LEN;
  port_conf.rxmode.split_hdr_size = 0;
  port_conf.rxmode.header_split   = 0; 
  port_conf.rxmode.hw_ip_checksum = 0; 
  port_conf.rxmode.hw_vlan_filter = 0; 
  port_conf.rxmode.jumbo_frame    = 0; 
  port_conf.rxmode.hw_strip_crc   = 0; 
  port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;
  port_conf.rx_adv_conf.rss_conf.rss_key = NULL; // Set Intel RSS hash
  port_conf.rx_adv_conf.rss_conf.rss_key_len = 0;
  port_conf.rx_adv_conf.rss_conf.rss_hf = ETH_RSS_IP;
}

typedef struct {
  struct ether_addr port_macaddr;
  struct rte_mempool *mempools[num_queues];
  struct rte_eth_dev_tx_buffer *buffers[num_queues];
} dpdk_context_t;

#define PKT_BURST 32
typedef struct  {
  dpdk_context_t *context;
  struct rte_mempool *mempool;
  struct rte_eth_dev_tx_buffer *buffer;
  struct ether_addr remote_mac;
  struct ether_addr local_mac;
  uint32_t flow_ip_src;
  uint32_t flow_ip_dst;
  int port_id;
  int queue_id;
  rte_mbuf *burst[PKT_BURST];
  int consumed;
  int buffered;
} dpdk_socket_t;


#define IP_DEFTTL  64   /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN  0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)

static void initialize_ipv4_header(struct ipv4_hdr *ip_hdr, 
				   uint32_t src_addr,
				   uint32_t dst_addr, 
				   uint16_t pkt_data_len)
{
  uint16_t pkt_len;
  unaligned_uint16_t *ptr16;
  uint32_t ip_cksum;

  /*
   * Initialize IP header.
   */
  pkt_len = (uint16_t) (pkt_data_len + sizeof(struct ipv4_hdr));

  ip_hdr->version_ihl   = IP_VHL_DEF;
  ip_hdr->type_of_service   = 0;
  ip_hdr->fragment_offset = 0;
  ip_hdr->time_to_live   = IP_DEFTTL;
  ip_hdr->next_proto_id = IPPROTO_IP;
  ip_hdr->packet_id = 0;
  ip_hdr->total_length   = rte_cpu_to_be_16(pkt_len);
  ip_hdr->src_addr = rte_cpu_to_be_32(src_addr);
  ip_hdr->dst_addr = rte_cpu_to_be_32(dst_addr);

  /*
   * Compute IP header checksum.
   */
  ptr16 = (unaligned_uint16_t *)ip_hdr;
  ip_cksum = 0;
  ip_cksum += ptr16[0]; ip_cksum += ptr16[1];
  ip_cksum += ptr16[2]; ip_cksum += ptr16[3];
  ip_cksum += ptr16[4];
  ip_cksum += ptr16[6]; ip_cksum += ptr16[7];
  ip_cksum += ptr16[8]; ip_cksum += ptr16[9];

  /*
   * Reduce 32 bit checksum to 16 bits and complement it.
   */
  ip_cksum = ((ip_cksum & 0xFFFF0000) >> 16) +
    (ip_cksum & 0x0000FFFF);
  ip_cksum %= 65536;
  ip_cksum = (~ip_cksum) & 0x0000FFFF;
  if (ip_cksum == 0)
    ip_cksum = 0xFFFF;
  ip_hdr->hdr_checksum = (uint16_t) ip_cksum;
}

// Best effort
static int cyclone_tx(void *socket,
		      const unsigned char *data,
		      unsigned long size,
		      const char *context) 
{
  dpdk_socket_t *dpdk_socket = (dpdk_socket_t *)socket;
  rte_mbuf *m = rte_pktmbuf_alloc(dpdk_socket->mempool);
  struct ether_hdr *eth;
  eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
  memset(eth, 0, sizeof(struct ether_hdr));
  ether_addr_copy(&dpdk_socket->remote_mac, &eth->d_addr);
  ether_addr_copy(&dpdk_socket->local_mac, &eth->s_addr);
  eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
  struct ipv4_hdr *ip = (struct ipv4_hdr *)(eth + 1);
  initialize_ipv4_header(ip, 
			 dpdk_socket->flow_ip_src,
			 dpdk_socket->flow_ip_dst,
			 size);
  rte_memcpy(ip + 1, data, size);
  
  ///////////////////////
  m->pkt_len = 
    sizeof(struct ether_hdr) + 
    sizeof(struct ipv4_hdr)  + 
    size;
  m->data_len = m->pkt_len;
  int sent = rte_eth_tx_buffer(dpdk_socket->port_id, 
			       dpdk_socket->queue_id, 
			       dpdk_socket->buffer, 
			       m);
  sent += rte_eth_tx_buffer_flush(dpdk_socket->port_id, 
				  dpdk_socket->queue_id, 
				  dpdk_socket->buffer);
  if(sent)
    return 0;
  else
    return -1;
}

// Keep trying until success
static void cyclone_tx_block(void *socket,
			     const unsigned char *data,
			     unsigned long size,
			     const char *context)
{
  int ok;
  do {
    ok = cyclone_tx(socket, data, size, context);
  } while(ok != 0);
}


// Best effort
static int cyclone_rx(void *socket,
		      unsigned char *data,
		      unsigned long size,
		      const char *context)
{
  int rc, nb_rx;
  rte_mbuf *m;
  dpdk_socket_t *dpdk_socket = (dpdk_socket_t *)socket;
  if(dpdk_socket->buffered == dpdk_socket->consumed) {
    dpdk_socket->consumed = 0;
    dpdk_socket->buffered = rte_eth_rx_burst(dpdk_socket->port_id, 
					     dpdk_socket->queue_id,
					     &dpdk_socket->burst[0], 
					     PKT_BURST);
  }
  if(dpdk_socket->consumed < dpdk_socket->buffered) {
    m = dpdk_socket->burst[dpdk_socket->consumed++];
    rte_prefetch0(rte_pktmbuf_mtod(m, void *));
    struct ether_hdr *e = rte_pktmbuf_mtod(m, struct ether_hdr *);
    // drop unless this is for me
    if(!is_same_ether_addr(&e->d_addr, &dpdk_socket->local_mac)) {
      rte_pktmbuf_free(m);
      return -1;
    }
    // Strip off headers
    int payload_offset = sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr);
    void *payload = rte_pktmbuf_mtod_offset(m, void *, payload_offset);
    int msg_size = m->data_len - payload_offset;
    rte_memcpy(data, payload, msg_size);
    rte_pktmbuf_free(m);
    return msg_size;
  }
  else {
    return -1;
  }
}

// Block till data available
static int cyclone_rx_block(void *socket,
			    unsigned char *data,
			    unsigned long size,
			    const char *context)
{
  int rc;
  while (true) {
    rc = cyclone_rx(socket, data, size, context);
  } while(rc < 0);
  return rc;
}

// Block till data available or timeout
static int cyclone_rx_timeout(void *socket,
			      unsigned char *data,
			      unsigned long size,
			      unsigned long timeout_usecs,
			      const char *context)
{
  int rc;
  unsigned long mark = rtc_clock::current_time();
  while (true) {
    rc = cyclone_rx(socket, data, size, context);
    if(rc >= 0) {
      break;
    }
    if((rtc_clock::current_time() - mark) >= timeout_usecs) {
      break;
    }
  }
  return rc;
}

static void* dpdk_context()
{
  dpdk_context_t *context = (dpdk_context_t *)malloc(sizeof(dpdk_context_t));
  int ret;
  
  char* fake_argv[1] = {(char *)"./fake"};

  /* init EAL */
  ret = rte_eal_init(1, fake_argv);
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
 
  if(rte_eth_dev_count() == 0) {
    rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");
  }
  
  init_port_conf();
  rte_eth_dev_configure(0, num_queues, num_queues, &port_conf);

  // Assume port 0, core 1 ....

  for(int i=0;i<num_queues;i++) {
    char pool_name[50];
    sprintf(pool_name, "mbuf_pool%d", i);
    // Mempool
    context->mempools[i] = rte_pktmbuf_pool_create(pool_name,
						   8191,
						   32,
						   0,
						   RTE_PKTMBUF_HEADROOM + MSG_MAXSIZE,
						   rte_socket_id());
    if (context->mempools[i] == NULL)
      rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

    //tx queue
    ret = rte_eth_tx_queue_setup(0, 
				 i, 
				 nb_txd,
				 rte_eth_dev_socket_id(0),
				 NULL);

    if (ret < 0)
      rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
	       ret, (unsigned) 0);
    

    
    context->buffers[i] = (rte_eth_dev_tx_buffer *)
      rte_zmalloc_socket("tx_buffer",
			 RTE_ETH_TX_BUFFER_SIZE(1), 
			 0,
			 rte_eth_dev_socket_id(0));
    if (context->buffers[i] == NULL)
      rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
	       (unsigned) 0);

    
    rte_eth_tx_buffer_init(context->buffers[i], 1);

    // rx queue
    ret = rte_eth_rx_queue_setup(0, 
				 i, 
				 nb_rxd,
				 rte_eth_dev_socket_id(0),
				 NULL,
				 context->mempools[i]);
    if (ret < 0)
      rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
	       ret, 0);

    BOOST_LOG_TRIVIAL(info) << "CYCLONE_COMM:DPDK setup queue " << i;
  }
  /* Start device */
  ret = rte_eth_dev_start(0);
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
	     ret, (unsigned) 0);
  rte_eth_promiscuous_enable(0);
  rte_eth_macaddr_get(0, &context->port_macaddr);
  
  return context;
}

static void* cyclone_socket_out(void *context)
{
  dpdk_socket_t *socket;
  dpdk_context_t *dpdk_context = (dpdk_context_t *)context;
  socket = (dpdk_socket_t *)malloc(sizeof(dpdk_context_t));
  socket->context = dpdk_context;
  ether_addr_copy(&dpdk_context->port_macaddr, &socket->local_mac);
  socket->port_id = 0;
  return socket;
}

static void* cyclone_socket_in(void *context)
{
  dpdk_socket_t *socket;
  dpdk_context_t *dpdk_context = (dpdk_context_t *)context;
  socket = (dpdk_socket_t *)malloc(sizeof(dpdk_context_t));
  socket->context = dpdk_context;
  ether_addr_copy(&dpdk_context->port_macaddr, &socket->local_mac);
  socket->port_id = 0;
  return socket;
}

static void* dpdk_set_socket_queue(void *socket, int q)
{
  dpdk_socket_t *s  = (dpdk_socket_t *)socket;
  dpdk_context_t *c = s->context;
  s->mempool  = c->mempools[q];
  s->buffer   = c->buffers[q];
  s->queue_id = q; 
  // flow ids to match to correct queue
  // assuming intel rss hash in effect at rx end
  s->flow_ip_src = 0;
  if(q == 0)
    s->flow_ip_dst = 0;
  else if(q == 1)
    s->flow_ip_dst = 101;
  else
    s->flow_ip_dst = 13203;
  s->buffered = 0;
  s->consumed = 0;
}


static void cyclone_connect_endpoint(void *socket, 
				     int mc,
				     int queue,
				     boost::property_tree::ptree *pt)
{
  std::stringstream key; 
  std::stringstream addr;
  dpdk_socket_t *s  = (dpdk_socket_t *)socket;
  dpdk_set_socket_queue(socket, queue);
  key.str("");key.clear();
  addr.str("");addr.clear();
  key << "machines.addr" << mc;
  addr << pt->get<std::string>(key.str().c_str());
  
  sscanf(addr.str().c_str(), 
	 "%02X:%02X:%02X:%02X:%02X:%02X",
	 &s->remote_mac.addr_bytes[0],
	 &s->remote_mac.addr_bytes[1],
	 &s->remote_mac.addr_bytes[2],
	 &s->remote_mac.addr_bytes[3],
	 &s->remote_mac.addr_bytes[4],
	 &s->remote_mac.addr_bytes[5]);
  BOOST_LOG_TRIVIAL(info) << "CYCLONE::COMM::DPDK Connecting to " 
			  << addr.str().c_str();
}

static void cyclone_bind_endpoint(void *socket, 
				  int mc,
				  int queue,
				  boost::property_tree::ptree *pt)
{
  dpdk_socket_t *s  = (dpdk_socket_t *)socket;
  dpdk_set_socket_queue(socket, queue);
  BOOST_LOG_TRIVIAL(info) << "CYCLONE::COMM::DPDK Binding to queue " 
			  << queue;
  // Connectionless
}

#endif
