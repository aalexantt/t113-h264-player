#!/bin/sh
# gopanel-watchdog — gopanel 看门狗
#
# 背景：这块板子内存只有 108MB，gopanel 是 Go 程序（total-vm 572MB，
# 实际 RSS 3.6MB），OOM 评分高，内存紧张时会被内核优先杀掉。
# 而 busybox init 的 rc.final 只是顺序执行 S 脚本一次，没有 respawn 机制，
# 所以进程被杀后就永久消失了 —— 表现为"Web 后台开机没自动启动"。
#
# 这个脚本定期检查，进程不在就重新拉起。
#
# 【设计要点】
# 1. 间隔不能太短。OOM 往往伴随持续的内存压力，重启用太频繁会变成
#    反复启动 → 又 OOM → 又重启的雪崩。60 秒是折中。
# 2. 连续失败要退避。如果连续 5 次都起不来，说明不是偶发被杀，
#    而是环境有问题（二进制损坏、端口占用、内存彻底不够），
#    这时拉长间隔并记日志，别一直硬试。
# 3. 每次重启都降低 oom_score_adj，让新进程不容易再被杀。

BIN=/opt/gopanel/gopanel
SCRIPT=/etc/init.d/S97gopanel
INTERVAL=60
MAX_FAIL=5
fail_count=0

log() {
    logger -t gopanel-wd "$@"
}

check_and_restart() {
    if pidof gopanel >/dev/null 2>&1; then
        # 正常运行，清零失败计数
        fail_count=0
        # 兜底：确保 oom 保护还在（进程被重启过的话会重置）
        pid=$(pidof gopanel)
        adj=$(cat /proc/$pid/oom_score_adj 2>/dev/null)
        if [ "$adj" != "-500" ]; then
            echo -500 > /proc/$pid/oom_score_adj 2>/dev/null
        fi
        return 0
    fi

    # 没在跑，尝试重启
    #
    # 【重要】必须直接执行脚本，不能用 `sh $SCRIPT start`。
    # S97gopanel 的首行是 `#!/bin/sh /etc/rc.common`，这是 OpenWrt 的特殊
    # shebang：rc.common 会根据 $0 和第一个参数派发到 start()/stop()。
    # 用 `sh script.sh start` 会绕过这个机制，start() 根本不会被调用，
    # 表现为"执行了但进程没起来"且没有任何报错。
    log "gopanel not running (fail_count=$fail_count), restarting"
    "$SCRIPT" start >/dev/null 2>&1

    sleep 2
    if pidof gopanel >/dev/null 2>&1; then
        log "restart OK (pid $(pidof gopanel))"
        fail_count=0
    else
        fail_count=$((fail_count + 1))
        log "restart FAILED (fail_count=$fail_count)"

        # 连续失败太多，退避
        if [ $fail_count -ge $MAX_FAIL ]; then
            log "too many failures, backing off"
            sleep 300
            fail_count=0
        fi
    fi
}

log "watchdog started, interval=${INTERVAL}s"

while true; do
    check_and_restart
    sleep $INTERVAL
done