#include "mantle_internal.h"
#include "amdilc.h"

typedef struct _Stage {
    const GR_PIPELINE_SHADER* shader;
    const VkShaderStageFlagBits flags;
} Stage;

static void copyDescriptorSetMapping(
    GR_DESCRIPTOR_SET_MAPPING* dst,
    const GR_DESCRIPTOR_SET_MAPPING* src);

static void copyDescriptorSlotInfo(
    GR_DESCRIPTOR_SLOT_INFO* dst,
    const GR_DESCRIPTOR_SLOT_INFO* src)
{
    dst->slotObjectType = src->slotObjectType;
    if (src->slotObjectType == GR_SLOT_NEXT_DESCRIPTOR_SET) {
        // Go down one level...
        dst->pNextLevelSet = malloc(sizeof(GR_DESCRIPTOR_SET_MAPPING));
        copyDescriptorSetMapping((GR_DESCRIPTOR_SET_MAPPING*)dst->pNextLevelSet,
                                 src->pNextLevelSet);
    } else {
        dst->shaderEntityIndex = src->shaderEntityIndex;
    }
}

static void copyDescriptorSetMapping(
    GR_DESCRIPTOR_SET_MAPPING* dst,
    const GR_DESCRIPTOR_SET_MAPPING* src)
{
    dst->descriptorCount = src->descriptorCount;
    dst->pDescriptorInfo = malloc(src->descriptorCount * sizeof(GR_DESCRIPTOR_SLOT_INFO));
    for (unsigned i = 0; i < src->descriptorCount; i++) {
        copyDescriptorSlotInfo((GR_DESCRIPTOR_SLOT_INFO*)&dst->pDescriptorInfo[i],
                               &src->pDescriptorInfo[i]);
    }
}

static void copyPipelineShader(
    GR_PIPELINE_SHADER* dst,
    const GR_PIPELINE_SHADER* src)
{
    dst->shader = src->shader;
    for (unsigned i = 0; i < COUNT_OF(dst->descriptorSetMapping); i++) {
        copyDescriptorSetMapping(&dst->descriptorSetMapping[i], &src->descriptorSetMapping[i]);
    }
    dst->linkConstBufferCount = 0; // Ignored
    dst->pLinkConstBufferInfo = NULL; // Ignored
    dst->dynamicMemoryViewMapping = src->dynamicMemoryViewMapping;
}

static VkDescriptorSetLayout getVkDescriptorSetLayout(
    const GrDevice* grDevice,
    const Stage* stage)
{
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    unsigned bindingCount = 0;
    VkDescriptorSetLayoutBinding* bindings = NULL;

    if (stage->shader->shader != GR_NULL_HANDLE) {
        const GrShader* grShader = stage->shader->shader;

        bindingCount = grShader->bindingCount;
        bindings = malloc(bindingCount * sizeof(VkDescriptorSetLayoutBinding));

        for (unsigned i = 0; i < grShader->bindingCount; i++) {
            const IlcBinding* binding = &grShader->bindings[i];

            bindings[i] = (VkDescriptorSetLayoutBinding) {
                .binding = binding->index,
                .descriptorType = binding->descriptorType,
                .descriptorCount = 1,
                .stageFlags = stage->flags,
                .pImmutableSamplers = NULL,
            };
        }
    }

    const VkDescriptorSetLayoutCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .bindingCount = bindingCount,
        .pBindings = bindings,
    };

    VkResult res = VKD.vkCreateDescriptorSetLayout(grDevice->device, &createInfo, NULL, &layout);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateDescriptorSetLayout failed (%d)\n", res);
    }

    free(bindings);
    return layout;
}

static VkPipelineLayout getVkPipelineLayout(
    const GrDevice* grDevice,
    unsigned stageCount,
    const Stage* stages,
    const VkDescriptorSetLayout* descriptorSetLayouts)
{
    VkPipelineLayout layout = VK_NULL_HANDLE;

    const VkPipelineLayoutCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .setLayoutCount = stageCount,
        .pSetLayouts = descriptorSetLayouts,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = NULL,
    };

    VkResult res = VKD.vkCreatePipelineLayout(grDevice->device, &createInfo, NULL, &layout);
    if (res != VK_SUCCESS) {
        LOGE("vkCreatePipelineLayout failed (%d)\n", res);
    }

    return layout;
}

