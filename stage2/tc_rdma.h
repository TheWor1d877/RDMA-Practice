#ifndef TC_RDMA_H
#define TC_RDMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 返回码约定 ----------
 * 普通函数：0 成功，负数失败（含义见各函数注释），句柄 0 表示无效。
 * tc_rdma_poll 特殊：1 = 取到一个成功 CQE，0 = 超时无事件，-1 = CQE 错误，
 *                    -2 = poll 本身失败。 */

/* ---------- 数据面五函数（docs/3 约定的交付接口，签名原样保留） ---------- */

/* 打开设备并创建 PD/CQ/QP，返回上下文句柄；QP 此时处于 RESET 态。
 * 失败返回 NULL，错误原因打在 stderr。 */
void *tc_rdma_init(const char *dev_name);

/* 注册一块内存：C 侧 malloc + ibv_reg_mr，返回句柄（1 起步的槽位号）。
 * 注册内存的指针不给调用方——Go 侧 GC 会移动内存，指针一旦移动 rkey 立即失效，
 * 这是第一章说过的坑，所以 Go 只持句柄，内容由 write_mem/read_mem 进出。
 * 权限：LOCAL_WRITE | REMOTE_WRITE | REMOTE_READ。失败返回 0。 */
uint64_t tc_rdma_register(void *ctx, size_t size);

/* 单边 RDMA_WRITE：把本端槽位 [offset, offset+len) 写到对端 remote_addr。
 * remote_addr/remote_rkey 来自带外控制面（模块不负责交换，docs/4）。
 * 只负责 post，完成事件要调 tc_rdma_poll 确认。
 * 返回 0 成功；-1 句柄无效；-2 越界；-3 post_send 失败。 */
int tc_rdma_write(void *ctx, uint64_t local_handle,
                  uint64_t remote_addr, uint32_t remote_rkey,
                  size_t offset, size_t len);

/* 轮询 CQ 取一个完成事件。
 * 1 = 成功 CQE；0 = timeout_ms 内没有事件；-1 = 收到错误 CQE（status 打 stderr）；
 * -2 = ibv_poll_cq 失败。timeout_ms <= 0 表示一直等。 */
int tc_rdma_poll(void *ctx, int timeout_ms);

/* 销毁：QP -> CQ -> 各槽位 MR -> buffer -> PD -> dev 严格反序。 */
void tc_rdma_shutdown(void *ctx);

/* ---------- 建链辅助（五函数之外的最小补充） ----------
 * QP 三段迁移需要对端 (qpn, psn, gid)。这组信息由调用方经带外控制面
 * （TCP JSON / gRPC，docs/4 说控制面协议不属于模块职责）交换，
 * 模块只负责迁移本身。所以补两个函数：一个取本端信息，一个吃对端信息。 */

typedef struct {
    uint32_t qpn;
    uint32_t psn;
    uint8_t  gid[16];
} tc_rdma_endpoint_t;

/* 取本端端点信息，打包给控制面发出去。返回 0 成功，-1 失败。 */
int tc_rdma_get_endpoint(void *ctx, tc_rdma_endpoint_t *out);

/* 拿到对端端点后调用，完成 INIT -> RTR -> RTS 三段迁移。
 * 返回 0 成功，负数失败（stderr 里有具体是哪一段）。 */
int tc_rdma_connect(void *ctx, const tc_rdma_endpoint_t *peer);

/* ---------- buffer 元数据与内容访问 ----------
 * 控制面要发 (addr, rkey, size)，Go 只持句柄拿不到这三个值；
 * 填数据、校验数据也需要不经过裸指针的通道。三个函数补齐这两件事。 */

/* 查槽位元数据：addr（虚拟地址）、rkey、len。返回 0 成功，-1 句柄无效。 */
int tc_rdma_query_buffer(void *ctx, uint64_t handle,
                         uint64_t *addr, uint32_t *rkey, size_t *len);

/* 把调用方内存 [src, src+len) 拷进槽位 [offset, offset+len)。
 * 返回 0 成功，-1 句柄无效，-2 越界。 */
int tc_rdma_write_mem(void *ctx, uint64_t handle, size_t offset,
                      const void *src, size_t len);

/* 把槽位 [offset, offset+len) 拷出到调用方内存 dst。
 * 返回 0 成功，-1 句柄无效，-2 越界。 */
int tc_rdma_read_mem(void *ctx, uint64_t handle, size_t offset,
                     void *dst, size_t len);

/* ---------- SEND/RECV 辅助（五函数之外的扩展点） ----------
 * docs/3 的五函数只覆盖单边 Write；SEND 是双边操作，云上验证想覆盖第二种
 * opcode 时用这一组。与 WRITE 的两个差别在 stage1 demo 里验证过：
 * SEND 不带对端 (addr, rkey)，数据落在对端 recv WR 指定的位置；
 * 对端先挂 recv WR 才能收，晚了发送端吃 RNR NAK。 */

/* post 一条 SEND，数据取自本端槽位 [offset, offset+len)。
 * 完成事件照常 tc_rdma_poll 确认，opcode 是 IBV_WC_SEND。
 * 返回 0 成功，-1 句柄无效，-2 越界，-3 post_send 失败。 */
int tc_rdma_send(void *ctx, uint64_t local_handle, size_t offset, size_t len);

/* post 一条 recv WR，接收位置为本端槽位 [offset, offset+len)。
 * SEND 到达时网卡把数据写进这个位置，并给本端 CQ 产生 opcode=IBV_WC_RECV 的 CQE。
 * 返回 0 成功，-1 句柄无效，-2 越界，-3 post_recv 失败。 */
int tc_rdma_post_recv(void *ctx, uint64_t handle, size_t offset, size_t len);

/* 最近一次 poll 消费的 CQE 信息。opcode 用于区分完成类型：
 * 0 = IBV_WC_SEND，1 = IBV_WC_RDMA_WRITE，128 = IBV_WC_RECV。
 * send/recv 流程靠它判断这次完成是本地发完还是收到对端消息。 */
typedef struct {
    uint64_t wr_id;
    int      opcode;
    uint32_t byte_len;
    int      status;   /* 0 = IBV_WC_SUCCESS */
} tc_rdma_wc_t;

int tc_rdma_get_last_wc(void *ctx, tc_rdma_wc_t *out);

#ifdef __cplusplus
}
#endif
#endif /* TC_RDMA_H */
