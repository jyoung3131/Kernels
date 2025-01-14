#ifndef PRK_CUDA_HPP
#define PRK_CUDA_HPP

//#include <cstdio>
//#include <cstdlib>

#include <iostream>
#include <vector>
#include <array>

// CUDA runtime
#include <cuda_to_cupla.hpp>

//For using Cupla without CMake we need to add the following defines - see this page for more information: https://github.com/alpaka-group/cupla/blob/master/doc/ConfigurationHeader.md
#define CUPLA_STREAM_ASYNC_ENABLED 0
// use cupla as header-only library
#define CUPLA_HEADER_ONLY 1

#if defined( CUPLA_ACC_CpuOmp2Blocks  )
#   include <cupla/config/CpuOmp2Blocks.hpp>
#elif defined( CUPLA_ACC_CpuOmp2Threads  )
#   include <cupla/config/CpuOmp2Threads.hpp>
#elif defined( CUPLA_ACC_CpuSerial )
#   include <cupla/config/CpuSerial.hpp>
#elif defined( CUPLA_ACC_CpuTbbBlocks  )
#   include <cupla/config/CpuTbbBlocks.hpp>
#elif defined( CUPLA_ACC_CpuThreads  )
#   include <cupla/config/CpuThreads.hpp>
#elif defined( CUPLA_ACC_GpuCudaRt  )
#   include <cupla/config/GpuCudaRt.hpp>
#elif defined( CUPLA_ACC_GpuHipRt  )
#   include <cupla/config/GpuHipRt.hpp>
#endif


#if defined(PRK_USE_CUBLAS)
#if defined(__NVCC__)
#include <cublas_v2.h>
#else
#error Sorry, no CUBLAS without NVCC.
#endif
#endif

typedef double prk_float;

namespace prk
{
    namespace CUDA
    {
        void check(cudaError_t rc)
        {
            if (rc==cudaSuccess) {
                return;
            } else {
                std::cerr << "PRK CUDA error: " << cudaGetErrorString(rc) << std::endl;
                std::abort();
            }
        }

#if defined(PRK_USE_CUBLAS)
        // It seems that Coriander defines cublasStatus_t to cudaError_t
        // because the compiler complains that this is a redefinition.
        void check(cublasStatus_t rc)
        {
            if (rc==CUBLAS_STATUS_SUCCESS) {
                return;
            } else {
                std::cerr << "PRK CUBLAS error: " << rc << std::endl;
                std::abort();
            }
        }
#endif

        class info {

            private:
                int nDevices;
                //std::vector<cudaDeviceProp> vDevices;
                //std::vector<AccDevProps> vDevices;

            public:
                int maxThreadsPerBlock;
                std::array<unsigned,3> maxThreadsDim;
                std::array<unsigned,3> maxGridSize;

                info() {
                    prk::CUDA::check( cudaGetDeviceCount(&nDevices) );
                    
		    for (size_t i=0; i< nDevices; ++i) {
			//Define acctype and device type
			//auto const accDev = alpaka::getDevByIdx<Acc>(0u);
			//std::cout<< " grid" << alpaka::getAccDevProps(accDev).m_gridBlockExtentMax; 

                        //cudaGetDeviceProperties(&(vDevices[i]), i);
			/*alpaka::getAccDevProps(&vDevices[i]); 
			if (i==0) {
                            maxThreadsPerBlock = vDevices[i].maxThreadsPerBlock;
                            for (int j=0; j<3; ++j) {
                                maxThreadsDim[j]   = vDevices[i].maxThreadsDim[j];
                                maxGridSize[j]     = vDevices[i].maxGridSize[j];
                            }*/
			if (i==0) {
                            maxThreadsPerBlock = 1;
                            for (int j=0; j<3; ++j) {
                                maxThreadsDim[j]   = 1;
                                maxGridSize[j]     = 1;
                            }
                        }
                    }
                }

                // do not use cached value as a hedge against weird stuff happening
                int num_gpus() {
                    int g;
                    prk::CUDA::check( cudaGetDeviceCount(&g) );
                    return g;
                }

                int get_gpu() {
                    int g;
                    prk::CUDA::check( cudaGetDevice(&g) );
                    return g;
                }

                void set_gpu(int g) {
                    prk::CUDA::check( cudaSetDevice(g) );
                }

