#ifndef _CYCLONE_COMM_DPDK_
#define _CYCLONE_COMM_DPDK_

#include <cinttypes>
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

#include "clwb_sim.hpp"

#define JUMBO_FRAME_MAX_SIZE    0x2600
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
#define RTE_RESP_RX_DESC_DEFAULT 128
#define RTE_RESP_TX_DESC_DEFAULT 512
#define PKT_BURST 32
#define CHAIN_SZ  32    /* Set to be same as pkt burst */ 
#define IP_DEFTTL  64   /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN  0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)


static const uint16_t nb_rxd = 8*RTE_TEST_RX_DESC_DEFAULT;
static const uint16_t nb_txd = 2*RTE_TEST_TX_DESC_DEFAULT;

const unsigned long magic_src_ip = 0xdeadbeef;

static struct rte_eth_conf port_conf;

// Because C++ did not see it fit to include struct inits
static void init_port_conf()
{
  port_conf.rxmode.mq_mode        = ETH_MQ_RX_NONE;
  port_conf.rxmode.max_rx_pkt_len = JUMBO_FRAME_MAX_SIZE;
  port_conf.rxmode.split_hdr_size = 0;
  port_conf.rxmode.header_split   = 0; 
  port_conf.rxmode.hw_ip_checksum = 0; 
  port_conf.rxmode.hw_vlan_filter = 0; 
  port_conf.rxmode.jumbo_frame    = 1; // needed for chained mbufs 
  port_conf.rxmode.hw_strip_crc   = 0; 
  port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;
  //port_conf.rx_adv_conf.rss_conf.rss_key = NULL; // Set Intel RSS hash
  //port_conf.rx_adv_conf.rss_conf.rss_key_len = 0;
  //port_conf.rx_adv_conf.rss_conf.rss_hf = ETH_RSS_IP;
}

/*
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
*/


typedef struct {
  struct ether_addr **mc_addresses;
  struct rte_mempool **mempools;
  struct rte_mempool **extra_pools;
  struct rte_eth_dev_tx_buffer **buffers;
  int me;
  int ports;
} dpdk_context_t;

typedef struct {
  rte_mbuf *burst[PKT_BURST];
  int buffered;
  int consumed;
} dpdk_rx_buffer_t;

// Mapping of quorums to ports
static int queue2port(int queue, int num_ports)
{
  return queue % num_ports;
}

static int num_queues_at_port(int port,
			      int num_queues,
			      int num_ports)
{
  int base = num_queues/num_ports;
  if(port < num_queues % num_ports) {
    base++;
  }
  return base;
}

static int queue_index_at_port(int queue, int num_ports)
{
  return queue/num_ports;
}

static void persist_mbuf(rte_mbuf *m)
{
  int block_counts = 0;
  while(m != NULL) {
    block_counts = clflush_partial(m, sizeof(rte_mbuf), block_counts);
    block_counts = clflush_partial(m->buf_addr, m->data_len, block_counts);
    m = m->next;
  }
}

static void initialize_ipv4_header(rte_mbuf *m,
				   struct ipv4_hdr *ip_hdr, 
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
  m->l2_len = sizeof(struct ether_hdr);
  m->l3_len = pkt_len;
  m->ol_flags |= PKT_TX_IP_CKSUM;
  ip_hdr->hdr_checksum = 0;
  /*
   * Compute IP header checksum.
   */
  /*
  ptr16 = (unaligned_uint16_t *)ip_hdr;
  ip_cksum = 0;
  ip_cksum += ptr16[0]; ip_cksum += ptr16[1];
  ip_cksum += ptr16[2]; ip_cksum += ptr16[3];
  ip_cksum += ptr16[4];
  ip_cksum += ptr16[6]; ip_cksum += ptr16[7];
  ip_cksum += ptr16[8]; ip_cksum += ptr16[9];
  */
  /*
   * Reduce 32 bit checksum to 16 bits and complement it.
   */
  /*
  ip_cksum = ((ip_cksum & 0xFFFF0000) >> 16) +
    (ip_cksum & 0x0000FFFF);
  ip_cksum %= 65536;
  ip_cksum = (~ip_cksum) & 0x0000FFFF;
  if (ip_cksum == 0)
    ip_cksum = 0xFFFF;
  ip_hdr->hdr_checksum = (uint16_t) ip_cksum;
  */
}


