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
#include <rte_eth_ctrl.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_byteorder.h>

#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
static const uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static const uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

const unsigned long magic_src_ip = 0xdeadbeef;

static struct rte_eth_conf port_conf;

// Because C++ did not see it fit to include struct inits
static void init_port_conf()
{
  port_conf.rxmode.mq_mode        = ETH_MQ_RX_NONE;
  port_conf.rxmode.max_rx_pkt_len = ETHER_MAX_LEN;
  port_conf.rxmode.split_hdr_size = 0;
  port_conf.rxmode.header_split   = 0; 
  port_conf.rxmode.hw_ip_checksum = 0; 
  port_conf.rxmode.hw_vlan_filter = 0; 
  port_conf.rxmode.jumbo_frame    = 0; 
  port_conf.rxmode.hw_strip_crc   = 0; 
  port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;
  //port_conf.rx_adv_conf.rss_conf.rss_key = NULL; // Set Intel RSS hash
  //port_conf.rx_adv_conf.rss_conf.rss_key_len = 0;
  //port_conf.rx_adv_conf.rss_conf.rss_hf = ETH_RSS_IP;
}

static struct rte_fdir_conf fdir_conf;
static void init_fdir_conf()
{
  fdir_conf.mode = RTE_FDIR_MODE_PERFECT;
  fdir_conf.pballoc = RTE_FDIR_PBALLOC_64K;
  fdir_conf.status = RTE_FDIR_REPORT_STATUS;
  fdir_conf.mask.vlan_tci_mask = 0x0;
  fdir_conf.mask.ipv4_mask.src_ip = 0xFFFFFFFF;
  fdir_conf.mask.ipv4_mask.dst_ip = 0xFFFFFFFF;
  fdir_conf.mask.ipv6_mask.src_ip = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  fdir_conf.mask.ipv6_mask.dst_ip = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  fdir_conf.mask.src_port_mask = 0xFFFF;
  fdir_conf.mask.dst_port_mask = 0xFFFF;
  fdir_conf.mask.mac_addr_byte_mask = 0xFF;
  fdir_conf.mask.tunnel_type_mask = 1;
  fdir_conf.mask.tunnel_id_mask = 0xFFFFFFFF;
  fdir_conf.drop_queue = 127;
};



typedef struct {
  struct ether_addr port_macaddr;
  struct rte_mempool **mempools;
  struct rte_eth_dev_tx_buffer **buffers;
} dpdk_context_t;

