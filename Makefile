# Tested that both gcc and clang-12 will successfully compile all executables
#
# clang-12 produces code that is slightly faster than gcc, but must use the
# -fno-inline flag when compiling s25
#
# Use gcc for consistent optimization behavior

all: s25 v25 525

CC=clang-13
#CC=gcc

CFLAGS=-O3 -Wall -march=native
LIBS=-lpthread

s25: s25.c utilities.h Makefile
	$(CC) $(CFLAGS) -o $@ s25.c $(LIBS)

v25: v25.c utilities.h Makefile
	$(CC) $(CFLAGS) -o $@ v25.c $(LIBS)

525: 525.c utilities.h Makefile
	$(CC) $(CFLAGS) -o $@ 525.c $(LIBS)

check:
	/bin/sh ./check.sh
