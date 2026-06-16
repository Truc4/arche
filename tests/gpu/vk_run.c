/* Minimal Vulkan compute runner — proves an arche `@gpu` map actually executes on the GPU and returns
 * the same result as the CPU path. It dispatches a compute shader with N `float[]` SSBOs (one per column,
 * binding i = column i) and a push-constant row count, then prints the readback for every buffer.
 * `make test-gpu-run` asserts GPU == CPU.
 *
 * Deliberately tiny and dependency-free (just -lvulkan): host-visible/coherent memory so there is no
 * staging copy. NOT the production runtime (that needs device-local buffers, persistent residency, async
 * submit, and per-map descriptor layouts — see docs/DECISIONS_gpu.md); it exists to verify the emission
 * path end-to-end on real hardware.
 *
 * usage: vk_run <shader.spv> <count> <nbuf> <buf0 floats...> <buf1 floats...> ...
 *        -> prints "<b0c0> <b0c1> ... | <b1c0> ..." (full precision), one space-separated group per buffer */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define VKCHECK(x)                                                                                                     \
	do {                                                                                                               \
		VkResult _r = (x);                                                                                             \
		if (_r != VK_SUCCESS) {                                                                                        \
			fprintf(stderr, "vk_run: %s failed (%d)\n", #x, _r);                                                       \
			return 2;                                                                                                  \
		}                                                                                                              \
	} while (0)

#define MAX_BUF 16

static uint32_t *read_spv(const char *path, size_t *out_bytes) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *buf = malloc((size_t)n);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	*out_bytes = (size_t)n;
	return buf;
}

