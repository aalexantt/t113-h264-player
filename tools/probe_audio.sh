#!/bin/sh
# 在音频确实播放期间采样 DAPM 状态
#
# 上一版用 "cmd &" + "wait" 的写法，采样时播放已经结束了，
# 读到的是 idle 状态（stream Playback inactive），会误判成"路径断了"。
# 这里改成：后台播放 5 秒，每秒采样一次 DAPM。

D=/sys/kernel/debug/asoc/audiocodec/2030000.codec/dapm

/tmp/alsatest >/dev/null 2>&1 &
Pid=$!

i=1
while [ $i -le 4 ]; do
    sleep 1
    echo "=== 第 $i 秒采样 ==="
    cat "$D/Playback" 2>/dev/null | sed -n '1p'
    for w in DACL DACR "LINEOUTL Output Select" LINEOUTL LINEOUTR LINEOUT; do
        printf '  %-24s ' "$w"
        cat "$D/$w" 2>/dev/null | sed -n '1p'
    done
    i=$((i+1))
done

kill $Pid 2>/dev/null
wait $Pid 2>/dev/null

echo "=== 播放结束后 ==="
cat "$D/Playback" 2>/dev/null | sed -n '1p'
cat "$D/LINEOUT" 2>/dev/null | sed -n '1p'