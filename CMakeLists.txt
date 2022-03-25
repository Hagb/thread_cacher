cmake_minimum_required(VERSION 3.20)

project(thread_reuse
	LANGUAGES C
	HOMEPAGE_URL https://github.com/Hagb/thread_reuse)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DTHREAD_SAFE")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LIBHL_SOURCE libhl/src)
include_directories(${LIBHL_SOURCE})

add_library(${PROJECT_NAME} SHARED thread_reuse.c)

add_library(hashtable STATIC
	${LIBHL_SOURCE}/hashtable.c
	${LIBHL_SOURCE}/linklist.c
	)
target_link_libraries(${PROJECT_NAME} PRIVATE hashtable)

add_library(queue STATIC
	${LIBHL_SOURCE}/queue.c
	${LIBHL_SOURCE}/refcnt.c
	${LIBHL_SOURCE}/rqueue.c
	)
target_link_libraries(${PROJECT_NAME} PRIVATE queue)

target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})



