#include <immintrin.h> /*_xbegin etc. defined in rtmintrin.h*/
#include <stdio.h>
#include <sched.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#define MAX_THREADS 32
#define UNIQ_STATUS (1 << 6)
#define USELESS_BITS 0xffffffc0 /*only the least sig 6 bits are zero*/
#define NUM_TRIES 100000000

#define MAX_RSET 1024
#define MAX_WSET 1024

#define ARRAYSIZE 1000000 //Working set/sizeof(int)

typedef enum {
    NO_TX = 0,
    SIMPLE_RW = 1,
    RTM_TX_RW = 2,
    BIGLOCK_TX_RW = 3
} tx_type_t;

tx_type_t tx_type = RTM_TX_RW;

int statLog[MAX_THREADS][UNIQ_STATUS];
int statRetry[MAX_THREADS];
int statFailed[MAX_THREADS];
int statTries[MAX_THREADS];
int statUAborted[MAX_THREADS];

int tx_rsz = 10;
int tx_wsz = 10;
bool debug = false;
int n_th = 1;
pthread_mutex_t big_mu;

typedef struct {
    long key; //actual key or the collision resistant hashvalue of key
    long value; //simulate an address
    int version;
    int gced_version;
} __attribute__((aligned(64))) node_t;

typedef struct {
    node_t *n;
    int version;
} rd_entry_t;

typedef struct {
    node_t *n;
    long key;
    long value;
} wr_entry_t;

typedef struct {
    bool read_only;
    int start_ts;
    rd_entry_t rset[MAX_RSET];
    int rset_sz;
    wr_entry_t wset[MAX_WSET];
    int wset_sz;
} tx_context_t;

node_t* kv_store;
int counter = 0;
int committed_counter = 0;

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

int
diff_timespec(const struct timespec &end, const struct timespec &start)
{
    int diff = (end.tv_sec > start.tv_sec)?(end.tv_sec-start.tv_sec)*1000:0;
    assert(diff || end.tv_sec == start.tv_sec);
    if (end.tv_nsec > start.tv_nsec) {
        diff += (end.tv_nsec-start.tv_nsec)/1000000;
    } else {
        diff -= (start.tv_nsec-end.tv_nsec)/1000000;
    }
    return diff;
}

#ifdef RTM_ENABLED
bool
rtm_begin(int id)
{   
    if (tx_type == NO_TX) return 1;

    while(true) { 
        unsigned stat;
        stat = _xbegin ();
        if(stat == _XBEGIN_STARTED) {
            return true;
        } else {
            //call some fallback function
            statRetry[id]++;
            assert((stat & USELESS_BITS)  == 0);
            statLog[id][stat& ~USELESS_BITS]++;

            if ((stat & _XABORT_EXPLICIT)) {
                statUAborted[id]++;
            }
            if ((stat &  _XABORT_RETRY) == 0) {
                //will not succeed on a retry
                statFailed[id]++;
                return false;
            }
        }
    }
}

int 
rtm_end(int id)
{
    if (tx_type == NO_TX) return 1;

    _xend();
    return 1;
}

void
rtm_abort()
{
    _xabort(1);
}

#endif /* RTM_ENABLED */

void
start_tx(tx_context_t *context, bool read_only)
{
    bzero(context, sizeof(tx_context_t));
    context->read_only = read_only;
    context->start_ts = committed_counter;
}

long
read_tx(tx_context_t *context, long key)
{
    if (context->read_only)  {
        if (context->start_ts > kv_store[key].version || 
                context->start_ts < kv_store[key].gced_version) {
            return 0;
        }else
            return kv_store[key].value;
    }else{
        int i = context->rset_sz++;
        context->rset[i].version = kv_store[key].version;
        context->rset[i].n = &kv_store[key];
        return kv_store[key].value;
    }
}

void
write_tx(tx_context_t *context, long key, long value)
{
    int i = context->wset_sz++;
    context->wset[i].key = key;
    context->wset[i].value = value;
    context->wset[i].n = &kv_store[key];
}

