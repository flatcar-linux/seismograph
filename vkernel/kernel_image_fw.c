/* Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Functions for verifying a verified boot kernel image.
 * (Firmware portion)
 */

#include "kernel_image_fw.h"

#include "cryptolib.h"
#include "rollback_index.h"
#include "utility.h"

/* Macro to determine the size of a field structure in the KernelImage
 * structure. */
#define FIELD_LEN(field) (sizeof(((KernelImage*)0)->field))
#define KERNEL_CONFIG_FIELD_LEN (FIELD_LEN(kernel_version) + FIELD_LEN(options.version) + \
                                 FIELD_LEN(options.cmd_line) + \
                                 FIELD_LEN(options.kernel_len) + \
                                 FIELD_LEN(options.kernel_load_addr) + \
                                 FIELD_LEN(options.kernel_entry_addr))

char* kVerifyKernelErrors[VERIFY_KERNEL_MAX] = {
  "Success.",
  "Invalid Image.",
  "Kernel Key Signature Failed.",
  "Invalid Kernel Verification Algorithm.",
  "Config Signature Failed.",
  "Kernel Signature Failed.",
  "Wrong Kernel Magic.",
};

int VerifyKernelHeader(const uint8_t* firmware_key_blob,
                       const uint8_t* header_blob,
                       const int dev_mode,
                       int* firmware_algorithm,
                       int* kernel_algorithm,
                       int* kernel_header_len) {
  int kernel_sign_key_len;
  int firmware_sign_key_len;
  uint16_t header_version, header_len;
  uint16_t firmware_sign_algorithm, kernel_sign_algorithm;
  uint8_t* header_checksum = NULL;

  /* Base Offset for the header_checksum field. Actual offset is
   * this + kernel_sign_key_len. */
  int base_header_checksum_offset = (FIELD_LEN(header_version) +
                                     FIELD_LEN(header_len) +
                                     FIELD_LEN(firmware_sign_algorithm) +
                                     FIELD_LEN(kernel_sign_algorithm) +
                                     FIELD_LEN(kernel_key_version));

  Memcpy(&header_version, header_blob, sizeof(header_version));
  Memcpy(&header_len, header_blob + FIELD_LEN(header_version),
         sizeof(header_len));
  Memcpy(&firmware_sign_algorithm,
         header_blob + (FIELD_LEN(header_version) +
                        FIELD_LEN(header_len)),
         sizeof(firmware_sign_algorithm));
  Memcpy(&kernel_sign_algorithm,
         header_blob + (FIELD_LEN(header_version) +
                        FIELD_LEN(header_len) +
                        FIELD_LEN(firmware_sign_algorithm)),
         sizeof(kernel_sign_algorithm));

  /* TODO(gauravsh): Make this return two different error types depending
   * on whether the firmware or kernel signing algorithm is invalid. */
  if (firmware_sign_algorithm >= kNumAlgorithms)
    return VERIFY_KERNEL_INVALID_ALGORITHM;
  if (kernel_sign_algorithm >= kNumAlgorithms)
    return VERIFY_KERNEL_INVALID_ALGORITHM;

  *firmware_algorithm = (int) firmware_sign_algorithm;
  *kernel_algorithm = (int) kernel_sign_algorithm;
  kernel_sign_key_len = RSAProcessedKeySize(kernel_sign_algorithm);
  firmware_sign_key_len = RSAProcessedKeySize(firmware_sign_algorithm);


  /* Verify if header len is correct? */
  if (header_len != (base_header_checksum_offset +
                     kernel_sign_key_len +
                     FIELD_LEN(header_checksum))) {
    debug("VerifyKernelHeader: Header length mismatch\n");
    return VERIFY_KERNEL_INVALID_IMAGE;
  }
  *kernel_header_len = (int) header_len;

  /* Verify if the hash of the header is correct. */
  header_checksum = DigestBuf(header_blob,
                              header_len - FIELD_LEN(header_checksum),
                              SHA512_DIGEST_ALGORITHM);
  if (SafeMemcmp(header_checksum,
                 header_blob + (base_header_checksum_offset +
                                kernel_sign_key_len),
                 FIELD_LEN(header_checksum))) {
    Free(header_checksum);
    debug("VerifyKernelHeader: Invalid header hash\n");
    return VERIFY_KERNEL_INVALID_IMAGE;
  }
  Free(header_checksum);

  /* Verify kernel key signature unless we are in dev mode. */
  if (!dev_mode) {
    if (!RSAVerifyBinary_f(firmware_key_blob, NULL,  /* Key to use */
                           header_blob,  /* Data to verify */
                           header_len, /* Length of data */
                           header_blob + header_len,  /* Expected Signature */
                           firmware_sign_algorithm))
      return VERIFY_KERNEL_KEY_SIGNATURE_FAILED;
  }
  return 0;
}

