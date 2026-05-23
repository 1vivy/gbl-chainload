CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -D__HOST_BUILD__
PROJ    := $(realpath ../..)
AVB_INC := $(PROJ)/crates/avb-parse/include
AVB_HOST := $(PROJ)/target/release/libavb_parse.a

TESTS   := test_avbparse

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

# PR2 Task 7: AvbParse.c is gone; link libavb_parse.a from
# crates/avb-parse (Rust). cargo builds it lazily via the .PHONY dep
# below — host-target only since this is a unit-test harness.
.PHONY: $(AVB_HOST)
$(AVB_HOST):
	cd $(PROJ) && cargo build --release -p avb-parse

test_avbparse: test_avbparse.c $(AVB_HOST)
	$(CC) $(CFLAGS) \
	  -I$(AVB_INC) \
	  -I$(PROJ)/GblChainloadPkg/Include/Library \
	  test_avbparse.c $(AVB_HOST) -o $@

clean:
	rm -f $(TESTS) *.o