#ifdef RTM_ENABLED
bool
rtm_commit_tx(int thread_id, tx_context_t *context)
{
    assert(!context->read_only);

    int c =  __sync_fetch_and_add(&counter, 1);
    bool in_rtm = rtm_begin(thread_id);
    if (!in_rtm) {
        return false;
    }

    //critical section
    for (int i = 0; i < context->rset_sz; i++) {
        if (context->rset[i].n->version != context->rset[i].version) {
            rtm_abort();
        }
    }

    for (int i = 0; i < context->wset_sz; i++) {
        //must check because acquiring commit timestamp and commiting are not atomic
        if (context->wset[i].n->version > c) {
            rtm_abort();
        }
        kv_store[context->wset[i].key].key = context->wset[i].key;
        kv_store[context->wset[i].key].value = context->wset[i].value;
        //hmm..a read-only transaction might interfere with this one
        kv_store[context->wset[i].key].gced_version = kv_store[context->wset[i].key].version;
        kv_store[context->wset[i].key].version = c;
    }
   
    rtm_end(thread_id);
}
#endif /*RTM_ENABLED*/

bool
biglock_commit_tx(int thread_id, tx_context_t *context)
{
    assert(!context->read_only);

    int c =  __sync_fetch_and_add(&counter, 1);
    bool status = false;

    assert(pthread_mutex_lock(&big_mu)==0);


    //critical section
    for (int i = 0; i < context->rset_sz; i++) {
        if (context->rset[i].n->version != context->rset[i].version) {
            goto DONE;
        }
    }

    for (int i = 0; i < context->wset_sz; i++) {
        //must check because acquiring commit timestamp and commiting are not atomic
        if (context->wset[i].n->version > c) {
            goto DONE;
        }
    }

    for (int i = 0; i < context->wset_sz; i++) {
        kv_store[context->wset[i].key].key = context->wset[i].key;
        kv_store[context->wset[i].key].value = context->wset[i].value;
        //hmm..a read-only transaction might interfere with this one
        kv_store[context->wset[i].key].gced_version = kv_store[context->wset[i].key].version;
        kv_store[context->wset[i].key].version = c;
    }

    status = true;
DONE:
    assert(pthread_mutex_unlock(&big_mu) == 0);
    return status;
}




inline void
simple_body(int id) {
    int tx_sz = tx_rsz + tx_wsz;
    long x = 0;
    for (int i = 0; i < tx_rsz; i++) {
        x += kv_store[id*tx_sz+i].value; 
    }
    for (int i = 0; i < tx_wsz; i++) {
        kv_store[id*tx_sz+tx_rsz+i].value = x; 
    }
}

inline void
tx_body(int id, tx_context_t *c) {
    int tx_sz = tx_rsz + tx_wsz;
    long x = 0;
    for (int i = 0; i < tx_rsz; i++) {
        x += read_tx(c, id*tx_sz+i); 
    }
    for (int i = 0; i < tx_wsz; i++) {
        write_tx(c, id*tx_sz+tx_rsz+i, x); 
    }
}


void *
thread_run(void *x)
{
    int id = *(int *)x;
    /*
       struct drand48_data r_state;
       assert(srand48_r(id, &r_state) == 0);
     */
    bool in_rtm;
    tx_context_t c;
    int committed = 0;

    for (int k = 0; k < NUM_TRIES; k++) {
        switch (tx_type) {
            case NO_TX:
                simple_body(id);
                break;
            case SIMPLE_RW:
#ifdef RTM_ENABLED
                in_rtm = rtm_begin(id);
                simple_body(id);
                if (in_rtm) {
                    assert(rtm_end(id));
                    committed++;
                }
#endif
                break;
            case RTM_TX_RW:
            case BIGLOCK_TX_RW:
                start_tx(&c, false);
                tx_body(id,&c);
#ifdef RTM_ENABLED
                if (tx_type == RTM_TX_RW) 
                    rtm_commit_tx(id, &c);
                else
#endif /* RTM_ENABLED */
                    biglock_commit_tx(id, &c);
                committed++;
                break;
            default:
                assert(0);
        }
        statTries[id]++;
    }
    if (debug)
        printf("thread-%d finished %d tries %d committed\n", id, NUM_TRIES, committed);
}