int VerifyKernelConfig(RSAPublicKey* kernel_sign_key,
                       const uint8_t* config_blob,
                       int algorithm,
                       uint64_t* kernel_len) {
  uint64_t len;
  if (!RSAVerifyBinary_f(NULL, kernel_sign_key,  /* Key to use */
                         config_blob,  /* Data to verify */
                         KERNEL_CONFIG_FIELD_LEN,  /* Length of data */
                         config_blob + KERNEL_CONFIG_FIELD_LEN,  /* Expected
                                                                  * Signature */
                         algorithm))
    return VERIFY_KERNEL_CONFIG_SIGNATURE_FAILED;

  Memcpy(&len,
         config_blob + (FIELD_LEN(kernel_version) + FIELD_LEN(options.version) +
                              FIELD_LEN(options.cmd_line)),
         sizeof(len));
  *kernel_len = len;
  return 0;
}

int VerifyKernelData(RSAPublicKey* kernel_sign_key,
                     const uint8_t* kernel_config_start,
                     const uint8_t* kernel_data_start,
                     uint64_t kernel_len,
                     int algorithm) {
  int signature_len = siglen_map[algorithm];
  uint8_t* digest;
  DigestContext ctx;

  /* Since the kernel signature is computed over the kernel version, options
   * and data, which does not form a contiguous region of memory, we calculate
   * the message digest ourselves. */
  DigestInit(&ctx, algorithm);
  DigestUpdate(&ctx, kernel_config_start, KERNEL_CONFIG_FIELD_LEN);
  DigestUpdate(&ctx, kernel_data_start + signature_len, kernel_len);
  digest = DigestFinal(&ctx);
  if (!RSAVerifyBinaryWithDigest_f(
          NULL, kernel_sign_key,  /* Key to use. */
          digest, /* Digest of the data to verify. */
          kernel_data_start,  /* Expected Signature */
          algorithm)) {
    Free(digest);
    return VERIFY_KERNEL_SIGNATURE_FAILED;
  }
  Free(digest);
  return 0;
}

int VerifyKernel(const uint8_t* firmware_key_blob,
                 const uint8_t* kernel_blob,
                 const int dev_mode) {
  int error_code;
  int firmware_sign_algorithm;  /* Firmware signing key algorithm. */
  int kernel_sign_algorithm;  /* Kernel Signing key algorithm. */
  RSAPublicKey* kernel_sign_key;
  int kernel_sign_key_len, kernel_key_signature_len, kernel_signature_len,
      header_len;
  uint64_t kernel_len;
  const uint8_t* header_ptr;  /* Pointer to header. */
  const uint8_t* kernel_sign_key_ptr;  /* Pointer to signing key. */
  const uint8_t* config_ptr;  /* Pointer to kernel config block. */
  const uint8_t* kernel_ptr;  /* Pointer to kernel signature/data. */

  /* Note: All the offset calculations are based on struct FirmwareImage which
   * is defined in include/firmware_image.h. */

  /* Compare magic bytes. */
  if (SafeMemcmp(kernel_blob, KERNEL_MAGIC, KERNEL_MAGIC_SIZE))
    return VERIFY_KERNEL_WRONG_MAGIC;
  header_ptr = kernel_blob + KERNEL_MAGIC_SIZE;

  /* Only continue if header verification succeeds. */
  if ((error_code = VerifyKernelHeader(firmware_key_blob, header_ptr, dev_mode,
                                       &firmware_sign_algorithm,
                                       &kernel_sign_algorithm, &header_len))) {
    debug("VerifyKernel: Kernel header verification failed.\n");
    return error_code;  /* AKA jump to recovery. */
  }
  /* Parse signing key into RSAPublicKey structure since it is required multiple
   * times. */
  kernel_sign_key_len = RSAProcessedKeySize(kernel_sign_algorithm);
  kernel_sign_key_ptr = header_ptr + (FIELD_LEN(header_version) +
                                      FIELD_LEN(header_len) +
                                      FIELD_LEN(firmware_sign_algorithm) +
                                      FIELD_LEN(kernel_sign_algorithm) +
                                      FIELD_LEN(kernel_key_version));
  kernel_sign_key = RSAPublicKeyFromBuf(kernel_sign_key_ptr,
                                        kernel_sign_key_len);
  kernel_signature_len = siglen_map[kernel_sign_algorithm];
  kernel_key_signature_len = siglen_map[firmware_sign_algorithm];

  /* Only continue if config verification succeeds. */
  config_ptr = (header_ptr + header_len + kernel_key_signature_len);
  if ((error_code = VerifyKernelConfig(kernel_sign_key, config_ptr,
                                       kernel_sign_algorithm,
                                       &kernel_len))) {
    RSAPublicKeyFree(kernel_sign_key);
    return error_code;  /* AKA jump to recovery. */
  }
  /* Only continue if kernel data verification succeeds. */
  kernel_ptr = (config_ptr +
                KERNEL_CONFIG_FIELD_LEN +  /* Skip config block/signature. */
                kernel_signature_len);

  if ((error_code = VerifyKernelData(kernel_sign_key, config_ptr, kernel_ptr,
                                     kernel_len,
                                     kernel_sign_algorithm))) {
    RSAPublicKeyFree(kernel_sign_key);
    return error_code;  /* AKA jump to recovery. */
  }
  RSAPublicKeyFree(kernel_sign_key);
  return 0;  /* Success! */
}

