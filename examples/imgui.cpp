#include "utils.hpp"
#include "vuk/AllocatorHelpers.hpp"
#include "vuk/CommandBuffer.hpp"
#include "vuk/Context.hpp"
#include "vuk/Partials.hpp"
#include "vuk/RenderGraph.hpp"
#include "vuk/SampledImage.hpp"

util::ImGuiData util::ImGui_ImplVuk_Init(vuk::Allocator& allocator) {
	vuk::Context& ctx = allocator.get_context();
	auto& io = ImGui::GetIO();
	io.BackendRendererName = "imgui_impl_vuk";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.

	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

	ImGuiData data;
	auto [tex, stub] = create_texture(allocator, vuk::Format::eR8G8B8A8Srgb, vuk::Extent3D{ (unsigned)width, (unsigned)height, 1u }, pixels, false);
	data.font_texture = std::move(tex);
	stub.get();
	ctx.debug.set_name(data.font_texture, "ImGui/font");
	vuk::SamplerCreateInfo sci;
	sci.minFilter = sci.magFilter = vuk::Filter::eLinear;
	sci.mipmapMode = vuk::SamplerMipmapMode::eLinear;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = vuk::SamplerAddressMode::eRepeat;
	data.font_sci = sci;
	data.font_si = std::make_unique<vuk::SampledImage>(vuk::SampledImage::Global{ *data.font_texture.view, sci, vuk::ImageLayout::eShaderReadOnlyOptimal });
	io.Fonts->TexID = (ImTextureID)data.font_si.get();
	{
		vuk::PipelineBaseCreateInfo pci;
		auto vpath = "../../examples/imgui.vert.spv";
		auto vcont = util::read_spirv(vpath);
		pci.add_spirv(std::move(vcont), vpath);
		auto fpath = "../../examples/imgui.frag.spv";
		auto fcont = util::read_spirv(fpath);
		pci.add_spirv(std::move(fcont), fpath);
		ctx.create_named_pipeline("imgui", pci);
	}
	return data;
}

