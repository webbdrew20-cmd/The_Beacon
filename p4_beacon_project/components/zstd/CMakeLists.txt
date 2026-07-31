idf_component_register(
    SRCS "zstddeclib.c"
    INCLUDE_DIRS "include"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function)
