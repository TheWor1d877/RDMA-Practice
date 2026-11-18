// Package tcgo 是 stage2/tc_rdma C 模块的 cgo 封装。
//
// 边界纪律（docs/3）：注册内存由 C 侧 malloc 并 reg_mr，Go 只持有句柄，
// 永远不把注册内存的裸指针暴露给 Go。原因：Go 的 GC 会移动堆内存，
// 指针一旦移动，MR 建立的 VA->PA 翻译立即失效，rkey 变成废纸。
// 所以填数据、验数据都走 ReadMem/WriteMem，在 C 侧完成拷贝。
package tcgo

/*
#cgo CFLAGS: -I${SRCDIR}/..
#cgo LDFLAGS: -L${SRCDIR}/.. -ltc_rdma -libverbs
#include "tc_rdma.h"
#include <stdlib.h>
*/
import "C"

import (
	"errors"
	"fmt"
	"unsafe"
)

// Endpoint 对应 C 的 tc_rdma_endpoint_t：QP 建链三要素，控制面交换的核心字段。
type Endpoint struct {
	QPN uint32
	PSN uint32
	GID [16]byte
}

// Buffer 是本端一个已注册槽位的元数据。控制面把 Addr/Rkey/Size 发给对端，
// 对端拿 Addr/Rkey 直接 Write，Size 用来做长度校验。
type Buffer struct {
	Handle uint64
	Addr   uint64
	Rkey   uint32
	Size   uint32
}

// Context 持有 C 侧上下文。零值不可用，必须经 Init 创建。
type Context struct {
	c unsafe.Pointer
}

var (
	ErrTimeout = errors.New("poll 超时，CQ 无事件")
	ErrWC      = errors.New("CQE 错误状态，具体 status 看 C 侧 stderr")
)

// Init 打开设备并创建 PD/CQ/QP（RESET 态）。
func Init(dev string) (*Context, error) {
	cs := C.CString(dev)
	defer C.free(unsafe.Pointer(cs))
	c := C.tc_rdma_init(cs)
	if c == nil {
		return nil, fmt.Errorf("tc_rdma_init(%s) 失败，看 stderr 日志", dev)
	}
	return &Context{unsafe.Pointer(c)}, nil
}

// Register 注册一块内存，返回槽位元数据（含控制面要用的 addr/rkey）。
func (c *Context) Register(size int) (Buffer, error) {
	h := C.tc_rdma_register(c.c, C.size_t(size))
	if h == 0 {
		return Buffer{}, fmt.Errorf("tc_rdma_register(%d) 失败", size)
	}
	var addr C.uint64_t
	var rkey C.uint32_t
	var slen C.size_t
	if rc := C.tc_rdma_query_buffer(c.c, C.uint64_t(h), &addr, &rkey, &slen); rc != 0 {
		return Buffer{}, fmt.Errorf("tc_rdma_query_buffer(handle=%d) 失败", h)
	}
	return Buffer{
		Handle: uint64(h),
		Addr:   uint64(addr),
		Rkey:   uint32(rkey),
		Size:   uint32(slen),
	}, nil
}

// GetEndpoint 取本端 QPN/PSN/GID，给控制面打包发出去。
func (c *Context) GetEndpoint() (Endpoint, error) {
	var ep C.tc_rdma_endpoint_t
	if rc := C.tc_rdma_get_endpoint(c.c, &ep); rc != 0 {
		return Endpoint{}, fmt.Errorf("tc_rdma_get_endpoint 失败")
	}
	var out Endpoint
	out.QPN = uint32(ep.qpn)
	out.PSN = uint32(ep.psn)
	gid := (*[16]C.uint8_t)(unsafe.Pointer(&ep.gid[0]))
	for i := 0; i < 16; i++ {
		out.GID[i] = byte(gid[i])
	}
	return out, nil
}

// Connect 拿到对端端点后调用，完成 INIT -> RTR -> RTS 三段迁移。
func (c *Context) Connect(peer Endpoint) error {
	var ep C.tc_rdma_endpoint_t
	ep.qpn = C.uint32_t(peer.QPN)
	ep.psn = C.uint32_t(peer.PSN)
	gid := (*[16]C.uint8_t)(unsafe.Pointer(&ep.gid[0]))
	for i := 0; i < 16; i++ {
		gid[i] = C.uint8_t(peer.GID[i])
	}
	rc := C.tc_rdma_connect(c.c, &ep)
	if rc != 0 {
		return fmt.Errorf("tc_rdma_connect 失败 rc=%d（-2 INIT / -3 RTR / -4 RTS，看 stderr）", rc)
	}
	return nil
}

// Write post 一条单边 RDMA_WRITE：把本端槽位 handle 的 [offset, offset+length)
// 写到对端 remoteAddr（对端 rkey 为 remoteRkey）。
// 只负责 post，完成事件必须再调 Poll 确认。
func (c *Context) Write(handle uint64, remoteAddr uint64, remoteRkey uint32,
	offset, length int) error {
	rc := C.tc_rdma_write(c.c, C.uint64_t(handle), C.uint64_t(remoteAddr),
		C.uint32_t(remoteRkey), C.size_t(offset), C.size_t(length))
	switch rc {
	case 0:
		return nil
	case -1:
		return fmt.Errorf("write: 句柄 %d 无效", handle)
	case -2:
		return fmt.Errorf("write: offset+len 越界")
	default:
		return fmt.Errorf("write: post_send 失败 rc=%d", rc)
	}
}

