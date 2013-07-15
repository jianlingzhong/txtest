CPP=/home/power/gcc-4.8.1/bin/g++
CPPFLAGS=-O2 
#CPPFLAGS=-O2 -DRTM_ENABLED

all: tx_test

tx_test : tx_test.o   
	$(CPP) -o $@ $< -lstdc++ -lpthread -lrt

%.o : %.cc
	$(CPP) $(CPPFLAGS) -c -mrtm $< 

clean :
	rm -f *.o
