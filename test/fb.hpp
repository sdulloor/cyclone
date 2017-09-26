#ifndef _FB_COMMON_
#define _FB_COMMON_
#include "rocksdb.hpp"
//### New values following distribution in facebook's paper
//VALUE_SIZE_ARR=( 16 32 64 128 256 512 1024 2048 4096 8192 )
//NUM_OPS_ARR=( 20132864 10066944 10066944 35232768 25165824 3145728 1888256 2359296 786432 1022976 )
//LIMIT_OPS_ARR=( 15728640 7864320 5242880 8388608 12582912 1048576 524288 524288 367616 157696 )

// Truncated value sizes
// Div num keys by factor of 10
#include<random>
#include<vector>
#include<algorithm>
#include<iostream>
#include<array>
using namespace std;

#if 0
// ETC trace
static int values = 6;
static int VALUE_SIZE_ARR[] = { 16, 32, 64, 128, 256, 512 };
static int NUM_OPS_ARR [] = { 2013286, 
			      1006694, 
			      1006694, 
			      3523276, 
			      2516582,
			      314572 };
static double access_probs[] = { 0.309278, 
				 0.154639, 
				 0.103093, 
				 0.164948,
				 0.247423,
				 0.020619 };

const double prob_rd = 0.95;
#else
// VAR trace
static int values = 3;
static int VALUE_SIZE_ARR[] = { 32, 64, 128 };
// For 10M ops
static int NUM_OPS_ARR [] = { 1048576, 	// 0.1 
			      7340032, 				// 0.7 
			      2097152 }; 			// 0.2
static double access_probs[] = { 0.05, 
				 0.40, 
				 0.55 };

const double prob_rd = 0.20;
#endif

// 8 byte key
// most significant byte is index into value size array
// remaining bytes unique counter
class load_gen {
  vector<double> cdf;
  mt19937* base_rng;
  double flip_coin()
  {
    return  ((double)(*base_rng)() - base_rng->min())/(base_rng->max() - base_rng->min());
  }
public:
  // 0 == read
  // 1 == wr
  int select_op()
  {
    double ur = flip_coin();
    if(ur <= prob_rd)
      return 0;
    else
      return 1;
  }

  int select_index()
  {
    double ur = flip_coin();
    auto index_itr = upper_bound(cdf.begin(), cdf.end(), ur);
    if(index_itr == cdf.end()) 
      return values - 1;
    return index_itr - cdf.begin();
  }

  unsigned long gen_key(int idx)
  {
    return NUM_OPS_ARR[idx]*flip_coin();
  }

  load_gen(int client_seed)
  {
    base_rng = new mt19937(client_seed);
    double sum = 0.0;
    for(int i=0;i<values;i++) {
      sum = sum + access_probs[i];
      cdf.push_back(sum);
    }
    cdf.back() = 1.0;
  }
  
  void unit_test()
  {
    cout << flip_coin() << "\n";
  }

  void unit_test2()
  {
    cout << select_index() << "\n";
  }

};			      

typedef struct fb_kv_st {
  unsigned long op;
  unsigned long key;
}fb_kv_t;

#endif

