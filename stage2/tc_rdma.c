/* tc_rdma：五函数数据面模块（docs/3）。
 * 第一章 stage1 的逻辑在这里做了三层整理：
 *   1. 资源生命周期收进一个不透明上下文，调用方只见 void* 句柄；
 *   2. 一端可注册多块内存（槽位表），Stage1 是单块；
 *   3. QP 建链信息 (qpn, psn, gid) 的交换交给调用方的控制面，模块只管迁移。
 * 日志前缀统一 [tc]，库只打关键节点和错误，正常收发的逐条进度由调用方打印。 */
#define _DEFAULT_SOURCE  /* usleep 在 -std=c11 下需要显式打开 POSIX 声明 */
#include "tc_rdma.h"

#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TC_MAX_SLOTS 16   /* 最多同时注册的 buffer 数，docs/3 预注册槽位模式按 8 个 1MB 用 */
#define TC_SQ_DEPTH  64   /* 发送队列深度：同步姿势（post 一条 poll 一条）下 1 都够，留余量 */
#define TC_CQ_DEPTH  256
#define TC_POLL_US   100  /* poll 间隔。CQE 走 Soft-RoCE 内核软栈要几十微秒，睡 100us 省满核空转 */

struct tc_slot {
    struct ibv_mr *mr;
    void   *buf;
    size_t  len;
};

struct tc_rdma_ctx {
    struct ibv_context *dev;
    struct ibv_pd      *pd;
    struct ibv_cq      *cq;
    struct ibv_qp      *qp;
    int      port_num;
    int      gid_index;
    uint32_t psn;
    struct tc_slot slots[TC_MAX_SLOTS];
    int      nslots;
    uint64_t next_wr_id;
    struct ibv_wc last_wc;   /* 最近一次 poll 消费的 CQE，get_last_wc 出口用 */
};

/* ---------- 五函数之 init：dev -> PD -> CQ -> QP(RESET) ----------
 * MR 不在这里创建：注册多少块、多大，是调用方的业务决定（tc_rdma_register）。 */
void *tc_rdma_init(const char *dev_name) {
    struct tc_rdma_ctx *c = calloc(1, sizeof(*c));
    if (!c) { fprintf(stderr, "[tc] calloc ctx 失败\n"); return NULL; }
    c->port_num  = 1;
    /* 云上真实网卡（vRoCE/eRDMA）的 GID 表和 rxe 排布不同，用环境变量覆盖默认值，
     * 代码不改——docs/3 的要求是"代码不改上云"。 */
    const char *g = getenv("TC_GID_INDEX");
    c->gid_index = g ? atoi(g) : 0;
    c->psn       = 0x12345;  /* demo 起点固定；真实部署应随机化防旧包混入 */

    int n = 0;
    struct ibv_device **list = ibv_get_device_list(&n);
    if (!list || n == 0) { fprintf(stderr, "[tc] 无 RDMA 设备，检查 rdma link show\n"); goto fail; }
    struct ibv_device *dev = NULL;
    for (int i = 0; i < n; i++)
        if (strcmp(ibv_get_device_name(list[i]), dev_name) == 0) { dev = list[i]; break; }
    if (!dev) { fprintf(stderr, "[tc] 找不到设备 %s\n", dev_name); goto fail; }
    c->dev = ibv_open_device(dev);
    ibv_free_device_list(list);
    if (!c->dev) { fprintf(stderr, "[tc] ibv_open_device 失败\n"); goto fail; }

    c->pd = ibv_alloc_pd(c->dev);
    if (!c->pd) { fprintf(stderr, "[tc] ibv_alloc_pd 失败\n"); goto fail; }

    c->cq = ibv_create_cq(c->dev, TC_CQ_DEPTH, NULL, NULL, 0);
    if (!c->cq) { fprintf(stderr, "[tc] ibv_create_cq 失败\n"); goto fail; }

    struct ibv_qp_init_attr qia = {
        .send_cq    = c->cq,
        .recv_cq    = c->cq,
        .qp_type    = IBV_QPT_RC,       /* Write/Read 只在 RC 上可用 */
        .sq_sig_all = 0,                /* 每条 WR 自己指定要不要 CQE，write 里显式 SIGNALED */
        .cap = { .max_send_wr = TC_SQ_DEPTH, .max_recv_wr = TC_SQ_DEPTH,
                 .max_send_sge = 1, .max_recv_sge = 1 },
    };
    c->qp = ibv_create_qp(c->pd, &qia);
    if (!c->qp) { fprintf(stderr, "[tc] ibv_create_qp 失败\n"); goto fail; }

    printf("[tc] init ok: dev=%s qpn=%u gid_index=%d（QP 处于 RESET，等 connect）\n",
           dev_name, c->qp->qp_num, c->gid_index);
    return c;

fail:
    if (c->qp)  ibv_destroy_qp(c->qp);
    if (c->cq)  ibv_destroy_cq(c->cq);
    if (c->pd)  ibv_dealloc_pd(c->pd);
    if (c->dev) ibv_close_device(c->dev);
    free(c);
    return NULL;
}

