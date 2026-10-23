#include "archive_crypto.hpp"
#include <certpp/crypto/pbkdf2.hpp>
#include <certpp/crypto/rng.hpp>
#include <certpp/crypto/syms/aes.hpp>
#include <certpp/utils/secure.hpp>

namespace hncomp {
namespace detail {
namespace {

certpp::SReadOnlyByteSpan inputSpan(SReadOnlyByteSpan bytes) { return {bytes.data(),bytes.size()}; }
certpp::SByteSpan outputSpan(SByteSpan bytes) { return {bytes.data(),bytes.size()}; }
void checked(certpp::ERetCode result) {
    if (result != certpp::ERET_OK) { throw std::runtime_error("libcertpp cryptographic operation failed ("+std::to_string(static_cast<int>(result))+")"); }
}
void require(bool success) { if (!success) { throw std::runtime_error("libcertpp cryptographic operation failed"); } }
SReadOnlyByteSpan passwordBytes(const std::string& password) {
    if (password.size()>4096) { limit("ZIP password exceeds 4096 bytes"); }
    return {reinterpret_cast<const std::uint8_t*>(password.data()),password.size()};
}
unsigned keyBytes(EZipEncryption mode) {
    switch (mode) { case EZIP_AES128: return 16; case EZIP_AES192: return 24; case EZIP_AES256: return 32; default: throw std::invalid_argument("unsupported ZIP encryption"); }
}
[[noreturn]] void authenticationError() { throw CCompressionError(EERROR_AUTHENTICATION,"incorrect ZIP password or damaged encrypted data"); }

/** Wipe temporary key material on normal and exceptional exits. */
struct SecretBytes {
    SBytes bytes;
    explicit SecretBytes(SBytes value) : bytes(std::move(value)) {}
    ~SecretBytes() { eraseSecret(bytes); }
};

} // namespace

void eraseSecret(SByteSpan bytes) noexcept { certpp::CSecure::zero(outputSpan(bytes)); }
bool equalSecret(SReadOnlyByteSpan left, SReadOnlyByteSpan right) noexcept { return certpp::CSecure::equals(inputSpan(left),inputSpan(right)); }
void archiveRandom(SByteSpan bytes) { checked(certpp::crypto::CRng::fill(outputSpan(bytes))); }

ArchiveHash::ArchiveHash(bool sha256) { checked(certpp::crypto::IHasher::create(sha256?certpp::crypto::EHASH_SHA256:certpp::crypto::EHASH_SHA1,_hash)); }
void ArchiveHash::update(SReadOnlyByteSpan bytes) { require(_hash->push(inputSpan(bytes))==bytes.size()); }
SBytes ArchiveHash::finish() const { SBytes result(_hash->byteWidth()); require(_hash->finish(outputSpan(result))); return result; }
ArchiveHmac::ArchiveHmac(SReadOnlyByteSpan key) { checked(_mac.reset(certpp::crypto::EHASH_SHA1,inputSpan(key))); }
void ArchiveHmac::update(SReadOnlyByteSpan bytes) { require(_mac.push(inputSpan(bytes))==bytes.size()); }
SBytes ArchiveHmac::finish() const { SBytes result(_mac.byteWidth()); require(_mac.finish(outputSpan(result))); return result; }
SBytes archivePbkdf2(SReadOnlyByteSpan password, SReadOnlyByteSpan salt, unsigned iterations, std::size_t size) {
    SecretBytes result{SBytes(size)};
    checked(certpp::crypto::CPbkdf2::derive(certpp::crypto::EHASH_SHA1,inputSpan(password),inputSpan(salt),iterations,outputSpan(result.bytes)));
    return std::move(result.bytes);
}

ZipAesCtr::ZipAesCtr(SReadOnlyByteSpan key) {
    certpp::crypto::AES algorithm; certpp::crypto::ISymmetricKeyPtr material;
    checked(algorithm.createKey(material,inputSpan(key)));
    certpp::crypto::ISymmetricContextPtr context; checked(algorithm.createContext(context,material));
    certpp::CBuffer iv(_previous.data(),_previous.size());
    if (iv.size()!=16) { throw std::bad_alloc(); }
    context->key(material,iv); context->padding(certpp::crypto::ESYMPAD_NONE);
    checked(context->createEncrypter(_cipher));
}
ZipAesCtr::~ZipAesCtr() { eraseSecret(_counter); eraseSecret(_stream); eraseSecret(_previous); }
void ZipAesCtr::process(SByteSpan bytes) {
    for (auto& byte:bytes) {
        if (_position==16) {
            unsigned i=0; while (i<16 && ++_counter[i]==0) { ++i; } if (i==16) { limit("ZIP AES counter exhausted"); }
            std::array<std::uint8_t,16> block{};
            // --> CBC(P XOR previousCipher) = AES(P). This uses only the public
            // libcertpp CBC API, with no private AesCore or duplicate AES primitive.
            for (unsigned j=0;j<16;++j) { block[j]=_counter[j]^_previous[j]; }
            auto output=outputSpan(_stream); checked(_cipher->transform(inputSpan(block),output)); require(output.size==16);
            _previous=_stream; eraseSecret(block); _position=0;
        }
        byte^=_stream[_position++];
    }
}

void archiveDecryptZip(const ArchiveStore& source,std::uint64_t offset,std::uint64_t size,
    const std::string& password,EZipEncryption encryption,const SArchiveSink& sink) {
    if (encryption==EZIP_NONE) { archiveCopy(source,offset,size,sink); return; }
    const auto keySize=keyBytes(encryption),saltSize=keySize/2;
    if (size<saltSize+12) { invalid("truncated AES ZIP encryption header"); }
    const auto header=source.bytes(offset,saltSize+2);
    SecretBytes keys(archivePbkdf2(passwordBytes(password),SReadOnlyByteSpan(header).first(saltSize),1000,2*keySize+2));
    if (!equalSecret(SReadOnlyByteSpan(keys.bytes).last(2),SReadOnlyByteSpan(header).last(2))) { authenticationError(); }
    ZipAesCtr cipher(SReadOnlyByteSpan(keys.bytes).first(keySize)); ArchiveHmac mac(SReadOnlyByteSpan(keys.bytes).subspan(keySize,keySize));
    eraseSecret(keys.bytes);
    const auto dataOffset=offset+saltSize+2,dataSize=size-saltSize-12;
    archiveCopy(source,dataOffset,dataSize,[&](auto bytes) { mac.update(bytes); }); const auto digest=mac.finish();
    if (!equalSecret(SReadOnlyByteSpan(digest).first(10),source.bytes(dataOffset+dataSize,10))) { authenticationError(); }
    archiveCopy(source,dataOffset,dataSize,[&](auto bytes) { SecretBytes plain{SBytes(bytes.begin(),bytes.end())}; cipher.process(plain.bytes); sink(plain.bytes); });
}

struct ZipEncryptor::Impl {
    SArchiveSink sink;
    std::unique_ptr<ZipAesCtr> aes;
    std::unique_ptr<ArchiveHmac> mac;
    bool finished=false;
    Impl(const SZipOptions& options,SArchiveSink output) : sink(std::move(output)) {
        if (options.encryption==EZIP_NONE) { return; }
        const auto size=keyBytes(options.encryption); SBytes header(size/2); archiveRandom(header);
        SecretBytes keys(archivePbkdf2(passwordBytes(options.password),header,1000,2*size+2));
        aes=std::make_unique<ZipAesCtr>(SReadOnlyByteSpan(keys.bytes).first(size));
        mac=std::make_unique<ArchiveHmac>(SReadOnlyByteSpan(keys.bytes).subspan(size,size));
        header.insert(header.end(),keys.bytes.end()-2,keys.bytes.end()); sink(header);
    }
};
ZipEncryptor::ZipEncryptor(const SZipOptions& options,SArchiveSink sink) : _impl(std::make_unique<Impl>(options,std::move(sink))) {}
ZipEncryptor::~ZipEncryptor()=default;
void ZipEncryptor::process(SReadOnlyByteSpan bytes) {
    if (_impl->finished) { throw std::logic_error("ZIP encryption already finished"); }
    if (!_impl->aes) { _impl->sink(bytes); return; }
    SecretBytes cipher{SBytes(bytes.begin(),bytes.end())}; _impl->aes->process(cipher.bytes); _impl->mac->update(cipher.bytes); _impl->sink(cipher.bytes);
}
void ZipEncryptor::finish() {
    if (_impl->finished) { throw std::logic_error("ZIP encryption already finished"); }
    _impl->finished=true;
    if (_impl->mac) { const auto digest=_impl->mac->finish(); _impl->sink(SReadOnlyByteSpan(digest).first(10)); }
}

} // namespace detail
} // namespace hncomp
