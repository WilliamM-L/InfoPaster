#!/bin/bash
set -e
mkdir -p bin
gcc -std=c99 -Wall -Wextra -O2 -o bin/info_paster main.c
echo "Built bin/info_paster"
