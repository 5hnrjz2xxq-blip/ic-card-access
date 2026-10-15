#!/bin/bash
# 门禁项目快捷编译/烧录脚本
# 用法：./build.sh        → 只编译
#       ./build.sh up     → 编译+烧录（upload port 按自己机器修改）
cd "$(dirname "$0")"
PIO=~/.platformio/penv/bin/pio
if [ "$1" = "up" ]; then
  $PIO run -t upload
else
  $PIO run
fi
