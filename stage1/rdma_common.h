#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <infiniband/verbs.h>
#include <stddef.h>
#include <stdint.h>

/* ---------- 带外交换的控制信息 ----------
 * 两端各填一份，通过 TCP 互发。TCP 建链换元数据，QP 建好后数据走 RDMA，两条链路分开。
 * 生产环境这一步由 gRPC / TCP JSON 完成（docs/3 的控制面），这里用定长结构体最直观。
 * 两端都是同一台机器上的同架构进程，直接按内存布局收发，不做字节序转换。 */
typedef struct {
    uint32_t qpn;      /* 对端 QP 编号，RTR 阶段填 dest_qp_num */
    uint32_t psn;      /* 起始包序号，两端各自随机生成，RTR 用对端的，RTS 用自己的 */
    uint8_t  gid[16];  /* 对端 GID，RoCE 下 RTR 的 ah_attr 靠它路由（无 InfiniBand 的 LID）*/
    uint64_t addr;     /* 对端 MR 的虚拟地址，Write WR 的 remote_addr */
    uint32_t rkey;     /* 对端 MR 的远程访问密钥，Write WR 的 rkey */
    uint32_t buf_len;  /* 对端 buffer 长度 */
} peer_info_t;

/* ---------- 一端持有的全部 verbs 资源 ----------
 * 创建顺序固定：dev -> PD -> MR -> CQ -> QP，销毁严格反序。
 * 顺序错了销毁会返回 EBUSY：QP 引用 CQ，CQ/QP 引用 PD，MR 锚定 PD。 */
typedef struct {
    struct ibv_context *ctx;   /* 打开的设备 */
    struct ibv_pd      *pd;    /* 保护域，把 MR/QP 圈进同一个权限范围 */
    struct ibv_mr      *mr;    /* 注册内存句柄，含 lkey/rkey */
    struct ibv_cq      *cq;    /* 完成队列，收两端 CQE */
    struct ibv_qp      *qp;    /* RC 类型队列对 */
    void    *buf;              /* 注册的 buffer 本体，malloc 出来的，地址不再变化 */
    size_t   buf_len;
    uint32_t psn;              /* 自己的起始 PSN，RTS 阶段填 sq_psn */
    int      port_num;         /* 物理端口号，RoCE 设备固定用 1 */
    int      gid_index;        /* GID 表索引，rxe 通常为 0 */
    uint64_t local_addr;       /* 本端 buffer 虚拟地址 */
    uint32_t local_rkey;
} rdma_ctx_t;

/* 约定好的数据格式：buffer 末尾 8 字节是魔数，其余是按位置生成的校验字节。
 * 接收端先等到魔数出现，再逐字节校验。 */
#define DATA_MAGIC      0x52444D414F4B3135ULL  /* "RDMAOK15" 的小端字节序 */
#define PATTERN(seed, i) ((uint8_t)((seed) + (i) * 7))

/* SEND 段的消息格式：64 字节，末尾 8 字节魔数，其余按位置生成。
 * SEND 是双边操作，两端程序各自按这个格式填/验，不需要带外协商。 */
#define SEND_MSG_LEN    64
#define SEND_MAGIC      0x53454E444F4B3135ULL  /* "SENDOK15" 的小端字节序 */

/* 资源生命周期 */
int  ctx_init(rdma_ctx_t *c, const char *dev_name, size_t buf_len, int gid_index);
void ctx_destroy(rdma_ctx_t *c);

/* QP 三段迁移。每段迁移前打印参数、迁移后检查返回值：
 * EINVAL 不会告诉你哪个字段错了，靠打印前后对比定位（docs/2 的调试纪律）。 */
int qp_to_init(rdma_ctx_t *c);
int qp_to_rtr(rdma_ctx_t *c, const peer_info_t *peer);
int qp_to_rts(rdma_ctx_t *c);

/* 填本端 peer_info，供 TCP 交换 */
void fill_local_info(const rdma_ctx_t *c, peer_info_t *info, uint32_t psn);

/* TCP 收发定长结构体。send 短写、recv 短读都要循环补齐，socket 没有保证一次到位。 */
int tcp_send_all(int fd, const void *buf, size_t len);
int tcp_recv_all(int fd, void *buf, size_t len);

#endif
