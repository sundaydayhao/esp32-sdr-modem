include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
idf_component_register(SRCS ${C2_SOURCES} "codec2_alloc.c" INCLUDE_DIRS "vendor/src" "include" REQUIRES heap)
target_compile_definitions(${COMPONENT_LIB} PUBLIC CODEC2_MODE_EN_DEFAULT=0 CODEC2_MODE_700C_EN=1)
# Upstream's embedded build keeps the read-only quantisers in flash and
# delegates working storage to the platform allocator. Do not copy 80 KiB of
# immutable 700C codebooks into the SRAM needed by USB DMA and task stacks.
target_compile_definitions(${COMPONENT_LIB} PRIVATE __EMBEDDED__)
# Preserve upstream files; narrow warnings for third-party code only.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-function)
