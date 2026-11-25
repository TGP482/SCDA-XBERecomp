// the Vulkan renderer's allocator (Vulkan Memory Allocator), compiled once here
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#if defined(__clang__)
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <vk_mem_alloc.h>
