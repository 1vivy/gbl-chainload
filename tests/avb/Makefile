CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -D__HOST_BUILD__
PROJ    := $(realpath ../..)

TESTS   := test_avbparse

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_avbparse: test_avbparse.c $(PROJ)/GblChainloadPkg/Library/AvbParseLib/AvbParse.c
	$(CC) $(CFLAGS) \
	  -I$(PROJ)/GblChainloadPkg/Library/AvbParseLib/Internal \
	  -I$(PROJ)/GblChainloadPkg/Include/Library \
	  $^ -o $@

clean:
	rm -f $(TESTS) *.o
