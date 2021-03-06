// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include "common/assert.h"
#include "core/core.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/textures/decoders.h"
#include "video_core/textures/texture.h"
#include "video_core/video_core.h"

namespace Tegra {
namespace Engines {

/// First register id that is actually a Macro call.
constexpr u32 MacroRegistersStart = 0xE00;

const std::unordered_map<u32, Maxwell3D::MethodInfo> Maxwell3D::method_handlers = {
    {0xE1A, {"BindTextureInfoBuffer", 1, &Maxwell3D::BindTextureInfoBuffer}},
    {0xE24, {"SetShader", 5, &Maxwell3D::SetShader}},
    {0xE2A, {"BindStorageBuffer", 1, &Maxwell3D::BindStorageBuffer}},
};

Maxwell3D::Maxwell3D(MemoryManager& memory_manager) : memory_manager(memory_manager) {}

void Maxwell3D::SubmitMacroCode(u32 entry, std::vector<u32> code) {
    uploaded_macros[entry * 2 + MacroRegistersStart] = std::move(code);
}

void Maxwell3D::CallMacroMethod(u32 method, const std::vector<u32>& parameters) {
    // TODO(Subv): Write an interpreter for the macros uploaded via registers 0x45 and 0x47

    // The requested macro must have been uploaded already.
    ASSERT_MSG(uploaded_macros.find(method) != uploaded_macros.end(), "Macro %08X was not uploaded",
               method);

    auto itr = method_handlers.find(method);
    ASSERT_MSG(itr != method_handlers.end(), "Unhandled method call %08X", method);

    ASSERT(itr->second.arguments == parameters.size());

    (this->*itr->second.handler)(parameters);

    // Reset the current macro and its parameters.
    executing_macro = 0;
    macro_params.clear();
}

void Maxwell3D::WriteReg(u32 method, u32 value, u32 remaining_params) {
    ASSERT_MSG(method < Regs::NUM_REGS,
               "Invalid Maxwell3D register, increase the size of the Regs structure");

    auto debug_context = Core::System::GetInstance().GetGPUDebugContext();

    // It is an error to write to a register other than the current macro's ARG register before it
    // has finished execution.
    if (executing_macro != 0) {
        ASSERT(method == executing_macro + 1);
    }

    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart) {
        // We're trying to execute a macro
        if (executing_macro == 0) {
            // A macro call must begin by writing the macro method's register, not its argument.
            ASSERT_MSG((method % 2) == 0,
                       "Can't start macro execution by writing to the ARGS register");
            executing_macro = method;
        }

        macro_params.push_back(value);

        // Call the macro when there are no more parameters in the command buffer
        if (remaining_params == 0) {
            CallMacroMethod(executing_macro, macro_params);
        }
        return;
    }

    if (debug_context) {
        debug_context->OnEvent(Tegra::DebugContext::Event::MaxwellCommandLoaded, nullptr);
    }

    regs.reg_array[method] = value;

#define MAXWELL3D_REG_INDEX(field_name) (offsetof(Regs, field_name) / sizeof(u32))

    switch (method) {
    case MAXWELL3D_REG_INDEX(code_address.code_address_high):
    case MAXWELL3D_REG_INDEX(code_address.code_address_low): {
        // Note: For some reason games (like Puyo Puyo Tetris) seem to write 0 to the CODE_ADDRESS
        // register, we do not currently know if that's intended or a bug, so we assert it lest
        // stuff breaks in other places (like the shader address calculation).
        ASSERT_MSG(regs.code_address.CodeAddress() == 0, "Unexpected CODE_ADDRESS register value.");
        break;
    }
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[0]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[1]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[2]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[3]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[4]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[5]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[6]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[7]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[8]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[9]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[10]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[11]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[12]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[13]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[14]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[15]): {
        ProcessCBData(value);
        break;
    }
    case MAXWELL3D_REG_INDEX(cb_bind[0].raw_config): {
        ProcessCBBind(Regs::ShaderStage::Vertex);
        break;
    }
    case MAXWELL3D_REG_INDEX(cb_bind[1].raw_config): {
        ProcessCBBind(Regs::ShaderStage::TesselationControl);
        break;
    }
    case MAXWELL3D_REG_INDEX(cb_bind[2].raw_config): {
        ProcessCBBind(Regs::ShaderStage::TesselationEval);
        break;
    }
    case MAXWELL3D_REG_INDEX(cb_bind[3].raw_config): {
        ProcessCBBind(Regs::ShaderStage::Geometry);
        break;
    }
    case MAXWELL3D_REG_INDEX(cb_bind[4].raw_config): {
        ProcessCBBind(Regs::ShaderStage::Fragment);
        break;
    }
    case MAXWELL3D_REG_INDEX(draw.vertex_end_gl): {
        DrawArrays();
        break;
    }
    case MAXWELL3D_REG_INDEX(query.query_get): {
        ProcessQueryGet();
        break;
    }
    default:
        break;
    }

#undef MAXWELL3D_REG_INDEX

