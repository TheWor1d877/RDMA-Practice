CC      = gcc
CFLAGS  = -O2 -Wall -g -std=c11
AR      = ar

# 静态库给 cgo 链接用；Go 构建见 go/ 目录
lib: libtc_rdma.a

libtc_rdma.a: tc_rdma.c tc_rdma.h
	$(CC) $(CFLAGS) -c tc_rdma.c -o tc_rdma.o
	$(AR) rcs $@ tc_rdma.o

clean:
	rm -f tc_rdma.o libtc_rdma.a

.PHONY: lib clean
