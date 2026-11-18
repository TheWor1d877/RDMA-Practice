// server（接收端角色）：预注册槽位，控制面把 (addr, rkey, size) 发给 client。
// 数据面两类完成路径：
//   WRITE：单边操作，本端 CQ 不产生 CQE（stage1 结论），校验等 client 的 done 行后做；
//   SEND：双边操作，本端要挂 recv WR，每条 SEND 到达产生一个 opcode=RECV 的 CQE，
//         边收边按 seed 顺序校验。
// recv WR 在 Connect 后立刻挂：QP 进 INIT 态才能 post_recv，而 client 的
// connect 一完成就可能发 SEND，期间由发送端 rnr_retry 兜底极端情况。
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
	listen := flag.String("listen", ":29999", "TCP 控制面监听地址")
	slots := flag.Int("slots", 8, "预注册 write 槽位数")
	slotSize := flag.Int("slotsize", 1<<20, "write 槽位大小（字节）")
	msgSize := flag.Int("msgsize", 65536, "单次操作字节数，也是 recv 槽位大小")
	flag.Parse()

	ctx, err := tcgo.Init(*dev)
	if err != nil {
		fmt.Fprintln(os.Stderr, "[server]", err)
		os.Exit(1)
	}
	defer ctx.Shutdown()

	// 预注册槽位模式（docs/3）：write 槽位 + 1 个 recv 槽位。
	// GID 索引用环境变量 TC_GID_INDEX 覆盖（云上真实网卡可能不是 0），代码不改。
	var bufs []tcgo.BufferInfo
	for i := 0; i < *slots; i++ {
		b, err := ctx.Register(*slotSize)
		if err != nil {
			fmt.Fprintln(os.Stderr, "[server]", err)
			os.Exit(1)
		}
		bufs = append(bufs, tcgo.BufferInfo{Addr: b.Addr, Rkey: b.Rkey, Size: b.Size, Role: "write"})
	}
	rb, err := ctx.Register(*msgSize)
	if err != nil {
		fmt.Fprintln(os.Stderr, "[server]", err)
		os.Exit(1)
	}
	recvHandle := rb.Handle
	bufs = append(bufs, tcgo.BufferInfo{Addr: rb.Addr, Rkey: rb.Rkey, Size: rb.Size, Role: "recv"})

	ep, err := ctx.GetEndpoint()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[server]", err)
		os.Exit(1)
	}
	own := tcgo.PeerInfo{
		QPN: ep.QPN, PSN: ep.PSN,
		GID:     tcgo.GIDString(ep.GID),
		Buffers: bufs,
	}

	ln, err := net.Listen("tcp", *listen)
	if err != nil {
		fmt.Fprintln(os.Stderr, "[server] 监听失败:", err)
		os.Exit(1)
	}
	fmt.Printf("[server] 预注册 %d 个 write 槽位（各 %d 字节）+ 1 个 recv 槽位（%d 字节），等 client 连接 %s ...\n",
		*slots, *slotSize, *msgSize, *listen)
	conn, err := ln.Accept()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[server] accept 失败:", err)
		os.Exit(1)
	}

	peer, err := tcgo.Exchange(conn, own)
	if err != nil {
		fmt.Fprintln(os.Stderr, "[server]", err)
		os.Exit(1)
	}
	pg, err := peer.GIDBytes()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[server]", err)
		os.Exit(1)
	}
	fmt.Printf("[server] 对端: qpn=%d psn=%d gid=%s\n", peer.QPN, peer.PSN, peer.GID)

	// done 行交给独立 goroutine 读：主循环要占住 CQ 轮询，网络读不能和它抢线程
	type doneResult struct {
		msg tcgo.DoneMessage
		err error
	}
	doneCh := make(chan doneResult, 1)
	go func() {
		conn.SetReadDeadline(time.Now().Add(30 * time.Minute))
		line, err := bufio.NewReader(conn).ReadBytes('\n')
		if err != nil {
			doneCh <- doneResult{err: err}
			return
		}
		var m tcgo.DoneMessage
		if err := json.Unmarshal(line, &m); err != nil {
			doneCh <- doneResult{err: err}
			return
		}
		doneCh <- doneResult{msg: m}
	}()

	// recv WR 要 QP 至少处于 INIT 态才能挂，RESET 态 post_recv 直接 EINVAL——
	// 所以放在 Connect（内含 INIT->RTR->RTS）之后。
	// 时序上不亏：client 发第一条 SEND 前要先填 64KB payload（约百微秒），
	// 本端建完链立刻挂 recv WR 赶得早；万一真撞上 RNR，发送端 rnr_retry=3 兜底。
	if err := ctx.Connect(tcgo.Endpoint{QPN: peer.QPN, PSN: peer.PSN, GID: pg}); err != nil {
		fmt.Fprintln(os.Stderr, "[server]", err)
		os.Exit(1)
	}
	if err := ctx.PostRecv(recvHandle, 0, *msgSize); err != nil {
		fmt.Fprintln(os.Stderr, "[server]", err)
		os.Exit(1)
	}
	fmt.Println("[server] QP 已到 RTS，等数据 ...")

	// ---- 收 SEND 循环：每条 SEND 校验 seed=recvCount 的内容 ----
	// RC 保证按序到达，client 的 send 计数与本端 RECV CQE 计数一一对应。
	// WRITE 不产生 CQE，混跑模式里轮询只会取到 RECV 事件，两个模式共用本循环。
	var done tcgo.DoneMessage
	doneGot, doneErr := false, error(nil)
	recvOK, recvBad := 0, 0
	recvFail := ""
	expect := make([]byte, *msgSize)
	actual := make([]byte, *msgSize)
	for !(doneGot && recvOK >= done.Sends) {
		select {
		case r := <-doneCh:
			doneGot, doneErr = true, r.err
			if r.err == nil {
				done = r.msg
				fmt.Printf("[server] client 完成: mode=%s writes=%d sends=%d err=%q\n",
					done.Mode, done.Writes, done.Sends, done.Err)
			}
		default:
		}
		if doneErr != nil {
			break
		}
		err := ctx.Poll(5000)
		if err == tcgo.ErrTimeout {
			if doneGot && recvOK >= done.Sends {
				break // done 已到且 SEND 收齐，多余的 recv WR 留给 shutdown 清理
			}
			continue
		}
		if err != nil {
			recvFail = err.Error()
			recvBad++
			break
		}
		wc, err := ctx.LastWC()
		if err != nil || wc.Opcode != tcgo.WcOpRecv {
			continue // 非 RECV 完成事件，write 模式不会走到这里
		}
		if wc.ByteLen != uint32(*msgSize) {
			recvFail = fmt.Sprintf("RECV 长度异常: %d != %d", wc.ByteLen, *msgSize)
			recvBad++
			break
		}
		for j := 0; j < *msgSize; j++ {
			expect[j] = tcgo.PatternByte(uint64(recvOK), uint64(j))
		}
		if err := ctx.ReadMem(recvHandle, 0, actual); err != nil {
			recvFail = err.Error()
			recvBad++
			break
		}
		bad := 0
		for j := 0; j < *msgSize; j++ {
			if expect[j] != actual[j] {
				bad++
			}
		}
		if bad > 0 {
			recvFail = fmt.Sprintf("第 %d 条 SEND 内容不一致（%d/%d 字节）", recvOK, bad, *msgSize)
			recvBad++
			break
		}
		recvOK++
		// 补一条 recv WR，保持任何时刻都有一条在等
		if err := ctx.PostRecv(recvHandle, 0, *msgSize); err != nil {
			recvFail = err.Error()
			recvBad++
			break
		}
	}
	if doneErr != nil {
		fmt.Fprintln(os.Stderr, "[server] 读 done 行失败（client 可能崩了）:", doneErr)
		os.Exit(1)
	}
	if recvBad > 0 {
		fmt.Fprintln(os.Stderr, "[server] SEND 接收失败:", recvFail)
	}

	// ---- write 槽位校验 ----
	// client 的写入规则：第 i 次 Write（0 起步）写 write 槽位 i%槽数 的前 msgSize 字节，
	// 内容 PatternByte(seed=i, j)。槽位 k 最后一次被写是第
	// k + ((writes-1-k)/槽数)*槽数 次；send 模式下 writes=0，全部槽位应保持全零。
	start := time.Now()
	var writeTargets []tcgo.BufferInfo
	for _, b := range bufs {
		if b.Role == "write" {
			writeTargets = append(writeTargets, b)
		}
	}
	mismatchSlots := 0
	if done.Writes > 0 {
		for k := 0; k < len(writeTargets) && k < done.Writes; k++ {
			last := k + ((done.Writes-1-k)/len(writeTargets))*len(writeTargets)
			for j := 0; j < done.MsgSize; j++ {
				expect[j] = tcgo.PatternByte(uint64(last), uint64(j))
			}
			if err := ctx.ReadMem(uint64(k+1), 0, actual); err != nil {
				fmt.Fprintln(os.Stderr, "[server]", err)
				os.Exit(1)
			}
			bad := 0
			for j := 0; j < done.MsgSize; j++ {
				if expect[j] != actual[j] {
					bad++
				}
			}
			if bad > 0 {
				mismatchSlots++
				fmt.Fprintf(os.Stderr, "[server] 槽位 %d 校验失败: %d/%d 字节不一致\n", k, bad, done.MsgSize)
			} else {
				fmt.Printf("[server] 槽位 %d 校验通过（最后一次写入 seed=%d，%d 字节一致）\n",
					k, last, done.MsgSize)
			}
		}
	}
	for k := done.Writes; k < len(writeTargets); k++ {
		head := make([]byte, 16)
		ctx.ReadMem(uint64(k+1), 0, head)
		tail := make([]byte, 16)
		ctx.ReadMem(uint64(k+1), int(writeTargets[k].Size)-16, tail)
		zero := true
		for _, b := range append(head, tail...) {
			if b != 0 {
				zero = false
				break
			}
		}
		if zero {
			continue
		}
		fmt.Fprintf(os.Stderr, "[server] 槽位 %d 不该有数据却非全零\n", k)
		mismatchSlots++
	}

	res := map[string]any{
		"role":           "server",
		"mode":           done.Mode,
		"writes":         done.Writes,
		"sends":          done.Sends,
		"recv_ok":        recvOK,
		"recv_fail":      recvFail,
		"mismatch_slots": mismatchSlots,
		"verify_ms":      time.Since(start).Milliseconds(),
	}
	b, _ := json.Marshal(res)
	fmt.Println("RESULT", string(b))
	if mismatchSlots == 0 && recvBad == 0 && done.Err == "" {
		os.Exit(0)
	}
	os.Exit(1)
}
