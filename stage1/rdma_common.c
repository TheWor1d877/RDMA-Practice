#include "rdma_common.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void die(const char *msg) {
    fprintf(stderr, "[FATAL] %s\n", msg);
    exit(1);
}

/* ---------- 打开设备并按序创建 PD/MR/CQ/QP ---------- */
int ctx_init(rdma_ctx_t *c, const char *dev_name, size_t buf_len, int gid_index) {
    memset(c, 0, sizeof(*c));
    c->port_num  = 1;
    c->gid_index = gid_index;
    c->psn       = 0x12345;  /* PSN 起点固定即可，同机 demo 不需要随机化 */

    /* 1. 枚举设备并按名字打开。dev_name 对应 rdma link show 里的 rxe0 */
    int n = 0;
    struct ibv_device **list = ibv_get_device_list(&n);
    if (!list || n == 0) die("没有可用的 RDMA 设备，先执行 rdma link add rxe0 type rxe netdev eth0");
    struct ibv_device *dev = NULL;
    for (int i = 0; i < n; i++)
        if (strcmp(ibv_get_device_name(list[i]), dev_name) == 0) { dev = list[i]; break; }
    if (!dev) die("找不到指定设备，用 rdma link show 确认名字");

    c->ctx = ibv_open_device(dev);
    if (!c->ctx) die("ibv_open_device 失败");
    ibv_free_device_list(list);  /* open 之后再释放枚举列表，列表本身不持有资源 */

    /* 2. PD：权限边界。MR 和 QP 必须属于同一个 PD，对端才能 Write 本端 MR */
    c->pd = ibv_alloc_pd(c->ctx);
    if (!c->pd) die("ibv_alloc_pd 失败");

    /* 3. MR：malloc 一块不再移动的内存再注册。
     * ibv_reg_mr 会 pin 住这些页并建立 VA->PA 翻译表，之后地址一旦变化，rkey 立即失效。
     * LOCAL_WRITE 是注册的前提（收到数据要写进来），REMOTE_WRITE 才允许对端 Write，
     * REMOTE_READ 允许对端 Read，demo 里顺手打开。 */
    c->buf_len = buf_len;
    c->buf = malloc(buf_len);
    if (!c->buf) die("malloc 失败");
    memset(c->buf, 0, buf_len);
    int mr_access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    c->mr = ibv_reg_mr(c->pd, c->buf, buf_len, mr_access);
    if (!c->mr) die("ibv_reg_mr 失败");
    c->local_addr = (uint64_t)(uintptr_t)c->buf;
    c->local_rkey = c->mr->rkey;
    printf("[init] MR registered: addr=0x%lx len=%zu rkey=0x%x lkey=0x%x\n",
           c->local_addr, buf_len, c->mr->rkey, c->mr->lkey);

    /* 4. CQ：收发共用一个 CQ。cqe 数量决定能攒多少个完成事件，demo 单条 WR，16 够用 */
    c->cq = ibv_create_cq(c->ctx, 16, NULL, NULL, 0);
    if (!c->cq) die("ibv_create_cq 失败");

    /* 5. QP：RC 类型，可靠连接，Write/Read 只在 RC 上可用。
     * sq_sig_all=0 表示每条 WR 自己决定要不要产生 CQE，demo 里发送方显式指定。 */
    struct ibv_qp_init_attr qia = {
        .send_cq = c->cq,
        .recv_cq = c->cq,
        .qp_type = IBV_QPT_RC,
        .sq_sig_all = 0,
        .cap = { .max_send_wr = 16, .max_recv_wr = 16,
                 .max_send_sge = 1, .max_recv_sge = 1 },
    };
    c->qp = ibv_create_qp(c->pd, &qia);
    if (!c->qp) die("ibv_create_qp 失败");
    printf("[init] QP created: qpn=%u, 新建 QP 固定处于 RESET 态\n", c->qp->qp_num);
    return 0;
}

/* ---------- 销毁：与创建严格反序 ---------- */
void ctx_destroy(rdma_ctx_t *c) {
    if (c->qp)    ibv_destroy_qp(c->qp);
    if (c->cq)    ibv_destroy_cq(c->cq);
    if (c->mr)    ibv_dereg_mr(c->mr);
    if (c->buf)   free(c->buf);
    if (c->pd)    ibv_dealloc_pd(c->pd);
    if (c->ctx)   ibv_close_device(c->ctx);
    printf("[destroy] 全部资源已按 QP->CQ->MR->buf->PD->dev 释放\n");
}

/* ---------- 第一段：RESET -> INIT ----------
 * 只做本端配置：端口、pkey、以及对端可以在本端 MR 上做哪些远程操作。
 * 此时 QP 还不知道对端是谁。 */
int qp_to_init(rdma_ctx_t *c) {
    printf("[QP] -> INIT: port=%d pkey_index=0 access=REMOTE_WRITE|REMOTE_READ\n",
           c->port_num);
    struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = (uint8_t)c->port_num,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ,
    };
    int rc = ibv_modify_qp(c->qp, &attr,
                           IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (rc) { fprintf(stderr, "RESET->INIT 失败: errno=%d\n", rc); return rc; }
    printf("[QP] INIT ok\n");
    return 0;
}

