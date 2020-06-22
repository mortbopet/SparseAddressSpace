cmake_minimum_required(VERSION 3.9)
set (CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(SparseAddressSpace CXX)

add_executable(sas_test tst_SparseAddressSpace.cpp SparseAddressSpace.h)
