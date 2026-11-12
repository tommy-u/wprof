/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/* Retained profiler controls: the current SDK keeps driver PFNs in
 * cudaProfilerTypedefs.h while omitting the old declaration headers.
 * Runtime controls retain their cudaError_t result. These four no-argument
 * contracts are also exercised by the real API-coverage workload.
 */
extern CUresult CUDAAPI cuProfilerStart(void);
extern CUresult CUDAAPI cuProfilerStop(void);
extern cudaError_t CUDARTAPI cudaProfilerStart(void);
extern cudaError_t CUDARTAPI cudaProfilerStop(void);