void util::ImGui_ImplVuk_Render(vuk::Allocator& allocator,
                                vuk::RenderGraph& rg,
                                vuk::Name src_target,
                                vuk::Name dst_target,
                                util::ImGuiData& data,
                                ImDrawData* draw_data,
                                const plf::colony<vuk::SampledImage>& sampled_images) {
	auto reset_render_state = [](const util::ImGuiData& data, vuk::CommandBuffer& command_buffer, ImDrawData* draw_data, vuk::Buffer vertex, vuk::Buffer index) {
		command_buffer.bind_image(0, 0, *data.font_texture.view).bind_sampler(0, 0, data.font_sci);
		if (index.size > 0) {
			command_buffer.bind_index_buffer(index, sizeof(ImDrawIdx) == 2 ? vuk::IndexType::eUint16 : vuk::IndexType::eUint32);
		}
		command_buffer.bind_vertex_buffer(0, vertex, 0, vuk::Packed{ vuk::Format::eR32G32Sfloat, vuk::Format::eR32G32Sfloat, vuk::Format::eR8G8B8A8Unorm });
		command_buffer.bind_graphics_pipeline("imgui");
		command_buffer.set_viewport(0, vuk::Rect2D::framebuffer());
		struct PC {
			float scale[2];
			float translate[2];
		} pc;
		pc.scale[0] = 2.0f / draw_data->DisplaySize.x;
		pc.scale[1] = 2.0f / draw_data->DisplaySize.y;
		pc.translate[0] = -1.0f - draw_data->DisplayPos.x * pc.scale[0];
		pc.translate[1] = -1.0f - draw_data->DisplayPos.y * pc.scale[1];
		command_buffer.push_constants(vuk::ShaderStageFlagBits::eVertex, 0, pc);
	};

	size_t vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
	size_t index_size = draw_data->TotalIdxCount * sizeof(ImDrawIdx);
	auto imvert = *allocate_buffer_cross_device(allocator, { vuk::MemoryUsage::eCPUtoGPU, vertex_size, 1 });
	auto imind = *allocate_buffer_cross_device(allocator, { vuk::MemoryUsage::eCPUtoGPU, index_size, 1 });

	size_t vtx_dst = 0, idx_dst = 0;
	for (int n = 0; n < draw_data->CmdListsCount; n++) {
		const ImDrawList* cmd_list = draw_data->CmdLists[n];
		auto imverto = imvert->add_offset(vtx_dst * sizeof(ImDrawVert));
		auto imindo = imind->add_offset(idx_dst * sizeof(ImDrawIdx));

		vuk::host_data_to_buffer(allocator, vuk::DomainFlagBits{}, imverto, std::span(cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size)).get();
		vuk::host_data_to_buffer(allocator, vuk::DomainFlagBits{}, imindo, std::span(cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size)).get();
		vtx_dst += cmd_list->VtxBuffer.Size;
		idx_dst += cmd_list->IdxBuffer.Size;
	}

	// add rendergraph dependencies to be transitioned
	// make all rendergraph sampled images available
	std::vector<vuk::Resource> resources;
	resources.emplace_back(vuk::Resource{ src_target, vuk::Resource::Type::eImage, vuk::eColorRW, dst_target });
	for (auto& si : sampled_images) {
		if (!si.is_global) {
			resources.emplace_back(vuk::Resource{ si.rg_attachment.attachment_name, vuk::Resource::Type::eImage, vuk::Access::eFragmentSampled });
		}
	}
	vuk::Pass pass{ .name = "imgui",
		              .resources = std::move(resources),
		              .execute = [&data, &allocator, verts = imvert.get(), inds = imind.get(), draw_data, reset_render_state, src_target](
		                             vuk::CommandBuffer& command_buffer) {
		                command_buffer.set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor);
		                command_buffer.set_rasterization(vuk::PipelineRasterizationStateCreateInfo{});
		                command_buffer.set_color_blend(src_target, vuk::BlendPreset::eAlphaBlend);
		                reset_render_state(data, command_buffer, draw_data, verts, inds);
		                // Will project scissor/clipping rectangles into framebuffer space
		                ImVec2 clip_off = draw_data->DisplayPos;         // (0,0) unless using multi-viewports
		                ImVec2 clip_scale = draw_data->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

		                // Render command lists
		                // (Because we merged all buffers into a single one, we maintain our own offset into them)
		                int global_vtx_offset = 0;
		                int global_idx_offset = 0;
		                for (int n = 0; n < draw_data->CmdListsCount; n++) {
			                const ImDrawList* cmd_list = draw_data->CmdLists[n];
			                for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
				                const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
				                if (pcmd->UserCallback != nullptr) {
					                // User callback, registered via ImDrawList::AddCallback()
					                // (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
					                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
						                reset_render_state(data, command_buffer, draw_data, verts, inds);
					                else
						                pcmd->UserCallback(cmd_list, pcmd);
				                } else {
					                // Project scissor/clipping rectangles into framebuffer space
					                ImVec4 clip_rect;
					                clip_rect.x = (pcmd->ClipRect.x - clip_off.x) * clip_scale.x;
					                clip_rect.y = (pcmd->ClipRect.y - clip_off.y) * clip_scale.y;
					                clip_rect.z = (pcmd->ClipRect.z - clip_off.x) * clip_scale.x;
					                clip_rect.w = (pcmd->ClipRect.w - clip_off.y) * clip_scale.y;

					                auto fb_width = command_buffer.get_ongoing_renderpass().extent.width;
					                auto fb_height = command_buffer.get_ongoing_renderpass().extent.height;
					                if (clip_rect.x < fb_width && clip_rect.y < fb_height && clip_rect.z >= 0.0f && clip_rect.w >= 0.0f) {
						                // Negative offsets are illegal for vkCmdSetScissor
						                if (clip_rect.x < 0.0f)
							                clip_rect.x = 0.0f;
						                if (clip_rect.y < 0.0f)
							                clip_rect.y = 0.0f;

						                // Apply scissor/clipping rectangle
						                vuk::Rect2D scissor;
						                scissor.offset.x = (int32_t)(clip_rect.x);
						                scissor.offset.y = (int32_t)(clip_rect.y);
						                scissor.extent.width = (uint32_t)(clip_rect.z - clip_rect.x);
						                scissor.extent.height = (uint32_t)(clip_rect.w - clip_rect.y);
						                command_buffer.set_scissor(0, scissor);

						                // Bind texture
						                if (pcmd->TextureId) {
							                auto& si = *reinterpret_cast<vuk::SampledImage*>(pcmd->TextureId);
							                if (si.is_global) {
								                command_buffer.bind_image(0, 0, si.global.iv).bind_sampler(0, 0, si.global.sci);
							                } else {
								                if (si.rg_attachment.ivci) {
									                auto ivci = *si.rg_attachment.ivci;
									                auto res_img = command_buffer.get_resource_image(si.rg_attachment.attachment_name);
									                ivci.image = *res_img;
									                auto iv = vuk::allocate_image_view(allocator, ivci);
									                command_buffer.bind_image(0, 0, **iv).bind_sampler(0, 0, si.rg_attachment.sci);
								                } else {
									                command_buffer.bind_image(0, 0, si.rg_attachment.attachment_name).bind_sampler(0, 0, si.rg_attachment.sci);
								                }
							                }
						                }
						                // Draw
						                command_buffer.draw_indexed(pcmd->ElemCount, 1, pcmd->IdxOffset + global_idx_offset, pcmd->VtxOffset + global_vtx_offset, 0);
					                }
				                }
			                }
			                global_idx_offset += cmd_list->IdxBuffer.Size;
			                global_vtx_offset += cmd_list->VtxBuffer.Size;
		                }
		              } };

	rg.add_pass(std::move(pass));
}
