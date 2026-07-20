cmake_minimum_required(VERSION 4.2)
project(lilyVM C)

set(CMAKE_C_STANDARD 11)

add_executable(vmm
        Globals.c
        Lists.c
        PTE.c
        PFN.c
        Threads.c
        Workload.c
        main.c
        Disc.c
        Fault.c
        Workload.c
)

target_link_libraries(vmm advapi32)
#
#target_compile_definitions(vmm PRIVATE
#        $<$<CONFIG:Debug>:VMM_DIAGNOSTICS=1>
#        $<$<CONFIG:Release>:VMM_DIAGNOSTICS=0>
#        VMM_SAFETY=1
#)