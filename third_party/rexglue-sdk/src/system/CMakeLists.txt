set(REXSYSTEM_SOURCES
    runtime.cpp
    kernel_state.cpp
    xmemory.cpp
    kernel_module.cpp
    user_module.cpp
    xthread.cpp
    xevent.cpp
    xfile.cpp
    xmutant.cpp
    xsemaphore.cpp
    xtimer.cpp
    xiocompletion.cpp
    xnotifylistener.cpp
    xenumerator.cpp
    xsocket.cpp
    xsymboliclink.cpp
    xobject.cpp
    xmodule.cpp
    shared_library.cpp
    guest_path.cpp

    # Utilities
    util/object_table.cpp
    util/native_list.cpp
    util/xdbf_utils.cpp
    util/gameinfo_utils.cpp

    # XAM services
    xam/app_manager.cpp
    xam/content_manager.cpp
    xam/user_profile.cpp

    ppc_types.cpp
    elf_module.cpp
    entry_table.cpp
    export_resolver.cpp
    lzx.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    function_dispatcher.cpp
    thread.cpp
    thread_state.cpp
    xex_module.cpp
)

add_library(rexruntime SHARED ${REXSYSTEM_SOURCES})
add_library(rex::runtime ALIAS rexruntime)
add_library(rex::system ALIAS rexruntime)
add_library(rex::kernel ALIAS rexruntime)

set_target_properties(rexruntime PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)

target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/thirdparty
)

target_link_libraries(rexruntime
    PUBLIC
        simde
        fmt::fmt
        spdlog::spdlog
        tomlplusplus
        SDL3::SDL3
    PRIVATE
        aes128
        mspack
)

if(REXGLUE_ENABLE_TRACY)
    target_link_libraries(rexruntime PUBLIC TracyClient)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(rexruntime PUBLIC REXGLUE_ENABLE_PERF_COUNTERS)
endif()

if(WIN32)
    target_link_libraries(rexruntime PRIVATE ws2_32)

    # Version metadata for the runtime DLL. Unsigned binaries without any
    # version resource are prone to antivirus false positives; embedding
    # identity here also lets crash dumps and support logs name the build.
    # The embedding application may override these before including the SDK.
    enable_language(RC)
    if(NOT REXGLUE_RUNTIME_RC_VERSION)
        set(REXGLUE_RUNTIME_RC_VERSION "0,0,0,0")
    endif()
    if(NOT REXGLUE_RUNTIME_RC_VERSION_STRING)
        set(REXGLUE_RUNTIME_RC_VERSION_STRING "0.0.0")
    endif()
    if(NOT REXGLUE_RUNTIME_RC_PRODUCT_NAME)
        set(REXGLUE_RUNTIME_RC_PRODUCT_NAME "rexglue")
    endif()
    if(NOT REXGLUE_RUNTIME_RC_COMPANY)
        set(REXGLUE_RUNTIME_RC_COMPANY "rexglue project")
    endif()
    if(NOT REXGLUE_RUNTIME_RC_DESCRIPTION)
        set(REXGLUE_RUNTIME_RC_DESCRIPTION "rexglue guest runtime library")
    endif()
    if(NOT REXGLUE_RUNTIME_RC_COPYRIGHT)
        set(REXGLUE_RUNTIME_RC_COPYRIGHT "(c) rexglue contributors")
    endif()
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/rexruntime_version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/rexruntime_version.rc
        @ONLY)
    target_sources(rexruntime PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/rexruntime_version.rc)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexruntime PUBLIC dxgi dxguid dxc-headers)
    target_compile_definitions(rexruntime PUBLIC REX_HAS_D3D12=1)
endif()
if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexruntime PUBLIC
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        SPIRV-Tools-static
    )
    target_compile_definitions(rexruntime PUBLIC REX_HAS_VULKAN=1)
endif()
