#include <immintrin.h>
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>



#define XBEGIN_STARTED_INDEX 0
#define XABORT_EXPLICIT_INDEX 1
#define XABORT_RETRY_INDEX 2
#define XABORT_CONFLICT_INDEX 3
#define XABORT_CAPACITY_INDEX 4
#define XABORT_DEBUG_INDEX 5
#define XABORT_NESTED_INDEX 6

int workingset = 20;
#define ARRAYSIZE workingset*1024/4 //Working set/sizeof(int)



int* bigArray;


class RTMScope {

  int localStatus[7];
  int localRetry;
 public:
  RTMScope() {
  	
    for(int i = 0; i < 7; i++)
      localStatus[i]= 0;

    while(true) {
      unsigned stat;
      stat = _xbegin ();
      if(stat == _XBEGIN_STARTED)
        return;
      else {
   		
   	//call some fallback function
        localRetry++;
   		
   	if((stat & _XABORT_EXPLICIT) != 0)
            localStatus[XABORT_EXPLICIT_INDEX]++;
   	else if((stat &  _XABORT_RETRY) != 0)
            localStatus[XABORT_RETRY_INDEX]++;
   	else if((stat & _XABORT_CONFLICT) != 0)
            localStatus[XABORT_CONFLICT_INDEX]++;
   	else if((stat & _XABORT_CAPACITY) != 0)
            localStatus[XABORT_CAPACITY_INDEX]++;
   	else if((stat & _XABORT_DEBUG) != 0)
            localStatus[XABORT_DEBUG_INDEX]++;
   	else if((stat &  _XABORT_NESTED) != 0)
   	    localStatus[XABORT_NESTED_INDEX]++;
   		
        continue;
      }
    }
  }
  
  ~RTMScope() 
  {  
    _xend ();
	
    printf("retry %d\n", localRetry);
	
    if(localStatus[XABORT_EXPLICIT_INDEX] != 0)
	  printf("XABORT_EXPLICIT %d\n", localStatus[XABORT_EXPLICIT_INDEX]);
    if(localStatus[XABORT_RETRY_INDEX] != 0)
	  printf("XABORT_RETRY %d\n", localStatus[XABORT_RETRY_INDEX]);
    if(localStatus[XABORT_CONFLICT_INDEX] != 0)
	  printf("XABORT_CONFLICT %d\n", localStatus[XABORT_CONFLICT_INDEX]);
    if(localStatus[XABORT_CAPACITY_INDEX] != 0)
	  printf("XABORT_CAPACITY %d\n", localStatus[XABORT_CAPACITY_INDEX]);
    if(localStatus[XABORT_DEBUG_INDEX] != 0)
	  printf("XABORT_DEBUG %d\n", localStatus[XABORT_DEBUG_INDEX]);
    if(localStatus[XABORT_NESTED_INDEX] != 0)
	  printf("XABORT_NESTED %d\n", localStatus[XABORT_NESTED_INDEX]);
 }

 private:
  RTMScope(const RTMScope&);
  void operator=(const RTMScope&);
};



int main(int argc, char**argv)
{

    for (int i = 1; i < argc; i++) {
		
	  int n = 0;
	  char junk;
	  if (strcmp(argv[i], "--help") == 0){
	    printf("./a.out --ws=size of workingset (KB default:20KB)\n");
	    return 1;
	  }
	  else if(sscanf(argv[i], "--ws=%d%c", &n, &junk) == 1) {
	    workingset = n;
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
	
	//Cache warmup	
    for(bigArray[0] = 1; bigArray[0] < ARRAYSIZE; bigArray[0]++)
	  bigArray[bigArray[0]] += bigArray[0];
	 
	//access in rtm protected region
    {	
      RTMScope beg;
	  for(bigArray[0] = 1; bigArray[0] < ARRAYSIZE; bigArray[0]++)
        bigArray[bigArray[0]] += bigArray[0];
	
    }
	
    printf("Hello World\n");
	
	return 0;
}