/* ---------- 五函数之 register：malloc -> reg_mr -> 进槽位表 ----------
 * malloc 后立即 memset 并注册，之后这块内存不再移动、不再 realloc。
 * ibv_reg_mr 会 pin 页并建 VA->PA 翻译表。 */
uint64_t tc_rdma_register(void *ctx, size_t size) {
    struct tc_rdma_ctx *c = ctx;
    if (!c || c->nslots >= TC_MAX_SLOTS) { fprintf(stderr, "[tc] 槽位已满\n"); return 0; }

    struct tc_slot *s = &c->slots[c->nslots];
    s->len = size;
    s->buf = malloc(size);
    if (!s->buf) { fprintf(stderr, "[tc] malloc(%zu) 失败\n", size); return 0; }
    memset(s->buf, 0, size);
    s->mr = ibv_reg_mr(c->pd, s->buf, size,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!s->mr) { fprintf(stderr, "[tc] ibv_reg_mr 失败\n"); free(s->buf); s->buf = NULL; return 0; }

    uint64_t handle = (uint64_t)(c->nslots + 1);  /* 句柄 = 槽位号，1 起步，0 留给"无效" */
    c->nslots++;
    printf("[tc] register: handle=%llu addr=0x%lx len=%zu rkey=0x%x\n",
           (unsigned long long)handle, (unsigned long)(uintptr_t)s->buf, size, s->mr->rkey);
    return handle;
}

/* ---------- 建链辅助：取本端端点 ---------- */
int tc_rdma_get_endpoint(void *ctx, tc_rdma_endpoint_t *out) {
    struct tc_rdma_ctx *c = ctx;
    if (!c || !out) return -1;
    out->qpn = c->qp->qp_num;
    out->psn = c->psn;
    union ibv_gid gid;
    if (ibv_query_gid(c->dev, c->port_num, c->gid_index, &gid) != 0) {
        fprintf(stderr, "[tc] ibv_query_gid 失败，gid_index=%d 可能不存在\n", c->gid_index);
        return -1;
    }
    memcpy(out->gid, gid.raw, 16);
    return 0;
}

/* ---------- 建链辅助：三段迁移，逻辑同 stage1，参数打印纪律照旧 ---------- */
static int tc_qp_to_init(struct tc_rdma_ctx *c) {
    struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = (uint8_t)c->port_num,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ,
    };
    int rc = ibv_modify_qp(c->qp, &attr,
                           IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (rc) fprintf(stderr, "[tc] RESET->INIT 失败: errno=%d\n", rc);
    return rc;
}

static int tc_qp_to_rtr(struct tc_rdma_ctx *c, const tc_rdma_endpoint_t *peer) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.dest_qp_num        = peer->qpn;
    attr.rq_psn             = peer->psn;
    attr.path_mtu           = IBV_MTU_1024;  /* rxe 上限 1024；云上真实网卡可到 4096，这里取公共值 */
    attr.min_rnr_timer      = 12;
    attr.max_dest_rd_atomic = 1;
    /* RoCE 无 LID，路由全靠 GRH/GID：dgid 填对端，sgid_index 选本端 GID 表项。
     * is_global 必须置 1，缺全局路由段 ibv_modify_qp 直接 EINVAL（第一章踩过的坑） */
    attr.ah_attr.is_global      = 1;
    attr.ah_attr.grh.dgid       = *(union ibv_gid *)peer->gid;
    attr.ah_attr.grh.sgid_index = (uint8_t)c->gid_index;
    attr.ah_attr.grh.hop_limit  = 1;
    attr.ah_attr.port_num       = (uint8_t)c->port_num;

    char dgid[64];
    inet_ntop(AF_INET6, peer->gid, dgid, sizeof(dgid));
    printf("[tc] RTR: dest_qpn=%u rq_psn=%u dgid=%s\n", peer->qpn, peer->psn, dgid);
    int rc = ibv_modify_qp(c->qp, &attr,
                           IBV_QP_STATE | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                           IBV_QP_PATH_MTU | IBV_QP_MIN_RNR_TIMER |
                           IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_AV);
    if (rc) fprintf(stderr, "[tc] INIT->RTR 失败: errno=%d（查 dgid/mtu/dest_qp_num）\n", rc);
    return rc;
}

