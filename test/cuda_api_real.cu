// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include <cuda.h>
#include <cuda_runtime.h>
#include <dlfcn.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

static_assert(sizeof(CUresult) == 4 && sizeof(cudaError_t) == 4);
static int failures;
static bool reporting;
#define STR_(x) #x
#define STR(x) STR_(x)
static void result(const char *name, int status, bool expected_error = false, const char *call_kind = "api")
{
    if (reporting)
        printf("{\"name\":\"%s\",\"status\":%d,\"pid\":%d,\"call_kind\":\"%s\",\"return_kind\":\"i32\"}\n",
               name, status, getpid(), call_kind);
    if ((status != 0) != expected_error)
        failures++;
}
#define D(api, ...) result(STR(api), (int)api(__VA_ARGS__))
#define R(api, ...) result(STR(api), (int)api(__VA_ARGS__))
__global__ void increment(unsigned *p) { if (!threadIdx.x && !blockIdx.x) ++*p; }
static const char ptx[] = R"PTX(.version 8.0
.target sm_80
.address_size 64
.visible .entry noop() { ret; }
)PTX";

extern "C" __attribute__((noinline)) void coverage_run(void)
{
    CUdevice device;
    D(cuDeviceGet, &device, 0);
    int value;
    D(cuDeviceGetAttribute, &value, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, device);
    result("cuDeviceGetAttribute", (int)cuDeviceGetAttribute(&value, (CUdevice_attribute)-1, device), true);
    CUcontext context;
    D(cuCtxGetCurrent, &context);
    CUdeviceptr driver_memory;
    D(cuMemAlloc, &driver_memory, 4096);
    D(cuMemsetD8, driver_memory, 0, 4096);
    unsigned host[1024] = {};
    D(cuMemcpyHtoD, driver_memory, host, sizeof(host));
    D(cuMemcpyDtoH, host, driver_memory, sizeof(host));
    CUstream driver_stream;
    D(cuStreamCreate, &driver_stream, CU_STREAM_NON_BLOCKING);
    D(cuMemcpyHtoDAsync, driver_memory, host, sizeof(host), driver_stream);
    D(cuStreamSynchronize, driver_stream);
    CUevent driver_event;
    D(cuEventCreate, &driver_event, CU_EVENT_DEFAULT);
    D(cuEventRecord, driver_event, driver_stream);
    D(cuEventSynchronize, driver_event);
    CUmodule module;
    CUfunction function;
    D(cuModuleLoadData, &module, ptx);
    D(cuModuleGetFunction, &function, module, "noop");
    D(cuLaunchKernel, function, 1, 1, 1, 1, 1, 1, 0, driver_stream, nullptr, nullptr);
    D(cuCtxSynchronize);
    CUgraph graph;
    CUgraphNode node;
    CUgraphExec executable;
    D(cuGraphCreate, &graph, 0);
    D(cuGraphAddEmptyNode, &node, graph, nullptr, 0);
    D(cuGraphInstantiateWithFlags, &executable, graph, 0);
    D(cuGraphLaunch, executable, driver_stream);
    D(cuStreamSynchronize, driver_stream);
    D(cuGraphExecDestroy, executable);
    D(cuGraphDestroy, graph);
    D(cuModuleUnload, module);
    D(cuEventDestroy, driver_event);
    D(cuStreamDestroy, driver_stream);
    D(cuMemFree, driver_memory);

    R(cudaSetDevice, 0);
    unsigned *memory;
    R(cudaMalloc, &memory, sizeof(host));
    R(cudaMemset, memory, 0, sizeof(host));
    R(cudaMemcpy, memory, host, sizeof(host), cudaMemcpyHostToDevice);
    cudaStream_t stream;
    cudaEvent_t event;
    R(cudaStreamCreate, &stream);
    R(cudaEventCreate, &event);
    void *arguments[] = {&memory};
    R(cudaLaunchKernel, (const void *)increment, dim3(1), dim3(1), arguments, 0, stream);
    R(cudaEventRecord, event, stream);
    R(cudaStreamWaitEvent, stream, event, 0);
    R(cudaEventSynchronize, event);
    R(cudaMemcpyAsync, host, memory, sizeof(host), cudaMemcpyDeviceToHost, stream);
    R(cudaStreamSynchronize, stream);
    R(cudaDeviceSynchronize);
    cudaGraph_t runtime_graph;
    cudaGraphNode_t runtime_node;
    cudaGraphExec_t runtime_executable;
    R(cudaGraphCreate, &runtime_graph, 0);
    R(cudaGraphAddEmptyNode, &runtime_node, runtime_graph, nullptr, 0);
    R(cudaGraphInstantiateWithFlags, &runtime_executable, runtime_graph, 0);
    // NVCC's per-thread inline wrapper forwards this call to the base runtime export.
    result(STR(cudaGraphLaunch), (int)cudaGraphLaunch(runtime_executable, stream));
    void *runtime = dlopen("libcudart.so.13", RTLD_NOW | RTLD_NOLOAD);
    auto launch = (decltype(&cudaGraphLaunch))dlsym(runtime, STR(cudaGraphLaunch));
    if (!launch)
        failures++;
    else
        result(STR(cudaGraphLaunch), (int)launch(runtime_executable, stream), false, "direct_export");
    dlclose(runtime);
    R(cudaStreamSynchronize, stream);
    R(cudaGraphExecDestroy, runtime_executable);
    R(cudaGraphDestroy, runtime_graph);
    const char *error = cudaGetErrorString(cudaErrorInvalidValue);
    if (reporting)
        printf("{\"name\":\"cudaGetErrorString\",\"status\":null,\"pid\":%d,\"call_kind\":\"api\",\"return_kind\":\"none\"}\n", getpid());
    if (!error)
        failures++;
    auto format = cudaCreateChannelDesc(32, 0, 0, 0, cudaChannelFormatKindSigned);
    if (reporting)
        printf("{\"name\":\"cudaCreateChannelDesc\",\"status\":null,\"pid\":%d,\"call_kind\":\"api\",\"return_kind\":\"none\"}\n", getpid());
    if (format.x != 32)
        failures++;
    R(cudaEventDestroy, event);
    R(cudaStreamDestroy, stream);
    R(cudaFree, memory);
}

int main(int argc, char **argv)
{
    int iterations = argc > 1 ? atoi(argv[1]) : 100;
    if (iterations <= 0)
        return 2;
    if (cudaSetDevice(0) != cudaSuccess || cudaFree(nullptr) != cudaSuccess)
        return 1;
    coverage_run();
    if (failures)
        return 1;
    reporting = true;
    puts("{\"ready\":true}");
    fflush(stdout);
    if (getchar() == EOF)
        return 1;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; i++)
        coverage_run();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    printf("{\"elapsed_ns\":%lld,\"failures\":%d,\"iterations\":%d}\n", (long long)elapsed, failures, iterations);
    fflush(stdout);
    if (getchar() == EOF)
        return 1;
    return failures != 0;
}
