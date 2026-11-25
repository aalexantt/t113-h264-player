# gopanel 开机自启与看门狗

> 平台：BingPi-M2 / T113-s3
> 文件：`/etc/init.d/S97gopanel`、`/etc/init.d/S98gopanel-watchdog`、`/opt/gopanel/watchdog.sh`
> 记录时间：2026-11-25

## 一、现象

Web 管理面板（gopanel）**开机没有自动启动**，8080 端口访问不了。

## 二、排查过程

### 2.1 先确认是不是脚本本身没被执行

```bash
ls -la /etc/init.d/S97gopanel     # 存在，权限 777，可执行
ls -la /opt/gopanel/gopanel       # 二进制在
ps | grep gopanel                 # 进程不存在
```

脚本和二进制都在，但进程没跑。

### 2.2 查内核日志，发现真相

板子上**没有 `logread`**（busybox 没编），只能看 dmesg：

```bash
dmesg | grep -iE 'gopanel|oom'
```

```
[ 1019.452583] [    202]     0   202   143048     1366    24576        0   0 gopanel
[ 1204.664876] [    202]     0   202   143048      764    24576        0   0 gopanel
[38650.016324] [    202]     0   202   143048      474    24576        0   0 gopanel
[38650.092258] oom-kill:constraint=CONSTRAINT_NONE,nodemask=(null),task=gopanel,pid=202,uid=0
[38650.101608] Out of memory: Killed process 202 (gopanel) total-vm:572192kB, anon-rss:1892kB, file-rss:4kB
```

**关键结论：gopanel 开机时是正常启动的（PID 202），但在系统运行 38650 秒
（约 10.7 小时）后被 OOM killer 杀掉了。**

所以不是"没启动"，而是"被杀了之后没被重新拉起"。

### 2.3 谁触发的 OOM

```
[38649.558627] adbd invoked oom-killer
```

**触发者是 `adbd`** —— 也就是调试时的 adb 操作。当时在做视频调试，
反复 `adb pull` 31MB 的视频文件、`adb push` 二进制，把本就只剩 9MB 的空闲内存压垮了。

内核按 oom_score 选了个"最大"的进程杀掉，选中的是 gopanel。

### 2.4 为什么总是杀 gopanel

查看实际内存占用（`/proc/<pid>/statm` 第 2 字段是 RSS 页数，每页 4KB）：

| 进程 | RSS | 实际占用 |
|---|---|---|
| gopanel | 913 页 | **3.6 MB** |
| wpa_supplicant | 364 页 | 1.5 MB |
| adbd | 200 页 | 0.8 MB |
| ntpd | 104 页 | 0.4 MB |
| 其余 | < 100 页 | < 0.4 MB |

gopanel 实际只用 **3.6MB**，是所有进程里最大的，但这不足以让它优先被杀。

**真正原因**：它是 Go 程序，启动时预留了大量虚拟地址空间
（`total-vm: 572192kB` = 572MB），而内核的 OOM 评分会综合 virtual size 和 RSS。
结果它的 `oom_score` 达到 33，是全场最高。

### 2.5 为什么杀了不会自己起来

```bash
cat /etc/init.d/rc.final
# for i in /etc/init.d/S??*; do ... done
```

这个构建的 `rc.final` 只是**顺序执行一遍 S 脚本**，没有 OpenWrt procd 那种
respawn 机制。`/etc/inittab` 里也没给 gopanel 配 respawn。

**所以进程被杀后就永久消失了。**

## 三、解决方案

两项措施：

### 3.1 降低 OOM 评分

```bash
echo -500 > /proc/$(pidof gopanel)/oom_score_adj
```

取值范围 `-1000 ~ 1000`。设为 `-500` 后 `oom_score` 从 33 大幅降低，
不会被优先选中。没设 `-1000` 是因为那等于完全豁免，真正内存耗尽时
内核只能去杀更重要的进程（比如 adbd 或 wpa_supplicant），反而更糟。

### 3.2 加看门狗定期检查重启

因为 `rc.final` 没有 respawn，只能自己实现。

`watchdog.sh` 每 60 秒检查一次，进程不在就通过 `S97gopanel start` 拉起。

**为什么间隔取 60 秒**：OOM 往往伴随持续的内存压力，重启太频繁会变成
"启动 → 又 OOM → 又重启"的雪崩。60 秒是个折中。

**连续失败退避**：连续 5 次起不来就说明不是偶发被杀（可能是二进制损坏、
端口占用、内存彻底不够），这时拉长到 300 秒再试，并记日志，不硬试。

