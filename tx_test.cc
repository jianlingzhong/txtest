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

#define ARRAYSIZE 1000000 //Working set/sizeof(int)

typedef struct {
    int n;
} __attribute__((aligned(64))) elm_t;

bool no_tx = false;
int statLog[MAX_THREADS][UNIQ_STATUS];
int statRetry[MAX_THREADS];
int statFailed[MAX_THREADS];
int tx_rsz = 10;
int tx_wsz = 10;


elm_t* bigArray;

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

/*
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
*/
int
my_xbegin(int id)
{   
    if (no_tx) return 1;

    while(true) { // infinite retry for now 
      unsigned stat;
      stat = _xbegin ();
      if(stat == _XBEGIN_STARTED) {
        return 1;
      } else {
        //call some fallback function
        statRetry[id]++;
        assert((stat & USELESS_BITS)  == 0);
        statLog[id][stat& ~USELESS_BITS]++;
            
        if ((stat &  _XABORT_RETRY) == 0) {
            //will not succeed on a retry
            return 0;
        }
      }
    }
}

int my_xend(int id)
{
    if (no_tx) return 1;

    _xend();
    return 1;
}

void *
thread_run(void *x)
{
    int id = *(int *)x;
    /*
    struct drand48_data r_state;
    assert(srand48_r(id, &r_state) == 0);
    */
    for (int k = 0; k < NUM_TRIES; k++) {
        //RTMScope tx(id);
        int tx_sz = tx_rsz + tx_wsz;
        int in_tx = my_xbegin(id);
        if (!in_tx)
            statFailed[id]++;

        //====== BEGIN_TX ===========
        int x = 0;
        for (int i = 0; i < tx_rsz; i++) {
            x += bigArray[id*tx_sz+i].n; 
        }
        for (int i = 0; i < tx_wsz; i++) {
            bigArray[id*tx_sz+tx_rsz+i].n = x; 
        }
        
        //====== END_TX ===========
        if (in_tx)
            assert(my_xend(id));
    }
    printf("thread-%d finished %d rtm regions\n", id, NUM_TRIES);
}


int main(int argc, char**argv)
{
    int n_th = 1;
    char ch = 0;
    while ((ch = getopt(argc, argv, "t:r:w:n"))!= -1) {
        switch (ch) {
        case 'n':
            no_tx = true;
            break;
        case 't':
            n_th = atoi(optarg);
            break;
        case 'r': 
            tx_rsz= atoi(optarg);
            break;
        case 'w': 
            tx_wsz= atoi(optarg);
            break;
        default:    
            assert(0);
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
	printf("each array element size %d tx_rsz %d tx_wsz %d\n", sizeof(elm_t), tx_rsz, tx_wsz);	
    bigArray = (elm_t *)malloc(ARRAYSIZE*sizeof(elm_t));
    assert(MAX_THREADS * (tx_rsz+tx_wsz) < ARRAYSIZE);
	
	//Cache warmup	
    for(bigArray[0].n = 1; bigArray[0].n < (tx_rsz+tx_wsz)*n_th; bigArray[0].n++)
	  bigArray[bigArray[0].n].n += bigArray[0].n;
	
    pthread_t th[MAX_THREADS];
    int ids[MAX_THREADS];
    for (int i = 0; i < n_th; i++) { 
        ids[i]=i;
        assert(pthread_create(&th[i], NULL, thread_run, (void *)&ids[i])==0);
    }

    for (int i = 0; i < n_th; i++) {
        pthread_join(th[i], NULL);
    }
    printf("all %d threads finished\n", n_th);

    int stats[UNIQ_STATUS];
    bzero(stats, UNIQ_STATUS*sizeof(int));
    for (int i = 0; i < UNIQ_STATUS; i++) {
        stats[i]=0;
        for (int j = 0; j < n_th; j++) {
            stats[i]+=statLog[j][i];
        }
        if (stats[i]!=0) {
            printf("stat %s count %d\n", byte_to_binary(i), stats[i]);
        }
    }
    int retries = 0;
    int failed = 0;
    for (int i = 0; i < n_th; i++) {
        retries += statRetry[i];
        failed += statFailed[i];
    }
    printf("total retries: %d avg retries: %.2f total failures: %d avg failures: %.2f\n", \
            retries, (double)retries/(n_th*NUM_TRIES), failed, (double)failed/(n_th*NUM_TRIES));
    return 0;
}
