/* SPDX-FileCopyrightText: Copyright (c) 2025-2026 Moore Threads / Contributors. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * mufile_compat.h — thin translation layer that lets the upstream GDS_MT plugin
 * sources (written against NVIDIA cuFile + CUDA Runtime) compile and link
 * against MUSA muFile + libmusa unchanged.
 *
 * Design premise (see docs/musa-nixl-guide): muFile is API-identical to cuFile
 * and libmusa is API-identical to libcudart. Two integration models are
 * therefore possible, controlled by the MUSA runtime headers themselves:
 *
 *   1. The common case: MUSA ships cuFile/CUDA-compatible header NAMES
 *      (<cufile.h>, <cuda_runtime.h>) that already wrap muFile/musa. In that
 *      case this shim is a no-op and the GDS_MT sources require ZERO changes.
 *
 *   2. The fallback case: MUSA only ships <musa_runtime.h> / <musa_cufile.h>
 *      with cuFile-compatible symbol names (muFileRead, musaSetDevice, ...).
 *      This shim then #includes those headers and provides the macro aliases so
 *      every cuFile/cuda* identifier resolves to its mu*/musa* twin.
 *
 * The shim is force-injected into every GDS_MT translation unit via meson's
 * `-include gds_mt/mufile_compat.h` (see src/plugins/gds_mt/meson.build), so the
 * existing gds_mt_*.cpp files stay byte-for-byte identical to upstream.
 *
 * Error model: cuFileRead/cuFileWrite return ssize_t (negative => errno set).
 * muFileRead/muFileWrite are assumed to follow the SAME ssize_t convention (this
 * is the "1:1" contract). If a future muFile build returns an error-code struct
 * instead, add a thin inline wrapper below to translate it back to ssize_t; the
 * plugin source itself never needs to change.
 */
#ifndef NIXL_GDS_MT_MUFILE_COMPAT_H
#define NIXL_GDS_MT_MUFILE_COMPAT_H

#ifdef __MUSA_PLATFORM__

/* ---- 1. Runtime header redirection ---------------------------------------- *
 * On MUSA, prefer the MUSA-provided headers. We try the CUDA-compatible header
 * names first (MUSA SDK commonly installs these), then fall back to the native
 * musa_* names. Only one set needs to resolve. */

#if __has_include(<cuda_runtime.h>)
  /* MUSA SDK provides a CUDA-compatible cuda_runtime.h wrapper. Nothing to do. */
#elif __has_include(<musa_runtime.h>)
  #include <musa_runtime.h>
  /* Alias the CUDA Runtime symbols the plugin uses. libmusa exports the musa*
   * symbols; we remap the cuda* calls emitted by the (unchanged) source. */
  #define cudaSetDevice    musaSetDevice
  #define cudaGetErrorString musaGetErrorString
  #define cudaSuccess      musaSuccess
  #define cudaError_t      musaError_t
#endif

/* ---- 2. cuFile / muFile header redirection -------------------------------- */
#if __has_include(<cufile.h>)
  /* MUSA provides a cuFile-compatible header. Nothing to do. */
#elif __has_include(<musa_cufile.h>)
  #include <musa_cufile.h>
  /* muFile is 1:1 with cuFile: same signatures, same return (ssize_t) model.
   * Remap only the names the GDS_MT sources reference. */
  #define cuFileDriverOpen       muFileDriverOpen
  #define cuFileDriverClose      muFileDriverClose
  #define cuFileBufRegister      muFileBufRegister
  #define cuFileBufDeregister    muFileBufDeregister
  #define cuFileHandleRegister   muFileHandleRegister
  #define cuFileHandleDeregister muFileHandleDeregister
  #define cuFileRead             muFileRead
  #define cuFileWrite            muFileWrite

  /* cuFile types -> muFile types (1:1). If the MUSA headers already define the
   * cuFile-prefixed names (model 1), these macros are harmlessly unused because
   * the #elif branch above is not taken. */
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
#endif /* NIXL_GDS_MT_MUFILE_COMPAT_H */