void *
periodic_stat(void *x)
{
    int last_stats[UNIQ_STATUS];
    int stats_cnt[UNIQ_STATUS];
    int tries = 0, last_tries = 0;
    int fails = 0, last_fails = 0;
    int retries = 0, last_retries = 0;

    bzero(last_stats, sizeof(int)*UNIQ_STATUS);

    struct timespec now, past;
    clock_gettime(CLOCK_REALTIME, &now);
    while (1) {
        if (debug) {
            for (int i = 0; i < UNIQ_STATUS; i++) {
                int x = 0;
                for (int j = 0; j < n_th; j++) {
                    x += statLog[j][i];
                }
                stats_cnt[i] = x - last_stats[i];
                last_stats[i] = x;
            }
        }

        last_retries = retries;
        last_fails = fails;
        last_tries = tries;
        retries = fails = tries = 0;
        for (int i = 0; i < n_th; i++) {
            retries += statRetry[i];
            fails += statFailed[i];
            tries += statTries[i];
        }

        past = now;
        clock_gettime(CLOCK_REALTIME, &now);
        int diff = diff_timespec(now, past);
        printf("%.2f s processed %d tx thput = %.2f tx/sec, avg-retries %.5f avg-fails %.5f\n", \
                diff/1000.0, tries - last_tries, 1000.0*(tries-last_tries)/diff,
                (retries-last_retries)/(double)(tries-last_tries), (fails-last_fails)/(double)(tries-last_tries));
        if (debug) {
            for (int i = 0; i < UNIQ_STATUS; i++) {
                if (stats_cnt[i]!=0) {
                    printf("   stat %s: %d out of %d tries\n", byte_to_binary(i), stats_cnt[i], tries-last_tries);
                }
            }
        }

        sleep(1);

    }
}

int 
main(int argc, char**argv)
{
    char ch = 0;
    while ((ch = getopt(argc, argv, "t:r:w:p:d:"))!= -1) {
        switch (ch) {
            case 'p':
                tx_type = (tx_type_t) atoi(optarg);
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
            case 'd':
                debug = atoi(optarg)?true:false;
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
    printf("each array element size %d tx_rsz %d tx_wsz %d tx_type %d\n", \
            sizeof(node_t), tx_rsz, tx_wsz, tx_type);	
    kv_store = (node_t *)malloc(ARRAYSIZE*sizeof(node_t));
    assert(n_th <= MAX_THREADS);
    assert(MAX_THREADS * (tx_rsz+tx_wsz) < ARRAYSIZE);
    assert(tx_rsz < MAX_RSET);
    assert(tx_wsz < MAX_WSET);

    //Cache warmup	
    for(int i = 0; i < ARRAYSIZE; i++) {
        kv_store[i].key = i; 
        kv_store[i].value = 0; 
        kv_store[i].version = 0; 
    }

    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);

    pthread_mutex_init(&big_mu, NULL);

    pthread_t th[MAX_THREADS];
    int ids[MAX_THREADS];
    for (int i = 0; i < n_th; i++) { 
        ids[i]=i;
        assert(pthread_create(&th[i], NULL, thread_run, (void *)&ids[i])==0);
    }

    pthread_t periodic_th;
    pthread_create(&periodic_th, NULL, periodic_stat, NULL);

    for (int i = 0; i < n_th; i++) {
        pthread_join(th[i], NULL);
    }

    clock_gettime(CLOCK_REALTIME, &end);

    int diff = diff_timespec(end, start);
    printf("all %d threads finished in %.2fs\n", n_th, diff/1000.0);

    return 0;
}
