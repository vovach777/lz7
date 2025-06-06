@echo off
..\build\lz7.exe "..\samples\1.txt" 2>"1-gen.log" 
..\build\lz7.exe "..\samples\2.txt" 2>"2-gen.log" 
..\build\lz7.exe "..\samples\3.txt" 2>"3-gen.log" 
..\build\lz7.exe "..\samples\4.txt" 2>"4-gen.log" 
..\build\lz7.exe "..\samples\5.txt" 2>"5-gen.log" 
..\build\lz7dec.exe "..\samples\1.txt.lz7" 2>"1-dec.log" 
..\build\lz7dec.exe "..\samples\2.txt.lz7" 2>"2-dec.log" 
..\build\lz7dec.exe "..\samples\3.txt.lz7" 2>"3-dec.log" 
..\build\lz7dec.exe "..\samples\4.txt.lz7" 2>"4-dec.log" 
..\build\lz7dec.exe "..\samples\5.txt.lz7" 2>"5-dec.log" 
fc 1-gen.log 1-dec.log >results.txt
fc 2-gen.log 2-dec.log >>results.txt
fc 3-gen.log 3-dec.log >>results.txt
fc 4-gen.log 4-dec.log >>results.txt
fc 5-gen.log 5-dec.log >>results.txt
..\build\lz7.exe "..\samples\sqlite3.c" 2>"sqlite3.c.log"
..\build\lz7dec.exe "..\samples\sqlite3.c.lz7" 2>"sqlite3.c.lz7.log"  
fc sqlite3.c.log sqlite3.c.lz7.log >>results.txt


