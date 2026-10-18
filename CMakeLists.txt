cmake_minimum_required(VERSION 3.10)
project(LZ7Project)

# Указываем версию C++ стандарта
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
add_definitions(-D_USE_MATH_DEFINES -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS )
# x86-флаги только для x86: на ARM -mavx/-march=native не соберутся
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|AMD64|amd64|i[3-6]86")
  add_compile_options(-mavx -msse4.2 -mssse3 -march=native)
endif()

# Добавляем директории для заголовочных файлов
include_directories(include)

# Добавляем исходные файлы
add_executable(lz7 src/main.cpp include/lz7.hpp include/profiling.hpp include/mio.hpp include/myargs.hpp)
add_executable(lz7parser src/parser.cpp include/lz7.hpp include/mio.hpp)
add_executable(lz7dec src/decoder.cpp include/lz7.hpp include/profiling.hpp include/mio.hpp)
