/**
    \file ssl_config.h

    \brief Configuration options (set of defines)

    This set of compile-time options may be used to enable
    or disable features selectively, and reduce the global
    memory footprint.
*/
/*
    Copyright (C) 2006-2018, ARM Limited, All Rights Reserved
    SPDX-License-Identifier: Apache-2.0

    Licensed under the Apache License, Version 2.0 (the "License"); you may
    not use this file except in compliance with the License.
    You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
    WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

    This file is part of mbed TLS (https://tls.mbed.org)
*/

#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#include "mbedtls/check_config.h"

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_DEPRECATE)
#define _CRT_SECURE_NO_DEPRECATE 1
#endif

// #define MBEDTLS_HAVE_ASM
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_REMOVE_ARC4_CIPHERSUITES
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#define MBEDTLS_PK_PARSE_EC_EXTENDED
#define MBEDTLS_ERROR_STRERROR_DUMMY
#define MBEDTLS_GENPRIME

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_PK_RSA_ALT_SUPPORT
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

#define MBEDTLS_SSL_ALL_ALERT_MESSAGES
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_ALPN
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_AESNI_C
#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

#define MBEDTLS_BASE64_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_DHM_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_OID_C

#define MBEDTLS_PEM_PARSE_C

#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_SHA256_C

/* ---- Discord / Google Trust Services chain -------------------------------
 *
 * discord.com presents an ECDSA chain:
 *     discord.com  <-  WE1  <-  GTS Root R4
 *
 * Every line below exists for some part of verifying that. Without them the
 * handshake fails at certificate parsing or at signature verification, which
 * from the outside looks the same as a network fault - so they are listed with
 * their reasons rather than as a block of defines.
 */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED        /* P-256: ECDHE key exchange */
#define MBEDTLS_ECDH_C                          /* ECDHE itself */
#define MBEDTLS_ECDSA_C                         /* verifying an EC chain */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED /* servers with EC certs */
#define MBEDTLS_ENTROPY_C                       /* CTR-DRBG depends on it */
#define MBEDTLS_SHA1_C                          /* some chains still sign with it */
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA384_C                        /* GTS Root R4 is ecdsa-with-SHA384 */

#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_VERSION_C

#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRL_PARSE_C
#define MBEDTLS_X509_CSR_PARSE_C

#define MBEDTLS_XTEA_C

#define MBEDTLS_MPI_MAX_SIZE 1024      /**< Maximum number of bytes for usable MPIs. */
#define MBEDTLS_ENTROPY_MAX_SOURCES 10 /**< Maximum number of sources supported */
#if defined(MBEDTLS_USER_CONFIG_FILE)
#include MBEDTLS_USER_CONFIG_FILE
#endif

#endif /* MBEDTLS_CONFIG_H */