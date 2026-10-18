cmake_minimum_required(VERSION 3.10)
project(LZ7Project)

# Указываем версию C++ стандарта
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
add_definitions(-D_USE_MATH_DEFINES -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS )
# Интринсиков нет: по умолчанию бинарь переносимый. -march=native по замерам скорость не меняет.
option(LZ7_NATIVE "Build with -march=native" OFF)
if(LZ7_NATIVE AND NOT MSVC)
  add_compile_options(-march=native)
endif()

# Добавляем директории для заголовочных файлов
include_directories(include)

# Добавляем исходные файлы
add_executable(lz7 src/main.cpp include/lz7.hpp include/profiling.hpp include/mio.hpp include/myargs.hpp)
add_executable(lz7parser src/parser.cpp include/lz7.hpp include/mio.hpp)
add_executable(lz7dec src/decoder.cpp include/lz7.hpp include/profiling.hpp include/mio.hpp)
