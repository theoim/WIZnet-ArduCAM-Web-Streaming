/**
 * @file    https_config.h
 * @brief   mbedTLS configuration for camera_stream_https.
 *
 * tls_bench_config.h without the client role. Kept apart from the repository's
 * shared port/mbedtls/inc/ssl_config.h for the same reason as the bench: the LH
 * field build's TLS client must not change what it offers.
 *
 * Choices, from tls_bench on this board (2026-10-22, 200 MHz, server side):
 *   ChaCha20-Poly1305  16 KB frame 5.0 ms     <- preferred
 *   AES-128-GCM        16 KB frame 16.1 ms    <- fallback for clients without ChaCha
 *   ECDHE-ECDSA P-256 full handshake 129 ms, ticket resumption 2.9 ms
 *   heap per connection ~37 KB (two 16 KB record buffers)
 */

#ifndef HTTPS_CONFIG_H
#define HTTPS_CONFIG_H

/* ---- platform ---- */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT       /* RP2350 TRNG through pico_rand */
#define MBEDTLS_ERROR_C

/* ---- symmetric ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_PKCS5_C                    /* PBKDF2 for stored passwords (Phase 3b) */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C

/* ---- public key ---- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_WINDOW_SIZE        6
#define MBEDTLS_ECP_FIXED_POINT_OPTIM  1
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ---- TLS ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_TICKET_C

#endif /* HTTPS_CONFIG_H */
