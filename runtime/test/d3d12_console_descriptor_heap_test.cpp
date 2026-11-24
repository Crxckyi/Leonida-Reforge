// Console Xbox CreateDescriptorHeap (issue #313 comment 7107265058) passes a 16-byte PC
// D3D12_DESCRIPTOR_HEAP_DESC followed by address at +0x10 and size at +0x18. The title asserts
// GetCPUDescriptorHandleForHeapStart() == address. This tests the explicit Leonida console-heap
// creation path used by the Xbox device proxy slot 14. It does not cover title-memory aliasing /
// section 3.3 backing.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"

#include <cstdint>
#include <cstdio>

namespace {

// The device proxy hands out Xbox-ABI heaps: slots 9 and 10 return the 8-byte handle in RAX, with no hidden return
// pointer (d3d12_xs_device.cpp, heap_get_cpu_handle). Called through the PC C++ declaration, the slot never writes
// the hidden pointer in RDX, and the caller reads an uninitialised stack word.
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);

std::uint64_t xbox_heap_handle(void* heap, unsigned slot) {
    return reinterpret_cast<XboxHeapHandleFn>((*static_cast<void***>(heap))[slot])(heap);
}

template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T** put() {
        if (p) {
            p->Release();
            p = nullptr;
        }
        return &p;
    }
    T* get() const { return p; }
    T* operator->() const { return p; }
};

bool check_console_addressed_heap(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 8;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NodeMask = 0;
    const D3D12_GPU_VIRTUAL_ADDRESS address = 0x112233440000ull;
    const std::uint64_t size = 8ull * 32ull;

    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = rt::d3d12::create_console_descriptor_heap(
        device, &desc, address, size, IID_PPV_ARGS(heap.put()));
    if (FAILED(hr) || !heap.get()) {
        std::fprintf(stderr, "create_console_descriptor_heap failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    if (cpu.ptr != static_cast<SIZE_T>(address)) {
        std::fprintf(stderr, "CPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(cpu.ptr), static_cast<unsigned long long>(address));
        return false;
    }
    const auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    if (gpu.ptr != address) {
        std::fprintf(stderr, "GPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(gpu.ptr), static_cast<unsigned long long>(address));
        return false;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC got = heap->GetDesc();
    if (got.Type != desc.Type || got.NumDescriptors != desc.NumDescriptors || got.Flags != desc.Flags ||
        got.NodeMask != 1) {
        std::fprintf(stderr, "GetDesc mismatch\n");
        return false;
    }
    return true;
}

bool check_xs_proxy_slot14(ID3D12Device* real_device) {
    // create_device_proxy takes one reference to the real device on success (d3d12_xs_device.h) and releases it
    // with the proxy; main's Ref still owns its own.
    real_device->AddRef();
    void* proxy_raw = nullptr;
    if (FAILED(rt::d3d12_xs::create_device_proxy(real_device, __uuidof(ID3D12Device), &proxy_raw)) || !proxy_raw) {
        real_device->Release();
        std::fprintf(stderr, "create_device_proxy failed\n");
        return false;
    }
    Ref<ID3D12Device> proxy;
    proxy.p = static_cast<ID3D12Device*>(proxy_raw);

    struct ConsoleDescriptorHeapDesc {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        std::uint64_t address = 0;
        std::uint64_t size = 0;
    } console{};
    console.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    console.desc.NumDescriptors = 4;
    console.desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    console.address = 0x556677880000ull;
    console.size = 4ull * 32ull;

    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = proxy->CreateDescriptorHeap(&console.desc, IID_PPV_ARGS(heap.put()));
    if (FAILED(hr) || !heap.get()) {
        std::fprintf(stderr, "proxy CreateDescriptorHeap(console) failed: 0x%08lx\n",
                     static_cast<unsigned long>(hr));
        return false;
    }
    const std::uint64_t cpu = xbox_heap_handle(heap.get(), 9);
    if (cpu != console.address) {
        std::fprintf(stderr, "proxy CPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(cpu), static_cast<unsigned long long>(console.address));
        return false;
    }
    const std::uint64_t gpu = xbox_heap_handle(heap.get(), 10);
    if (gpu != console.address) {
        std::fprintf(stderr, "proxy GPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(gpu), static_cast<unsigned long long>(console.address));
        return false;
    }
    return true;
}

bool check_normal_heap_unchanged(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 4;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.put()));
    if (FAILED(hr) || !heap.get()) {
        std::fprintf(stderr, "CreateDescriptorHeap(normal) failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    if (cpu.ptr == 0) {
        std::fprintf(stderr, "normal heap CPU start is null\n");
        return false;
    }
    return true;
}

}  // namespace

int main() {
    vktest::Device vk;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;

    Ref<ID3D12Device> device;
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk.instance_proc;
    info.instance = vk.instance;
    info.physical = vk.physical;
    info.device = vk.device;
    info.queue = vk.queue;
    info.queue_family = vk.family;
    if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(device.put()))) || !device.get()) {
        std::fprintf(stderr, "create_vulkan_device failed\n");
        return 1;
    }

    if (!check_normal_heap_unchanged(device.get())) return 1;
    if (!check_console_addressed_heap(device.get())) return 1;
    // The proxy takes the console path only with a factory registered (LEONIDA_CONSOLE_DESCRIPTOR_HEAPS=1 in gta6rt).
    rt::d3d12_xs::set_console_descriptor_heap_factory(&rt::d3d12::create_console_descriptor_heap);
    const bool proxy_ok = check_xs_proxy_slot14(device.get());
    rt::d3d12_xs::set_console_descriptor_heap_factory(nullptr);
    if (!proxy_ok) return 1;
    std::fprintf(stderr, "ok\n");
    return 0;
}
