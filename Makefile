CC=clang++ -c
LD=clang++

CFLAGS=-std=c++17

LIBS=-lfmt


.PHONY: all clean

all: main

main: main.o
	$(LD) $(LDFAGS) $^ $(LIBS) -o $@

main.o: oracle.cpp
	$(CC) $(CFLAGS) $^ -o $@


clean:
	rm main
	rm *.o