// Poll 轮询一个完成事件。nil = 成功 CQE；ErrTimeout = 超时；ErrWC = 错误 CQE。
func (c *Context) Poll(timeoutMs int) error {
	switch C.tc_rdma_poll(c.c, C.int(timeoutMs)) {
	case 1:
		return nil
	case 0:
		return ErrTimeout
	case -1:
		return ErrWC
	default:
		return errors.New("ibv_poll_cq 失败")
	}
}

// CQE 的 opcode 值，对应 verbs 的 enum ibv_wc_opcode。
const (
	WcOpSend  = 0   // IBV_WC_SEND：本端 SEND 发完
	WcOpWrite = 1   // IBV_WC_RDMA_WRITE：本端 Write 完成
	WcOpRecv  = 128 // IBV_WC_RECV：收到对端 SEND，数据已落进 recv WR 指定的位置
)

// WC 是最近一次 Poll 消费的完成事件信息。
type WC struct {
	WrID    uint64
	Opcode  int
	ByteLen uint32
	Status  int
}

// LastWC 取最近一次 Poll 的 CQE 详情，send/recv 流程靠它区分完成类型。
func (c *Context) LastWC() (WC, error) {
	var w C.tc_rdma_wc_t
	if rc := C.tc_rdma_get_last_wc(c.c, &w); rc != 0 {
		return WC{}, errors.New("tc_rdma_get_last_wc 失败")
	}
	return WC{
		WrID:    uint64(w.wr_id),
		Opcode:  int(w.opcode),
		ByteLen: uint32(w.byte_len),
		Status:  int(w.status),
	}, nil
}

// Send post 一条双边 SEND：数据取自本端槽位，落点由对端 recv WR 决定。
// 对端必须已挂 recv WR，否则本端吃 RNR NAK。完成后照常 Poll 确认。
func (c *Context) Send(handle uint64, offset, length int) error {
	rc := C.tc_rdma_send(c.c, C.uint64_t(handle), C.size_t(offset), C.size_t(length))
	switch rc {
	case 0:
		return nil
	case -1:
		return fmt.Errorf("send: 句柄 %d 无效", handle)
	case -2:
		return fmt.Errorf("send: offset+len 越界")
	default:
		return fmt.Errorf("send: post_send 失败 rc=%d", rc)
	}
}

// PostRecv post 一条 recv WR，指定对端 SEND 的落点。
// SEND 到达时本端 CQ 产生 opcode=WcOpRecv 的 CQE，数据在 handle 槽位的 offset 处。
func (c *Context) PostRecv(handle uint64, offset, length int) error {
	rc := C.tc_rdma_post_recv(c.c, C.uint64_t(handle), C.size_t(offset), C.size_t(length))
	switch rc {
	case 0:
		return nil
	case -1:
		return fmt.Errorf("post_recv: 句柄 %d 无效", handle)
	case -2:
		return fmt.Errorf("post_recv: offset+len 越界")
	default:
		return fmt.Errorf("post_recv 失败 rc=%d", rc)
	}
}

// WriteMem 把 b 拷进本端槽位（填数据用）。b 传空切片等于没操作。
func (c *Context) WriteMem(handle uint64, offset int, b []byte) error {
	if len(b) == 0 {
		return nil
	}
	rc := C.tc_rdma_write_mem(c.c, C.uint64_t(handle), C.size_t(offset),
		unsafe.Pointer(&b[0]), C.size_t(len(b)))
	if rc != 0 {
		return fmt.Errorf("write_mem 失败 rc=%d（-1 句柄无效 / -2 越界）", rc)
	}
	return nil
}

// ReadMem 把本端槽位内容拷进 b（校验用）。
func (c *Context) ReadMem(handle uint64, offset int, b []byte) error {
	if len(b) == 0 {
		return nil
	}
	rc := C.tc_rdma_read_mem(c.c, C.uint64_t(handle), C.size_t(offset),
		unsafe.Pointer(&b[0]), C.size_t(len(b)))
	if rc != 0 {
		return fmt.Errorf("read_mem 失败 rc=%d（-1 句柄无效 / -2 越界）", rc)
	}
	return nil
}

// Shutdown 销毁全部资源。调过之后 Context 不可再用。
func (c *Context) Shutdown() {
	if c == nil || c.c == nil {
		return
	}
	C.tc_rdma_shutdown(c.c)
	c.c = nil
}

// PatternByte 与 stage1 的 PATTERN 宏保持一致：(seed + i*7) 截断到 uint8。
// 两端程序用同一个 Go 函数生成/校验，保证逐字节可比。
func PatternByte(seed, i uint64) byte {
	return byte(seed + i*7)
}
