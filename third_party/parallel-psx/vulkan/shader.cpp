/* Copyright (c) 2017-2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "shader.hpp"
#include "device.hpp"
#include "spirv_layout.hpp"

#ifdef GRANITE_SPIRV_DUMP
#include "filesystem.hpp"
#endif

using namespace Util;

namespace Vulkan
{
PipelineLayout::PipelineLayout(Hash hash, Device *device, const CombinedResourceLayout &layout)
	: IntrusiveHashMapEnabled<PipelineLayout>(hash)
	, device(device)
	, layout(layout)
{
	VkDescriptorSetLayout layouts[VULKAN_NUM_DESCRIPTOR_SETS] = {};
	unsigned num_sets = 0;
	for (unsigned i = 0; i < VULKAN_NUM_DESCRIPTOR_SETS; i++)
	{
		set_allocators[i] = device->request_descriptor_set_allocator(layout.sets[i], layout.stages_for_bindings[i]);
		layouts[i] = set_allocators[i]->get_layout();
		if (layout.descriptor_set_mask & (1u << i))
			num_sets = i + 1;
	}

	VkPipelineLayoutCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	if (num_sets)
	{
		info.setLayoutCount = num_sets;
		info.pSetLayouts = layouts;
	}

	if (layout.push_constant_range.stageFlags != 0)
	{
		info.pushConstantRangeCount = 1;
		info.pPushConstantRanges = &layout.push_constant_range;
	}

	LOGI("Creating pipeline layout.\n");
	if (vkCreatePipelineLayout(device->get_device(), &info, nullptr, &pipe_layout) != VK_SUCCESS)
		LOGE("Failed to create pipeline layout.\n");
}

PipelineLayout::~PipelineLayout()
{
	if (pipe_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(device->get_device(), pipe_layout, nullptr);
}

const char *Shader::stage_to_name(ShaderStage stage)
{
	switch (stage)
	{
	case ShaderStage::Compute:
		return "compute";
	case ShaderStage::Vertex:
		return "vertex";
	case ShaderStage::Fragment:
		return "fragment";
	case ShaderStage::Geometry:
		return "geometry";
	case ShaderStage::TessControl:
		return "tess_control";
	case ShaderStage::TessEvaluation:
		return "tess_evaluation";
	default:
		return "unknown";
	}
}

static bool get_stock_sampler(StockSampler &sampler, const std::string &name)
{
	if (name.find("NearestClamp") != std::string::npos)
		sampler = StockSampler::NearestClamp;
	else if (name.find("LinearClamp") != std::string::npos)
		sampler = StockSampler::LinearClamp;
	else if (name.find("TrilinearClamp") != std::string::npos)
		sampler = StockSampler::TrilinearClamp;
	else if (name.find("NearestWrap") != std::string::npos)
		sampler = StockSampler::NearestWrap;
	else if (name.find("LinearWrap") != std::string::npos)
		sampler = StockSampler::LinearWrap;
	else if (name.find("TrilinearWrap") != std::string::npos)
		sampler = StockSampler::TrilinearWrap;
	else if (name.find("NearestShadow") != std::string::npos)
		sampler = StockSampler::NearestShadow;
	else if (name.find("LinearShadow") != std::string::npos)
		sampler = StockSampler::LinearShadow;
	else
		return false;

	return true;
}

Shader::Shader(Hash hash, Device *device, const uint32_t *data, size_t size)
	: IntrusiveHashMapEnabled<Shader>(hash)
	, device(device)
{
#ifdef GRANITE_SPIRV_DUMP
	if (!Granite::Filesystem::get().write_buffer_to_file(std::string("cache://spirv/") + std::to_string(hash) + ".spv", data, size))
		LOGE("Failed to dump shader to file.\n");
#endif

	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = size;
	info.pCode = data;

	LOGI("Creating shader module.\n");
	if (vkCreateShaderModule(device->get_device(), &info, nullptr, &module) != VK_SUCCESS)
		LOGE("Failed to create shader module.\n");

	reflect_layout(data, size / sizeof(uint32_t), layout);

}

Shader::~Shader()
{
	if (module)
		vkDestroyShaderModule(device->get_device(), module, nullptr);
}

void Program::set_shader(ShaderStage stage, Shader *handle)
{
	shaders[(unsigned)stage] = handle;
}

Program::Program(Device *device, Shader *vertex, Shader *fragment)
    : device(device)
{
	set_shader(ShaderStage::Vertex, vertex);
	set_shader(ShaderStage::Fragment, fragment);
	device->bake_program(*this);
}

Program::Program(Device *device, Shader *compute)
    : device(device)
{
	set_shader(ShaderStage::Compute, compute);
	device->bake_program(*this);
}

VkPipeline Program::get_pipeline(Hash hash) const
{
	IntrusivePODWrapper<VkPipeline> *ret = pipelines.find(hash);
	return ret ? ret->get() : VK_NULL_HANDLE;
}

VkPipeline Program::add_pipeline(Hash hash, VkPipeline pipeline)
{
	return pipelines.emplace_yield(hash, pipeline)->get();
}

Program::~Program()
{
	for (IntrusivePODWrapper<VkPipeline> &pipe : pipelines)
	{
		if (internal_sync)
			device->destroy_pipeline_nolock(pipe.get());
		else
			device->destroy_pipeline(pipe.get());
	}
}
}