static void updateDescriptorTypeCounts(
    unsigned descriptorTypeCountSize,
    unsigned* descriptorTypeCounts,
    unsigned stageCount,
    const Stage* stages)
{
    // Count descriptor types from shader bindings in all stages
    for (unsigned i = 0; i < stageCount; i++) {
        const GrShader* grShader = (GrShader*)stages[i].shader->shader;

        if (grShader != NULL) {
            for (unsigned j = 0; j < grShader->bindingCount; j++) {
                const IlcBinding* binding = &grShader->bindings[j];

                if (binding->descriptorType >= descriptorTypeCountSize) {
                    LOGE("unexpected descriptor type %d\n", binding->descriptorType);
                    assert(false);
                }

                descriptorTypeCounts[binding->descriptorType]++;
            }
        }
    }
}

static VkRenderPass getVkRenderPass(
    const GrDevice* grDevice,
    const GR_PIPELINE_CB_TARGET_STATE* cbTargets,
    const GR_PIPELINE_DB_STATE* dbTarget)
{
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkAttachmentDescription descriptions[GR_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference colorReferences[GR_MAX_COLOR_TARGETS];
    VkAttachmentReference depthStencilReference;
    unsigned descriptionCount = 0;
    unsigned colorReferenceCount = 0;
    bool hasDepthStencil = false;

    for (int i = 0; i < GR_MAX_COLOR_TARGETS; i++) {
        const GR_PIPELINE_CB_TARGET_STATE* target = &cbTargets[i];
        VkFormat vkFormat = getVkFormat(target->format);

        if (vkFormat == VK_FORMAT_UNDEFINED) {
            continue;
        }

        descriptions[descriptionCount] = (VkAttachmentDescription) {
            .flags = 0,
            .format = vkFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = (target->channelWriteMask & 0xF) != 0 ?
                       VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
        };

        colorReferences[colorReferenceCount] = (VkAttachmentReference) {
            .attachment = descriptionCount,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        };

        descriptionCount++;
        colorReferenceCount++;
    }

    VkFormat dbVkFormat = getVkFormat(dbTarget->format);
    if (dbVkFormat != VK_FORMAT_UNDEFINED) {
        // Table 10 in the API reference
        bool hasDepth = dbTarget->format.channelFormat == GR_CH_FMT_R16 ||
                        dbTarget->format.channelFormat == GR_CH_FMT_R32 ||
                        dbTarget->format.channelFormat == GR_CH_FMT_R16G8 ||
                        dbTarget->format.channelFormat == GR_CH_FMT_R32G8;
        bool hasStencil = dbTarget->format.channelFormat == GR_CH_FMT_R8 ||
                          dbTarget->format.channelFormat == GR_CH_FMT_R16G8 ||
                          dbTarget->format.channelFormat == GR_CH_FMT_R32G8;

        descriptions[descriptionCount] = (VkAttachmentDescription) {
            .flags = 0,
            .format = dbVkFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = hasDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = hasDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = hasStencil ?
                             VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = hasStencil ?
                             VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
        };

        depthStencilReference = (VkAttachmentReference) {
            .attachment = descriptionCount,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        };

        descriptionCount++;
        hasDepthStencil = true;
    }

    const VkSubpassDescription subpass = {
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = NULL,
        .colorAttachmentCount = colorReferenceCount,
        .pColorAttachments = colorReferences,
        .pResolveAttachments = NULL,
        .pDepthStencilAttachment = hasDepthStencil ? &depthStencilReference : NULL,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = NULL,
    };

    const VkRenderPassCreateInfo renderPassCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .attachmentCount = descriptionCount,
        .pAttachments = descriptions,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = NULL,
    };

    VkResult res = VKD.vkCreateRenderPass(grDevice->device, &renderPassCreateInfo, NULL,
                                          &renderPass);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateRenderPass failed (%d)\n", res);
        return VK_NULL_HANDLE;
    }

    return renderPass;
}

