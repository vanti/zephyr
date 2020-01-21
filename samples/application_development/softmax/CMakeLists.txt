# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
include($ENV{ZEPHYR_BASE}/cmake/app/boilerplate.cmake NO_POLICY_SCOPE)
project(softmax)


target_include_directories(app PRIVATE
        ${PROJECT_SOURCE_DIR}/src
	/home/user/zephyr-sdks/zephyr-sdk-0.10.3/arm-zephyr-eabi/arm-zephyr-eabi/include/c++/8.3.0
	/home/user/zephyr-sdks/zephyr-sdk-0.10.3/arm-zephyr-eabi/arm-zephyr-eabi/include/c++/8.3.0/arm-zephyr-eabi
	/home/user/zephyr-sdks/zephyr-sdk-0.10.3/arm-zephyr-eabi/arm-zephyr-eabi/include
	#/home/user/zephyr-sdks/zephyr-sdk-0.10.3/arm-zephyr-eabi/arm-zephyr-eabi/include/c++/8.3.0/tr1
	${PROJECT_SOURCE_DIR}/src/third_party/flatbuffers/include
	${PROJECT_SOURCE_DIR}/src/third_party/gemmlowp
        )

file(GLOB_RECURSE SRC_FILES
       ${PROJECT_SOURCE_DIR}/src/*.cpp
       ${PROJECT_SOURCE_DIR}/src/*.cc)
target_sources(app PRIVATE ${SRC_FILES})
