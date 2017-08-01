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
using namespace std;
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

// 8 byte key
// most significant byte is index into value size array
// remaining bytes unique counter
class load_gen {
  vector<double> cdf;
  mt19937* base_rng;
  uniform_real_distribution<double> * rng;
  double flip_coin()
  {
    return (*rng)(*base_rng);
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


  load_gen(int client_seed)
  {
    array<unsigned int, mt19937::state_size> seeds;
    for(int i=0;i<mt19937::state_size;i++) {
      seeds[i] = client_seed*(i + 1); // seed as a deterministic fn of client_seed
    }
    seed_seq seeds_in_seq(begin(seeds), end(seeds));
    base_rng = new mt19937(seeds_in_seq);
    rng = new uniform_real_distribution<double>(0.0, 1.0);
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