static VkPipeline getVkPipeline(
    const GrPipeline* grPipeline,
    const GrColorBlendStateObject* grColorBlendState,
    const GrRasterStateObject* grRasterState)
{
    const GrDevice* grDevice = GET_OBJ_DEVICE(grPipeline);
    const PipelineCreateInfo* createInfo = grPipeline->createInfo;
    VkPipeline vkPipeline = VK_NULL_HANDLE;
    VkResult vkRes;

    const VkPipelineVertexInputStateCreateInfo vertexInputStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = NULL,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = NULL,
    };

    const VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .topology = createInfo->topology,
        .primitiveRestartEnable = VK_FALSE,
    };

    // Ignored if no tessellation shaders are present
    const VkPipelineTessellationStateCreateInfo tessellationStateCreateInfo =  {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .patchControlPoints = createInfo->patchControlPoints,
    };

    const VkPipelineViewportStateCreateInfo viewportStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .viewportCount = 0, // Dynamic state
        .pViewports = NULL, // Dynamic state
        .scissorCount = 0, // Dynamic state
        .pScissors = NULL, // Dynamic state
    };

    const VkPipelineRasterizationDepthClipStateCreateInfoEXT depthClipStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT,
        .pNext = NULL,
        .flags = 0,
        .depthClipEnable = createInfo->depthClipEnable,
    };

    const VkPipelineRasterizationStateCreateInfo rasterizationStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = &depthClipStateCreateInfo,
        .flags = 0,
        .depthClampEnable = VK_TRUE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = grRasterState->polygonMode,
        .cullMode = 0, // Dynamic state
        .frontFace = 0, // Dynamic state
        .depthBiasEnable = VK_TRUE,
        .depthBiasConstantFactor = 0.f, // Dynamic state
        .depthBiasClamp = 0.f, // Dynamic state
        .depthBiasSlopeFactor = 0.f, // Dynamic state
        .lineWidth = 1.f,
    };

    const VkPipelineMultisampleStateCreateInfo msaaStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, // TODO implement MSAA
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 0.f,
        .pSampleMask = NULL, // TODO implement MSAA
        .alphaToCoverageEnable = createInfo->alphaToCoverageEnable,
        .alphaToOneEnable = VK_FALSE,
    };

    const VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .depthTestEnable = 0, // Dynamic state
        .depthWriteEnable = 0, // Dynamic state
        .depthCompareOp = 0, // Dynamic state
        .depthBoundsTestEnable = 0, // Dynamic state
        .stencilTestEnable = 0, // Dynamic state
        .front = { 0 }, // Dynamic state
        .back = { 0 }, // Dynamic state
        .minDepthBounds = 0.f, // Dynamic state
        .maxDepthBounds = 0.f, // Dynamic state
    };

    unsigned attachmentCount = 0;
    VkPipelineColorBlendAttachmentState attachments[GR_MAX_COLOR_TARGETS];

    for (unsigned i = 0; i < GR_MAX_COLOR_TARGETS; i++) {
        const VkPipelineColorBlendAttachmentState* blendState = &grColorBlendState->states[i];
        VkColorComponentFlags colorWriteMask = createInfo->colorWriteMasks[i];

        if (colorWriteMask == ~0u) {
            continue;
        }

        attachments[attachmentCount] = (VkPipelineColorBlendAttachmentState) {
            .blendEnable = blendState->blendEnable,
            .srcColorBlendFactor = blendState->srcColorBlendFactor,
            .dstColorBlendFactor = blendState->dstColorBlendFactor,
            .colorBlendOp = blendState->colorBlendOp,
            .srcAlphaBlendFactor = blendState->srcAlphaBlendFactor,
            .dstAlphaBlendFactor = blendState->dstAlphaBlendFactor,
            .alphaBlendOp = blendState->alphaBlendOp,
            .colorWriteMask = colorWriteMask,
        };
        attachmentCount++;
    }

    const VkPipelineColorBlendStateCreateInfo colorBlendStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .logicOpEnable = createInfo->logicOpEnable,
        .logicOp = createInfo->logicOp,
        .attachmentCount = attachmentCount,
        .pAttachments = attachments,
        .blendConstants = { 0.f }, // Dynamic state
    };

    const VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE_EXT,
        VK_DYNAMIC_STATE_FRONT_FACE_EXT,
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT,
        VK_DYNAMIC_STATE_STENCIL_OP_EXT,
    };

    const VkPipelineDynamicStateCreateInfo dynamicStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .dynamicStateCount = COUNT_OF(dynamicStates),
        .pDynamicStates = dynamicStates,
    };

    const VkGraphicsPipelineCreateInfo pipelineCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = NULL,
        .flags = createInfo->createFlags,
        .stageCount = createInfo->stageCount,
        .pStages = createInfo->stageCreateInfos,
        .pVertexInputState = &vertexInputStateCreateInfo,
        .pInputAssemblyState = &inputAssemblyStateCreateInfo,
        .pTessellationState = &tessellationStateCreateInfo,
        .pViewportState = &viewportStateCreateInfo,
        .pRasterizationState = &rasterizationStateCreateInfo,
        .pMultisampleState = &msaaStateCreateInfo,
        .pDepthStencilState = &depthStencilStateCreateInfo,
        .pColorBlendState = &colorBlendStateCreateInfo,
        .pDynamicState = &dynamicStateCreateInfo,
        .layout = grPipeline->pipelineLayout,
        .renderPass = grPipeline->renderPass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };

    vkRes = VKD.vkCreateGraphicsPipelines(grDevice->device, VK_NULL_HANDLE, 1, &pipelineCreateInfo,
                                          NULL, &vkPipeline);
    if (vkRes != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed (%d)\n", vkRes);
    }

    return vkPipeline;
}

