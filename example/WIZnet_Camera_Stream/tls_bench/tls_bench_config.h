/**
 * @file    tls_bench_config.h
 * @brief   mbedTLS configuration for tls_bench only.
 *
 * Deliberately NOT port/mbedtls/inc/ssl_config.h. That file is shared by every
 * image in this repository, including the LH water tank build whose Discord
 * TLS client is in the field: adding a server role or ChaCha20 there would
 * change the ciphersuites that client offers. tls_bench compiles the mbedTLS
 * sources itself with this file, and nothing else sees it.
 *
 * What is here is what an HTTPS camera would plausibly run:
 *   TLS 1.2, ECDHE-ECDSA on P-256 (an EC certificate - the RSA handshake costs
 *   1.2 s on this part, measured for OPC UA), AES-128-GCM and
 *   ChaCha20-Poly1305, session tickets so a browser's second connection resumes
 *   instead of running ECDHE again.
 *
 * A configuration file REPLACES mbedtls_config.h rather than extending it, so
 * every option has to be named here. MBEDTLS_ECP_NIST_OPTIM in particular was
 * missing from another project's file once and cost a 6.4x slower ECC.
 */

#ifndef TLS_BENCH_CONFIG_H
#define TLS_BENCH_CONFIG_H

/* ---- platform ---- */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY            /* counted calloc/free, see tls_bench.c */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ERROR_C                    /* readable failures */
#define MBEDTLS_ENTROPY_HARDWARE_ALT       /* RP2350 TRNG through pico_rand */

/* ---- symmetric ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
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
#define MBEDTLS_SSL_CLI_C                  /* the in-memory peer */
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_TICKET_C
#define MBEDTLS_SSL_SERVER_NAME_INDICATION

#endif /* TLS_BENCH_CONFIG_H */
