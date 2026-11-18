/* 发送端：通过 TCP 拿到接收端的 (addr, rkey, qpn, psn, gid)，
 * QP 迁完三段后直接 post 一条 RDMA_WRITE——全程不需要接收端参与。
 * 完成事件由本端 CQ 轮询确认。 */
#include "rdma_common.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT 20000

int main(int argc, char **argv) {
    const char *dev_name  = argc > 1 ? argv[1] : "rxe0";
    const char *server_ip = argc > 2 ? argv[2] : "127.0.0.1";
    int         port      = argc > 3 ? atoi(argv[3]) : DEFAULT_PORT;
    int         gid_index = argc > 4 ? atoi(argv[4]) : 0;

    rdma_ctx_t c;
    if (ctx_init(&c, dev_name, 65536, gid_index) != 0) return 1;

    /* ---- TCP 控制面：客户端，先发自己的再收对方的 ---- */
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    if (inet_pton(AF_INET, server_ip, &sa.sin_addr) != 1) {
        fprintf(stderr, "非法 IP: %s\n", server_ip);
        return 1;
    }
    printf("[send] 连接 TCP %s:%d ...\n", server_ip, port);
    if (connect(cfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { perror("connect"); return 1; }

    peer_info_t local_info, recv_info;
    fill_local_info(&c, &local_info, c.psn);
    if (tcp_send_all(cfd, &local_info, sizeof(local_info)) != 0) { perror("send"); return 1; }
    if (tcp_recv_all(cfd, &recv_info, sizeof(recv_info)) != 0) { perror("recv"); return 1; }
    printf("[send] 元数据交换完成：对端 qpn=%u rkey=0x%x addr=0x%lx\n",
           recv_info.qpn, recv_info.rkey, recv_info.addr);

    /* ---- QP 迁移：RTR 段用对端的四元组，RTS 段用自己的 PSN ---- */
    if (qp_to_init(&c) != 0) return 1;
    if (qp_to_rtr(&c, &recv_info) != 0) return 1;
    if (qp_to_rts(&c) != 0) return 1;

    /* ---- 填充数据并 post 一条单边 Write ----
     * WR 里的 remote_addr/rkey 就是 TCP 换来的对端 MR 信息：
     * 硬件拿着这两个值直接写对端内存，对端 CPU 全程无感。 */
    uint8_t *p = c.buf;
    size_t data_len = c.buf_len - 8;
    for (size_t i = 0; i < data_len; i++) p[i] = PATTERN(0x11, i);
    uint64_t magic = DATA_MAGIC;
    memcpy(p + data_len, &magic, 8);

    struct ibv_sge sge = {
        .addr   = (uint64_t)(uintptr_t)p,   /* 本端数据地址，配合 lkey 由硬件做 DMA 读 */
        .length = (uint32_t)c.buf_len,
        .lkey   = c.mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id      = 1,                    /* 轮询 CQ 时靠 wr_id 区分是哪条 WR 完成了 */
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,    /* 显式要求完成时产生 CQE；sq_sig_all=0 时必须手动指定 */
    };
    wr.wr.rdma.remote_addr = recv_info.addr;
    wr.wr.rdma.rkey        = recv_info.rkey;

    struct ibv_send_wr *bad_wr = NULL;
    int rc = ibv_post_send(c.qp, &wr, &bad_wr);
    if (rc != 0) { fprintf(stderr, "ibv_post_send 失败: rc=%d\n", rc); return 1; }
    printf("[send] WR 已 post：%zu 字节 -> remote_addr=0x%lx rkey=0x%x\n",
           c.buf_len, recv_info.addr, recv_info.rkey);

    /* ---- 轮询 CQ 等发送完成 ----
     * Soft-RoCE 的事件通知（ibv_get_cq_event）不可靠，轮询是唯一可靠姿势（docs/2）。
     * ibv_poll_cq 返回 0 表示 CQ 还空着，返回 1 表示取到一个 CQE。 */
    printf("[send] 轮询 CQ ...\n");
    struct ibv_wc wc;
    int ne;
    do { ne = ibv_poll_cq(c.cq, 1, &wc); } while (ne == 0);
    if (ne < 0) { fprintf(stderr, "ibv_poll_cq 出错\n"); return 1; }
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "发送完成但状态异常: %s (opcode=%d wr_id=%lu)\n",
                ibv_wc_status_str(wc.status), wc.opcode, (unsigned long)wc.wr_id);
        return 1;
    }
    printf("[send] 收到 CQE：RDMA_WRITE 完成 status=%s bytes=%d\n",
            ibv_wc_status_str(wc.status), wc.byte_len);

    close(cfd);
    ctx_destroy(&c);
    return 0;
}