                void print() {
                    for (int i=0; i<nDevices; ++i) {
                        /*std::cout << "device name: " << vDevices[i].name << "\n";
                        std::cout << "total global memory:     " << vDevices[i].totalGlobalMem << "\n";
                        std::cout << "max threads per block:   " << vDevices[i].maxThreadsPerBlock << "\n";
                        std::cout << "max threads dim:         " << vDevices[i].maxThreadsDim[0] << ","
                                                                 << vDevices[i].maxThreadsDim[1] << ","
                                                                 << vDevices[i].maxThreadsDim[2] << "\n";
                        std::cout << "max grid size:           " << vDevices[i].maxGridSize[0] << ","
                                                                 << vDevices[i].maxGridSize[1] << ","
                                                                 << vDevices[i].maxGridSize[2] << "\n";
                        std::cout << "memory clock rate (KHz): " << vDevices[i].memoryClockRate << "\n";
                        std::cout << "memory bus width (bits): " << vDevices[i].memoryBusWidth << "\n";*/
                    }
                }

                bool checkDims(dim3 dimBlock, dim3 dimGrid) {
                    if (dimBlock.x > maxThreadsDim[0]) {
                        std::cout << "dimBlock.x too large" << std::endl;
                        return false;
                    }
                    if (dimBlock.y > maxThreadsDim[1]) {
                        std::cout << "dimBlock.y too large" << std::endl;
                        return false;
                    }
                    if (dimBlock.z > maxThreadsDim[2]) {
                        std::cout << "dimBlock.z too large" << std::endl;
                        return false;
                    }
                    if (dimGrid.x  > maxGridSize[0])   {
                        std::cout << "dimGrid.x  too large" << std::endl;
                        return false;
                    }
                    if (dimGrid.y  > maxGridSize[1]) {
                        std::cout << "dimGrid.y  too large" << std::endl;
                        return false;
                    }
                    if (dimGrid.z  > maxGridSize[2]) {
                        std::cout << "dimGrid.z  too large" << std::endl;
                        return false;
                    }
                    return true;
                }
        };

        template <typename T>
        T * malloc_device(size_t n) {
            T * ptr;
            size_t bytes = n * sizeof(T);
            prk::CUDA::check( cudaMalloc((void**)&ptr, bytes) );
            return ptr;
        }

        template <typename T>
        T * malloc_host(size_t n) {
            T * ptr;
            size_t bytes = n * sizeof(T);
            prk::CUDA::check( cudaMallocHost((void**)&ptr, bytes) );
            return ptr;
        }

        /*template <typename T>
        T * malloc_managed(size_t n) {
            T * ptr;
            size_t bytes = n * sizeof(T);
            prk::CUDA::check( cudaMallocManaged((void**)&ptr, bytes) );
            return ptr;
        }*/

        template <typename T>
        void free(T * ptr) {
            prk::CUDA::check( cudaFree((void*)ptr) );
        }

        template <typename T>
        void free_host(T * ptr) {
            prk::CUDA::check( cudaFreeHost((void*)ptr) );
        }

        template <typename T>
        void copyD2H(T * output, T * const input, size_t n) {
            size_t bytes = n * sizeof(T);
            prk::CUDA::check( cudaMemcpy(output, input, bytes, cudaMemcpyDeviceToHost) );
        }

        template <typename T>
        void copyH2D(T * output, T * const input, size_t n) {
            size_t bytes = n * sizeof(T);
            prk::CUDA::check( cudaMemcpy(output, input, bytes, cudaMemcpyHostToDevice) );
        }

        template <typename T>
        void copyD2Hasync(T * output, T * const input, size_t n) {
            size_t bytes = n * sizeof(T);
            prk::CUDA::check( cudaMemcpyAsync(output, input, bytes, cudaMemcpyDeviceToHost) );
        }

        template <typename T>
        void copyH2Dasync(T * output, T * const input, size_t n) {
            size_t bytes = n * sizeof(T);
            prk::CUDA::check( cudaMemcpyAsync(output, input, bytes, cudaMemcpyHostToDevice) );
        }

        template <typename T>
        void prefetch(T * ptr, size_t n, int device = 0) {
            size_t bytes = n * sizeof(T);
            //std::cout << "device=" << device << "\n";
            prk::CUDA::check( cudaMemPrefetchAsync(ptr, bytes, device) );
        }

        void sync(void) {
            prk::CUDA::check( cudaDeviceSynchronize() );
        }

        void set_device(int i) {
            prk::CUDA::check( cudaSetDevice(i) );
        }

    } // CUDA namespace

} // prk namespace

#endif // PRK_CUDA_HPP
