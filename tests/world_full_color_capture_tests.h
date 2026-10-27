#pragma once
#include "renderer/engine/render_trace.h"
#include "runtime/native/graphics_settings.h"
#include <filesystem>
#include <fstream>
#include <iterator>

// Exercise real render-target transfers. Binary source lanes and a -4 copy
// exponent give independently known UNORM16 lanes: exactly 0 or 4096. Both
// snapshots reuse one destination; later transfers must not replace old bytes.
static void fullColorResolveCaptureContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    constexpr unsigned scale=2,width=16,height=16;
    constexpr size_t rawSize=size_t(width*scale)*height*scale*8;
    const auto savedSettings=Native::graphicsSettings();
    struct RestoreSettings {
        Native::GraphicsSettings saved;
        ~RestoreSettings(){Native::setGraphicsSettings(saved);}
    } restoreSettings{savedSettings};
    auto settings=savedSettings;settings.antialiasing=Native::AntialiasingMode::Off;
    require(Native::setGraphicsSettings(settings),"Full-color capture graphics settings rejected");
    const auto directory=std::filesystem::temp_directory_path()/
        ("DarkRecomp-full-color-capture-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    require(!std::filesystem::exists(directory),"Full-color capture fixture directory already exists");
    Native::configureShadowCapture(directory);
    struct DisableCapture {~DisableCapture(){Native::configureShadowCapture({});}} disable;
    context->ClearState();
    WorldRendererD3D11 ordinary(device,context,scale),inspected(device,context,scale);
    const auto quad=colorGradeQuad(width,height,width,height);
    auto decoy=std::make_shared<ColorImage>();decoy->width=decoy->height=1;decoy->pixels={255,0,255,255};
    auto imageFor=[](unsigned phase) {
        auto image=std::make_shared<ColorImage>();image->width=width*scale;image->height=height*scale;
        image->authoredMips=true;image->pixels.resize(size_t(image->width)*image->height*4);
        for(unsigned y=0;y<image->height;++y)for(unsigned x=0;x<image->width;++x) {
            auto* p=image->pixels.data()+(size_t(y)*image->width+x)*4;
            p[0]=(x+phase)%2?255:0;p[1]=(y+phase)%3?255:0;
            p[2]=(x+y+phase)%4<2?255:0;p[3]=255;
        }
        return image;
    };
    auto rawOracle=[&](unsigned phase) {
        const auto image=imageFor(phase);std::vector<uint8_t> bytes(rawSize);
        for(size_t lane=0;lane<image->pixels.size();++lane) {
            const uint16_t value=image->pixels[lane]?4096:0;
            std::memcpy(bytes.data()+lane*2,&value,2);
        }
        return bytes;
    };
    auto checkSample=[&](const std::vector<uint8_t>& bytes,unsigned phase,int fetchExponent,double gain) {
        require(bytes.size()==rawSize,"Full-color sampled surface extent differs");
        const auto image=imageFor(phase);
        for(size_t lane=0;lane<image->pixels.size();++lane) {
            uint16_t value=0;std::memcpy(&value,bytes.data()+lane*2,2);
            const double expected=image->pixels[lane]?std::exp2(double(fetchExponent-4))*gain:0;
            require(materialHalf(value)==expected,"Full-color sampled pixels differ from independent binary/exponent oracle");
        }
    };
    auto readRaw=[&](WorldRendererD3D11& renderer,const WorldTexture& texture) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width*scale;desc.Height=height*scale;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R16G16B16A16_UNORM;
        ComPtr<ID3D11Texture2D> output,staging;
        check(device->CreateTexture2D(&desc,nullptr,&output),"Full-color raw output texture");
        require(renderer.present(texture,output.Get()),"Full-color raw presentation rejected");
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc,nullptr,&staging),"Full-color raw staging texture");
        context->CopyResource(staging.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Full-color raw map");
        std::vector<uint8_t> bytes(rawSize);
        for(unsigned y=0;y<height*scale;++y)
            std::memcpy(bytes.data()+size_t(y)*width*scale*8,
                static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,width*scale*8);
        context->Unmap(staging.Get(),0);return bytes;
    };
    struct Event {const char* kind;unsigned reason=0,draw=0;};
    struct Frame {
        std::vector<Event> events;
        std::array<unsigned,3> resolves{};
        std::array<std::vector<uint8_t>,8> pixels;
    };
    auto render=[&](WorldRendererD3D11& renderer,bool inspect,unsigned phase) {
        context->ClearState();renderer.beginFrame();
        // Prepare all nonzero attachments before inspection. Their HDR resolves
        // differ from eligible transfers solely in the selected attachment.
        WorldClear auxiliary;auxiliary.targets={0,6102,0,0,6103};
        auxiliary.viewport={0,0,width,height};auxiliary.flags=1|48;
        auxiliary.color={1,0,1,1};auxiliary.depth=.5f;renderer.clear(auxiliary);
        const auto initial=imageFor(phase);
        const auto initialTexture=bloomSeed(renderer,initial,scale,6101,6201);
        (void)initialTexture;
        if(inspect)renderer.inspectNextFrame();
        Frame frame;unsigned draws=0;
        auto clear=[&](const WorldClear& c) {frame.events.push_back({"clear"});renderer.clear(c);};
        auto draw=[&](const WorldDraw& d) {
            frame.events.push_back({"draw",0,draws++});
            require(renderer.draw(d),"Full-color fixture draw rejected");
        };
        auto resolve=[&](const WorldResolve& r,unsigned reason=0) {
            const auto command=unsigned(frame.events.size());frame.events.push_back({"resolve",reason});
            require(renderer.resolve(r)==(reason==0),"Full-color fixture resolve acceptance differs");
            // These flags generate separately traced source clears after the
            // completed transfer and its snapshot, on the same command stream.
            if(reason==0 && (r.flags&0x100))frame.events.push_back({"clear"});
            return command;
        };
        WorldResolve full;full.targets[0]=6101;full.viewport={0,0,width,height};
        full.rectangle={0,0,width,height};full.destination={6202,0x6202000,width,height,26,4};full.exponent=-4;
        uint32_t nextObject=6300;
        auto exclusions=[&] {
            auto attempt=[&](WorldResolve r,unsigned reason=0) {
                r.destination.object=nextObject++;r.destination.storage=r.destination.object*4096u;
                resolve(r,reason);
            };
            auto r=full;r.destination.format=6;attempt(r); // Non-HDR.
            r=full;r.rectangle[2]=8;attempt(r); // Completed copy is still partial after rounding.
            r=full;r.rectangle[3]=8;attempt(r);
            r=full;r.rectangle[2]=8;r.offset[0]=8;attempt(r);
            r=full;r.rectangle[3]=8;r.offset[1]=8;attempt(r);
            r=full;r.viewport[0]=1;attempt(r);
            r=full;r.viewport[1]=1;attempt(r);
            // LUT-shaped mismatch: a full destination copy in a larger viewport.
            r=full;r.destination.width=8;r.rectangle[2]=8;attempt(r);
            r=full;r.destination.height=8;r.rectangle[3]=8;attempt(r);
            r=full;r.destination.faces=6;attempt(r);
            r=full;r.targets=auxiliary.targets;r.flags=1;attempt(r);
            r=full;r.targets=auxiliary.targets;r.flags=4;attempt(r); // HDR depth transfer.
            r.destination.format=23;attempt(r); // Ordinary depth transfer.
            r=full;r.mip=1;attempt(r,1);
            r=full;r.targets[0]=6199;attempt(r,2);
            r=full;r.rectangle[2]=0;attempt(r,5);
            r=full;r.offset[0]=1;attempt(r,7);
        };
        exclusions();
        constexpr std::array<int,3> fetchExponents{4,0,-2};
        WorldDraw sample=colorGradeDraw(quad,6104,width,height);
        sample.fragmentName="XREngine_CCFuser";sample.fragmentConstants[0]={1,1,1,1};
        sample.samplers[1]=colorGradeSampler(false,2);sample.textures[1]=decoy;
        WorldClear output;output.targets=sample.targets;output.viewport=sample.viewport;output.flags=1;
        for(unsigned pass=0;pass<3;++pass) {
            if(pass) {
                auto seed=colorGradeDraw(quad,6101,width,height);
                seed.fragmentName="XREngine_CCFuser";seed.fragmentConstants[0]={1,1,1,1};
                seed.textures[1]=imageFor(phase+pass);seed.samplers[1]=colorGradeSampler(false,2);
                WorldClear reset;reset.targets=seed.targets;reset.viewport=seed.viewport;reset.flags=1;
                clear(reset);draw(seed);
            }
            auto r=full;r.destination.exponent=fetchExponents[pass];
            if(pass<2)r.flags=0x100; // Capture must precede clearing the source to zero.
            // Eligibility uses the completed rounded copy, not the requested
            // rectangle. This still transfers all 16x16 logical pixels.
            if(pass==1)r.rectangle={0,0,width-1,height-1};
            frame.resolves[pass]=resolve(r);
            sample.textureObjects[1]=r.destination;
            clear(output);draw(sample);frame.pixels[pass*2]=renderer.readSurface(sample,0);
            // No intervening state-changing operation: exercise cached draw bindings.
            draw(sample);frame.pixels[pass*2+1]=renderer.readSurface(sample,0);
            require(frame.pixels[pass*2]==frame.pixels[pass*2+1],"Full-color repeated sampled draw changed bytes");
            checkSample(frame.pixels[pass*2],phase+pass,fetchExponents[pass],1);
            if(pass==0)exclusions(); // Ineligible transfers cannot consume the second slot either.
        }
        sample.fragmentConstants[0]={.5f,.5f,.5f,.5f};draw(sample);
        frame.pixels[6]=renderer.readSurface(sample,0);checkSample(frame.pixels[6],phase+2,fetchExponents[2],.5);
        require(frame.pixels[6]!=frame.pixels[4],"Full-color following draw control is vacuous");
        renderer.endFrame();
        // Read the final overwrite directly, with no float conversion or fetch
        // exponent applied. Earlier emitted snapshots must retain other bytes.
        frame.pixels[7]=readRaw(renderer,sample.textureObjects[1]);
        require(frame.pixels[7]==rawOracle(phase+2),"Full-color third resolve differs from raw UNORM16 oracle");
        return frame;
    };
    auto readBytes=[](const std::filesystem::path& path) {
        std::ifstream input(path,std::ios::binary);require(bool(input),"Full-color capture file missing");
        auto bytes=std::vector<uint8_t>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
        require(!input.bad(),"Full-color capture file read failed");return bytes;
    };
    auto snapshot=[&] {
        std::vector<std::pair<std::string,std::vector<uint8_t>>> files;
        for(const auto& entry:std::filesystem::directory_iterator(directory))if(entry.is_regular_file())
            files.emplace_back(entry.path().filename().string(),readBytes(entry.path()));
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.first<b.first;});return files;
    };
    auto scalar=[](const std::string& line,const char* field) {
        const auto marker=std::string("\"")+field+"\":";auto first=line.find(marker);
        require(first!=std::string::npos,"Full-color metadata field missing");
        first=line.find_first_not_of(" \t",first+marker.size());
        require(first!=std::string::npos,"Full-color metadata value empty");
        if(line[first]=='\"') {
            const auto last=line.find('\"',first+1);require(last!=std::string::npos,"Full-color metadata string incomplete");
            return line.substr(first+1,last-first-1);
        }
        return line.substr(first,line.find_first_of(",]} \t\r\n",first)-first);
    };
    auto number=[&](const std::string& line,const char* field){return std::stoull(scalar(line,field));};
    auto key=[&](const std::string& line){return std::stoull(scalar(line,"keyHex"),nullptr,16);};
    const auto uncaptured=render(inspected,false,0);
    require(std::filesystem::is_empty(directory),"Full-color capture wrote without explicit inspection");
    require(uncaptured.pixels==render(ordinary,false,0).pixels,"Full-color ordinary fixture differs");
    for(unsigned inspection=1;inspection<=2;++inspection) {
        const unsigned phase=inspection*3;
        const auto expected=render(ordinary,false,phase),actual=render(inspected,true,phase);
        require(expected.pixels==actual.pixels,"Full-color inspection changed first/repeated/following output");
        const auto stem="inspection-"+std::to_string(inspection);
        std::ifstream input(directory/(stem+"-commands.jsonl"),std::ios::binary);
        require(bool(input),"Full-color command metadata missing");
        std::vector<std::string> records;std::string line;
        while(std::getline(input,line)) {
            if(!line.empty() && line.back()=='\r')line.pop_back();
            require(line.size()>=2 && line.front()=='{' && line.back()=='}',"Full-color metadata line incomplete");
            records.push_back(line);
        }
        require(input.eof() && records.size()>2,"Full-color metadata empty or unreadable");
        require(scalar(records.front(),"event")=="begin" && scalar(records.back(),"event")=="end" &&
                number(records.front(),"scale")==scale && number(records.front(),"maxGpuBytes")==128u*1024*1024,
                "Full-color frame boundaries, scale or shared GPU limit differ");
        const auto gpu=readBytes(directory/(stem+"-gpu.bin"));
        require(gpu.size()==2*rawSize && gpu.size()<128u*1024*1024,"Full-color GPU byte count differs from two tiny snapshots");
        unsigned commandCount=0,drawCount=0,fullCount=0;size_t nextOffset=0;
        std::array<std::vector<uint8_t>,2> captured;
        std::string previous;
        for(const auto& record:records) {
            require(number(record,"inspection")==inspection,"Full-color mixed inspection identities");
            const auto event=scalar(record,"event");if(event=="begin" || event=="end")continue;
            if(event!="gpu") {
                require(commandCount<actual.events.size(),"Full-color unexpected extra command");
                const auto& expectedEvent=actual.events[commandCount];
                require(event==expectedEvent.kind && number(record,"command")==commandCount,
                        "Full-color command order differs from submitted operations");
                if(event=="draw") {
                    require(number(record,"draw")==expectedEvent.draw && number(record,"reason")==0,
                            "Full-color draw ordinal or acceptance differs");++drawCount;
                } else if(event=="resolve")require(number(record,"reason")==expectedEvent.reason,
                                                   "Full-color rejected/successful resolve outcome differs");
                ++commandCount;previous=record;continue;
            }
            require(scalar(record,"stage")=="full-color-resolve" && scalar(record,"role")=="resolved-color" &&
                    record.find("\"omitted\"")==std::string::npos && fullCount<2,
                    "Full-color missing, omitted or extra bounded snapshot");
            const auto pass=number(record,"pass");
            require(pass==fullCount && number(record,"command")==actual.resolves[fullCount] &&
                    number(record,"draw")==std::numeric_limits<unsigned>::max() && number(record,"slot")==0 &&
                    key(record)==WorldTexture{6202,0x6202000,width,height,26,4}.key(),
                    "Full-color snapshot pass, sentinel, slot or destination identity differs");
            require(scalar(previous,"event")=="resolve" && number(previous,"command")==number(record,"command") &&
                    number(previous,"reason")==0 && key(previous.substr(previous.find("\"destination\":")))==key(record) &&
                    previous.find("\"copied\":[0,0,16,16]")!=std::string::npos &&
                    previous.find("\"offset\":[0,0]")!=std::string::npos && scalar(previous.substr(previous.find("\"offset\":")),"exponent")=="-4",
                    "Full-color snapshot does not join the exact completed resolve before its clear");
            require(number(record,"width")==width*scale && number(record,"height")==height*scale &&
                    number(record,"sourceFormat")==DXGI_FORMAT_R16G16B16A16_UNORM && number(record,"subresource")==0 &&
                    number(record,"rowBytes")==width*scale*8 && number(record,"bytes")==rawSize &&
                    scalar(record,"encoding")=="RGBA16_UNORM-LE","Full-color raw physical format or extent differs");
            const auto offset=number(record,"offset"),bytes=number(record,"bytes");
            require(offset==nextOffset && offset<=gpu.size() && bytes<=gpu.size()-offset,
                    "Full-color GPU offsets overlap or exceed the blob");
            captured[fullCount].assign(gpu.begin()+size_t(offset),gpu.begin()+size_t(offset+bytes));
            require(captured[fullCount]==rawOracle(phase+fullCount),"Full-color snapshot lost original resolved bytes after overwrite");
            nextOffset+=size_t(bytes);++fullCount;
        }
        require(commandCount==actual.events.size() && number(records.back(),"commands")==commandCount &&
                number(records.back(),"draws")==drawCount && fullCount==2 && nextOffset==gpu.size(),
                "Full-color capture lost commands, reset quota or GPU bytes");
        require(captured[0]!=captured[1] && captured[0]!=actual.pixels[7] && captured[1]!=actual.pixels[7],
                "Full-color first/second/third byte patterns are not distinguishable");
        const auto saved=snapshot();
        require(render(ordinary,false,phase+9).pixels==render(inspected,false,phase+9).pixels,
                "Full-color inspection changed the subsequent uninspected frame");
        require(snapshot()==saved,"Full-color capture wrote during an uninspected frame");
    }
    const auto saved=snapshot();
    require(render(ordinary,false,17).pixels==render(inspected,true,17).pixels,
            "Full-color third inspection changed output");
    require(snapshot()==saved && !std::filesystem::exists(directory/"inspection-3-commands.jsonl"),
            "Full-color capture exceeded the native two-inspection limit");
    context->ClearState();
    std::printf("FullColorCapture PASS: first two full HDR resolves, exact raw exponent bytes, exclusions, overwritten destination, reset and two-frame bounds, identical draws/frames; artifacts=%s\n",
                directory.string().c_str());
}
