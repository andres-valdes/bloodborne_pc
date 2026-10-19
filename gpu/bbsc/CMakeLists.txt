# bbport seamless co-op module: libbbsc.so, the BBSC mod (VENDOR.txt) built for Linux. The runtime
# loads it only for co-op (BB_BBSC=1, src/runtime_bbsc.c) and starts it with bbsc_attach before the
# game runs; everything else in it is hidden and bound to itself. It sits in its own folder with the
# notices BBSC requires beside it. BBSC's stand-ins for its emulator (mod/shim) come before its
# sources; SDL3 is only read at run time (the host's), so only its headers are used.

find_package(Threads REQUIRED)

add_library(bbsc SHARED mod/bbsc_mod.cpp mod/bbsc_hooks.cpp mod/bbsc_relay.cpp mod/bbsc_channel.cpp
    mod/bbsc_http.cpp mod/bbsc_discord.cpp mod/bbsc_platform_posix.cpp src/core/bbsc_memscan.cpp
    src/core/bloodborne_re.cpp)
target_include_directories(bbsc PRIVATE mod/shim src "${XBYAK_INCLUDE}" ${SDL3_INCLUDE_DIRS})
target_compile_definitions(bbsc PRIVATE BBSC_MOD)
target_compile_options(bbsc PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_options(bbsc PRIVATE -Wl,-Bsymbolic -Wl,--exclude-libs,ALL -Wl,--no-undefined)
target_link_libraries(bbsc PRIVATE fmt::fmt Threads::Threads)

set(BBSC_OUTPUT "${CMAKE_BINARY_DIR}/bbsc")
set_target_properties(bbsc PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${BBSC_OUTPUT}")
configure_file(LICENSE "${BBSC_OUTPUT}/LICENSE.txt" COPYONLY)
configure_file(NOTICE.md "${BBSC_OUTPUT}/NOTICE.txt" COPYONLY)
configure_file(SUPPORTERS.txt "${BBSC_OUTPUT}/SUPPORTERS.txt" COPYONLY)
