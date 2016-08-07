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

#define JUMBO_FRAME_MAX_SIZE    0x2600
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
#define PKT_BURST 32
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
  struct ether_addr *mc_addresses;
  struct rte_mempool **mempools;
  struct rte_mempool *extra_pool;
  struct rte_mempool *clone_pool;
  struct rte_eth_dev_tx_buffer **buffers;
  int me;
  int port_id;
} dpdk_context_t;

typedef struct {
  rte_mbuf *burst[PKT_BURST];
  int buffered;
  int consumed;
} dpdk_rx_buffer_t;

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
  ether_addr_copy(&context->mc_addresses[dst], &eth->d_addr);
  ether_addr_copy(&context->mc_addresses[context->me], &eth->s_addr);
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
			     int dst,
			     int dst_q,
			     struct ether_hdr *eth)
{
  memset(eth, 0, sizeof(struct ether_hdr));
  ether_addr_copy(&context->mc_addresses[dst], &eth->d_addr);
  ether_addr_copy(&context->mc_addresses[context->me], &eth->s_addr);
  eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
}

// Best effort
static int cyclone_tx(dpdk_context_t *context, rte_mbuf *m, int q)
{
  int sent = rte_eth_tx_buffer(context->port_id, q, context->buffers[q], m);
  sent += rte_eth_tx_buffer_flush(context->port_id, q, context->buffers[q]);
  if(sent)
    return 0;
  else
    return -1;
}


static int cyclone_buffer_pkt(dpdk_context_t *context, rte_mbuf *m, int q)
{
  return rte_eth_tx_buffer(context->port_id, q, context->buffers[q], m);
}

static int cyclone_flush_buffer(dpdk_context_t *context, int q)
{
  return rte_eth_tx_buffer_flush(context->port_id, q, context->buffers[q]);
}

// Best effort
static int cyclone_rx_buffered(dpdk_context_t *context,
			       int q,
			       dpdk_rx_buffer_t *buf,
			       unsigned char *data,
			       unsigned long size)
{
  int rc, nb_rx;
  rte_mbuf *m;
  if(buf->buffered == buf->consumed) {
    buf->consumed = 0;
    buf->buffered = rte_eth_rx_burst(context->port_id, 
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
			      int q,
			      dpdk_rx_buffer_t *buf,
			      unsigned char *data,
			      unsigned long size,
			      unsigned long timeout_usecs)
{
  int rc;
  unsigned long mark = rtc_clock::current_time();
  while (true) {
    rc = cyclone_rx_buffered(context, q, buf, data, size);
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

static void install_eth_filters(int queues)
{
  struct rte_eth_ntuple_filter filter;
  init_filter_clean();
  if(rte_eth_dev_filter_supported(0, RTE_ETH_FILTER_NTUPLE) != 0) {
    rte_exit(EXIT_FAILURE, "rte_eth_dev does not support ntuple filter");
  }
  for(int i=0;i < queues;i++) {
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

static void dpdk_context_init(dpdk_context_t *context, 
			      int max_pktsize, 
			      int pack_ratio,
			      int queues)
{
  int ret;
  
  char* fake_argv[1] = {(char *)"./fake"};

  struct rte_eth_dev_info dev_info;
  struct rte_eth_txconf *txconf;
  
  BOOST_LOG_TRIVIAL(info) << "MAXIMUM PKTSIZE = " << max_pktsize;
  BOOST_LOG_TRIVIAL(info) << "PACK RATIO = " << pack_ratio;

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
  rte_eth_dev_configure(0, queues, queues, &port_conf);
  context->port_id = 0;

  context->mempools = (rte_mempool **)malloc(queues*sizeof(rte_mempool *));
  context->buffers   = (rte_eth_dev_tx_buffer **)malloc
    (queues*sizeof(rte_eth_dev_tx_buffer *));
  // Assume port 0, core 1 ....

  for(int i=0;i<queues;i++) {
    char pool_name[50];
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
    
    
    BOOST_LOG_TRIVIAL(info) << "Init mempool max pktsize = " << max_pktsize;
    context->mempools[i] = rte_pktmbuf_pool_create(pool_name,
						   i != q_dispatcher ? Q_BUFS:Q_BUFS*pack_ratio,
						   32,
						   0,
						   RTE_PKTMBUF_HEADROOM + max_pktsize,
						   rte_socket_id());
    if (context->mempools[i] == NULL)
      rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

    if(i == q_raft) {
      strcat(pool_name, "extra");
      context->extra_pool = rte_pktmbuf_pool_create(pool_name,
						    Q_BUFS,
						    4*PKT_BURST,
						    0,
						    RTE_PKTMBUF_HEADROOM + sizeof(struct ether_hdr),
						    rte_socket_id());

      if (context->extra_pool == NULL)
	rte_exit(EXIT_FAILURE, "Cannot init mbuf extra pool\n");
      
      
      strcat(pool_name, "clone");
      context->clone_pool = rte_pktmbuf_pool_create(pool_name,
						    Q_BUFS*pack_ratio,
						    4*PKT_BURST,
						    0,
						    0,
						    rte_socket_id());
      if (context->clone_pool == NULL)
	rte_exit(EXIT_FAILURE, "Cannot init mbuf clone pool\n");
	       
    }

    //tx queue
    rte_eth_dev_info_get(0, &dev_info);
    txconf = &dev_info.default_txconf;
    txconf->txq_flags = 0;
    ret = rte_eth_tx_queue_setup(0, 
				 i, 
				 nb_txd,
				 rte_eth_dev_socket_id(0),
				 txconf);

    if (ret < 0)
      rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
	       ret, (unsigned) 0);
    

    
    context->buffers[i] = (rte_eth_dev_tx_buffer *)
      rte_zmalloc_socket("tx_buffer",
			 RTE_ETH_TX_BUFFER_SIZE(PKT_BURST), 
			 0,
			 rte_eth_dev_socket_id(0));
    if (context->buffers[i] == NULL)
      rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
	       (unsigned) 0);

    
    rte_eth_tx_buffer_init(context->buffers[i], PKT_BURST);

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
  install_eth_filters(queues);
  //rte_eth_dev_set_mtu(0, 2500);
  rte_eth_macaddr_get(0, &context->mc_addresses[context->me]);
}

#endif
