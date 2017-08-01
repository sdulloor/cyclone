//### New values following distribution in facebook's paper
//VALUE_SIZE_ARR=( 16 32 64 128 256 512 1024 2048 4096 8192 )
//NUM_OPS_ARR=( 20132864 10066944 10066944 35232768 25165824 3145728 1888256 2359296 786432 1022976 )
//LIMIT_OPS_ARR=( 15728640 7864320 5242880 8388608 12582912 1048576 524288 524288 367616 157696 )

// Truncated value sizes
// Div num keys by factor of 10
static int values = 6;
static int VALUE_SIZE_ARR[] = { 16, 32, 64, 128, 256, 512 };
static int NUM_OPS_ARR [] = { 2013286, 1006694, 1006694, 3523276, 2516582,
			      314572 };
// 8 byte key
// most significant byte is index into value size array
// remaining bytes unique counter
			      

