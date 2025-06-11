#!/bin/bash

# Пути к исполняемым файлам (возможно, вам нужно указать полный путь)
LZ7_PARSER="../build/lz7parser"
LZ7_COMP="../build/lz7"
LZ7_DEC="../build/lz7dec"

# Проверка, существуют ли бинарники
if [[ ! -f "$LZ7_COMP" ]]; then
    echo "Error: $LZ7_COMP not found!" >&2
    exit 1
fi
if [[ ! -f "$LZ7_PARSER" ]]; then
    echo "Error: $LZ7_PARSER not found!" >&2
    exit 1
fi
if [[ ! -f "$LZ7_DEC" ]]; then
    echo "Error: $LZ7_DEC not found!" >&2
    exit 1
fi

# Проверка папки samples
if [[ ! -d "../samples" ]]; then
    echo "Error: ../samples directory not found!" >&2
    exit 1
fi

# Очистка старых логов (если нужно)
rm -f *.log results.txt

# Запуск lz7parser & lz7 для каждого файла
for i in {1..5}; do
    input_file="../samples/$i.txt"
    if [[ ! -f "$input_file" ]]; then
        echo "Warning: $input_file not found, skipping..." >&2
        continue
    fi
    "$LZ7_PARSER" "$input_file" 2>"$i-gen.log"
    "$LZ7_COMP" "$input_file"
done

# Запуск lz7dec для каждого сжатого файла
for i in {1..5}; do
    compressed_file="../samples/$i.txt.lz7"
    if [[ ! -f "$compressed_file" ]]; then
        echo "Warning: $compressed_file not found, skipping..." >&2
        continue
    fi
    "$LZ7_DEC" "$compressed_file" 2>"$i-dec.log"
done

# Сравнение логов
for i in {1..5}; do
    if [[ ! -f "$i-gen.log" || ! -f "$i-dec.log" ]]; then
        echo "Warning: Logs for $i not found, skipping comparison..." >&2
        continue
    fi
    echo "=== Comparing $i-gen.log and $i-dec.log ===" >> results.txt
    diff -q "$i-gen.log" "$i-dec.log" >> results.txt
done

# Обработка sqlite3.c (если есть)
if [[ -f "../samples/sqlite3.c" ]]; then
    "$LZ7_PARSER" "../samples/sqlite3.c" 2>"sqlite3.c.log"
     "$LZ7_COMP" "../samples/sqlite3.c"
    "$LZ7_DEC" "../samples/sqlite3.c.lz7" 2>"sqlite3.c.lz7.log"
    echo "=== Comparing sqlite3.c.log and sqlite3.c.lz7.log ===" >> results.txt
    diff -q "sqlite3.c.log" "sqlite3.c.lz7.log" >> results.txt
else
    echo "Warning: ../samples/sqlite3.c not found, skipping..." >&2
fi

echo "Done! Check results.txt for differences."