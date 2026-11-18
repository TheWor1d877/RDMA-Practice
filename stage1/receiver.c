/* 接收端：注册内存，通过 TCP 把 (addr, rkey) 和 QP 信息发给发送端，
 * 之后不做任何事——单边 Write 的"单边"就在这里：接收端不 post recv、不参与传输。
 * 数据落进自己的 MR 后，靠轮询内存内容发现写入完成。 */
#define _DEFAULT_SOURCE  /* 打开 usleep 的 POSIX 声明，-std=c11 严格模式下默认隐藏 */
#include "rdma_common.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT 20000
#define WAIT_TIMEOUT_SEC 30

int main(int argc, char **argv) {
    const char *dev_name  = argc > 1 ? argv[1] : "rxe0";
    int         port      = argc > 2 ? atoi(argv[2]) : DEFAULT_PORT;
    int         gid_index = argc > 3 ? atoi(argv[3]) : 0;

    rdma_ctx_t c;
    if (ctx_init(&c, dev_name, 65536, gid_index) != 0) return 1;

    /* ---- TCP 控制面：服务端，等发送端来换元数据 ---- */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { perror("bind"); return 1; }
    listen(lfd, 1);
    printf("[recv] 等待发送端连接 TCP %d ...\n", port);

    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) { perror("accept"); return 1; }

    /* 交换顺序：先收对方的，再发自己的。两端顺序互补即可，谁先谁后都行 */
    peer_info_t sender_info, local_info;
    if (tcp_recv_all(cfd, &sender_info, sizeof(sender_info)) != 0) { perror("recv"); return 1; }
    fill_local_info(&c, &local_info, c.psn);
    if (tcp_send_all(cfd, &local_info, sizeof(local_info)) != 0) { perror("send"); return 1; }
    printf("[recv] 元数据交换完成，开始 QP 迁移\n");

    /* ---- QP 迁移到 RTS。接收端虽然只收不发，三段照样全走一遍 ---- */
    if (qp_to_init(&c) != 0) return 1;
    if (qp_to_rtr(&c, &sender_info) != 0) return 1;
    if (qp_to_rts(&c) != 0) return 1;
    printf("[recv] 等待数据落入 buffer（单边 Write，本端无感知）...\n");

    /* ---- 等待写入完成 ----
     * 普通 RDMA_WRITE 在接收端不产生 CQE（CQE 只发给发起方），
     * 所以接收端只能轮询内存：先等到末尾魔数出现，再逐字节校验。
     * 想让接收端也收到 CQE 要用 WRITE_WITH_IMM，但那会消耗一条 recv WR，
     * 违背"接收端不 post recv"的练习目标，这里不用。 */
    uint8_t *p = c.buf;
    size_t data_len = c.buf_len - 8;
    for (int t = 0; t < WAIT_TIMEOUT_SEC * 100; t++) {
        uint64_t magic;
        memcpy(&magic, p + data_len, 8);
        if (magic == DATA_MAGIC) goto got_data;
        usleep(10000);  /* 10ms 一次，内存轮询对 CPU 的消耗远小于 CQ 轮询 */
    }
    fprintf(stderr, "[recv] 超时：30s 内 buffer 末尾未出现魔数\n");
    return 1;

got_data:
    printf("[recv] 检测到魔数，开始逐字节校验\n");
    size_t bad = 0;
    for (size_t i = 0; i < data_len; i++)
        if (p[i] != PATTERN(0x11, i)) bad++;
    if (bad == 0) {
        printf("[recv] 校验通过：%zu 字节与发送端完全一致\n", data_len);
        printf("[recv] 前 16 字节: ");
        for (int i = 0; i < 16; i++) printf("%02x ", p[i]);
        printf("\n");
    } else {
        fprintf(stderr, "[recv] 校验失败：%zu 个字节不一致\n", bad);
    }

    close(cfd);
    close(lfd);
    ctx_destroy(&c);
    return bad == 0 ? 0 : 1;
}
