#ifndef __SRC_HNFS_DISK_INTERNAL_HPP__
#define __SRC_HNFS_DISK_INTERNAL_HPP__
#include "internal.hpp"
namespace hnfs::detail {
inline std::uint64_t little(SReadOnlyByteSpan b,std::size_t p,unsigned n) {
    if (p>b.size() || n>b.size()-p || n>8) { fail(EERROR_TRUNCATED,"truncated numeric field"); }
    std::uint64_t v=0; for (unsigned i=0;i<n;++i) { v|=std::uint64_t(b[p+i])<<(8*i); } return v;
}
inline std::uint64_t big(SReadOnlyByteSpan b,std::size_t p,unsigned n) {
    if (p>b.size() || n>b.size()-p || n>8) { fail(EERROR_TRUNCATED,"truncated numeric field"); }
    std::uint64_t v=0; for (unsigned i=0;i<n;++i) { v=(v<<8)|b[p+i]; } return v;
}
inline std::uint64_t multiply(std::uint64_t a,std::uint64_t b) {
    if (b && a>UINT64_MAX/b) { fail(EERROR_RANGE,"disk offset multiplication overflow"); } return a*b;
}
inline std::uint64_t add(std::uint64_t a,std::uint64_t b) {
    if (a>UINT64_MAX-b) { fail(EERROR_RANGE,"disk offset addition overflow"); } return a+b;
}
inline void utf8(std::string& out,std::uint32_t c) {
    if (!c || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) { fail(EERROR_INVALID,"invalid Unicode scalar"); }
    if (c<0x80) { out.push_back(static_cast<char>(c)); }
    else if (c<0x800) { out.push_back(static_cast<char>(0xc0|(c>>6))); out.push_back(static_cast<char>(0x80|(c&63))); }
    else if (c<0x10000) { out.push_back(static_cast<char>(0xe0|(c>>12))); out.push_back(static_cast<char>(0x80|((c>>6)&63))); out.push_back(static_cast<char>(0x80|(c&63))); }
    else { out.push_back(static_cast<char>(0xf0|(c>>18))); out.push_back(static_cast<char>(0x80|((c>>12)&63))); out.push_back(static_cast<char>(0x80|((c>>6)&63))); out.push_back(static_cast<char>(0x80|(c&63))); }
}
inline std::string utf16(SReadOnlyByteSpan b,bool be,bool terminated) {
    if (b.size()%2) { fail(EERROR_INVALID,"odd UTF-16 byte length"); } std::string out;
    for (std::size_t i=0;i<b.size();i+=2) {
        auto c=static_cast<std::uint32_t>(be?big(b,i,2):little(b,i,2));
        if (!c && terminated) {
            for (std::size_t j=i;j<b.size();++j) { if (b[j]) { fail(EERROR_INVALID,"data follows UTF-16 terminator"); } } break;
        }
        if (c>=0xd800 && c<=0xdbff) {
            i+=2; const auto low=be?big(b,i,2):little(b,i,2);
            if (low<0xdc00 || low>0xdfff) { fail(EERROR_INVALID,"invalid UTF-16 pair"); }
            c=0x10000+((c-0xd800)<<10)+static_cast<std::uint32_t>(low-0xdc00);
        }
        utf8(out,c);
    } return out;
}
inline std::uint32_t crc32(SReadOnlyByteSpan b,std::uint32_t crc=UINT32_MAX) {
    for (auto c:b) { crc^=c; for (unsigned i=0;i<8;++i) { crc=(crc>>1)^(UINT32_C(0xedb88320)&(0-(crc&1))); } } return crc;
}
} // namespace hnfs::detail
#endif