// Exported Functions

VkPipeline grPipelineFindOrCreateVkPipeline(
    GrPipeline* grPipeline,
    const GrColorBlendStateObject* grColorBlendState,
    const GrRasterStateObject* grRasterState)
{
    VkPipeline vkPipeline = VK_NULL_HANDLE;

    EnterCriticalSection(&grPipeline->pipelineSlotsMutex);

    for (unsigned i = 0; i < grPipeline->pipelineSlotCount; i++) {
        const PipelineSlot* slot = &grPipeline->pipelineSlots[i];

        if (grColorBlendState == slot->grColorBlendState &&
            grRasterState == slot->grRasterState) {
            vkPipeline = slot->pipeline;
            break;
        }
    }

    if (vkPipeline == VK_NULL_HANDLE) {
        vkPipeline = getVkPipeline(grPipeline, grColorBlendState, grRasterState);

        grPipeline->pipelineSlotCount++;
        grPipeline->pipelineSlots = realloc(grPipeline->pipelineSlots,
                                            grPipeline->pipelineSlotCount * sizeof(PipelineSlot));
        grPipeline->pipelineSlots[grPipeline->pipelineSlotCount - 1] = (PipelineSlot) {
            .pipeline = vkPipeline,
            .grColorBlendState = grColorBlendState,
            .grRasterState = grRasterState,
        };
    }

    LeaveCriticalSection(&grPipeline->pipelineSlotsMutex);

    return vkPipeline;
}

// Shader and Pipeline Functions

