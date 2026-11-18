/* 接收端：注册内存，通过 TCP 把 (addr, rkey) 和 QP 信息发给发送端。
 * 数据面分两段对照：
 *   WRITE 段：单边操作，接收端不 post recv、不参与传输，靠轮询内存发现写入；
 *   SEND  段：双边操作，接收端要提前挂 recv WR，完成时本端 CQ 产生 CQE。 */
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

static void die(const char *msg) {
    fprintf(stderr, "[FATAL] %s\n", msg);
    exit(1);
}

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

    /* ---- SEND/RECV 准备：注册小 buffer 并提前 post 一条 recv WR ----
     * 同一个 PD 上可以注册多个 MR，各自有独立的 lkey/rkey。
     * SEND 只需要 LOCAL_WRITE 权限（数据由网卡写进来），不需要 REMOTE_*：
     * 对端发 SEND 时只带本端的 QPN，不带本端的 addr/rkey——这正是 SEND 和
     * WRITE 的关键差别之一，WRITE 必须把 (addr, rkey) 告诉对端。
     * recv WR 必须在对端 SEND 到达之前挂好，否则对端会吃到 RNR NAK；
     * 所以放在 RTS 迁移完立刻做，而不是等 WRITE 校验之后。 */
    uint8_t *sbuf = malloc(SEND_MSG_LEN);
    if (!sbuf) die("malloc 失败");
    memset(sbuf, 0, SEND_MSG_LEN);
    struct ibv_mr *smr = ibv_reg_mr(c.pd, sbuf, SEND_MSG_LEN, IBV_ACCESS_LOCAL_WRITE);
    if (!smr) die("ibv_reg_mr（SEND buffer）失败");

    struct ibv_sge rsge = {
        .addr   = (uint64_t)(uintptr_t)sbuf,
        .length = SEND_MSG_LEN,
        .lkey   = smr->lkey,
    };
    struct ibv_recv_wr rwr = {
        .wr_id   = 2,           /* 和发送段 wr_id=1 区分开 */
        .sg_list = &rsge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_rwr = NULL;
    if (ibv_post_recv(c.qp, &rwr, &bad_rwr) != 0) {
        fprintf(stderr, "ibv_post_recv 失败\n");
        return 1;
    }
    printf("[recv] recv WR 已挂好（wr_id=2，等对端 SEND）\n");
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
        printf("[recv] WRITE 校验通过：%zu 字节与发送端完全一致\n", data_len);
        printf("[recv] 前 16 字节: ");
        for (int i = 0; i < 16; i++) printf("%02x ", p[i]);
        printf("\n");
    } else {
        fprintf(stderr, "[recv] WRITE 校验失败：%zu 个字节不一致\n", bad);
    }

    /* ---- 从 CQ 收对端 SEND 的完成事件 ----
     * 和 WRITE 相对照的另一面：WRITE 接收端无 CQE，SEND 接收端有 CQE。
     * CQE 的 opcode 是 IBV_WC_RECV，byte_len 是消息实际长度，数据在 recv WR
     * 指定的 sbuf 里——接收端事先不知道对端要写哪，只管挂 WR 等消息。 */
    printf("[recv] 轮询 CQ 等对端的 SEND ...\n");
    struct ibv_wc swc;
    int sne;
    do { sne = ibv_poll_cq(c.cq, 1, &swc); } while (sne == 0);
    if (sne < 0) { fprintf(stderr, "ibv_poll_cq 出错\n"); return 1; }
    if (swc.status != IBV_WC_SUCCESS || swc.opcode != IBV_WC_RECV) {
        fprintf(stderr, "SEND 接收异常: status=%s opcode=%d\n",
                ibv_wc_status_str(swc.status), swc.opcode);
        return 1;
    }
    uint64_t smagic;
    memcpy(&smagic, sbuf + SEND_MSG_LEN - 8, 8);
    int sbad = 0;
    for (int i = 0; i < SEND_MSG_LEN - 8; i++)
        if (sbuf[i] != PATTERN(0x22, i)) sbad++;
    printf("[recv] SEND 收到 CQE：opcode=RECV bytes=%d wr_id=%lu 魔数=%s 校验%s\n",
           swc.byte_len, (unsigned long)swc.wr_id,
           smagic == SEND_MAGIC ? "匹配" : "不匹配",
           sbad == 0 ? "通过" : "失败");

    close(cfd);
    close(lfd);
    ibv_dereg_mr(smr);      /* SEND 用的 MR 先销毁，再走主 buffer 的反序销毁 */
    free(sbuf);
    ctx_destroy(&c);
    return (bad == 0 && sbad == 0 && smagic == SEND_MAGIC) ? 0 : 1;
}
