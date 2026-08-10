/* SPDX-FileCopyrightText: Copyright (c) 2025-2026 Moore Threads / Contributors. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * musafile_compat.h — translation layer for the MUSA_MT plugin (Route 3).
 *
 * muFile is API-identical to NVIDIA cuFile, and libmusa is API-identical to
 * libcudart. The MUSA SDK commonly ships headers under the cuFile/CUDA-
 * compatible names; when it does, this shim is a no-op. When only the native
 * <musa_runtime.h> / <musa_cufile.h> names exist, this shim #includes them and
 * remaps every cuFile/cuda* identifier to its mu*/musa* twin so the plugin
 * sources (which mirror the upstream cuFile/GDS_MT call shape) compile and link
 * against muFile/libmusa unchanged.
 *
 * Included directly by musa_mt_utils.h.
 */
#ifndef NIXL_MUSA_MT_MUSAFILE_COMPAT_H
#define NIXL_MUSA_MT_MUSAFILE_COMPAT_H

#ifdef __MUSA_PLATFORM__

/* ---- 1. Runtime header redirection ---------------------------------------- */
#if __has_include(<cuda_runtime.h>)
  /* MUSA SDK provides a CUDA-compatible cuda_runtime.h wrapper. */
#elif __has_include(<musa_runtime.h>)
  #include <musa_runtime.h>
  #define cudaSetDevice      musaSetDevice
  #define cudaGetErrorString musaGetErrorString
  #define cudaSuccess        musaSuccess
  #define cudaError_t        musaError_t
#endif

/* ---- 2. cuFile / muFile header redirection -------------------------------- */
#if __has_include(<cufile.h>)
  #include <cufile.h>
#elif __has_include(<musa_cufile.h>)
  #include <musa_cufile.h>
  #define cuFileDriverOpen       muFileDriverOpen
  #define cuFileDriverClose      muFileDriverClose
  #define cuFileBufRegister      muFileBufRegister
  #define cuFileBufDeregister    muFileBufDeregister
  #define cuFileHandleRegister   muFileHandleRegister
  #define cuFileHandleDeregister muFileHandleDeregister
  #define cuFileRead             muFileRead
  #define cuFileWrite            muFileWrite

  #ifndef CUfileHandle_t
    #define CUfileHandle_t       muFileHandle_t
  #endif
  #ifndef CUfileDescr_t
    #define CUfileDescr_t        muFileDescr_t
  #endif
  #ifndef CUfileError_t
    #define CUfileError_t        muFileError_t
  #endif
  #ifndef CU_FILE_SUCCESS
    #define CU_FILE_SUCCESS      MU_FILE_SUCCESS
  #endif
  #ifndef CU_FILE_HANDLE_TYPE_OPAQUE_FD
    #define CU_FILE_HANDLE_TYPE_OPAQUE_FD MU_FILE_HANDLE_TYPE_OPAQUE_FD
  #endif
  #ifndef CUFILE_READ
    #define CUFILE_READ          MUFILE_READ
  #endif
  #ifndef CUFILE_WRITE
    #define CUFILE_WRITE         MUFILE_WRITE
  #endif
  #ifndef CUfileOpcode_t
    #define CUfileOpcode_t       muFileOpcode_t
  #endif
#endif

#endif /* __MUSA_PLATFORM__ */
#endif /* NIXL_MUSA_MT_MUSAFILE_COMPAT_H */