static int tc_qp_to_rts(struct tc_rdma_ctx *c) {
    struct ibv_qp_attr attr = {
        .qp_state      = IBV_QPS_RTS,
        .sq_psn        = c->psn,
        .timeout       = 14,   /* ACK 超时约 67ms；链路异常实验靠它耗尽 retry 出错误 CQE */
        .retry_cnt     = 3,
        .rnr_retry     = 3,
        .max_rd_atomic = 1,
    };
    int rc = ibv_modify_qp(c->qp, &attr,
                           IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                           IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
    if (rc) fprintf(stderr, "[tc] RTR->RTS 失败: errno=%d\n", rc);
    return rc;
}

int tc_rdma_connect(void *ctx, const tc_rdma_endpoint_t *peer) {
    struct tc_rdma_ctx *c = ctx;
    if (!c || !peer) return -1;
    if (tc_qp_to_init(c) != 0) return -2;
    if (tc_qp_to_rtr(c, peer) != 0) return -3;
    if (tc_qp_to_rts(c) != 0) return -4;
    printf("[tc] connect ok: QP 迁移到 RTS\n");
    return 0;
}

static struct tc_slot *find_slot(struct tc_rdma_ctx *c, uint64_t handle) {
    if (!c || handle == 0 || handle > (uint64_t)c->nslots) return NULL;
    return &c->slots[handle - 1];
}

/* ---------- 五函数之 write：post 一条单边 WRITE ----------
 * 同步使用姿势下 SQ 不会积压；并发 post 多条时注意 TC_SQ_DEPTH 上限，
 * 超出的 post 会返回 ENOMEM，调用方应 post 一批 poll 一批。 */
int tc_rdma_write(void *ctx, uint64_t local_handle,
                  uint64_t remote_addr, uint32_t remote_rkey,
                  size_t offset, size_t len) {
    struct tc_rdma_ctx *c = ctx;
    struct tc_slot *s = find_slot(c, local_handle);
    if (!s) return -1;
    if (offset + len > s->len) return -2;

    struct ibv_sge sge = {
        .addr   = (uint64_t)(uintptr_t)s->buf + offset,
        .length = (uint32_t)len,
        .lkey   = s->mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id      = ++c->next_wr_id,
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
    };
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = remote_rkey;

    struct ibv_send_wr *bad = NULL;
    int rc = ibv_post_send(c->qp, &wr, &bad);
    if (rc) { fprintf(stderr, "[tc] post_send 失败: rc=%d\n", rc); return -3; }
    return 0;
}

/* ---------- 五函数之 poll：取一个完成事件 ----------
 * Soft-RoCE 上事件通知不可靠，轮询是唯一可靠姿势（docs/2）。
 * 收到错误 CQE 时把 status 打出来再返回 -1，调用方拿返回码计数即可。
 * 每次 poll 到 CQE 都存进 last_wc，调用方经 get_last_wc 看 opcode。 */
int tc_rdma_poll(void *ctx, int timeout_ms) {
    struct tc_rdma_ctx *c = ctx;
    if (!c) return -2;
    long budget = (timeout_ms <= 0) ? (1L << 60) : (long)timeout_ms * 1000;
    struct ibv_wc wc;
    for (;;) {
        int ne = ibv_poll_cq(c->cq, 1, &wc);
        if (ne == 1) {
            c->last_wc = wc;
            if (wc.status == IBV_WC_SUCCESS) return 1;
            fprintf(stderr, "[tc] CQE 错误: status=%s opcode=%d wr_id=%llu\n",
                    ibv_wc_status_str(wc.status), wc.opcode, (unsigned long long)wc.wr_id);
            return -1;
        }
        if (ne < 0) { fprintf(stderr, "[tc] ibv_poll_cq 失败\n"); return -2; }
        if (budget <= 0) return 0;
        usleep(TC_POLL_US);
        budget -= TC_POLL_US;
    }
}

/* ---------- SEND/RECV 辅助 ---------- */
int tc_rdma_send(void *ctx, uint64_t local_handle, size_t offset, size_t len) {
    struct tc_rdma_ctx *c = ctx;
    struct tc_slot *s = find_slot(c, local_handle);
    if (!s) return -1;
    if (offset + len > s->len) return -2;

    struct ibv_sge sge = {
        .addr   = (uint64_t)(uintptr_t)s->buf + offset,
        .length = (uint32_t)len,
        .lkey   = s->mr->lkey,
    };
    /* SEND 的 WR 不填 wr.rdma：没有 remote_addr/rkey，数据落点由对端 recv WR 决定 */
    struct ibv_send_wr wr = {
        .wr_id      = ++c->next_wr_id,
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad = NULL;
    int rc = ibv_post_send(c->qp, &wr, &bad);
    if (rc) { fprintf(stderr, "[tc] post_send(SEND) 失败: rc=%d\n", rc); return -3; }
    return 0;
}

int tc_rdma_post_recv(void *ctx, uint64_t handle, size_t offset, size_t len) {
    struct tc_rdma_ctx *c = ctx;
    struct tc_slot *s = find_slot(c, handle);
    if (!s) return -1;
    if (offset + len > s->len) return -2;

    struct ibv_sge sge = {
        .addr   = (uint64_t)(uintptr_t)s->buf + offset,
        .length = (uint32_t)len,
        .lkey   = s->mr->lkey,
    };
    struct ibv_recv_wr wr = {
        .wr_id   = ++c->next_wr_id,
        .sg_list = &sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad = NULL;
    int rc = ibv_post_recv(c->qp, &wr, &bad);
    if (rc) { fprintf(stderr, "[tc] post_recv 失败: rc=%d\n", rc); return -3; }
    return 0;
}

int tc_rdma_get_last_wc(void *ctx, tc_rdma_wc_t *out) {
    struct tc_rdma_ctx *c = ctx;
    if (!c || !out) return -1;
    out->wr_id   = c->last_wc.wr_id;
    out->opcode  = (int)c->last_wc.opcode;
    out->byte_len = c->last_wc.byte_len;
    out->status  = (int)c->last_wc.status;
    return 0;
}

/* ---------- buffer 元数据与内容访问 ---------- */
int tc_rdma_query_buffer(void *ctx, uint64_t handle,
                         uint64_t *addr, uint32_t *rkey, size_t *len) {
    struct tc_slot *s = find_slot(ctx, handle);
    if (!s) return -1;
    if (addr) *addr = (uint64_t)(uintptr_t)s->buf;
    if (rkey) *rkey = s->mr->rkey;
    if (len)  *len  = s->len;
    return 0;
}

int tc_rdma_write_mem(void *ctx, uint64_t handle, size_t offset,
                      const void *src, size_t len) {
    struct tc_slot *s = find_slot(ctx, handle);
    if (!s) return -1;
    if (offset + len > s->len) return -2;
    memcpy((char *)s->buf + offset, src, len);
    return 0;
}

int tc_rdma_read_mem(void *ctx, uint64_t handle, size_t offset,
                     void *dst, size_t len) {
    struct tc_slot *s = find_slot(ctx, handle);
    if (!s) return -1;
    if (offset + len > s->len) return -2;
    memcpy(dst, (const char *)s->buf + offset, len);
    return 0;
}

/* ---------- 五函数之 shutdown：严格反序销毁 ---------- */
void tc_rdma_shutdown(void *ctx) {
    struct tc_rdma_ctx *c = ctx;
    if (!c) return;
    if (c->qp) ibv_destroy_qp(c->qp);
    if (c->cq) ibv_destroy_cq(c->cq);
    int n = 0;
    for (int i = 0; i < c->nslots; i++) {
        if (c->slots[i].mr)  { ibv_dereg_mr(c->slots[i].mr); n++; }
        if (c->slots[i].buf) free(c->slots[i].buf);
    }
    if (c->pd)  ibv_dealloc_pd(c->pd);
    if (c->dev) ibv_close_device(c->dev);
    printf("[tc] shutdown ok: 释放 %d 个槽位，资源按 QP->CQ->MR->buf->PD->dev 反序销毁\n", n);
    free(c);
}