## 四、踩到的三个坑

### 坑 1：`sh script.sh start` 不会调用 start()

`S97gopanel` 的首行是：

```sh
#!/bin/sh /etc/rc.common
```

这是 OpenWrt 的特殊 shebang。`rc.common` 会根据 `$0`（脚本自身路径）
和第一个参数派发到 `start()` / `stop()`。

用 `sh /etc/init.d/S97gopanel start` 会**绕过这个机制** ——
`start()` 根本不会被调用，输出里也没有任何报错，
表现为"执行了但进程没起来"。

**必须直接执行**：

```bash
/etc/init.d/S97gopanel start     # 正确
sh /etc/init.d/S97gopanel start  # 错误，静默失效
```

### 坑 2：`pidof watchdog.sh` 匹配不到

看门狗进程实际是 `/bin/sh /opt/gopanel/watchdog.sh`。
`pidof` 按进程的 `comm` 字段（可执行文件名）匹配，
这个进程的 `comm` 是 `sh` 而不是 `watchdog.sh`。

同样的原因，`killall watchdog.sh` 也杀不掉它，会报 `no process killed`。

**改用扫 `/proc/*/cmdline` 找完整路径**：

```sh
for p in /proc/[0-9]*/cmdline; do
    tr '\0' ' ' < "$p" 2>/dev/null | grep -q "watchdog\.sh" && return 0
done
```

### 坑 3：gopanel 只监听 IPv6

服务正常运行时，`/proc/net/tcp`（IPv4 表）里**找不到 8080**，
会误以为没起来。实际在 `/proc/net/tcp6` 里：

```
00000000000000000000000000000000:1F90  ...  inode 90784
```

`0x1F90 = 8080`。Go 的 `net.Listen("tcp", ":8080")` 在双栈环境下
可能只绑 IPv6（带 v6only 语义取决于系统配置）。

**判断服务是否正常，不要只看 `/proc/net/tcp`**，两个表都要看，
或者直接 `wget` 测一下：

```bash
wget -q -O - http://127.0.0.1:8080/ | head -3
```

## 五、文件说明

| 文件 | 部署位置 | 作用 |
|---|---|---|
| `S97gopanel` | `/etc/init.d/` | 启动 gopanel，并设置 oom_score_adj |
| `S98gopanel-watchdog` | `/etc/init.d/` | 启动看门狗（START=98，在其后） |
| `watchdog.sh` | `/opt/gopanel/` | 每 60 秒检查，挂了就重启 |

`START=98` 让看门狗排在 `S97gopanel` 之后 —— 先让 gopanel 正常启动，
看门狗再接管。反过来的话它会在 gopanel 起来之前就判"挂了"，
白打一条重启日志。

## 六、部署与验证

```bash
# 部署
adb push S97gopanel /etc/init.d/S97gopanel
adb push S98gopanel-watchdog /etc/init.d/S98gopanel-watchdog
adb push watchdog.sh /opt/gopanel/watchdog.sh
adb shell "sed -i 's/\r$//' /etc/init.d/S97gopanel /etc/init.d/S98gopanel-watchdog /opt/gopanel/watchdog.sh"
adb shell "chmod +x /etc/init.d/S97gopanel /etc/init.d/S98gopanel-watchdog /opt/gopanel/watchdog.sh"

# 启动
adb shell /etc/init.d/S97gopanel start
adb shell /etc/init.d/S98gopanel-watchdog start
```

验证自动恢复：

```bash
adb shell "killall -9 gopanel"          # 模拟 OOM 被杀
# 等 60 秒以上
adb shell "pidof gopanel"                # 应该已经恢复
adb shell "cat /proc/\$(pidof gopanel)/oom_score_adj"   # 应该是 -500
```

实测结果：

```
=== 杀掉 gopanel ===
杀后: 已死
=== 70 秒后 ===
✅ 已自动恢复 pid=8521
oom_adj=-500
```

## 七、备注

- **这个方案只是缓解，不是根治。** 根因是板子内存只有 108MB，
  而调试时的 adb 大文件传输会瞬间吃掉大量内存。
- 如果后续要做音频解码（AAC），内存会更紧张，需要重新评估。
- 原脚本已备份到 `/opt/gopanel/S97gopanel.bak`，出问题可以回滚：

  ```bash
  cp /opt/gopanel/S97gopanel.bak /etc/init.d/S97gopanel
  ```