static void cyclone_prep_mbuf(dpdk_context_t *context,
			      int dst,
			      int dst_q,
			      rte_mbuf *m,
			      void *data,
			      int size)
{
  struct ether_hdr *eth;
  eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
  memset(eth, 0, sizeof(struct ether_hdr));
  int port   = queue2port(dst_q, context->ports);
  int dst_qindex = queue_index_at_port(dst_q, context->ports);
  ether_addr_copy(&context->mc_addresses[dst][port], &eth->d_addr);
  ether_addr_copy(&context->mc_addresses[context->me][port], &eth->s_addr);
  eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
  struct ipv4_hdr *ip = (struct ipv4_hdr *)(eth + 1);
  initialize_ipv4_header(m,
			 ip,
			 magic_src_ip, 
			 dst_qindex,
			 size);
  rte_memcpy(ip + 1, data, size);
  
  ///////////////////////
  m->pkt_len = 
    sizeof(struct ether_hdr) + 
    sizeof(struct ipv4_hdr)  + 
    size;
  m->data_len = m->pkt_len;
}

static void cyclone_prep_mbuf_server2client(dpdk_context_t *context,
					    int port,
					    int dst,
					    int dst_q,
					    rte_mbuf *m,
					    void *data,
					    int size)
{
  struct ether_hdr *eth;
  eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
  memset(eth, 0, sizeof(struct ether_hdr));
  ether_addr_copy(&context->mc_addresses[dst][0], &eth->d_addr);
  ether_addr_copy(&context->mc_addresses[context->me][port], &eth->s_addr);
  eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
  struct ipv4_hdr *ip = (struct ipv4_hdr *)(eth + 1);
  initialize_ipv4_header(m,
			 ip,
			 magic_src_ip, 
			 dst_q,
			 size);
  rte_memcpy(ip + 1, data, size);
  
  ///////////////////////
  m->pkt_len = 
    sizeof(struct ether_hdr) + 
    sizeof(struct ipv4_hdr)  + 
    size;
  m->data_len = m->pkt_len;
}


static void cyclone_prep_mbuf_client2server(dpdk_context_t *context,
					    int port,
					    int dst,
					    int dst_q,
					    rte_mbuf *m,
					    void *data,
					    int size)
{
  struct ether_hdr *eth;
  eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
  memset(eth, 0, sizeof(struct ether_hdr));
  ether_addr_copy(&context->mc_addresses[dst][port], &eth->d_addr);
  ether_addr_copy(&context->mc_addresses[context->me][0], &eth->s_addr);
  eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
  struct ipv4_hdr *ip = (struct ipv4_hdr *)(eth + 1);
  initialize_ipv4_header(m,
			 ip,
			 magic_src_ip, 
			 dst_q,
			 size);
  rte_memcpy(ip + 1, data, size);
  
  ///////////////////////
  m->pkt_len = 
    sizeof(struct ether_hdr) + 
    sizeof(struct ipv4_hdr)  + 
    size;
  m->data_len = m->pkt_len;
}

static void cyclone_prep_eth(dpdk_context_t *context,
			     int port,
			     int dst,
			     struct ether_hdr *eth)
{
  memset(eth, 0, sizeof(struct ether_hdr));
  ether_addr_copy(&context->mc_addresses[dst][port], &eth->d_addr);
  ether_addr_copy(&context->mc_addresses[context->me][port], &eth->s_addr);
  eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
}

// Best effort
static int cyclone_tx(dpdk_context_t *context, rte_mbuf *m, int q)
{
  int port = queue2port(q, context->ports);
  int qindex = queue_index_at_port(q, context->ports);
  int sent = rte_eth_tx_buffer(port, qindex, context->buffers[q], m);
  sent += rte_eth_tx_buffer_flush(port, qindex, context->buffers[q]);
  if(sent)
    return 0;
  else
    return -1;
}


static int cyclone_buffer_pkt(dpdk_context_t *context, int port, rte_mbuf *m, int q)
{
  int qindex = queue_index_at_port(q, context->ports);
  return rte_eth_tx_buffer(port, qindex, context->buffers[q], m);
}

static int cyclone_flush_buffer(dpdk_context_t *context, int port, int q)
{
  int qindex = queue_index_at_port(q, context->ports);
  return rte_eth_tx_buffer_flush(port, qindex, context->buffers[q]);
}

static int cyclone_rx_burst(int port, 
			    int q, 
			    rte_mbuf **buffers,
			    int burst_size)
{
  return rte_eth_rx_burst(port, q, buffers, burst_size);
}

