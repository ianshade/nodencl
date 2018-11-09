/* Copyright 2018 Streampunk Media Ltd.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "cl_memory.h"
#include "noden_context.h"
#include "noden_program.h"
#include "noden_util.h"
#include <vector>

class iGpuAccess {
public:
  virtual ~iGpuAccess() {}
  virtual cl_int unmapMem() = 0;
  virtual cl_int getKernelMem(iRunParams *runParams, bool isImageParam, iKernelArg::eAccess access, cl_mem &kernelMem) = 0;
  virtual void onGpuReturn() = 0;
};

class gpuMemory : public iGpuMemory {
public:
  gpuMemory(iGpuAccess *gpuAccess)
    : mGpuAccess(gpuAccess) {}
  ~gpuMemory() {
    mGpuAccess->onGpuReturn();
  }

  cl_int setKernelParam(cl_kernel kernel, uint32_t paramIndex, bool isImageParam, iKernelArg::eAccess access, iRunParams *runParams) const {
    cl_int error = CL_SUCCESS;
    error = mGpuAccess->unmapMem();
    PASS_CL_ERROR;

    cl_mem kernelMem = nullptr;
    error = mGpuAccess->getKernelMem(runParams, isImageParam, access, kernelMem);
    PASS_CL_ERROR;

    error = clSetKernelArg(kernel, paramIndex, sizeof(cl_mem), &kernelMem);
    return error;
  }

private:
  iGpuAccess *mGpuAccess;
};

class clMemory : public iClMemory, public iGpuAccess {
public:
  clMemory(cl_context context, cl_command_queue commands, eMemFlags memFlags, eSvmType svmType, 
           uint32_t numBytes, deviceInfo *devInfo)
    : mContext(context), mCommands(commands), mMemFlags(memFlags), mSvmType(svmType), mNumBytes(numBytes),
      mPinnedMem(nullptr), mImageMem(nullptr), mHostBuf(nullptr), mGpuLocked(false), mHostMapped(false),
      mImageAccessAttribute(iKernelArg::eAccess::READONLY), mDevInfo(devInfo) {}
  ~clMemory() {
    cl_int error = CL_SUCCESS;
    error = unmapMem();
    if (CL_SUCCESS != error)
      printf("OpenCL error in subroutine. Location %s(%d). Error %i: %s\n",
        __FILE__, __LINE__, error, clGetErrorString(error));

    if (eSvmType::NONE != mSvmType)
      clSVMFree(mContext, mHostBuf);

    if (mImageMem) {
      error = clReleaseMemObject(mImageMem);
      if (CL_SUCCESS != error)
        printf("OpenCL error in subroutine. Location %s(%d). Error %i: %s\n",
          __FILE__, __LINE__, error, clGetErrorString(error));
    }

    if (mPinnedMem) {
      error = clReleaseMemObject(mPinnedMem);
      if (CL_SUCCESS != error)
        printf("OpenCL error in subroutine. Location %s(%d). Error %i: %s\n",
          __FILE__, __LINE__, error, clGetErrorString(error));
    }
  }

  bool allocate() {
    cl_int error = CL_SUCCESS;
    cl_svm_mem_flags clMemFlags = (eMemFlags::READONLY == mMemFlags) ? CL_MEM_READ_ONLY :
                                  (eMemFlags::WRITEONLY == mMemFlags) ? CL_MEM_WRITE_ONLY :
                                  CL_MEM_READ_WRITE;
    if (eSvmType::FINE == mSvmType)
      clMemFlags |= CL_MEM_SVM_FINE_GRAIN_BUFFER;

    switch (mSvmType) {
    case eSvmType::FINE:
    case eSvmType::COARSE:
      mHostBuf = clSVMAlloc(mContext, clMemFlags, mNumBytes, 0);
      mPinnedMem = clCreateBuffer(mContext, clMemFlags | CL_MEM_USE_HOST_PTR, mNumBytes, mHostBuf, &error);
      break;
    case eSvmType::NONE:
    default:
      mPinnedMem = clCreateBuffer(mContext, clMemFlags | CL_MEM_ALLOC_HOST_PTR, mNumBytes, nullptr, &error);
      if (CL_SUCCESS == error) {
        cl_map_flags clMapFlags = (eMemFlags::READONLY == mMemFlags) ? CL_MAP_WRITE_INVALIDATE_REGION : 
                                  (eMemFlags::WRITEONLY == mMemFlags) ? CL_MAP_READ :
                                  CL_MAP_READ | CL_MAP_WRITE;
        mHostBuf = clEnqueueMapBuffer(mCommands, mPinnedMem, CL_TRUE, clMapFlags, 0, mNumBytes, 0, nullptr, nullptr, nullptr);
      } else
        printf("OpenCL error in subroutine. Location %s(%d). Error %i: %s\n",
          __FILE__, __LINE__, error, clGetErrorString(error));
      mHostMapped = true;
      break;
    }

    return nullptr != mHostBuf;
  }

  std::shared_ptr<iGpuMemory> getGPUMemory() {
    // printf("getGpuMemory type %d, host mapped %s, numBytes %d\n", mSvmType, mHostMapped?"true":"false", mNumBytes);
    mGpuLocked = true;
    return std::make_shared<gpuMemory>(this);
  }

  void setHostAccess(cl_int &error, eMemFlags haFlags) {
    error = CL_SUCCESS;
    if (mGpuLocked) {
      printf("GPU buffer access must be released before host access - %d\n", mNumBytes);
      error = CL_MAP_FAILURE;
      return;
    }

    if (!mHostMapped) {
      cl_map_flags mapFlags = (eMemFlags::READWRITE == haFlags) ? CL_MAP_WRITE | CL_MAP_READ :
                              (eMemFlags::WRITEONLY == haFlags) ? CL_MAP_WRITE :
                              CL_MAP_READ;
      if (mImageMem) {
        if ((mDevInfo->oclVer < clVersion(2,0)) && (CL_MAP_WRITE != mapFlags)) {
          error = copyImageToBuffer();
          if (CL_SUCCESS != error) return;
        }

        // Optimisation - if readonly access need not release mImageMem - but need to know if buffer is still up-to-date before next buffer kernel op?
        // if (CL_MAP_READ != mapFlags) {
          error = clReleaseMemObject(mImageMem);
          mImageMem = nullptr;
          mImageAccessAttribute = iKernelArg::eAccess::READONLY;
          if (CL_SUCCESS != error) return;
        // }
      }

      if (eSvmType::NONE == mSvmType) {
        void *hostBuf = clEnqueueMapBuffer(mCommands, mPinnedMem, CL_TRUE, mapFlags, 0, mNumBytes, 0, nullptr, nullptr, nullptr);
        if (mHostBuf != hostBuf) {
          printf("Unexpected behaviour - mapped buffer address is not the same: %p != %p\n", mHostBuf, hostBuf);
          error = CL_MAP_FAILURE;
          return;
        }
        mHostMapped = true;
      } else if (eSvmType::COARSE == mSvmType) {
        error = clEnqueueSVMMap(mCommands, CL_TRUE, mapFlags, mHostBuf, mNumBytes, 0, 0, 0);
        mHostMapped = true;
      }
    }
  }

  uint32_t numBytes() const { return mNumBytes; }
  eMemFlags memFlags() const { return mMemFlags; }
  eSvmType svmType() const { return mSvmType; }
  std::string svmTypeName() const {
    switch (mSvmType) {
    case eSvmType::FINE: return "fine";
    case eSvmType::COARSE: return "coarse";
    case eSvmType::NONE: return "none";
    default: return "unknown";
    }
  }
  void* hostBuf() const { return mHostBuf; }

private:
  cl_context mContext;
  cl_command_queue mCommands;
  const eMemFlags mMemFlags;
  const eSvmType mSvmType;
  const uint32_t mNumBytes;
  cl_mem mPinnedMem;
  cl_mem mImageMem;
  void *mHostBuf;
  bool mGpuLocked;
  bool mHostMapped;
  iKernelArg::eAccess mImageAccessAttribute;
  deviceInfo *mDevInfo;

  cl_int unmapMem() {
    cl_int error = CL_SUCCESS;
    if (mHostMapped) {
      if (eSvmType::NONE == mSvmType)
        error = clEnqueueUnmapMemObject(mCommands, mPinnedMem, mHostBuf, 0, nullptr, nullptr);
      else if (eSvmType::COARSE == mSvmType)
        error = clEnqueueSVMUnmap(mCommands, mHostBuf, 0, 0, 0);
      mHostMapped = false;
    }
    return error;
  }

  cl_int copyImageToBuffer() {
    cl_int error = CL_SUCCESS;
    if (mImageMem) {
      const size_t origin[3] = { 0, 0, 0 };
      size_t region[3] = { 1, 1, 1 };
      error = clGetImageInfo(mImageMem, CL_IMAGE_WIDTH, sizeof(size_t), &region[0], NULL);
      PASS_CL_ERROR;
      error = clGetImageInfo(mImageMem, CL_IMAGE_HEIGHT, sizeof(size_t), &region[1], NULL);
      PASS_CL_ERROR;
      size_t depth = 0;
      error = clGetImageInfo(mImageMem, CL_IMAGE_DEPTH, sizeof(size_t), &depth, NULL);
      PASS_CL_ERROR;
      if (depth) region[2] = depth;

      printf("Copying image memory to buffer size %zdx%zd\n", region[0], region[1]);
      error = clEnqueueCopyImageToBuffer(mCommands, mImageMem, mPinnedMem, origin, region, 0, 0, nullptr, nullptr);
      PASS_CL_ERROR;
    }
    return error;
  }

  cl_int getKernelMem(iRunParams *runParams, bool isImageParam, iKernelArg::eAccess access, cl_mem &kernelMem) {
    kernelMem = mImageMem ? mImageMem : mPinnedMem;
    const size_t origin[3] = { 0, 0, 0 };
    cl_int error = CL_SUCCESS;

    if (isImageParam && !mImageMem) {
      // create new image object based on global work items as image dimensions
      std::vector<size_t> imageDims;
      for (size_t i = 0; i < runParams->numDims(); ++i)
        imageDims.push_back(runParams->globalWorkItems()[i]);

      cl_image_format clImageFormat;
      memset(&clImageFormat, 0, sizeof(clImageFormat));
      clImageFormat.image_channel_order = CL_RGBA;
      clImageFormat.image_channel_data_type = CL_FLOAT;

      cl_image_desc clImageDesc;
      memset(&clImageDesc, 0, sizeof(clImageDesc));
      clImageDesc.image_type = imageDims.size() > 2 ? CL_MEM_OBJECT_IMAGE3D : CL_MEM_OBJECT_IMAGE2D;
      clImageDesc.image_width = imageDims[0];
      clImageDesc.image_height = imageDims.size() > 1 ? imageDims[1] : 0;
      clImageDesc.image_depth = imageDims.size() > 2 ? imageDims[2] : 0;
      if (mDevInfo->oclVer >= clVersion(2,0))
        clImageDesc.mem_object = mPinnedMem;

      cl_mem_flags clMemFlags = (iKernelArg::eAccess::WRITEONLY == access) ? CL_MEM_WRITE_ONLY : CL_MEM_READ_ONLY;
      mImageMem = clCreateImage(mContext, clMemFlags, &clImageFormat, &clImageDesc, nullptr, &error);
      PASS_CL_ERROR;

      kernelMem = mImageMem;
      mImageAccessAttribute = access;
      if ((mDevInfo->oclVer < clVersion(2,0)) && (iKernelArg::eAccess::WRITEONLY != mImageAccessAttribute)) {
        // don't copy from buffer if this has a write-only argument attribute - it will be overwritten by the kernel
        printf("Copying image memory from buffer size %zdx%zd\n", imageDims[0], imageDims[1]);
        size_t region[3] = { 1, 1, 1 };
        for (size_t i = 0; i < imageDims.size(); ++i)
          region[i] = imageDims[i];
        error = clEnqueueCopyBufferToImage(mCommands, mPinnedMem, mImageMem, 0, origin, region, 0, nullptr, nullptr);
        PASS_CL_ERROR;
      }
    } else if (!isImageParam && mImageMem) {
      if ((mDevInfo->oclVer < clVersion(2,0)) && (iKernelArg::eAccess::READONLY != mImageAccessAttribute)) {
        // don't copy back to buffer if this had a read-only argument attribute - the buffer is already up-to-date
        error = copyImageToBuffer();
        PASS_CL_ERROR;
      }
      error = clReleaseMemObject(mImageMem);
      PASS_CL_ERROR;
      mImageMem = nullptr;
      mImageAccessAttribute = iKernelArg::eAccess::READONLY;

      kernelMem = mPinnedMem;
    }

    return error;
  }

  void onGpuReturn() {
    mGpuLocked = false;
  }
};

iClMemory *iClMemory::create(cl_context context, cl_command_queue commands, eMemFlags memFlags, eSvmType svmType, uint32_t numBytes, deviceInfo *devInfo) {
  return new clMemory(context, commands, memFlags, svmType, numBytes, devInfo);
}