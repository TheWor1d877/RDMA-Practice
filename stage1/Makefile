CC      = gcc
CFLAGS  = -O2 -Wall -g -std=c11
LDLIBS  = -libverbs

all: receiver sender

receiver: receiver.c rdma_common.c rdma_common.h
	$(CC) $(CFLAGS) -o $@ receiver.c rdma_common.c $(LDLIBS)

sender: sender.c rdma_common.c rdma_common.h
	$(CC) $(CFLAGS) -o $@ sender.c rdma_common.c $(LDLIBS)

clean:
	rm -f receiver sender

.PHONY: all clean
