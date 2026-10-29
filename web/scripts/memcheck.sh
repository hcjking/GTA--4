#!/bin/bash
# Free memory %, wired, swap, and GPU (IOGPU) allocation, one line.
free=$(memory_pressure 2>/dev/null | awk -F': ' '/free percentage/{print $2}')
pg=$(sysctl -n hw.pagesize)
wired=$(vm_stat | awk -v pg=$pg '/Pages wired down/{gsub("\\.","",$4); printf "%.1f", $4*pg/1073741824}')
swap=$(sysctl -n vm.swapusage | awk '{print $6}')
gpu=$(ioreg -r -d1 -c IOAccelerator 2>/dev/null | grep -o '"Alloc system memory"=[0-9]*' | head -1 | awk -F= '{printf "%.1f", $2/1073741824}')
echo "$(date +%T) free $free wired ${wired}G swap $swap gpu ${gpu}G"