// Best effort
static int cyclone_rx_buffered(dpdk_context_t *context,
			       int port,
			       int q,
			       dpdk_rx_buffer_t *buf,
			       unsigned char *data,
			       unsigned long size)
{
  int rc, nb_rx;
  rte_mbuf *m;
  if(buf->buffered == buf->consumed) {
    buf->consumed = 0;
    buf->buffered = cyclone_rx_burst(port, 
				     q,
				     &buf->burst[0], 
				     PKT_BURST);
  }
  if(buf->consumed < buf->buffered) {
    m = buf->burst[buf->consumed++];
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

// Block till data available or timeout
static int cyclone_rx_timeout(dpdk_context_t *context,
			      int port,
			      int q,
			      dpdk_rx_buffer_t *buf,
			      unsigned char *data,
			      unsigned long size,
			      unsigned long timeout_usecs)
{
  int rc;
  unsigned long mark = rtc_clock::current_time();
  while (true) {
    rc = cyclone_rx_buffered(context, port, q, buf, data, size);
    if(rc >= 0) {
      break;
    }
    if((rtc_clock::current_time() - mark) >= timeout_usecs) {
      break;
    }
  }
  return rc;
}


static void init_filter_clean(struct rte_eth_ntuple_filter *filter)
{
  filter->flags = RTE_5TUPLE_FLAGS;
  filter->dst_ip = 0; // To be set
  filter->dst_ip_mask = UINT32_MAX; /* Enable */
  filter->src_ip = magic_src_ip;
  filter->src_ip_mask = UINT32_MAX; /* Enable */
  filter->dst_port = 0;
  filter->dst_port_mask = 0; /* Disable */
  filter->src_port = 0;
  filter->src_port_mask = 0; /* Disable */
  filter->proto = 0;
  filter->proto_mask = 0; /* Disable */
  filter->tcp_flags = 0;
  filter->priority = 7; /* Highest */
  filter->queue = 0; // To be set
}

static void install_eth_filters(int port, int queues)
{
  struct rte_eth_ntuple_filter filter;
  struct rte_eth_ntuple_filter filter_clean;
  init_filter_clean(&filter_clean);
  if(rte_eth_dev_filter_supported(port, RTE_ETH_FILTER_NTUPLE) != 0) {
    rte_exit(EXIT_FAILURE, "rte_eth_dev does not support ntuple filter");
  }

  for(int i=0;i < queues;i++) {
    if(i > 0) {
      memcpy(&filter, &filter_clean, sizeof(rte_eth_ntuple_filter));
      filter.dst_ip = i;
      filter.queue  = i;
      int ret = rte_eth_dev_filter_ctrl(port,
					RTE_ETH_FILTER_NTUPLE,
					RTE_ETH_FILTER_ADD,
					&filter);
      
      if (ret != 0)
	rte_exit(EXIT_FAILURE, "rte_eth_dev_filter_ctrl:err=%d, port=%u\n",
		 ret, (unsigned) port);
      else
	BOOST_LOG_TRIVIAL(info) << "Added filter for rxq " << i;
    }
    else { // stop the queue to drop unwanted packets
      int ret = rte_eth_dev_rx_queue_stop(port, 0);
      if(ret != 0)
	rte_exit(EXIT_FAILURE, "rte_eth_dev_queue_stop:err=%d, port=%u\n",
		 ret, (unsigned) port);
      else
	BOOST_LOG_TRIVIAL(info) << "Stopped rxq 0 on port" << port;
    }
  }
}

static void dpdk_context_init(dpdk_context_t *context, 
			      int max_pktsize, 
			      int pack_ratio,
			      int queues)
{
  int ret;
  
  char* fake_argv[1] = {(char *)"./fake"};

  struct rte_eth_dev_info dev_info;
  struct rte_eth_txconf *txconf;
  unsigned long max_req_size;
  
  BOOST_LOG_TRIVIAL(info) << "MAXIMUM PKTSIZE = " << max_pktsize;
  pack_ratio = 32; // Forced by burst recv. limitations
  BOOST_LOG_TRIVIAL(info) << "PACK RATIO = " << pack_ratio;
  BOOST_LOG_TRIVIAL(info) << "PORTS = " << context->ports;

  /* init EAL */
  ret = rte_eal_init(1, fake_argv);
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
 
  if(rte_eth_dev_count() == 0) {
    rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");
  }
  
  for(int i=0; i<context->ports; i++) {
    init_port_conf();
    int qs_at_port = num_queues_at_port(i, queues, context->ports);
    rte_eth_dev_configure(i, qs_at_port, qs_at_port, &port_conf);
  }
  context->mempools = (rte_mempool **)malloc(queues*sizeof(rte_mempool *));
  context->buffers   = (rte_eth_dev_tx_buffer **)malloc
    (queues*sizeof(rte_eth_dev_tx_buffer *));
  context->extra_pools = (rte_mempool **)malloc(num_quorums*sizeof(rte_mempool *));
  for(int i=0;i<queues;i++) {
    char pool_name[500];
    sprintf(pool_name, "mbuf_pool%d", i);
    // Mempool
    if(max_pktsize < 2048) {
      max_pktsize = 2048;
    }
    
    if(max_pktsize > 2048 && max_pktsize < 4096) {
      max_pktsize = 4096;
    }
    
    if(max_pktsize > 4096 && max_pktsize < 8192) {
      max_pktsize = 8192;
    }

    if(max_pktsize > 8192 && max_pktsize < 16384) {
      max_pktsize = 16384;
    }
    
    max_req_size = max_pktsize/pack_ratio;
    if(max_req_size < RTE_MBUF_DEFAULT_DATAROOM) {
      max_req_size = RTE_MBUF_DEFAULT_DATAROOM;
    }
    

    BOOST_LOG_TRIVIAL(info) << "Init mempool max pktsize = " << max_pktsize;
    BOOST_LOG_TRIVIAL(info) << "Init mempool max reqsize = " << max_req_size;
    int my_port = queue2port(i, context->ports);

    bool is_raft_pool =
      (i >= context->ports) &&
      (i < (context->ports + num_queues*num_quorums)) && 
      ((i - context->ports) % num_queues == q_raft);
    bool is_disp_pool = 
      (i >= context->ports) &&
      (i < (context->ports + num_queues*num_quorums)) &&
      ((i - context->ports) % num_queues == q_dispatcher);
    if(is_disp_pool) {
      context->mempools[i] = rte_pktmbuf_pool_create(pool_name,
						     Q_BUFS*pack_ratio,
						     32,
						     0,
						     RTE_PKTMBUF_HEADROOM + max_req_size,
						     rte_eth_dev_socket_id(my_port));
    }
    else if(is_raft_pool) {
      context->mempools[i] = rte_pktmbuf_pool_create(pool_name,
						     Q_BUFS,
						     32,
						     0,
						     RTE_PKTMBUF_HEADROOM + max_pktsize,
						     rte_eth_dev_socket_id(my_port));
    }
    else {
      context->mempools[i] = rte_pktmbuf_pool_create(pool_name,
						     R_BUFS,
						     32,
						     0,
						     RTE_PKTMBUF_HEADROOM + max_req_size,
						     rte_eth_dev_socket_id(my_port));
    }
    if (context->mempools[i] == NULL)
      rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

    if(is_raft_pool) {
      sprintf(pool_name, "extra_%d", i);
      context->extra_pools[(i - context->ports)/num_queues] = rte_pktmbuf_pool_create(pool_name,
								  Q_BUFS,
								  4*PKT_BURST,
								  0,
								  RTE_PKTMBUF_HEADROOM + sizeof(struct ether_hdr),
								  rte_eth_dev_socket_id(my_port));
      
      if (context->extra_pools[(i - context->ports)/num_queues] == NULL)
	rte_exit(EXIT_FAILURE, "Cannot init mbuf extra pool\n");
      
    }

    //tx queue
    rte_eth_dev_info_get(my_port, &dev_info);
    txconf = &dev_info.default_txconf;
    txconf->txq_flags = 0;
    ret = rte_eth_tx_queue_setup(my_port, 
				 queue_index_at_port(i, context->ports), 
				 (is_raft_pool || is_disp_pool) ? nb_txd:RTE_RESP_TX_DESC_DEFAULT,
				 rte_eth_dev_socket_id(my_port),
				 txconf);
    if (ret < 0)
      rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
	       ret, (unsigned) my_port);
    
    context->buffers[i] = (rte_eth_dev_tx_buffer *)
      rte_zmalloc_socket("tx_buffer",
			 RTE_ETH_TX_BUFFER_SIZE(PKT_BURST), 
			 0,
			 rte_eth_dev_socket_id(my_port));
    if (context->buffers[i] == NULL)
      rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
	       (unsigned) my_port);

    
    rte_eth_tx_buffer_init(context->buffers[i], PKT_BURST);
    
    // rx queue
    ret = rte_eth_rx_queue_setup(my_port, 
				 queue_index_at_port(i, context->ports), 
				 (is_raft_pool || is_disp_pool) ? nb_rxd:RTE_RESP_RX_DESC_DEFAULT,
				 rte_eth_dev_socket_id(my_port),
				 NULL,
				 context->mempools[i]);
    if (ret < 0)
      rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
	       ret, my_port);
    BOOST_LOG_TRIVIAL(info) << "CYCLONE_COMM:DPDK setup queue " << i;
  }

  /* Start device */
  for(int j=0;j<context->ports;j++) {
    ret = rte_eth_dev_start(j);
    if (ret < 0)
      rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
	       ret, (unsigned) j);
    // NOTE:DO NOT ENABLE PROMISCOUS MODE
    // OW need to check eth addr on all incoming packets
    //rte_eth_promiscuous_enable(0);
    install_eth_filters(j, num_queues_at_port(j, queues, context->ports));
    //rte_eth_dev_set_mtu(0, 2500);
    rte_eth_macaddr_get(j, &context->mc_addresses[context->me][j]);
  }
}

#endif
