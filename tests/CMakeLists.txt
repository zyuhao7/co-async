# 回归测试与 example 冒烟层。
#
# 只在 co_async 作为顶层项目构建时进入：根 CMakeLists.txt 的
# PROJECT_SOURCE_DIR 判据把 add_subdirectory(tests) 挡在库被别的项目
# add_subdirectory 的场景之外，所以这里定义的可执行文件不会跟着库一起被带过去。
#
# 用法（本机 5.15 需要 INVALFIX 才能建起 socket）：
#   cmake -B build-dbg -DCO_ASYNC_DEBUG=ON -DCO_ASYNC_INVALFIX=ON
#   cmake --build build-dbg -j4
#   ctest --test-dir build-dbg --output-on-failure
#   ctest --test-dir build-dbg -L unit      # 只跑第二层
#   ctest --test-dir build-dbg -L smoke     # 只跑第一层
#
# 内存症状（各用例头部记的那种）用 valgrind 跑同一批：
#   cmake -B build-vg -DCO_ASYNC_INVALFIX=ON -DCO_ASYNC_CTEST_VALGRIND=ON
#   cmake --build build-vg -j4 && ctest --test-dir build-vg --output-on-failure

option(CO_ASYNC_CTEST_VALGRIND
       "用 valgrind 跑各用例（抓断言看不出的非法访问，跑得慢很多）" OFF)

# ---- 第二层：关键路径用例 --------------------------------------------------
# 每个 tests/*.cpp 自带断言，失败以非零退出码表达，所以直接跑就能判通过。
# 退出码看不见的那些问题（use-after-free 不一定 abort）交给上面的 valgrind 开关。
file(GLOB test_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
foreach(path ${test_sources})
    get_filename_component(name ${path} NAME_WE)
    add_executable(${name} ${path})
    target_link_libraries(${name} PRIVATE co_async)

    if (CO_ASYNC_CTEST_VALGRIND)
        find_program(CO_ASYNC_VALGRIND_EXECUTABLE valgrind REQUIRED)
        add_test(NAME unit/${name}
                 COMMAND ${CO_ASYNC_VALGRIND_EXECUTABLE} --error-exitcode=99
                         --quiet $<TARGET_FILE:${name}>)
        set_tests_properties(unit/${name} PROPERTIES TIMEOUT 300)
    else()
        add_test(NAME unit/${name} COMMAND $<TARGET_FILE:${name}>)
        set_tests_properties(unit/${name} PROPERTIES TIMEOUT 60)
    endif()
    set_tests_properties(unit/${name} PROPERTIES LABELS unit)
endforeach()

# ---- 第一层：example 冒烟 --------------------------------------------------
# 29 个 example 能不能在本机启动、并按各自应有的方式结束。分类沿用阶段 0 的
# 现象清单（/home/xh/co_async/run-examples.sh），阶段 2 与阶段 6 各复认过一次。
#
# timeout：常驻服务；以及要等本机给不出的输入的两个（inotify 等外部写手，
#   io_multiplex 等 stdin 和 /dev/input/mouse0）。超时正是预期结局。
# either：inotify 是否被唤醒取决于测试期间有没有人写它监视的目录——CTest 自己
#   往 Testing/ 写日志就算——两种结局都不算失败。
# disabled：https_fetch 要外网，openai_test 要凭据。
set(CO_ASYNC_SMOKE_LIMIT 10)
set(CO_ASYNC_SMOKE_SLOW_LIMIT 40)

set(CO_ASYNC_EXAMPLES_TIMEOUT
    server
    server_mt
    echo_server
    chat_server
    file_server
    websocket_server
    proxy_route
    io_multiplex)
set(CO_ASYNC_EXAMPLES_EITHER inotify)
set(CO_ASYNC_EXAMPLES_SLOW generator)   # 61 次 co_sleep(300ms) ≈ 18s 后自行退出
set(CO_ASYNC_EXAMPLES_DISABLED https_fetch openai_test)

file(GLOB example_sources ${PROJECT_SOURCE_DIR}/examples/*.cpp)
foreach(path ${example_sources})
    get_filename_component(name ${path} NAME_WE)

    if (name IN_LIST CO_ASYNC_EXAMPLES_DISABLED)
        continue()
    endif()

    if (name IN_LIST CO_ASYNC_EXAMPLES_TIMEOUT)
        set(expect timeout)
        set(limit ${CO_ASYNC_SMOKE_LIMIT})
    elseif (name IN_LIST CO_ASYNC_EXAMPLES_EITHER)
        set(expect either)
        set(limit ${CO_ASYNC_SMOKE_LIMIT})
    elseif (name IN_LIST CO_ASYNC_EXAMPLES_SLOW)
        set(expect exit-zero)
        set(limit ${CO_ASYNC_SMOKE_SLOW_LIMIT})
    else()
        set(expect exit-zero)
        set(limit ${CO_ASYNC_SMOKE_LIMIT})
    endif()

    # 示例自己用 execute_process 限时，CTest 的 TIMEOUT 只作兜底。
    math(EXPR ctest_timeout "${limit} + 5")
    add_test(NAME smoke/${name}
             COMMAND ${CMAKE_COMMAND}
                     -DNAME=${name}
                     -DEXECUTABLE=$<TARGET_FILE:${name}>
                     -DLIMIT=${limit}
                     -DEXPECT=${expect}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/run_example.cmake)
    # 服务进程会绑各自的默认端口，别让 ctest -j 把它们叠在一起。
    if (expect STREQUAL "timeout")
        set_tests_properties(smoke/${name} PROPERTIES RUN_SERIAL TRUE)
    endif()
    # read_file 读的是 ../examples/read_file.cpp，从构建目录出发才对得上。
    set_tests_properties(smoke/${name} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        TIMEOUT ${ctest_timeout}
        LABELS smoke)
endforeach()
