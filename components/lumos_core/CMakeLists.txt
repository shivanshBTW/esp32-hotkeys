idf_component_register(
    SRCS
        "src/logger.cpp"
        "src/framebuffer.cpp"
        "src/color.cpp"
    INCLUDE_DIRS "include"
    REQUIRES log
)
