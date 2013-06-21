#include <immintrin.h>
#include <stdio.h>
#include <sched.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>



#define XBEGIN_STARTED_INDEX 0
#define XABORT_EXPLICIT_INDEX 1
#define XABORT_RETRY_INDEX 2
#define XABORT_CONFLICT_INDEX 3
#define XABORT_CAPACITY_INDEX 4
#define XABORT_DEBUG_INDEX 5
#define XABORT_NESTED_INDEX 6

#define MAX_THREADS 8
#define UNIQ_STATUS (1 << 6)
#define USELESS_BITS 0xffffffc0 /*only the least sig 6 bits are zero*/
#define NUM_TRIES 10000

int workingset = 20;
#define ARRAYSIZE workingset*1024/4 //Working set/sizeof(int)

int statLog[MAX_THREADS][UNIQ_STATUS];
int statRetry[MAX_THREADS];
int tx_sz = 10;


int* bigArray;

const char *byte_to_binary(int x)
{
    static char b[9];
    b[0] = '\0';

    int z;
    for (z = 128; z > 0; z >>= 1)
    {
        strcat(b, ((x & z) == z) ? "1" : "0");
    }

    return b;
}


class RTMScope {

 public:
  RTMScope(int id) {
    while(true) {
      unsigned stat;
      stat = _xbegin ();
      if(stat == _XBEGIN_STARTED) {
        return;
      } else {
        //call some fallback function
        statRetry[id]++;
        assert((stat & USELESS_BITS)  == 0);
        statLog[id][stat& ~USELESS_BITS]++;
            
        if ((stat &  _XABORT_RETRY) == 0) {
            //will not succeed on a retry
            return;
        }
      }
    }
  }
  
  ~RTMScope() 
  {  
    _xend ();
 }

 private:
  RTMScope(const RTMScope&);
  void operator=(const RTMScope&);
};

void *
thread_run(void *x)
{
    int id = *(int *)x;
    /*
    struct drand48_data r_state;
    assert(srand48_r(id, &r_state) == 0);
    */
    for (int k = 0; k < NUM_TRIES; k++) {
        RTMScope tx(id);
        for (int i = 0; i < tx_sz; i++) {
            bigArray[id*MAX_THREADS+i]++; 
        }
    }
}


int main(int argc, char**argv)
{
    int n_th = 1;
    char ch = 0;
    while ((ch = getopt(argc, argv, "t:"))!= -1) {
        switch (ch) {
        case 't':
            n_th = atoi(optarg);
            break;
        case 's': 
            tx_sz= atoi(optarg);
            break;
        default:    
            break;
        }
    }        


	//bound to the third core
	/*
	cpu_set_t  mask;
	CPU_ZERO(&mask);
	CPU_SET(2, &mask);
	sched_setaffinity(0, sizeof(mask), &mask);
	*/
		
    bigArray = (int *)malloc(ARRAYSIZE);
    assert(MAX_THREADS * tx_sz < ARRAYSIZE);
	
	//Cache warmup	
    for(bigArray[0] = 1; bigArray[0] < ARRAYSIZE; bigArray[0]++)
	  bigArray[bigArray[0]] += bigArray[0];
	
    pthread_t th[MAX_THREADS];
    int ids[MAX_THREADS];
    for (int i = 0; i < n_th; i++) { 
        ids[i]=i;
        assert(pthread_create(&th[i], NULL, thread_run, (void *)&ids[i])==0);
    }

    for (int i = 0; i < n_th; i++) {
        pthread_join(th[i], NULL);
    }

    int stats[UNIQ_STATUS];
    bzero(stats, UNIQ_STATUS*sizeof(int));
    for (int i = 0; i < UNIQ_STATUS; i++) {
        stats[i]=0;
        for (int j = 0; j < n_th; j++) {
            stats[i]+=statLog[i][j];
        }
        printf("stat %s count %d\n", byte_to_binary(i), stats[i]);
    }

    return 0;
}
