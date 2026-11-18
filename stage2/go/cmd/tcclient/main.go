// client（发送端角色）：从控制面拿到对端 (addr, rkey, size) 后发起数据面操作。
// 三种模式（-mode）：
//   write  单边 RDMA_WRITE，对端无感知，本端 poll 确认（docs/3 五函数的主路径）
//   send   双边 SEND，对端必须挂 recv WR，落点由对端决定
//   mixed  一次 Write 一次 SEND 交替，两种 opcode 混跑
// -depth N 只对 write 生效：批量 post N 条再批量 poll N 个完成，
// 验证 SQ 深度与多 outstanding 场景（docs/4 的批处理扩展点）。
// 本端 buffer 按 depth 注册多个槽位轮转：WR 的 DMA 读发生在传输时刻，
// 同一槽位在上一批 poll 完成前复用会读到被覆盖的内容，所以 depth <= 本端槽位数。
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"os"
	"time"

	tcgo "tcgo"
)

func main() {
	dev := flag.String("dev", "rxe0", "RDMA 设备名")
	server := flag.String("server", "127.0.0.1:29999", "server 的 TCP 控制面地址")
	mode := flag.String("mode", "write", "write / send / mixed")
	n := flag.Int("n", 100, "迭代次数（云上稳定性验证用 -n 1000）")
	depth := flag.Int("depth", 1, "write 模式批量 outstanding 条数（1-16）")
	msgSize := flag.Int("msgsize", 65536, "每次操作的字节数")
	pollMs := flag.Int("pollms", 5000, "单次 poll 超时（毫秒）")
	flag.Parse()
	if *depth < 1 {
		*depth = 1
	}
	if *depth > 16 {
		*depth = 16
	}

	ctx, err := tcgo.Init(*dev)
	if err != nil {
		fmt.Fprintln(os.Stderr, "[client]", err)
		os.Exit(1)
	}
	defer ctx.Shutdown()

	// 本端槽位：write 批量模式按 depth 注册轮转，send/mixed 一块就够
	numLocal := 1
	if *mode == "write" && *depth > 1 {
		numLocal = *depth
	}
	handles := make([]uint64, numLocal)
	for i := 0; i < numLocal; i++ {
		b, err := ctx.Register(*msgSize)
		if err != nil {
			fmt.Fprintln(os.Stderr, "[client]", err)
			os.Exit(1)
		}
		handles[i] = b.Handle
	}
	ep, err := ctx.GetEndpoint()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[client]", err)
		os.Exit(1)
	}
	// 控制面只发 QP 端点三项；本端槽位对端用不上，buffer 列表以对端发的为准
	own := tcgo.PeerInfo{QPN: ep.QPN, PSN: ep.PSN, GID: tcgo.GIDString(ep.GID)}

	conn, err := net.Dial("tcp", *server)
	if err != nil {
		fmt.Fprintln(os.Stderr, "[client] 连接失败:", err)
		os.Exit(1)
	}
	peer, err := tcgo.Exchange(conn, own)
	if err != nil {
		fmt.Fprintln(os.Stderr, "[client]", err)
		os.Exit(1)
	}
	pg, err := peer.GIDBytes()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[client]", err)
		os.Exit(1)
	}
	var writeTargets []tcgo.BufferInfo
	for _, b := range peer.Buffers {
		if b.Role == "write" {
			writeTargets = append(writeTargets, b)
		}
	}
	if len(writeTargets) == 0 {
		fmt.Fprintln(os.Stderr, "[client] 对端没有 role=write 的槽位")
		os.Exit(1)
	}
	fmt.Printf("[client] 对端: qpn=%d psn=%d gid=%s write槽位=%d mode=%s depth=%d\n",
		peer.QPN, peer.PSN, peer.GID, len(writeTargets), *mode, *depth)
	if err := ctx.Connect(tcgo.Endpoint{QPN: peer.QPN, PSN: peer.PSN, GID: pg}); err != nil {
		fmt.Fprintln(os.Stderr, "[client]", err)
		os.Exit(1)
	}

	payload := make([]byte, *msgSize)
	fill := func(seed uint64) {
		for j := 0; j < *msgSize; j++ {
			payload[j] = tcgo.PatternByte(seed, uint64(j))
		}
	}
	writeIdx, sendIdx, fail := 0, 0, 0
	lastErr := ""
	start := time.Now()

	// 主循环。任一步失败即停：链路异常实验要的就是第一次错误的现场
	switch *mode {
	case "write":
		for writeIdx < *n {
			batchLen := *depth
			if rest := *n - writeIdx; rest < batchLen {
				batchLen = rest
			}
			// 先填本端槽位再整批 post：槽位 k 本轮内容 seed=writeIdx+k，
			// 整批 poll 完才进下一轮，槽位复用发生在上一批完成之后，内容不会被覆盖
			for k := 0; k < batchLen; k++ {
				fill(uint64(writeIdx + k))
				if err := ctx.WriteMem(handles[k%numLocal], 0, payload); err != nil {
					lastErr = err.Error()
					break
				}
			}
			if lastErr != "" {
				fail++
				break
			}
			for k := 0; k < batchLen; k++ {
				target := writeTargets[(writeIdx+k)%len(writeTargets)]
				if err := ctx.Write(handles[k%numLocal], target.Addr, target.Rkey, 0, *msgSize); err != nil {
					lastErr = err.Error()
					fail++
					break
				}
			}
			if lastErr != "" {
				break
			}
			for k := 0; k < batchLen; k++ {
				if err := ctx.Poll(*pollMs); err != nil {
					lastErr = err.Error()
					fail++
					break
				}
				writeIdx++
			}
			if lastErr != "" {
				break
			}
			if writeIdx%100 < batchLen {
				fmt.Printf("[client] 进度 %d/%d（用时 %s）\n",
					writeIdx, *n, time.Since(start).Round(time.Millisecond))
			}
		}
	case "send":
		for sendIdx < *n {
			fill(uint64(sendIdx))
			if err := ctx.WriteMem(handles[0], 0, payload); err != nil {
				lastErr = err.Error()
				fail++
				break
			}
			if err := ctx.Send(handles[0], 0, *msgSize); err != nil {
				lastErr = err.Error()
				fail++
				break
			}
			if err := ctx.Poll(*pollMs); err != nil {
				lastErr = err.Error()
				fail++
				break
			}
			if wc, err := ctx.LastWC(); err == nil && wc.Opcode != tcgo.WcOpSend {
				lastErr = fmt.Sprintf("SEND 完成的 opcode 异常: %d", wc.Opcode)
				fail++
				break
			}
			sendIdx++
			if sendIdx%100 == 0 {
				fmt.Printf("[client] 进度 %d/%d（用时 %s）\n",
					sendIdx, *n, time.Since(start).Round(time.Millisecond))
			}
		}
	case "mixed":
		for writeIdx+sendIdx < 2**n {
			// Write 半段：seed 用 write 自身计数，server 按槽位数学校验
			fill(uint64(writeIdx))
			if err := ctx.WriteMem(handles[0], 0, payload); err != nil {
				lastErr = err.Error()
				fail++
				break
			}
			target := writeTargets[writeIdx%len(writeTargets)]
			if err := ctx.Write(handles[0], target.Addr, target.Rkey, 0, *msgSize); err != nil {
				lastErr = err.Error()
				fail++
				break
			}
			if err := ctx.Poll(*pollMs); err != nil {
				lastErr = err.Error()
				fail++
				break
			}
			writeIdx++
			// SEND 半段：seed 用 send 自身计数，server 按 RECV CQE 顺序校验
			fill(uint64(sendIdx))
			if err := ctx.WriteMem(handles[0], 0, payload); err != nil {
				lastErr = err.Error()
				fail++
				break
			}
			if err := ctx.Send(handles[0], 0, *msgSize); err != nil {
				lastErr = err.Error()
				fail++
				break
			}
			if err := ctx.Poll(*pollMs); err != nil {
				lastErr = err.Error()
				fail++
				break
			}
			sendIdx++
			if (writeIdx+sendIdx)%100 == 0 {
				fmt.Printf("[client] 进度 write=%d send=%d / %d（用时 %s）\n",
					writeIdx, sendIdx, *n, time.Since(start).Round(time.Millisecond))
			}
		}
	default:
		fmt.Fprintln(os.Stderr, "[client] 未知 mode:", *mode)
		os.Exit(1)
	}
	if fail > 0 {
		fmt.Fprintf(os.Stderr, "[client] 失败: %s\n", lastErr)
	}

	// done 行：server 拿 writes/sends 决定校验哪些内容
	done := tcgo.DoneMessage{
		Mode: *mode, Writes: writeIdx, Sends: sendIdx, MsgSize: *msgSize, Err: lastErr,
	}
	line, _ := json.Marshal(done)
	w := bufio.NewWriter(conn)
	w.Write(append(line, '\n'))
	w.Flush()

	res := map[string]any{
		"role":       "client",
		"mode":       *mode,
		"requested":  *n,
		"depth":      *depth,
		"writes":     writeIdx,
		"sends":      sendIdx,
		"fail":       fail,
		"msgsize":    *msgSize,
		"elapsed_ms": time.Since(start).Milliseconds(),
	}
	if lastErr != "" {
		res["err"] = lastErr
	}
	b, _ := json.Marshal(res)
	fmt.Println("RESULT", string(b))
	if fail == 0 {
		os.Exit(0)
	}
	os.Exit(2)
}