#define PKT_BURST 32
typedef struct  {
  dpdk_context_t *context;
  struct rte_mempool *mempool;
  struct rte_eth_dev_tx_buffer *buffer;
  struct ether_addr remote_mac;
  struct ether_addr local_mac;
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
  //ip_hdr->src_addr = rte_cpu_to_be_32(src_addr);
  //ip_hdr->dst_addr = rte_cpu_to_be_32(dst_addr);
  ip_hdr->src_addr = src_addr;
  ip_hdr->dst_addr = dst_addr;

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
  if(m == NULL) {
    BOOST_LOG_TRIVIAL(warning) << "Unable to allocate pktmbuf "
			       << "for queue:" << dpdk_socket->queue_id;
    return -1;
  }
  struct ether_hdr *eth;
  eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
  memset(eth, 0, sizeof(struct ether_hdr));
  ether_addr_copy(&dpdk_socket->remote_mac, &eth->d_addr);
  ether_addr_copy(&dpdk_socket->local_mac, &eth->s_addr);
  eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
  struct ipv4_hdr *ip = (struct ipv4_hdr *)(eth + 1);
  initialize_ipv4_header(ip,
			 magic_src_ip, 
			 dpdk_socket->queue_id,
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

static int cyclone_tx_queue_queue(void *socket,
				  const unsigned char *data,
				  unsigned long size,
				  int remote_queue,
				  int local_queue,
				  const char *context) 
{
  dpdk_socket_t *dpdk_socket = (dpdk_socket_t *)socket;
  rte_mbuf *m = rte_pktmbuf_alloc(dpdk_socket->context->mempools[local_queue]);
  if(m == NULL) {
    BOOST_LOG_TRIVIAL(warning) << "Unable to allocate pktmbuf "
			       << "for queue:" << local_queue;
    return -1;
  }
  struct ether_hdr *eth;
  eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
  memset(eth, 0, sizeof(struct ether_hdr));
  ether_addr_copy(&dpdk_socket->remote_mac, &eth->d_addr);
  ether_addr_copy(&dpdk_socket->local_mac, &eth->s_addr);
  eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
  struct ipv4_hdr *ip = (struct ipv4_hdr *)(eth + 1);
  initialize_ipv4_header(ip, 
			 magic_src_ip,
			 remote_queue,
			 size);
  rte_memcpy(ip + 1, data, size);
  
  ///////////////////////
  m->pkt_len = 
    sizeof(struct ether_hdr) + 
    sizeof(struct ipv4_hdr)  + 
    size;
  m->data_len = m->pkt_len;
  int sent = rte_eth_tx_buffer(dpdk_socket->port_id, 
			       local_queue, 
			       dpdk_socket->context->buffers[local_queue],
			       m);
  sent += rte_eth_tx_buffer_flush(dpdk_socket->port_id, 
				  local_queue,
				  dpdk_socket->context->buffers[local_queue]);
  if(sent)
    return 0;
  else
    return -1;
}


static int cyclone_tx_rand_queue(void *socket,
				 const unsigned char *data,
				 unsigned long size,
				 const char *context) 
{
  dpdk_socket_t *dpdk_socket = (dpdk_socket_t *)socket;
  rte_mbuf *m = rte_pktmbuf_alloc(dpdk_socket->mempool);
  if(m == NULL) {
    BOOST_LOG_TRIVIAL(warning) << "Unable to allocate pktmbuf "
			       << "for queue:" << dpdk_socket->queue_id;
    return -1;
  }
  struct ether_hdr *eth;
  eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
  memset(eth, 0, sizeof(struct ether_hdr));
  ether_addr_copy(&dpdk_socket->remote_mac, &eth->d_addr);
  ether_addr_copy(&dpdk_socket->local_mac, &eth->s_addr);
  eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
  struct ipv4_hdr *ip = (struct ipv4_hdr *)(eth + 1);
  initialize_ipv4_header(ip, 
			 magic_src_ip,
			 num_queues + rand()%executor_threads,
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
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(e + 1);
    if(e->ether_type != rte_cpu_to_be_16(ETHER_TYPE_IPv4)) {
      BOOST_LOG_TRIVIAL(warning) << "Dropping junk. Protocol mismatch";
      rte_pktmbuf_free(m);
      return -1;
    }
    else if(ip->src_addr != magic_src_ip) {
      BOOST_LOG_TRIVIAL(warning) << "Dropping junk. non magic ip";
      rte_pktmbuf_free(m);
      return -1;
    }
    else if(m->data_len <= sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr)) {
      BOOST_LOG_TRIVIAL(warning) << "Dropping junk = pkt size too small";
      rte_pktmbuf_free(m);
      return -1;
    }
    // Turn on following check if the NIC is left in promiscous mode
    // drop unless this is for me
    //if(!is_same_ether_addr(&e->d_addr, &dpdk_socket->local_mac)) {
    //  rte_pktmbuf_free(m);
    //  return -1;
    //}
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
  dpdk_socket_t *dpdk_socket = (dpdk_socket_t *)socket;
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


static struct rte_eth_ntuple_filter filter_clean;


static void init_filter_clean()
{
  filter_clean.flags = RTE_5TUPLE_FLAGS;
  filter_clean.dst_ip = 0; // To be set
  filter_clean.dst_ip_mask = UINT32_MAX; /* Enable */
  filter_clean.src_ip = 0;
  filter_clean.src_ip_mask = 0; /* Disable */
  filter_clean.dst_port = 0;
  filter_clean.dst_port_mask = 0; /* Disable */
  filter_clean.src_port = 0;
  filter_clean.src_port_mask = 0; /* Disable */
  filter_clean.proto = 0;
  filter_clean.proto_mask = 0; /* Disable */
  filter_clean.tcp_flags = 0;
  filter_clean.priority = 7; /* Highest */
  filter_clean.queue = 0; // To be set
}

static void install_eth_filters_server()
{
  struct rte_eth_ntuple_filter filter;
  init_filter_clean();
  if(rte_eth_dev_filter_supported(0, RTE_ETH_FILTER_NTUPLE) != 0) {
    rte_exit(EXIT_FAILURE, "rte_eth_dev does not support ntuple filter");
  }
  for(int i=0;i < num_queues + executor_threads;i++) {
    memcpy(&filter, &filter_clean, sizeof(rte_eth_ntuple_filter));
    filter.dst_ip = i;
    filter.queue  = i;
    int ret = rte_eth_dev_filter_ctrl(0,
				      RTE_ETH_FILTER_NTUPLE,
				      RTE_ETH_FILTER_ADD,
				      &filter);
    
    if (ret != 0)
      rte_exit(EXIT_FAILURE, "rte_eth_dev_filter_ctrl:err=%d, port=%u\n",
	       ret, (unsigned) 0);
    else
      BOOST_LOG_TRIVIAL(info) << "Added filter for rxq " << i;
    
  }
}

static void install_eth_filters_client(int threads)
{
  struct rte_eth_ntuple_filter filter;
  init_filter_clean();
  if(rte_eth_dev_filter_supported(0, RTE_ETH_FILTER_NTUPLE) != 0) {
    rte_exit(EXIT_FAILURE, "rte_eth_dev does not support ntuple filter");
  }
  for(int i=0;i < (num_queues + threads);i++) {
    memcpy(&filter, &filter_clean, sizeof(rte_eth_ntuple_filter));
    filter.dst_ip = i;
    filter.queue  = i;
    int ret = rte_eth_dev_filter_ctrl(0,
				      RTE_ETH_FILTER_NTUPLE,
				      RTE_ETH_FILTER_ADD,
				      &filter);
    
    if (ret != 0)
      rte_exit(EXIT_FAILURE, "rte_eth_dev_filter_ctrl:err=%d, port=%u\n",
	       ret, (unsigned) 0);
    else
      BOOST_LOG_TRIVIAL(info) << "Added filter for rxq " << i;
    
  }
}

static void* dpdk_context()
{
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
  //init_fdir_conf();
  //port_conf.fdir_conf = fdir_conf;
  rte_eth_dev_configure(0, num_queues + executor_threads, num_queues + executor_threads, &port_conf);

  dpdk_context_t *context = 
    (dpdk_context_t *)rte_malloc("context", sizeof(dpdk_context_t), 0);

  int total_queues = num_queues + executor_threads;

  context->mempools = (rte_mempool **)malloc(total_queues*sizeof(rte_mempool *));
  context->buffers   = (rte_eth_dev_tx_buffer **)malloc
    (total_queues*sizeof(rte_eth_dev_tx_buffer *));
  // Assume port 0, core 1 ....

  for(int i=0;i<num_queues + executor_threads;i++) {
    char pool_name[50];
    sprintf(pool_name, "mbuf_pool%d", i);
    // Mempool
    int pktsize = sizeof(struct ether_hdr) + MSG_MAXSIZE;
    if(pktsize < 2048) {
      pktsize = 2048;
    }
    context->mempools[i] = rte_pktmbuf_pool_create(pool_name,
						   8191,
						   32,
						   0,
						   RTE_PKTMBUF_HEADROOM + pktsize,
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
  // NOTE:DO NOT ENABLE PROMISCOUS MODE
  // OW need to check eth addr on all incoming packets
  //rte_eth_promiscuous_enable(0);
  install_eth_filters_server();
  rte_eth_macaddr_get(0, &context->port_macaddr);
  
  return context;
}

static void* dpdk_context_client(int threads)
{
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
  //init_fdir_conf();
  //port_conf.fdir_conf = fdir_conf;
  rte_eth_dev_configure(0, num_queues + threads, num_queues + threads, &port_conf);

  dpdk_context_t *context = 
    (dpdk_context_t *)rte_malloc("context", sizeof(dpdk_context_t), 0);

  int total_queues = num_queues + threads;

  context->mempools = (rte_mempool **)malloc(total_queues*sizeof(rte_mempool *));
  context->buffers   = (rte_eth_dev_tx_buffer **)malloc
    (total_queues*sizeof(rte_eth_dev_tx_buffer *));
  // Assume port 0, core 1 ....

  for(int i=0;i< (num_queues + threads);i++) {
    char pool_name[50];
    sprintf(pool_name, "mbuf_pool%d", i);
    // Mempool
    int pktsize = sizeof(struct ether_hdr) + MSG_MAXSIZE;
    if(pktsize < 2048) {
      pktsize = 2048;
    }
    context->mempools[i] = rte_pktmbuf_pool_create(pool_name,
						   8191,
						   32,
						   0,
						   RTE_PKTMBUF_HEADROOM + pktsize,
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
  // NOTE:DO NOT ENABLE PROMISCOUS MODE
  // OW need to check eth addr on all incoming packets
  //rte_eth_promiscuous_enable(0);
  install_eth_filters_client(threads);
  rte_eth_macaddr_get(0, &context->port_macaddr);
  return context;
}

static void* cyclone_socket_out(void *context)
{
  dpdk_socket_t *socket;
  dpdk_context_t *dpdk_context = (dpdk_context_t *)context;
  socket = (dpdk_socket_t *)rte_malloc("socket", sizeof(dpdk_socket_t), 0);
  socket->context = dpdk_context;
  ether_addr_copy(&dpdk_context->port_macaddr, &socket->local_mac);
  socket->port_id = 0;
  return socket;
}

static void* cyclone_socket_in(void *context)
{
  dpdk_socket_t *socket;
  dpdk_context_t *dpdk_context = (dpdk_context_t *)context;
  socket = (dpdk_socket_t *)rte_malloc("socket", sizeof(dpdk_socket_t), 0);
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