GR_RESULT grCreateShader(
    GR_DEVICE device,
    const GR_SHADER_CREATE_INFO* pCreateInfo,
    GR_SHADER* pShader)
{
    LOGT("%p %p %p\n", device, pCreateInfo, pShader);
    GrDevice* grDevice = (GrDevice*)device;
    VkShaderModule vkShaderModule = VK_NULL_HANDLE;

    if ((pCreateInfo->flags & GR_SHADER_CREATE_ALLOW_RE_Z) != 0) {
        LOGW("unhandled Re-Z flag\n");
    }

    IlcShader ilcShader = ilcCompileShader(pCreateInfo->pCode, pCreateInfo->codeSize);

    const VkShaderModuleCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .codeSize = ilcShader.codeSize,
        .pCode = ilcShader.code,
    };

    VkResult res = VKD.vkCreateShaderModule(grDevice->device, &createInfo, NULL, &vkShaderModule);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateShaderModule failed (%d)\n", res);
        free(ilcShader.code);
        return getGrResult(res);
    }

    free(ilcShader.code);

    GrShader* grShader = malloc(sizeof(GrShader));
    *grShader = (GrShader) {
        .grObj = { GR_OBJ_TYPE_SHADER, grDevice },
        .shaderModule = vkShaderModule,
        .bindingCount = ilcShader.bindingCount,
        .bindings = ilcShader.bindings,
    };

    *pShader = (GR_SHADER)grShader;
    return GR_SUCCESS;
}