uint32_t GetLogicalKernelVersion(uint8_t* kernel_blob) {
  uint8_t* kernel_ptr;
  uint16_t kernel_key_version;
  uint16_t kernel_version;
  uint16_t firmware_sign_algorithm;
  uint16_t kernel_sign_algorithm;
  int kernel_key_signature_len;
  int kernel_sign_key_len;
  kernel_ptr = kernel_blob + (FIELD_LEN(magic) +
                              FIELD_LEN(header_version) +
                              FIELD_LEN(header_len));
  Memcpy(&firmware_sign_algorithm, kernel_ptr, sizeof(firmware_sign_algorithm));
  kernel_ptr += FIELD_LEN(firmware_sign_algorithm);
  Memcpy(&kernel_sign_algorithm, kernel_ptr, sizeof(kernel_sign_algorithm));
  kernel_ptr += FIELD_LEN(kernel_sign_algorithm);
  Memcpy(&kernel_key_version, kernel_ptr, sizeof(kernel_key_version));

  if (firmware_sign_algorithm >= kNumAlgorithms)
    return 0;
  if (kernel_sign_algorithm >= kNumAlgorithms)
    return 0;
  kernel_key_signature_len = siglen_map[firmware_sign_algorithm];
  kernel_sign_key_len = RSAProcessedKeySize(kernel_sign_algorithm);
  kernel_ptr += (FIELD_LEN(kernel_key_version) +
                 kernel_sign_key_len +
                 FIELD_LEN(header_checksum) +
                 kernel_key_signature_len);
  Memcpy(&kernel_version, kernel_ptr, sizeof(kernel_version));
  return CombineUint16Pair(kernel_key_version, kernel_version);
}

