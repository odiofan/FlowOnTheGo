/**
 * Implements a resize and gradient kernel
 */

// System
#include <iostream>
#include <chrono>
#include <string>
#include <stdexcept>

// OpenCV
#include <opencv2/opencv.hpp>

// CUDA
#include <cuda.h>
#include <cuda_runtime.h>

// NVIDIA Perf Primitives
#include <nppi.h>
#include <nppi_filtering_functions.h>

// Local
#include "../common/timer.h"

#include "resizeGrad.h"

using namespace timer;

namespace cu {

  /**
   *
   */
  void resizeGrad(
      const cv::Mat& src,
      cv::Mat& dst,
      cv::Mat& dst_x,
      cv::Mat& dst_y,
      double scaleX, double scaleY) {

    if (src.type() != CV_32FC3) {
      throw std::invalid_argument("resizeGrad: invalid input matrix type");
    }

    // Compute time of relevant kernel
    double compute_time = 0.0;
    double total_time = 0.0;

    // CV_32FC3 is made up of RGB floats
    int channels = 3;
    size_t elemSize = 3 * sizeof(float);

    cv::Size sz = src.size();
    int width   = sz.width;
    int height  = sz.height;

    std::cout << "[start] resizeGrad: processing " << width << "x" << height << " image" << std::endl;

    // pSrc pointer to image data
    Npp32f* pHostSrc = (float*) src.data;

    // The width, in bytes, of the image, sometimes referred to as pitch
    unsigned int nSrcStep = width * elemSize;

    NppiSize oSrcSize = { width, height };
    NppiRect oSrcROI  = { 0, 0, width, height };

    NppiRect dstRect;

    double shiftX = 0.0;
    double shiftY = 0.0;

    int eInterpolation = NPPI_INTER_LINEAR;    // Linear interpolation


    auto start_get_resize_rect = now();

    // Get the destination size
    NPP_CHECK_NPP(
        nppiGetResizeRect (oSrcROI, &dstRect, scaleX, scaleY, shiftX, shiftY, eInterpolation) );

    total_time += calc_print_elapsed("get_resize_rect", start_get_resize_rect);

    unsigned int dstSize = dstRect.width * dstRect.height;
    unsigned int nDstStep = dstRect.width * elemSize;

    auto start_cuda_malloc = now();

    // Allocate device memory
    Npp32f* pDeviceSrc, *pDeviceDst, *pDeviceDstX, *pDeviceDstY;

    checkCudaErrors( cudaMalloc((void**) &pDeviceSrc, width * height * elemSize) );
    checkCudaErrors( cudaMalloc((void**) &pDeviceDst, dstSize * elemSize) );
    checkCudaErrors( cudaMalloc((void**) &pDeviceDstX, dstSize * elemSize) );
    checkCudaErrors( cudaMalloc((void**) &pDeviceDstY, dstSize * elemSize) );

    total_time += calc_print_elapsed("cudaMalloc", start_cuda_malloc);


    // Host memory allocation
    auto start_host_alloc = now();

    float* pHostDst   = new float[dstSize * channels];
    float* pHostDstX = new float[dstSize * channels];
    float* pHostDstY = new float[dstSize * channels];

    total_time += calc_print_elapsed("host_alloc", start_host_alloc);


    // Initial image memcpy H->D
    auto start_memcpy_hd = now();

    checkCudaErrors(
        cudaMemcpy(pDeviceSrc, pHostSrc, width * height * elemSize, cudaMemcpyHostToDevice) );

    total_time += calc_print_elapsed("cudaMemcpy H->D", start_memcpy_hd);

    // Resize image
    auto start_resize = now();
    NPP_CHECK_NPP(

        nppiResizeSqrPixel_32f_C3R (
          pDeviceSrc, oSrcSize, nSrcStep, oSrcROI,
          pDeviceDst, nDstStep, dstRect,
          scaleX, scaleY, shiftX, shiftY, eInterpolation)

        );
    compute_time += calc_print_elapsed("resize", start_resize);

    auto start_cp_resize = now();
    // Copy resized image to host
    checkCudaErrors(
        cudaMemcpy(pHostDst, pDeviceDst,
          dstSize * elemSize, cudaMemcpyDeviceToHost) );
    total_time += calc_print_elapsed("resized cudaMemcpy D->H", start_cp_resize);

    // Do gradients
    NppiBorderType eBorderType = NPP_BORDER_REPLICATE;
    NppiSize  rSize   = { dstRect.width, dstRect.height };
    NppiPoint rOffset = { 0, 0 };
    NppiSize  rROI    = { dstRect.width, dstRect.height };

    // dx's
    auto start_dx = now();
    NPP_CHECK_NPP(
        nppiFilterSobelHorizBorder_32f_C3R (
          pDeviceDst, nDstStep, rSize, rOffset,
          pDeviceDstX, nDstStep, rROI, eBorderType) );
    compute_time += calc_print_elapsed("dx", start_dx);

    auto start_cp_dx = now();
    checkCudaErrors(
        cudaMemcpy(pHostDstX, pDeviceDstX,
          dstSize * elemSize, cudaMemcpyDeviceToHost) );
    total_time += calc_print_elapsed("dx cudaMemcpy D->H", start_cp_dx);


    // dy's
    auto start_dy = now();
    NPP_CHECK_NPP(
        nppiFilterSobelVertBorder_32f_C3R  (
          pDeviceDst, nDstStep, rSize, rOffset,
          pDeviceDstY, nDstStep, rROI, eBorderType) );
    compute_time += calc_print_elapsed("dy", start_dy);

    auto start_cp_dy = now();
    checkCudaErrors(
        cudaMemcpy(pHostDstY, pDeviceDstY,
          dstSize * elemSize, cudaMemcpyDeviceToHost) );
    total_time += calc_print_elapsed("dy cudaMemcpy D->H", start_cp_dy);


    auto start_cp_mat = now();
    cv::Mat dstWrapper(dstRect.height, dstRect.width, CV_32FC3, pHostDst);
    cv::Mat dstXWrapper(dstRect.height, dstRect.width, CV_32FC3, pHostDstX);
    cv::Mat dstYWrapper(dstRect.height, dstRect.width, CV_32FC3, pHostDstY);

    dstWrapper.copyTo(dst);
    dstXWrapper.copyTo(dst_x);
    dstYWrapper.copyTo(dst_y);
    total_time += calc_print_elapsed("copy to cv::Mat's", start_cp_mat);

    cudaFree((void*) pDeviceSrc);
    cudaFree((void*) pDeviceDst);
    cudaFree((void*) pDeviceDstX);
    cudaFree((void*) pDeviceDstY);

    delete[] pHostDst;
    delete[] pHostDstX;
    delete[] pHostDstY;

    std::cout << "[done] resizeGrad" << std::endl;
    std::cout << "  primary compute time: " << compute_time << " (ms)" << std::endl;
    std::cout << "  total compute time:   " << total_time + compute_time << " (ms)" << std::endl;
  }

}