static uint32_t find_mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: vk_run <shader.spv> <count> <nbuf> <floats...>\n");
		return 1;
	}
	const char *spv_path = argv[1];
	uint32_t count = (uint32_t)atoi(argv[2]);
	uint32_t nbuf = (uint32_t)atoi(argv[3]);
	if (nbuf == 0 || nbuf > MAX_BUF) {
		fprintf(stderr, "vk_run: nbuf must be 1..%d\n", MAX_BUF);
		return 1;
	}
	if ((uint32_t)(argc - 4) != count * nbuf) {
		fprintf(stderr, "vk_run: expected %u floats (%u bufs x %u), got %d\n", count * nbuf, nbuf, count, argc - 4);
		return 1;
	}
	VkDeviceSize bytes = sizeof(float) * count;
	float *host[MAX_BUF];
	for (uint32_t b = 0; b < nbuf; b++) {
		host[b] = malloc(bytes);
		for (uint32_t i = 0; i < count; i++)
			host[b][i] = (float)atof(argv[4 + b * count + i]);
	}

	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	VkInstance inst;
	VKCHECK(vkCreateInstance(&ici, NULL, &inst));

	uint32_t npd = 0;
	vkEnumeratePhysicalDevices(inst, &npd, NULL);
	if (npd == 0) {
		fprintf(stderr, "vk_run: no Vulkan device\n");
		return 2;
	}
	VkPhysicalDevice *pds = malloc(sizeof(*pds) * npd);
	vkEnumeratePhysicalDevices(inst, &npd, pds);
	VkPhysicalDevice pd = pds[0];
	uint32_t qfam = 0, nqf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
	VkQueueFamilyProperties *qfs = malloc(sizeof(*qfs) * nqf);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfs);
	int found_q = 0;
	for (uint32_t i = 0; i < nqf; i++)
		if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
			qfam = i;
			found_q = 1;
			break;
		}
	if (!found_q) {
		fprintf(stderr, "vk_run: no compute queue\n");
		return 2;
	}

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	                               .queueFamilyIndex = qfam,
	                               .queueCount = 1,
	                               .pQueuePriorities = &prio};
	VkDeviceCreateInfo dci = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
	VkDevice dev;
	VKCHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue queue;
	vkGetDeviceQueue(dev, qfam, 0, &queue);

	/* one host-visible storage buffer per column, seeded with its inputs */
	VkBuffer buf[MAX_BUF];
	VkDeviceMemory mem[MAX_BUF];
	void *mapped[MAX_BUF];
	for (uint32_t b = 0; b < nbuf; b++) {
		VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		                          .size = bytes,
		                          .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		                          .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
		VKCHECK(vkCreateBuffer(dev, &bci, NULL, &buf[b]));
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(dev, buf[b], &mr);
		uint32_t mt = find_mem_type(pd, mr.memoryTypeBits,
		                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VkMemoryAllocateInfo mai = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
		VKCHECK(vkAllocateMemory(dev, &mai, NULL, &mem[b]));
		VKCHECK(vkBindBufferMemory(dev, buf[b], mem[b], 0));
		VKCHECK(vkMapMemory(dev, mem[b], 0, bytes, 0, &mapped[b]));
		memcpy(mapped[b], host[b], bytes);
	}

	/* descriptor set layout: binding b = storage buffer (column b) */
	VkDescriptorSetLayoutBinding dslb[MAX_BUF];
	for (uint32_t b = 0; b < nbuf; b++)
		dslb[b] = (VkDescriptorSetLayoutBinding){.binding = b,
		                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		                                         .descriptorCount = 1,
		                                         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
	VkDescriptorSetLayoutCreateInfo dslci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nbuf, .pBindings = dslb};
	VkDescriptorSetLayout dsl;
	VKCHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

	VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(uint32_t)};
	VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	                                   .setLayoutCount = 1,
	                                   .pSetLayouts = &dsl,
	                                   .pushConstantRangeCount = 1,
	                                   .pPushConstantRanges = &pcr};
	VkPipelineLayout playout;
	VKCHECK(vkCreatePipelineLayout(dev, &plci, NULL, &playout));

	size_t spv_bytes = 0;
	uint32_t *spv = read_spv(spv_path, &spv_bytes);
	if (!spv) {
		fprintf(stderr, "vk_run: cannot read %s\n", spv_path);
		return 1;
	}
	VkShaderModuleCreateInfo smci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_bytes, .pCode = spv};
	VkShaderModule sm;
	VKCHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));
	VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	                                    .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	                                              .stage = VK_SHADER_STAGE_COMPUTE_BIT,
	                                              .module = sm,
	                                              .pName = "main"},
	                                    .layout = playout};
	VkPipeline pipe;
	VKCHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

	VkDescriptorPoolSize dps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = nbuf};
	VkDescriptorPoolCreateInfo dpci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps};
	VkDescriptorPool dpool;
	VKCHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
	VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	                                    .descriptorPool = dpool,
	                                    .descriptorSetCount = 1,
	                                    .pSetLayouts = &dsl};
	VkDescriptorSet dset;
	VKCHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));
	VkDescriptorBufferInfo dbi[MAX_BUF];
	VkWriteDescriptorSet wds[MAX_BUF];
	for (uint32_t b = 0; b < nbuf; b++) {
		dbi[b] = (VkDescriptorBufferInfo){.buffer = buf[b], .offset = 0, .range = bytes};
		wds[b] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		                                .dstSet = dset,
		                                .dstBinding = b,
		                                .descriptorCount = 1,
		                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		                                .pBufferInfo = &dbi[b]};
	}
	vkUpdateDescriptorSets(dev, nbuf, wds, 0, NULL);

	VkCommandPoolCreateInfo cpci2 = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam};
	VkCommandPool cpool;
	VKCHECK(vkCreateCommandPool(dev, &cpci2, NULL, &cpool));
	VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                    .commandPool = cpool,
	                                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                    .commandBufferCount = 1};
	VkCommandBuffer cmd;
	VKCHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
	VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VKCHECK(vkBeginCommandBuffer(cmd, &cbbi));
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, NULL);
	vkCmdPushConstants(cmd, playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &count);
	vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
	VKCHECK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
	VKCHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	VKCHECK(vkQueueWaitIdle(queue));

	/* readback (host-coherent — no invalidate needed); full precision, buffers separated by " | " */
	for (uint32_t b = 0; b < nbuf; b++) {
		float *out = (float *)mapped[b];
		for (uint32_t i = 0; i < count; i++)
			printf("%g%s", out[i], i + 1 < count ? " " : "");
		printf("%s", b + 1 < nbuf ? " | " : "\n");
	}

	for (uint32_t b = 0; b < nbuf; b++)
		free(host[b]);
	free(spv);
	return 0;
}