- 板子上**没有** `logread` / `head` / `tail` / `sort` / `pgrep` / `nohup`，
  写脚本时只能用 `grep` / `sed` / `awk` / `cat` / `tr` / `pidof`。

## 八、局域网域名访问 `http://linuxhost.local/`

### 8.1 现象

`http://linuxhost.local/` 访问不了。

### 8.2 三重原因

| 问题 | 实测 |
|---|---|
| 没有 mDNS 广播 | 板子上没有 avahi/mdnsd，gopanel 的 `-mdns` 也没启用 |
| 主机名不匹配 | 系统 hostname 是 `TinaLinux`，不是 `linuxhost` |
| 没监听 80 端口 | 原来监听 8080，`http://xxx.local/` 默认打到 80 |

### 8.3 gopanel 自带 mDNS，但要显式开启

```bash
/opt/gopanel/gopanel -h
```

```
-host string    mDNS 主机名，默认取系统 hostname
-mdns           启用 mDNS 广播 (hostname.local)，需要网络允许客户端间多播
```

原来的启动参数只有 `-addr :8080 -data ...`，没有 `-mdns`。

### 8.4 改法

启动脚本里加两个变量：

```sh
PORT=80                 # 用 80 就不用带端口号
MDNS_ENABLE=1
MDNS_HOST=linuxhost
```

参数拼接：

```sh
ar_args="-addr :$PORT -data $DATADIR"
[ "$MDNS_ENABLE" = "1" ] && ar_args="$ar_args -mdns -host $MDNS_HOST"
start-stop-daemon -S -b -x "$BIN" -m -p "$PIDFILE" -- $ar_args
```

### 8.5 验证结果

```
$ ping linuxhost.local
回复自 192.168.3.250: 字节=32 时间=4ms TTL=64

$ curl -o /dev/null -w "%{http_code}" http://linuxhost.local/
200
```

### 8.6 注意：`nslookup` 查不到是正常的

```bash
nslookup linuxhost.local
# *** Non-existent domain
```

mDNS 走**多播 UDP 5353**，不经过 DNS 服务器。`nslookup` 只问配置的 DNS 服务器，
所以永远查不到 `.local`。

要用 `ping`、`curl` 或浏览器来测——这些走系统解析链，Windows 10/11、
macOS、装有 avahi 的 Linux 都能自动处理 `.local`。

## 九、这一轮踩到的四个坑

### 坑 1：`-addr 80` 少了冒号

```
gopanel listening on 80
2026/11/25 11:03:13 listen tcp: address 80: missing port in address
```

gopanel 的 `-addr` 是 Go 的 `host:port` 格式，监听所有地址必须写 `:80`。
写成 `80` 会当成主机名解析，报错后进程立即退出。

**这个坑最耗时间**，因为：
- `start-stop-daemon` 返回 0（成功）
- pidfile 也正常写了
- 但进程立刻消失，`pidof` 找不到
- 没有任何错误输出到终端（被 `-b` 重定向到 /dev/null 了）

只有手动前台执行才看得到那行 `missing port in address`。

**教训**：`start-stop-daemon -b` 启动后必须用 `pidof` 二次确认，
不能只看它的返回值。

### 坑 2：`set --` 破坏 rc.common 的参数派发

`rc.common` 的逻辑是：

```sh
initscript=$1
action=${2:-help}
shift 2
...
"$action" "$@" &
```

它在最外层取走了 `$1`（脚本路径）和 `$2`（start/stop），然后调用 `start()`。

如果 `start()` 内部用 `set -- -addr ...`，会**覆盖脚本自身的位置参数**。
虽然此时 `action` 已经存进变量了，但如果 start() 里还要用位置参数，
就会拿到错的值。

改成普通变量拼接更安全。

### 坑 3：`start-stop-daemon -b` 的静默失败

这和环境无关，是通用行为：`-b` 让进程脱离终端，stdout/stderr 都指向
/dev/null，**任何启动期错误都会被吞掉**。

排查这类"启动了但没起来"的问题，唯一可靠的办法是手动前台跑一次：

```bash
/opt/gopanel/gopanel -addr :80 -data /opt/gopanel/data -mdns -host linuxhost
```

### 坑 4：误以为是 `-b` 和 rc.common 的 `&` 冲突

排查过程中一度认为是"双层后台"导致进程被挂断，改成不带 `-b`。
实际验证后发现 `-b` 完全正常，真正的错误是坑 1 的冒号。

**教训**：不要基于推理下结论，要实测。当时如果不做那个
"`-b` 启动 8080 vs 80 对比"的实验，就会一直往错误方向改。