#include <hnfs.hpp>
#include "console.hpp"
#include <iostream>
#include <string>
namespace {
using namespace hnfs;
void hex(SReadOnlyByteSpan bytes) { constexpr char digits[]="0123456789abcdef"; for (auto b:bytes) { std::cout<<digits[b>>4]<<digits[b&15]; } std::cout<<'\n'; }
}
int main(int argc,char** argv) {
    configureFailures();
    try {
        if (argc<3) { throw std::invalid_argument("disk oracle: tree|virtual|compact|partitions|write-partitions path ..."); }
        const std::string command=argv[1];
        if (command=="virtual" && argc==5) { auto disk=CVirtualDisk::openFile(argv[2]); hex(disk->readBytes(std::stoull(argv[3]),std::stoul(argv[4]))); }
        else if (command=="compact" && argc==4) { auto disk=CVirtualDisk::openFile(argv[2]); auto result=disk->compactToFile(argv[3],{true,{8192}}); std::cout<<result.virtualBytes<<' '<<result.previousImageBytes<<' '<<result.imageBytes<<' '<<result.retainedBlocks<<' '<<result.removedBlocks<<'\n'; }
        else if (command=="tree" && argc==5) {
            auto tree=COpticalFileSystem::open(CFileDevice::open(argv[2],false,{8192}),{},static_cast<EFormat>(std::stoi(argv[3])));
            auto file=tree->openFile(argv[4]); if (file->size()>1024*1024) { throw std::runtime_error("oracle file exceeds test bound"); } SBytes b(static_cast<std::size_t>(file->size())); file->readAt(0,b); hex(b);
        }
        else if (command=="partitions" && argc==4) {
            SPartitionOpenOptions o; o.logicalSectorSize=static_cast<std::uint32_t>(std::stoul(argv[3])); auto table=CPartitionTable::open(CFileDevice::open(argv[2]),o);
            for (const auto& p:table->list(0)) { std::cout<<p.index<<' '<<p.firstLba<<' '<<p.sectorCount<<' '<<unsigned(p.mbrType)<<'\n'; }
        }
        else if (command=="write-partitions" && argc==4) {
            auto d=CFileDevice::create(argv[2],32*1024*1024,{8192}); SPartitionLayout l; l.scheme=std::string(argv[3])=="gpt"?EPARTITION_GPT:EPARTITION_MBR;
            l.diskGuid[0]=1; SPartition p; p.index=1; p.firstLba=2048; p.sectorCount=4096; p.mbrType=0x83; p.typeGuid={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4}; p.uniqueGuid[0]=2; p.name="hnfs-data"; l.partitions.push_back(p);
            if (l.scheme==EPARTITION_MBR) { p.index=2; p.firstLba=8192; p.sectorCount=8192; p.mbrType=15; l.partitions.push_back(p); p.index=5; p.firstLba=8193; p.sectorCount=1024; p.mbrType=0x83; p.ebrLba=8192; l.partitions.push_back(p); }
            CPartitionTable::write(d,l);
        } else { throw std::invalid_argument("bad oracle arguments"); }
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
