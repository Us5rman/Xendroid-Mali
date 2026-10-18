// Mbed TLS modules xenia builds. Each one needs its library/*.c file listed in
// the mbedtls target in third_party/CMakeLists.txt.
#ifndef XENIA_MBEDTLS_CONFIG_H_
#define XENIA_MBEDTLS_CONFIG_H_

#define MBEDTLS_AES_C
// The AES tables as constant data. Otherwise aes.c fills them on the first key
// setup behind a plain flag, which another thread can see set before the tables
// (ARM64 orders neither), and the XeCryptAesEcb/Cbc exports can run on a guest
// state no key setup in this process ever built them for.
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_DES_C
#define MBEDTLS_DHM_C
#define MBEDTLS_MD5_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C

#endif  // XENIA_MBEDTLS_CONFIG_H_
