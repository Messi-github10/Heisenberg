add_library(HeisenbergPreviewer STATIC
    Common/Packet.cpp
    Demuxer/Demuxer.cpp
    Demuxer/DemuxerFactory.cpp
    Decoder/IDecoder.hpp
    Decoder/SoftwareDecoder.hpp
        Decoder/SoftwareDecoder.cpp
        Decoder/D3D11Decoder.cpp
        Decoder/D3D11Decoder.hpp
        Decoder/CudaDecoder.cpp
        Decoder/CudaDecoder.hpp
        Decoder/DecoderFactory.cpp
        Decoder/DecoderFactory.hpp
        Decoder/HwTransfer.cpp
        Decoder/HwTransfer.hpp
        Render/SwapChain.hpp
        Render/SwapChain.cpp
        Render/RenderEngine.hpp
        Render/RenderEngine.cpp
        Render/GpuContext.hpp
        Render/GpuContext.cpp
        Render/VulkanContext.hpp
        Render/VulkanContext.cpp
)

target_include_directories(HeisenbergPreviewer PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(HeisenbergPreviewer PUBLIC
    ffmpeg
)

if(MSVC)
    target_compile_options(HeisenbergPreviewer PRIVATE /utf-8)
endif()