GR_RESULT grCreateGraphicsPipeline(
    GR_DEVICE device,
    const GR_GRAPHICS_PIPELINE_CREATE_INFO* pCreateInfo,
    GR_PIPELINE* pPipeline)
{
    LOGT("%p %p %p\n", device, pCreateInfo, pPipeline);
    GrDevice* grDevice = (GrDevice*)device;
    GR_RESULT res = GR_SUCCESS;
    VkDescriptorSetLayout descriptorSetLayouts[MAX_STAGE_COUNT] = { VK_NULL_HANDLE };
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;

    // TODO validate parameters

    // Ignored parameters:
    // - iaState.disableVertexReuse (hint)
    // - tessState.optimalTessFactor (hint)

    Stage stages[MAX_STAGE_COUNT] = {
        { &pCreateInfo->vs, VK_SHADER_STAGE_VERTEX_BIT },
        { &pCreateInfo->hs, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT },
        { &pCreateInfo->ds, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT },
        { &pCreateInfo->gs, VK_SHADER_STAGE_GEOMETRY_BIT },
        { &pCreateInfo->ps, VK_SHADER_STAGE_FRAGMENT_BIT },
    };

    unsigned stageCount = 0;
    VkPipelineShaderStageCreateInfo shaderStageCreateInfo[COUNT_OF(stages)];

    for (int i = 0; i < COUNT_OF(stages); i++) {
        Stage* stage = &stages[i];

        if (stage->shader->shader == GR_NULL_HANDLE) {
            continue;
        }

        if (stage->shader->linkConstBufferCount > 0) {
            // TODO implement
            LOGW("link-time constant buffers are not implemented\n");
        }

        GrShader* grShader = (GrShader*)stage->shader->shader;

        shaderStageCreateInfo[stageCount] = (VkPipelineShaderStageCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = NULL,
            .flags = 0,
            .stage = stage->flags,
            .module = grShader->shaderModule,
            .pName = "main",
            .pSpecializationInfo = NULL,
        };

        stageCount++;
    }

    // TODO implement
    if (pCreateInfo->cbState.dualSourceBlendEnable) {
        LOGW("dual source blend is not implemented\n");
    }

    unsigned attachmentCount = 0;
    VkColorComponentFlags colorWriteMasks[GR_MAX_COLOR_TARGETS];

    for (int i = 0; i < GR_MAX_COLOR_TARGETS; i++) {
        const GR_PIPELINE_CB_TARGET_STATE* target = &pCreateInfo->cbState.target[i];

        if (!target->blendEnable &&
            target->format.channelFormat == GR_CH_FMT_UNDEFINED &&
            target->format.numericFormat == GR_NUM_FMT_UNDEFINED &&
            target->channelWriteMask == 0) {
            colorWriteMasks[attachmentCount] = ~0u;
        } else {
            colorWriteMasks[attachmentCount] = getVkColorComponentFlags(target->channelWriteMask);
        }

        attachmentCount++;
    }

    PipelineCreateInfo* pipelineCreateInfo = malloc(sizeof(PipelineCreateInfo));
    *pipelineCreateInfo = (PipelineCreateInfo) {
        .createFlags = (pCreateInfo->flags & GR_PIPELINE_CREATE_DISABLE_OPTIMIZATION) != 0 ?
                       VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0,
        .stageCount = stageCount,
        .stageCreateInfos = { { 0 } }, // Initialized below
        .topology = getVkPrimitiveTopology(pCreateInfo->iaState.topology),
        .patchControlPoints = pCreateInfo->tessState.patchControlPoints,
        .depthClipEnable = !!pCreateInfo->rsState.depthClipEnable,
        .alphaToCoverageEnable = !!pCreateInfo->cbState.alphaToCoverageEnable,
        .logicOpEnable = pCreateInfo->cbState.logicOp != GR_LOGIC_OP_COPY,
        .logicOp = getVkLogicOp(pCreateInfo->cbState.logicOp),
        .colorWriteMasks = { 0 }, // Initialized below
    };

    memcpy(pipelineCreateInfo->stageCreateInfos, shaderStageCreateInfo,
           stageCount * sizeof(VkPipelineShaderStageCreateInfo));
    memcpy(pipelineCreateInfo->colorWriteMasks, colorWriteMasks,
           GR_MAX_COLOR_TARGETS * sizeof(VkColorComponentFlags));

    // Create one descriptor set layout per stage
    for (unsigned i = 0; i < COUNT_OF(stages); i++) {
        descriptorSetLayouts[i] = getVkDescriptorSetLayout(grDevice, &stages[i]);
        if (descriptorSetLayouts[i] == VK_NULL_HANDLE) {
            res = GR_ERROR_OUT_OF_MEMORY;
            goto bail;
        }
    }

    pipelineLayout = getVkPipelineLayout(grDevice, COUNT_OF(stages), stages, descriptorSetLayouts);
    if (pipelineLayout == VK_NULL_HANDLE) {
        res = GR_ERROR_OUT_OF_MEMORY;
        goto bail;
    }

    renderPass = getVkRenderPass(grDevice, pCreateInfo->cbState.target, &pCreateInfo->dbState);
    if (renderPass == VK_NULL_HANDLE)
    {
        res = GR_ERROR_OUT_OF_MEMORY;
        goto bail;
    }

    GrPipeline* grPipeline = malloc(sizeof(GrPipeline));
    *grPipeline = (GrPipeline) {
        .grObj = { GR_OBJ_TYPE_PIPELINE, grDevice },
        .createInfo = pipelineCreateInfo,
        .pipelineSlotCount = 0,
        .pipelineSlots = NULL,
        .pipelineSlotsMutex = { 0 }, // Initialized below
        .pipelineLayout = pipelineLayout,
        .renderPass = renderPass,
        .descriptorTypeCounts = { 0 }, // Initialized below
        .stageCount = COUNT_OF(stages),
        .descriptorSetLayouts = { 0 }, // Initialized below
        .shaderInfos = { { 0 } }, // Initialized below
    };

    InitializeCriticalSectionAndSpinCount(&grPipeline->pipelineSlotsMutex, 0);
    updateDescriptorTypeCounts(COUNT_OF(grPipeline->descriptorTypeCounts),
                               grPipeline->descriptorTypeCounts, COUNT_OF(stages), stages);
    for (unsigned i = 0; i < COUNT_OF(stages); i++) {
        grPipeline->descriptorSetLayouts[i] = descriptorSetLayouts[i];
        copyPipelineShader(&grPipeline->shaderInfos[i], stages[i].shader);
    }

    *pPipeline = (GR_PIPELINE)grPipeline;
    return GR_SUCCESS;

bail:
    for (unsigned i = 0; i < COUNT_OF(descriptorSetLayouts); i++) {
        VKD.vkDestroyDescriptorSetLayout(grDevice->device, descriptorSetLayouts[i], NULL);
    }
    VKD.vkDestroyPipelineLayout(grDevice->device, pipelineLayout, NULL);
    return res;
}