/* ---------- 第二段：INIT -> RTR（Receive Transfer Ready）----------
 * 在这里告诉 QP 对端是谁：对端 QPN、对端起始 PSN、路由信息（RoCE 走 GID）。
 * MTU、max_dest_rd_atomic 影响对端发过来的请求怎么处理，两端协商值要对得上。
 * 迁到 RTR 后本端就能接收对端发来的 Write 了。 */
int qp_to_rtr(rdma_ctx_t *c, const peer_info_t *peer) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.dest_qp_num        = peer->qpn;
    attr.rq_psn             = peer->psn;
    attr.path_mtu           = IBV_MTU_1024;       /* rxe 支持的最大 MTU 路径，1024 稳妥 */
    attr.min_rnr_timer      = 12;                 /* 对端发 RNR NAK 时的重试间隔档位 */
    attr.max_dest_rd_atomic = 1;                  /* 本端能同时应答的对端 Read/Atomic 数 */
    /* RoCE 没有 IB 的 LID，路由全靠 GRH/GID：dgid 填对端，sgid_index 选本端 GID 表项。
     * is_global 必须置 1——rxe/RoCE 无 LID，地址向量缺全局路由段时 ibv_modify_qp 直接 EINVAL */
    attr.ah_attr.is_global          = 1;
    attr.ah_attr.grh.dgid           = *(union ibv_gid *)peer->gid;
    attr.ah_attr.grh.flow_label     = 0;
    attr.ah_attr.grh.sgid_index     = (uint8_t)c->gid_index;
    attr.ah_attr.grh.hop_limit      = 1;
    attr.ah_attr.grh.traffic_class  = 0;
    attr.ah_attr.sl                 = 0;
    attr.ah_attr.src_path_bits      = 0;
    attr.ah_attr.dlid               = 0;
    attr.ah_attr.port_num           = (uint8_t)c->port_num;

    printf("[QP] -> RTR: dest_qpn=%u rq_psn=%u gid_index=%d mtu=1024\n",
           attr.dest_qp_num, attr.rq_psn, c->gid_index);
    char dgid[64];
    inet_ntop(AF_INET6, peer->gid, dgid, sizeof(dgid));
    printf("[QP]     对端 dgid=%s remote_addr=0x%lx rkey=0x%x\n",
           dgid, peer->addr, peer->rkey);
    int rc = ibv_modify_qp(c->qp, &attr,
                           IBV_QP_STATE | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                           IBV_QP_PATH_MTU | IBV_QP_MIN_RNR_TIMER |
                           IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_AV);
    if (rc) { fprintf(stderr, "INIT->RTR 失败: errno=%d（重点查 dgid/mtu/dest_qp_num）\n", rc); return rc; }
    printf("[QP] RTR ok\n");
    return 0;
}

/* ---------- 第三段：RTR -> RTS（Ready To Send）----------
 * 配的是本端发送行为：起始 PSN、超时重传、对端 RNR 时怎么等。
 * 三段都迁完，QPs 才能真正 post WR。 */
int qp_to_rts(rdma_ctx_t *c) {
    struct ibv_qp_attr attr = {
        .qp_state      = IBV_QPS_RTS,
        .sq_psn        = c->psn,      /* 自己发出的第一个包用这个 PSN，必须和对端 RTR 里的 rq_psn 对上 */
        .timeout       = 14,          /* ACK 超时 = 4.096us * 2^14，约 67ms */
        .retry_cnt     = 3,
        .rnr_retry     = 3,
        .max_rd_atomic = 1,           /* 本端能同时发出的 Read/Atomic 请求数 */
    };
    printf("[QP] -> RTS: sq_psn=%u timeout=14 retry=3\n", attr.sq_psn);
    int rc = ibv_modify_qp(c->qp, &attr,
                           IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                           IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
    if (rc) { fprintf(stderr, "RTR->RTS 失败: errno=%d\n", rc); return rc; }
    printf("[QP] RTS ok, 迁移完成\n");
    return 0;
}

void fill_local_info(const rdma_ctx_t *c, peer_info_t *info, uint32_t psn) {
    memset(info, 0, sizeof(*info));
    info->qpn = c->qp->qp_num;
    info->psn = psn;
    union ibv_gid gid;
    if (ibv_query_gid(c->ctx, c->port_num, c->gid_index, &gid) != 0)
        die("ibv_query_gid 失败，检查 gid_index 是否存在");
    memcpy(info->gid, &gid.raw, 16);
    info->addr    = c->local_addr;
    info->rkey    = c->local_rkey;
    info->buf_len = (uint32_t)c->buf_len;
}

/* ---------- TCP 收发：循环补齐短读短写 ---------- */
int tcp_send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

int tcp_recv_all(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}
