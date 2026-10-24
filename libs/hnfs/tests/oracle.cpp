#include <hnfs.hpp>
#include "console.hpp"
#include <charconv>
#include <array>
#include <iostream>

/* Hex bridge for test-only independent GNU cpio verification. */
int main(int argc,char** argv) {
    configureFailures();
    try {
        if (argc!=3) { throw std::invalid_argument("oracle format encode|decode"); }
        int id=0; const std::string text=argv[1]; const auto parsed=std::from_chars(text.data(),text.data()+text.size(),id);
        if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) { throw std::invalid_argument("bad format number"); }
        std::string line;
        while (std::getline(std::cin,line)) {
            try {
                if (line.size()%2) { throw std::invalid_argument("bad hex input"); }
                hnfs::SBytes input;
                for (std::size_t i=0;i<line.size();i+=2) {
                    unsigned byte=0; const auto result=std::from_chars(line.data()+i,line.data()+i+2,byte,16);
                    if (result.ec!=std::errc{} || result.ptr!=line.data()+i+2) { throw std::invalid_argument("bad hex byte"); }
                    input.push_back(static_cast<std::uint8_t>(byte));
                }
                hnfs::SBytes output;
                if (std::string(argv[2])=="encode") {
                    auto device=std::make_shared<hnfs::CMemoryDevice>(input.size()+8192);
                    auto builder=hnfs::IArchiveBuilder::create(device,{static_cast<hnfs::EFormat>(id)});
                    builder->beginEntry({"docs",hnfs::ENODE_DIRECTORY,0,0755});
                    builder->beginEntry({"docs/data.bin",hnfs::ENODE_FILE,input.size()}); builder->write(input); builder->finish();
                    const auto padded=(builder->usedBytes()+511)/512*512; output=device->readBytes(0,static_cast<std::size_t>(padded));
                } else {
                    auto reader=hnfs::IArchiveReader::open(std::make_shared<hnfs::CMemoryDevice>(input)); bool selected=false;
                    while (auto entry=reader->next()) {
                        if (entry->kind!=hnfs::ENODE_FILE || selected) { continue; } selected=true;
                        std::array<std::uint8_t,65536> buffer{};
                        for (;;) { const auto count=reader->read(buffer); if (!count) { break; } output.insert(output.end(),buffer.begin(),buffer.begin()+count); }
                    }
                }
                constexpr char digits[]="0123456789abcdef";
                for (auto byte:output) { std::cout<<digits[byte>>4]<<digits[byte&15]; } std::cout<<'\n';
            } catch (const std::exception& error) { std::cout<<"ERROR "<<error.what()<<'\n'; }
        }
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