GR_RESULT grCreateComputePipeline(
    GR_DEVICE device,
    const GR_COMPUTE_PIPELINE_CREATE_INFO* pCreateInfo,
    GR_PIPELINE* pPipeline)
{
    LOGT("%p %p %p\n", device, pCreateInfo, pPipeline);
    GrDevice* grDevice = (GrDevice*)device;
    GR_RESULT res = GR_SUCCESS;
    VkResult vkRes;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline vkPipeline = VK_NULL_HANDLE;

    // TODO validate parameters

    Stage stage = { &pCreateInfo->cs, VK_SHADER_STAGE_COMPUTE_BIT };

    if (stage.shader->linkConstBufferCount > 0) {
        // TODO implement
        LOGW("link-time constant buffers are not implemented\n");
    }

    GrShader* grShader = (GrShader*)stage.shader->shader;

    const VkPipelineShaderStageCreateInfo shaderStageCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .stage = stage.flags,
        .module = grShader->shaderModule,
        .pName = "main",
        .pSpecializationInfo = NULL,
    };

    descriptorSetLayout = getVkDescriptorSetLayout(grDevice, &stage);
    if (descriptorSetLayout == VK_NULL_HANDLE) {
        res = GR_ERROR_OUT_OF_MEMORY;
        goto bail;
    }

    pipelineLayout = getVkPipelineLayout(grDevice, 1, &stage, &descriptorSetLayout);
    if (pipelineLayout == VK_NULL_HANDLE) {
        res = GR_ERROR_OUT_OF_MEMORY;
        goto bail;
    }

    const VkComputePipelineCreateInfo pipelineCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = NULL,
        .flags = (pCreateInfo->flags & GR_PIPELINE_CREATE_DISABLE_OPTIMIZATION) != 0 ?
                 VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0,
        .stage = shaderStageCreateInfo,
        .layout = pipelineLayout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };

    vkRes = VKD.vkCreateComputePipelines(grDevice->device, VK_NULL_HANDLE, 1, &pipelineCreateInfo,
                                         NULL, &vkPipeline);
    if (vkRes != VK_SUCCESS) {
        LOGE("vkCreateComputePipelines failed (%d)\n", vkRes);
        res = getGrResult(vkRes);
        goto bail;
    }

    PipelineSlot* pipelineSlot = malloc(sizeof(PipelineSlot));
    *pipelineSlot = (PipelineSlot) {
        .pipeline = vkPipeline,
        .grColorBlendState = NULL,
    };

    GrPipeline* grPipeline = malloc(sizeof(GrPipeline));
    *grPipeline = (GrPipeline) {
        .grObj = { GR_OBJ_TYPE_PIPELINE, grDevice },
        .createInfo = NULL,
        .pipelineSlotCount = 1,
        .pipelineSlots = pipelineSlot,
        .pipelineSlotsMutex = { 0 }, // Initialized below
        .pipelineLayout = pipelineLayout,
        .renderPass = VK_NULL_HANDLE,
        .descriptorTypeCounts = { 0 }, // Initialized below
        .stageCount = 1,
        .descriptorSetLayouts = { descriptorSetLayout },
        .shaderInfos = { { 0 } }, // Initialized below
    };

    InitializeCriticalSectionAndSpinCount(&grPipeline->pipelineSlotsMutex, 0);
    updateDescriptorTypeCounts(COUNT_OF(grPipeline->descriptorTypeCounts),
                               grPipeline->descriptorTypeCounts, 1, &stage);
    copyPipelineShader(&grPipeline->shaderInfos[0], stage.shader);

    *pPipeline = (GR_PIPELINE)grPipeline;
    return GR_SUCCESS;

bail:
    VKD.vkDestroyDescriptorSetLayout(grDevice->device, descriptorSetLayout, NULL);
    VKD.vkDestroyPipelineLayout(grDevice->device, pipelineLayout, NULL);
    return res;
}
