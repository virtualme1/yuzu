add_library(video_core STATIC
    command_processor.cpp
    command_processor.h
    debug_utils/debug_utils.cpp
    debug_utils/debug_utils.h
    engines/fermi_2d.cpp
    engines/fermi_2d.h
    engines/maxwell_3d.cpp
    engines/maxwell_3d.h
    engines/maxwell_compute.cpp
    engines/maxwell_compute.h
    gpu.cpp
    gpu.h
    memory_manager.cpp
    memory_manager.h
    rasterizer_interface.h
    renderer_base.cpp
    renderer_base.h
    renderer_opengl/gl_rasterizer.cpp
    renderer_opengl/gl_rasterizer.h
    renderer_opengl/gl_rasterizer_cache.cpp
    renderer_opengl/gl_rasterizer_cache.h
    renderer_opengl/gl_resource_manager.h
    renderer_opengl/gl_shader_decompiler.cpp
    renderer_opengl/gl_shader_decompiler.h
    renderer_opengl/gl_shader_gen.cpp
    renderer_opengl/gl_shader_gen.h
    renderer_opengl/gl_shader_util.cpp
    renderer_opengl/gl_shader_util.h
    renderer_opengl/gl_state.cpp
    renderer_opengl/gl_state.h
    renderer_opengl/gl_stream_buffer.cpp
    renderer_opengl/gl_stream_buffer.h
    renderer_opengl/maxwell_to_gl.h
    renderer_opengl/renderer_opengl.cpp
    renderer_opengl/renderer_opengl.h
    textures/decoders.cpp
    textures/decoders.h
    textures/texture.h
    utils.h
    video_core.cpp
    video_core.h
)

create_target_directory_groups(video_core)

target_link_libraries(video_core PUBLIC common core)
target_link_libraries(video_core PRIVATE glad)
