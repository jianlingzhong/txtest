CPP=g++-4.8.0

all: tx_test

tx_test : tx_test.o   
	$(CPP) -o $@ $< -lstdc++

%.o : %.cc
	$(CPP) -c -mrtm $<

clean :
	rm -f *.o
