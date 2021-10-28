#include "qemu/osdep.h"
#include "qapi/error.h"
#include "crypto/block.h"
#include "crypto/cipher.h"
#include "crypto/init.h"
#include "crypto/tlssession.h"

QCryptoBlock *qcrypto_block_open(QCryptoBlockOpenOptions *options,
                                 const char *optprefix,
                                 QCryptoBlockReadFunc readfunc,
                                 void *opaque,
                                 unsigned int flags,
                                 Error **errp)
{
    error_setg_errno(errp, ENOSYS, "No crypto");
    return NULL;
}

QCryptoBlock *qcrypto_block_create(QCryptoBlockCreateOptions *options,
                                   const char *optprefix,
                                   QCryptoBlockInitFunc initfunc,
                                   QCryptoBlockWriteFunc writefunc,
                                   void *opaque,
                                   Error **errp)
{
    error_setg_errno(errp, ENOSYS, "No crypto");
    return NULL;
}

int qcrypto_block_decrypt(QCryptoBlock *block,
                          uint64_t startsector,
                          uint8_t *buf,
                          size_t len,
                          Error **errp)
{
    error_setg_errno(errp, ENOSYS, "No crypto");
    return -1;
}

int qcrypto_block_encrypt(QCryptoBlock *block,
                          uint64_t startsector,
                          uint8_t *buf,
                          size_t len,
                          Error **errp)
{
    error_setg_errno(errp, ENOSYS, "No crypto");
    return -1;
}

void qcrypto_block_free(QCryptoBlock *block)
{
}

uint64_t qcrypto_block_get_payload_offset(QCryptoBlock *block)
{
    return 0;
}

uint64_t qcrypto_block_get_sector_size(QCryptoBlock *block)
{
    return 0;
}

bool qcrypto_block_has_format(QCryptoBlockFormat format,
                              const uint8_t *buf,
                              size_t buflen)
{
    return false;
}

QCryptoBlockInfo *qcrypto_block_get_info(QCryptoBlock *block,
                                         Error **errp)
{
    error_setg_errno(errp, ENOSYS, "No crypto");
    return NULL;
}

QCryptoTLSSession *qcrypto_tls_session_new(QCryptoTLSCreds *creds,
                                           const char *hostname,
                                           const char *aclname,
                                           QCryptoTLSCredsEndpoint endpoint,
                                           Error **errp)
{
    error_setg_errno(errp, ENOSYS, "No crypto");
    return NULL;
}

int qcrypto_tls_session_handshake(QCryptoTLSSession *sess,
                                  Error **errp)
{
    error_setg_errno(errp, ENOSYS, "No crypto");
    return -1;
}

ssize_t qcrypto_tls_session_read(QCryptoTLSSession *sess,
                                 char *buf,
                                 size_t len)
{
    return -1;
}

ssize_t qcrypto_tls_session_write(QCryptoTLSSession *sess,
                                  const char *buf,
                                  size_t len)
{
    return -1;
}

QCryptoTLSSessionHandshakeStatus
qcrypto_tls_session_get_handshake_status(QCryptoTLSSession *sess)
{
    return 0;
}

void qcrypto_tls_session_set_callbacks(QCryptoTLSSession *sess,
                                       QCryptoTLSSessionWriteFunc writeFunc,
                                       QCryptoTLSSessionReadFunc readFunc,
                                       void *opaque)
{
}

char *qcrypto_tls_session_get_peer_name(QCryptoTLSSession *sess)
{
    return NULL;
}

void qcrypto_tls_session_free(QCryptoTLSSession *sess)
{
}

int qcrypto_tls_session_check_credentials(QCryptoTLSSession *sess,
                                          Error **errp)
{
    error_setg_errno(errp, ENOSYS, "No crypto");
    return -1;
}

QCryptoCipher *qcrypto_cipher_new(QCryptoCipherAlgorithm alg,
                                  QCryptoCipherMode mode,
                                  const uint8_t *key, size_t nkey,
                                  Error **errp)
{
    error_setg_errno(errp, ENOSYS, "No crypto");
    return NULL;
}

void qcrypto_cipher_free(QCryptoCipher *cipher)
{
}

int qcrypto_cipher_encrypt(QCryptoCipher *cipher,
                           const void *in,
                           void *out,
                           size_t len,
                           Error **errp)
{
    error_setg_errno(errp, ENOSYS, "No crypto");
    return -1;
}

bool qcrypto_cipher_supports(QCryptoCipherAlgorithm alg,
                             QCryptoCipherMode mode)
{
    return false;
}

int qcrypto_init(Error **errp)
{
    return 0;
}
