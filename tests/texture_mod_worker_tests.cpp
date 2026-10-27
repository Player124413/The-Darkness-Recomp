#include "renderer/d3d11/texture_mods.h"
#include "renderer/d3d11/texture_mod_file_io.h"
#include "renderer/engine/simple_mesh.h"
#include "renderer/engine/texture_mod_dds.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <new>
#include <stdexcept>
#include <thread>

using namespace DarkRecomp;
namespace Mods=DarkRecomp::TextureMods;
template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
static unsigned checks=0;
static void check(bool ok,const char* why) {++checks; if(!ok)throw std::runtime_error(why);}
static void writeNew(const std::filesystem::path& path,const std::vector<uint8_t>& bytes) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(file!=INVALID_HANDLE_VALUE,"fixture output must be new");
    DWORD written=0; const auto ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr);
    CloseHandle(file); check(ok && written==bytes.size(),"cannot write synthetic fixture");
}
static std::vector<uint8_t> read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary | std::ios::ate);
    check(file && file.tellg()>=0 && file.tellg()<4096,"synthetic fixture extent invalid");
    std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg())); file.seekg(0);
    check(bool(file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()))),"cannot read synthetic fixture");
    return bytes;
}
static std::shared_ptr<ColorImage> image(unsigned width=2,unsigned height=2,uint8_t seed=1) {
    auto result=std::make_shared<ColorImage>(); result->width=width; result->height=height;
    result->authoredMips=true; result->sourceCodec=2; result->pixels.resize(size_t(width)*height*4);
    for(size_t i=0;i<result->pixels.size();++i)result->pixels[i]=uint8_t(seed+uint8_t(i));
    return result;
}
template<class Condition> static void until(Condition condition,const char* why) {
    const auto start=GetTickCount64();
    while(!condition()) {check(GetTickCount64()-start<5000,why); std::this_thread::sleep_for(std::chrono::milliseconds(2));}
}
static std::string sha(std::span<const uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"test hash provider unavailable");
    std::array<uint8_t,32> digest{};
    const auto status=BCryptHash(algorithm,nullptr,0,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),digest.data(),ULONG(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm,0); check(status>=0,"test hash failed");
    constexpr char digits[]="0123456789abcdef"; std::string result;
    for(auto b:digest) {result+=digits[b>>4]; result+=digits[b&15];} return result;
}
static void verifyView(ID3D11Device* device,ID3D11DeviceContext* context,const Mods::Replacement& replacement,
        std::span<const uint8_t> expectedDds) {
    Mods::DdsImage expected; check(!Mods::parseDds(expectedDds,expected),"bad expected DDS");
    Ptr<ID3D11Resource> resource; replacement.view->GetResource(&resource);
    Ptr<ID3D11Texture2D> texture; check(SUCCEEDED(resource.As(&texture)),"replacement is not2D");
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    check(desc.Usage==D3D11_USAGE_IMMUTABLE && desc.Width==expected.width && desc.Height==expected.height &&
        desc.MipLevels==expected.levels.size(),"worker GPU descriptor differs");
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Ptr<ID3D11Texture2D> staging;
    check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"cannot create test readback");
    context->CopyResource(staging.Get(),texture.Get());
    for(unsigned mip=0;mip<desc.MipLevels;++mip) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(context->Map(staging.Get(),mip,D3D11_MAP_READ,0,&mapped)),"cannot map worker GPU pixels");
        const unsigned width=(std::max)(1u,desc.Width>>mip),height=(std::max)(1u,desc.Height>>mip);
        bool equal=true;
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width*4;++x)
            equal=equal && static_cast<const uint8_t*>(mapped.pData)[size_t(y)*mapped.RowPitch+x]==expected.levels[mip][size_t(y)*width*4+x];
        context->Unmap(staging.Get(),mip); check(equal,"worker GPU mip pixels differ");
    }
}
static void identityAndExport() {
    check(!Mods::makeWorker(nullptr,{}),"default-off created a worker/device dependency");
    auto source=image(3,2,0);
    check(Mods::contentKey(*source)=="032d050984247327dc1369630ec346f231a6a13d7d918d23616df5bc63303fbf","base content key differs from independent Node SHA256");
    const auto exported=Mods::sourceDds(*source);
    check(exported.size()==156 && sha(exported)=="a8348105710dc63afe2d825e6c9eeeab3d083ddd957f08bffb09562a58741135",
        "C++ dump differs from independently qualified Rust CLI DDS bytes");
    source->mips.push_back({10,11,12,13});
    check(Mods::contentKey(*source)=="ef4e137dfd254325cb93d02a9088f677f5f570dd177922d18ce3844881f07a35","authored mip count/bytes not included in key");
    check(Mods::sourceDds(*source)==exported,"existing authored mip was changed");
    auto other=std::make_shared<ColorImage>(*source); other->pixels[0]^=1;
    check(Mods::contentKey(*other)!=Mods::contentKey(*source),"pixel change retained key");
    for(unsigned kind=0;kind<5;++kind) {
        auto bad=*source;
        if(kind==0)bad.firstMip=1;
        if(kind==1)bad.faces=6;
        if(kind==2)bad.promptOrigin=1;
        if(kind==3)bad.generatedSubtitleFont=true;
        if(kind==4)bad.authoredMips=false;
        check(!Mods::eligibleSource(bad),"forbidden texture source accepted");
    }
}
static void pendingIoCancellation() {
    struct OwnedHandle {
        HANDLE value;
        ~OwnedHandle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    } stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    check(stop.value!=nullptr,"cannot create cancellation fixture event");
    const auto name=L"\\\\.\\pipe\\DarkRecompTextureContract-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    OwnedHandle server{CreateNamedPipeW(name.c_str(),PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,1,0,1024,0,nullptr)};
    check(server.value!=INVALID_HANDLE_VALUE,"cannot create owned cancellation pipe");
    OwnedHandle client{CreateFileW(name.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr)};
    check(client.value!=INVALID_HANDLE_VALUE,"cannot connect owned cancellation client");
    // A client may connect between CreateNamedPipe and ConnectNamedPipe. Here
    // that ordering is intentional, so no pending connect can outlive its stack.
    OVERLAPPED connect{};
    const BOOL connected=ConnectNamedPipe(server.value,&connect);
    check(connected || GetLastError()==ERROR_PIPE_CONNECTED,"owned pipe did not connect synchronously");
    std::atomic<bool> returned=false,result=true;
    std::array<uint8_t,16> bytes{};
    std::thread reader([&]{result.store(Mods::Detail::transfer(server.value,stop.value,false,bytes.data(),bytes.size())); returned.store(true);});
    struct Join {
        HANDLE stop; std::thread& reader;
        ~Join(){SetEvent(stop); if(reader.joinable())reader.join();}
    } join{stop.value,reader};
    until([&]{BOOL pending=FALSE; check(GetThreadIOPendingFlag(reader.native_handle(),&pending)!=FALSE,"cannot inspect owned pending I/O"); return pending!=FALSE;},
        "fixture never issued its real pending read");
    check(!returned.load(),"pipe read did not block without bytes");
    SetEvent(stop.value);
    until([&]{return returned.load();},"stop event did not cancel/drain the pending read");
    reader.join();
    check(!result.load(),"cancelled pending read reported success");
    check(std::all_of(bytes.begin(),bytes.end(),[](uint8_t b){return b==0;}),"cancelled read changed destination");
}
static void replacementAndOwnership(ID3D11Device* device,ID3D11DeviceContext* context,const std::filesystem::path& root) {
    const auto pack=root/L"pack"; check(std::filesystem::create_directory(pack),"pack fixture exists");
    alignas(ColorImage) std::array<std::byte,sizeof(ColorImage)> storage{};
    auto placed=[&](uint8_t seed) {
        auto* at=std::construct_at(reinterpret_cast<ColorImage*>(storage.data()));
        *at=*image(2,2,seed);
        return std::shared_ptr<ColorImage>(at,[](ColorImage* p){std::destroy_at(p);});
    };
    auto source=placed(1); const auto original=source->pixels;
    const auto pixels=Mods::sourceDds(*image(4,4,91));
    writeNew(pack/(Mods::contentKey(*source)+".dds"),pixels);
    Mods::Config config; config.packDirectory=pack; config.queueEntries=1; config.cacheEntries=1;
    auto worker=Mods::makeWorker(device,config);
    check(!worker->request(source).view,"first request blocked for immediate replacement");
    until([&]{return worker->stats().finished>=1;},"worker did not finish valid DDS");
    auto retained=worker->request(source);
    check(retained.view && retained.width==4 && retained.height==4 && retained.mipLevels==3,"valid scaled replacement absent");
    check(source->pixels==original && worker->stats().gpuBytes==84,"source mutation or wrong cached GPU byte count");
    verifyView(device,context,retained,pixels);
    std::weak_ptr<const ColorImage> old=source; const auto address=source.get(); source.reset();
    until([&]{return old.expired();},"job retained source after completion");
    auto reused=placed(55); check(reused.get()==address,"fixture did not reuse address");
    check(!worker->request(reused).view,"new owner at reused address inherited replacement");
    until([&]{return worker->stats().finished>=2;},"reused-address lookup did not finish");
    check(!worker->request(reused).view && worker->stats().gpuBytes==0,"missing new key retained old view");
    worker->stop(); worker->stop();
    check(!worker->request(reused).view && worker->stats().pending==0 && worker->stats().queuedSourceBytes==0,"stopped worker accepted input");
    verifyView(device,context,retained,pixels); // Eviction/stop cannot invalidate a retained frame view.
    reused.reset();
}
static void rejectedFilesAndLimits(ID3D11Device* device,const std::filesystem::path& root) {
    for(unsigned kind=0;kind<3;++kind) {
        const auto directory=root/(L"rejected-"+std::to_wstring(kind));
        check(std::filesystem::create_directory(directory),"rejection fixture exists");
        auto source=image(2,2,uint8_t(100+kind));
        auto data=kind==0?std::vector<uint8_t>(128,0):Mods::sourceDds(*image(kind==1?4u:8u,kind==1?4u:4u,19));
        writeNew(directory/(Mods::contentKey(*source)+".dds"),data);
        Mods::Config config; config.packDirectory=directory;
        if(kind==1)config.gpuBytes=16;
        auto worker=Mods::makeWorker(device,config); worker->request(source);
        until([&]{return worker->stats().finished>=1;},"rejection worker did not finish");
        check(!worker->request(source).view && worker->stats().rejected==1 && worker->stats().gpuBytes==0,
            "malformed/overbudget/aspect DDS did not retain original");
        worker->stop();
    }
    Mods::Config queueLimited; queueLimited.packDirectory=root/L"pack"; queueLimited.sourceQueueBytes=1;
    auto bounded=Mods::makeWorker(device,queueLimited); auto source=image();
    bounded->request(source);
    check(bounded->stats().pending==0 && bounded->stats().queuedSourceBytes==0,"source byte queue limit exceeded");
    bounded->stop();
    Mods::Config pending; pending.packDirectory=root/L"pack"; pending.queueEntries=1; pending.cacheEntries=2;
    auto stopped=Mods::makeWorker(device,pending); stopped->request(source);
    const auto start=GetTickCount64(); stopped->stop(); stopped->stop();
    check(GetTickCount64()-start<5000 && stopped->stats().pending==0,"queued-work shutdown did not join");
}
static void dumpAndNoOverwrite(ID3D11Device* device,const std::filesystem::path& root) {
    auto source=image(3,2,0); const auto key=Mods::contentKey(*source); const auto expected=Mods::sourceDds(*source);
    Mods::Config config; config.dumpDirectory=root/L"dump-new";
    auto worker=Mods::makeWorker(device,config); worker->request(source);
    until([&]{return worker->stats().finished>=1;},"dump did not finish"); worker->stop();
    check(read(config.dumpDirectory/(key+".dds"))==expected && worker->stats().dumpedBytes==156,"dump bytes/count differ");
    bool refused=false; try {auto duplicate=Mods::makeWorker(device,config);}catch(const std::invalid_argument&){refused=true;}
    check(refused,"existing dump directory accepted");
    config.dumpDirectory=root/L"dump-preserve"; auto preserve=Mods::makeWorker(device,config);
    const std::vector<uint8_t> sentinel{77,88,99}; writeNew(config.dumpDirectory/(key+".dds"),sentinel);
    preserve->request(source); until([&]{return preserve->stats().finished>=1;},"existing dump check did not finish");
    preserve->stop();
    check(read(config.dumpDirectory/(key+".dds"))==sentinel && preserve->stats().dumpedBytes==0,"existing dump output overwritten/count wrong");
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"provide the private fixture parent directory");
        const auto parent=std::filesystem::absolute(argv[1]); std::filesystem::create_directories(parent);
        const auto root=parent/(L"run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        check(std::filesystem::create_directory(root),"fixture root exists");
        identityAndExport();
        pendingIoCancellation();
        Ptr<ID3D11Device> device; Ptr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL level{};
        const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_0};
        check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,requested,1,D3D11_SDK_VERSION,
            &device,&level,&context)),"cannot create hidden WARP contract device");
        replacementAndOwnership(device.Get(),context.Get(),root);
        rejectedFilesAndLimits(device.Get(),root);
        dumpAndNoOverwrite(device.Get(),root);
        std::printf("TextureModWorkerContract PASS checks=%u; hidden WARP/synthetic files only; no guest/game/performance acceptance.\n",checks);
        std::printf("Fixtures preserved: %ls\n",root.c_str());
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"TextureModWorkerContract FAIL: %s\n",e.what()); return 1;}
}