int VerifyKernelDriver_f(uint8_t* firmware_key_blob,
                         kernel_entry* kernelA,
                         kernel_entry* kernelB,
                         int dev_mode) {
  int i;
  /* Contains the logical kernel version (32-bit) which is calculated as
   * (kernel_key_version << 16 | kernel_version) where
   * [kernel_key_version], [firmware_version] are both 16-bit.
   */
  uint32_t kernelA_lversion, kernelB_lversion;
  uint32_t min_lversion;  /* Minimum of kernel A and kernel B lversion. */
  uint32_t stored_lversion;  /* Stored logical version in the TPM. */
  kernel_entry* try_kernel[2];  /* Kernel in try order. */
  int try_kernel_which[2];  /* Which corresponding kernel in the try order */
  uint32_t try_kernel_lversion[2];  /* Their logical versions. */

  /* [kernel_to_boot] will eventually contain the boot path to follow
   * and is returned to the caller. Initially, we set it to recovery. If
   * a valid bootable kernel is found, it will be set to that. */
  int kernel_to_boot = BOOT_KERNEL_RECOVERY_CONTINUE;


  /* The TPM must already have be initialized, so no need to call SetupTPM(). */

  /* We get the key versions by reading directly from the image blobs without
   * any additional (expensive) sanity checking on the blob since it's faster to
   * outright reject a kernel with an older kernel key version. A malformed
   * or corrupted kernel blob will still fail when VerifyKernel() is called
   * on it.
   */
  kernelA_lversion = GetLogicalKernelVersion(kernelA->kernel_blob);
  kernelB_lversion = GetLogicalKernelVersion(kernelB->kernel_blob);
  min_lversion  = Min(kernelA_lversion, kernelB_lversion);
  stored_lversion = CombineUint16Pair(GetStoredVersion(KERNEL_KEY_VERSION),
                                      GetStoredVersion(KERNEL_VERSION));

  /* TODO(gauravsh): The kernel entries kernelA and kernelB come from the
   * partition table - verify its signature/checksum before proceeding
   * further. */

  /* The logic for deciding which kernel to boot from is taken from the
   * the Chromium OS Drive Map design document.
   *
   * We went to consider the kernels in their according to their boot
   * priority attribute value.
   */

  if (kernelA->boot_priority >= kernelB->boot_priority) {
    try_kernel[0] = kernelA;
    try_kernel_which[0] = BOOT_KERNEL_A_CONTINUE;
    try_kernel_lversion[0] = kernelA_lversion;
    try_kernel[1] = kernelB;
    try_kernel_which[1] = BOOT_KERNEL_B_CONTINUE;
    try_kernel_lversion[1] = kernelB_lversion;
  } else {
    try_kernel[0] = kernelB;
    try_kernel_which[0] = BOOT_KERNEL_B_CONTINUE;
    try_kernel_lversion[0] = kernelB_lversion;
    try_kernel[1] = kernelA;
    try_kernel_which[1] = BOOT_KERNEL_A_CONTINUE;
    try_kernel_lversion[1] = kernelA_lversion;
  }

  /* TODO(gauravsh): Changes to boot_tries_remaining and boot_priority
   * below should be propagated to partition table. This will be added
   * once the firmware parition table parsing code is in. */
  for (i = 0; i < 2; i++) {
    if ((try_kernel[i]->boot_success_flag ||
         try_kernel[i]->boot_tries_remaining) &&
        (VERIFY_KERNEL_SUCCESS == VerifyKernel(firmware_key_blob,
                                               try_kernel[i]->kernel_blob,
                                               dev_mode))) {
      if (try_kernel[i]->boot_tries_remaining > 0)
        try_kernel[i]->boot_tries_remaining--;
      if (stored_lversion > try_kernel_lversion[i])
        continue;  /* Rollback: I am afraid I can't let you do that Dave. */
      if (i == 0 && (stored_lversion < try_kernel_lversion[1])) {
        /* The higher priority kernel is valid and bootable, See if we
         * need to update the stored version for rollback prevention. */
        if (VERIFY_KERNEL_SUCCESS == VerifyKernel(firmware_key_blob,
                                                  try_kernel[1]->kernel_blob,
                                                  dev_mode)) {
          WriteStoredVersion(KERNEL_KEY_VERSION,
                             (uint16_t) (min_lversion >> 16));
          WriteStoredVersion(KERNEL_VERSION,
                             (uint16_t) (min_lversion & 0xFFFF));
          stored_lversion = min_lversion;  /* Update stored version as it's
                                            * used later. */
        }
      }
      kernel_to_boot = try_kernel_which[i];
      break;  /* We found a valid kernel. */
    }
    try_kernel[i]->boot_priority = 0;
    }  /* for loop. */

  /* Lock Kernel TPM rollback indices from further writes.
   * TODO(gauravsh): Figure out if these can be combined into one
   * 32-bit location since we seem to always use them together. This can help
   * us minimize the number of NVRAM writes/locks (which are limited over flash
   * memory lifetimes.
   */
  LockStoredVersion(KERNEL_KEY_VERSION);
  LockStoredVersion(KERNEL_VERSION);
  return kernel_to_boot;
}