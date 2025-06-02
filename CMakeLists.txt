cmake_minimum_required(VERSION 3.10)
project(LZ7Project)

# Указываем версию C++ стандарта
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Добавляем директории для заголовочных файлов
include_directories(include)
add_definitions(-D_USE_MATH_DEFINES -DNOMINMAX )

# Добавляем исходные файлы
add_executable(lz7 src/main.cpp include/lz7.hpp include/profiling.hpp)
add_executable(lz7dec src/decoder.cpp)
