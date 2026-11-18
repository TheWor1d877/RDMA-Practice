package tcgo

// 带外控制面：TCP 上交换一行 JSON，内容是本端 QP 端点 + 已注册 buffer 列表。
// docs/4 把控制面划在模块职责之外（gRPC / TCP JSON 均可），这里放在 Go 侧实现，
// C 模块保持纯数据面。真实 TrainCache 集成时，把这一段换成 gRPC 即可，C 模块不动。

import (
	"encoding/json"
	"fmt"
	"net"
)

// BufferInfo 是发给对端的槽位元数据。Role 区分槽位用途：
// "write" 是单边 Write 的目标槽位，"recv" 是 SEND/RECV 模式的接收槽位。
type BufferInfo struct {
	Addr uint64 `json:"addr"`
	Rkey uint32 `json:"rkey"`
	Size uint32 `json:"size"`
	Role string `json:"role"`
}

// PeerInfo 是控制面一行 JSON 的结构：QP 建链信息 + 可写的 buffer 列表。
type PeerInfo struct {
	QPN     uint32       `json:"qpn"`
	PSN     uint32       `json:"psn"`
	GID     string       `json:"gid"` // IPv6 文本形式，如 fe80::de90:9ff:fe63:5e2
	Buffers []BufferInfo `json:"buffers"`
}

// DoneMessage 是 client 跑完后经控制面发的一行 JSON，server 拿它决定校验什么。
// Mode: write / send / mixed。Writes 与 Sends 分别是两种 opcode 的成功次数。
type DoneMessage struct {
	Mode    string `json:"mode"`
	Writes  int    `json:"writes"`
	Sends   int    `json:"sends"`
	MsgSize int    `json:"msgsize"`
	Err     string `json:"err,omitempty"`
}

// GIDBytes 把 GID 文本还原成 16 字节，供 Connect 使用。
func (p PeerInfo) GIDBytes() ([16]byte, error) {
	ip := net.ParseIP(p.GID)
	if ip == nil || len(ip.To16()) != 16 {
		return [16]byte{}, fmt.Errorf("非法 GID 文本: %q", p.GID)
	}
	var g [16]byte
	copy(g[:], ip.To16())
	return g, nil
}

// Endpoint 转成 PeerInfo 用的 GID 文本。
func GIDString(g [16]byte) string {
	return net.IP(g[:]).String()
}

// Exchange 各自调一次：发出自己的 PeerInfo（一行 JSON），收回对端的。
// 两端谁先谁后都行，收发顺序天然互补；分隔符用 '\n'，json.Decoder 按行读。
func Exchange(conn net.Conn, own PeerInfo) (PeerInfo, error) {
	line, err := json.Marshal(own)
	if err != nil {
		return PeerInfo{}, err
	}
	if _, err := conn.Write(append(line, '\n')); err != nil {
		return PeerInfo{}, fmt.Errorf("发送本端信息失败: %w", err)
	}
	var peer PeerInfo
	if err := json.NewDecoder(conn).Decode(&peer); err != nil {
		return PeerInfo{}, fmt.Errorf("解析对端信息失败: %w", err)
	}
	return peer, nil
}