    if (debug_context) {
        debug_context->OnEvent(Tegra::DebugContext::Event::MaxwellCommandProcessed, nullptr);
    }
}

void Maxwell3D::ProcessQueryGet() {
    GPUVAddr sequence_address = regs.query.QueryAddress();
    // Since the sequence address is given as a GPU VAddr, we have to convert it to an application
    // VAddr before writing.
    VAddr address = memory_manager.PhysicalToVirtualAddress(sequence_address);

    switch (regs.query.query_get.mode) {
    case Regs::QueryMode::Write: {
        // Write the current query sequence to the sequence address.
        u32 sequence = regs.query.query_sequence;
        Memory::Write32(address, sequence);
        break;
    }
    default:
        UNIMPLEMENTED_MSG("Query mode %u not implemented",
                          static_cast<u32>(regs.query.query_get.mode.Value()));
    }
}

void Maxwell3D::DrawArrays() {
    LOG_DEBUG(HW_GPU, "called, topology=%d, count=%d", regs.draw.topology.Value(),
              regs.vertex_buffer.count);

    auto debug_context = Core::System::GetInstance().GetGPUDebugContext();

    if (debug_context) {
        debug_context->OnEvent(Tegra::DebugContext::Event::IncomingPrimitiveBatch, nullptr);
    }

    if (debug_context) {
        debug_context->OnEvent(Tegra::DebugContext::Event::FinishedPrimitiveBatch, nullptr);
    }

    VideoCore::g_renderer->Rasterizer()->AccelerateDrawBatch(false /*is_indexed*/);
}

void Maxwell3D::BindTextureInfoBuffer(const std::vector<u32>& parameters) {
    /**
     * Parameters description:
     * [0] = Shader stage, usually 4 for FragmentShader
     */

    u32 stage = parameters[0];

    // Perform the same operations as the real macro code.
    GPUVAddr address = static_cast<GPUVAddr>(regs.tex_info_buffers.address[stage]) << 8;
    u32 size = regs.tex_info_buffers.size[stage];

    regs.const_buffer.cb_size = size;
    regs.const_buffer.cb_address_high = address >> 32;
    regs.const_buffer.cb_address_low = address & 0xFFFFFFFF;
}

void Maxwell3D::SetShader(const std::vector<u32>& parameters) {
    /**
     * Parameters description:
     * [0] = Shader Program.
     * [1] = Unknown, presumably the shader id.
     * [2] = Offset to the start of the shader, after the 0x30 bytes header.
     * [3] = Shader Stage.
     * [4] = Const Buffer Address >> 8.
     */
    auto shader_program = static_cast<Regs::ShaderProgram>(parameters[0]);
    // TODO(Subv): This address is probably an offset from the CODE_ADDRESS register.
    GPUVAddr address = parameters[2];
    auto shader_stage = static_cast<Regs::ShaderStage>(parameters[3]);
    GPUVAddr cb_address = parameters[4] << 8;

    auto& shader = state.shader_programs[static_cast<size_t>(shader_program)];
    shader.program = shader_program;
    shader.stage = shader_stage;
    shader.address = address;

    // Perform the same operations as the real macro code.
    // TODO(Subv): Early exit if register 0xD1C + shader_program contains the same as params[1].
    auto& shader_regs = regs.shader_config[static_cast<size_t>(shader_program)];
    shader_regs.start_id = address;
    // TODO(Subv): Write params[1] to register 0xD1C + shader_program.
    // TODO(Subv): Write params[2] to register 0xD22 + shader_program.

    // Note: This value is hardcoded in the macro's code.
    static constexpr u32 DefaultCBSize = 0x10000;
    regs.const_buffer.cb_size = DefaultCBSize;
    regs.const_buffer.cb_address_high = cb_address >> 32;
    regs.const_buffer.cb_address_low = cb_address & 0xFFFFFFFF;

    // Write a hardcoded 0x11 to CB_BIND, this binds the current const buffer to buffer c1[] in the
    // shader. It's likely that these are the constants for the shader.
    regs.cb_bind[static_cast<size_t>(shader_stage)].valid.Assign(1);
    regs.cb_bind[static_cast<size_t>(shader_stage)].index.Assign(1);

    ProcessCBBind(shader_stage);
}

void Maxwell3D::BindStorageBuffer(const std::vector<u32>& parameters) {
    /**
     * Parameters description:
     * [0] = Buffer offset >> 2
     */

    u32 buffer_offset = parameters[0] << 2;

    // Perform the same operations as the real macro code.
    // Note: This value is hardcoded in the macro's code.
    static constexpr u32 DefaultCBSize = 0x5F00;
    regs.const_buffer.cb_size = DefaultCBSize;

    GPUVAddr address = regs.ssbo_info.BufferAddress();
    regs.const_buffer.cb_address_high = address >> 32;
    regs.const_buffer.cb_address_low = address & 0xFFFFFFFF;

    regs.const_buffer.cb_pos = buffer_offset;
}

