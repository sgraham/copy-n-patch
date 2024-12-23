@echo off
python clang_rip.py
clang -Wall -Wextra -Werror -Oz cnp.c -o cnp.exe
