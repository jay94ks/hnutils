#ifndef __SRC_HNCOMP_ARCHIVE_CRYPTO_HPP__
#define __SRC_HNCOMP_ARCHIVE_CRYPTO_HPP__

#include "archive_internal.hpp"
#include <certpp/crypto/hasher.hpp>
#include <certpp/crypto/hmac.hpp>
#include <certpp/crypto/sym.hpp>

namespace hncomp {
namespace detail {

void eraseSecret(SByteSpan bytes) noexcept;
bool equalSecret(SReadOnlyByteSpan left, SReadOnlyByteSpan right) noexcept;
void archiveRandom(SByteSpan bytes);

/** Private adapters: all cryptographic primitives are libcertpp public APIs. */
class ArchiveHash {
private:
    certpp::crypto::IHasherPtr _hash;
public:
    explicit ArchiveHash(bool sha256 = false);
    ArchiveHash(const ArchiveHash&)=delete;
    ArchiveHash& operator=(const ArchiveHash&)=delete;
    void update(SReadOnlyByteSpan bytes);
    SBytes finish() const;
};

class ArchiveHmac {
private:
    mutable certpp::crypto::CHmac _mac;
public:
    explicit ArchiveHmac(SReadOnlyByteSpan key);
    void update(SReadOnlyByteSpan bytes);
    SBytes finish() const;
};

SBytes archivePbkdf2(SReadOnlyByteSpan password, SReadOnlyByteSpan salt, unsigned iterations, std::size_t size);

/** ZIP-specific little-endian counter framing over libcertpp AES. */
class ZipAesCtr {
private:
    certpp::crypto::ISymmetricTransformerPtr _cipher;
    std::array<std::uint8_t,16> _counter{}, _stream{}, _previous{};
    unsigned _position = 16;
public:
    explicit ZipAesCtr(SReadOnlyByteSpan key);
    ~ZipAesCtr();
    void process(SByteSpan bytes);
};

/** Authenticate the whole ciphertext before emitting any AES plaintext. */
void archiveDecryptZip(const ArchiveStore& source, std::uint64_t offset, std::uint64_t size,
    const std::string& password, EZipEncryption encryption, const SArchiveSink& sink);

class ZipEncryptor {
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
public:
    ZipEncryptor(const SZipOptions& options, SArchiveSink sink);
    ~ZipEncryptor();
    void process(SReadOnlyByteSpan bytes);
    void finish();
};

} // namespace detail
} // namespace hncomp
#endif