void Maxwell3D::ProcessCBBind(Regs::ShaderStage stage) {
    // Bind the buffer currently in CB_ADDRESS to the specified index in the desired shader stage.
    auto& shader = state.shader_stages[static_cast<size_t>(stage)];
    auto& bind_data = regs.cb_bind[static_cast<size_t>(stage)];

    auto& buffer = shader.const_buffers[bind_data.index];

    buffer.enabled = bind_data.valid.Value() != 0;
    buffer.index = bind_data.index;
    buffer.address = regs.const_buffer.BufferAddress();
    buffer.size = regs.const_buffer.cb_size;
}

void Maxwell3D::ProcessCBData(u32 value) {
    // Write the input value to the current const buffer at the current position.
    GPUVAddr buffer_address = regs.const_buffer.BufferAddress();
    ASSERT(buffer_address != 0);

    // Don't allow writing past the end of the buffer.
    ASSERT(regs.const_buffer.cb_pos + sizeof(u32) <= regs.const_buffer.cb_size);

    VAddr address =
        memory_manager.PhysicalToVirtualAddress(buffer_address + regs.const_buffer.cb_pos);

    Memory::Write32(address, value);

    // Increment the current buffer position.
    regs.const_buffer.cb_pos = regs.const_buffer.cb_pos + 4;
}

Texture::TICEntry Maxwell3D::GetTICEntry(u32 tic_index) const {
    GPUVAddr tic_base_address = regs.tic.TICAddress();

    GPUVAddr tic_address_gpu = tic_base_address + tic_index * sizeof(Texture::TICEntry);
    VAddr tic_address_cpu = memory_manager.PhysicalToVirtualAddress(tic_address_gpu);

    Texture::TICEntry tic_entry;
    Memory::ReadBlock(tic_address_cpu, &tic_entry, sizeof(Texture::TICEntry));

    ASSERT_MSG(tic_entry.header_version == Texture::TICHeaderVersion::BlockLinear,
               "TIC versions other than BlockLinear are unimplemented");

    ASSERT_MSG(tic_entry.texture_type == Texture::TextureType::Texture2D,
               "Texture types other than Texture2D are unimplemented");

    auto r_type = tic_entry.r_type.Value();
    auto g_type = tic_entry.g_type.Value();
    auto b_type = tic_entry.b_type.Value();
    auto a_type = tic_entry.a_type.Value();

    // TODO(Subv): Different data types for separate components are not supported
    ASSERT(r_type == g_type && r_type == b_type && r_type == a_type);

    return tic_entry;
}

Texture::TSCEntry Maxwell3D::GetTSCEntry(u32 tsc_index) const {
    GPUVAddr tsc_base_address = regs.tsc.TSCAddress();

    GPUVAddr tsc_address_gpu = tsc_base_address + tsc_index * sizeof(Texture::TSCEntry);
    VAddr tsc_address_cpu = memory_manager.PhysicalToVirtualAddress(tsc_address_gpu);

    Texture::TSCEntry tsc_entry;
    Memory::ReadBlock(tsc_address_cpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

std::vector<Texture::FullTextureInfo> Maxwell3D::GetStageTextures(Regs::ShaderStage stage) const {
    std::vector<Texture::FullTextureInfo> textures;

    auto& fragment_shader = state.shader_stages[static_cast<size_t>(stage)];
    auto& tex_info_buffer = fragment_shader.const_buffers[regs.tex_cb_index];
    ASSERT(tex_info_buffer.enabled && tex_info_buffer.address != 0);

    GPUVAddr tic_base_address = regs.tic.TICAddress();

    GPUVAddr tex_info_buffer_end = tex_info_buffer.address + tex_info_buffer.size;

    // Offset into the texture constbuffer where the texture info begins.
    static constexpr size_t TextureInfoOffset = 0x20;

    for (GPUVAddr current_texture = tex_info_buffer.address + TextureInfoOffset;
         current_texture < tex_info_buffer_end; current_texture += sizeof(Texture::TextureHandle)) {

        Texture::TextureHandle tex_handle{
            Memory::Read32(memory_manager.PhysicalToVirtualAddress(current_texture))};

        Texture::FullTextureInfo tex_info{};
        // TODO(Subv): Use the shader to determine which textures are actually accessed.
        tex_info.index = (current_texture - tex_info_buffer.address - TextureInfoOffset) /
                         sizeof(Texture::TextureHandle);

        // Load the TIC data.
        if (tex_handle.tic_id != 0) {
            tex_info.enabled = true;

            auto tic_entry = GetTICEntry(tex_handle.tic_id);
            // TODO(Subv): Workaround for BitField's move constructor being deleted.
            std::memcpy(&tex_info.tic, &tic_entry, sizeof(tic_entry));
        }

        // Load the TSC data
        if (tex_handle.tsc_id != 0) {
            auto tsc_entry = GetTSCEntry(tex_handle.tsc_id);
            // TODO(Subv): Workaround for BitField's move constructor being deleted.
            std::memcpy(&tex_info.tsc, &tsc_entry, sizeof(tsc_entry));
        }

        if (tex_info.enabled)
            textures.push_back(tex_info);
    }

    return textures;
}

} // namespace Engines
} // namespace Tegra
