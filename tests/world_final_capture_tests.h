#pragma once
#include "renderer/engine/render_trace.h"
#include "runtime/native/graphics_settings.h"
#include <filesystem>
#include <fstream>
#include <iterator>

// Small, asset-free Final5 fixture. The resolved scene retains physical detail;
// bloom must use the actual guest-sized FP32 view, and the LUT remains scaled.
// All inputs are prepared before inspection so command/draw ordinals are fixed.
static void finalCompositeCaptureContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    constexpr unsigned scale=2,width=16,height=12,bloomWidth=8,bloomHeight=6;
    const auto savedSettings=Native::graphicsSettings();
    struct RestoreSettings {
        Native::GraphicsSettings saved;
        ~RestoreSettings(){Native::setGraphicsSettings(saved);}
    } restoreSettings{savedSettings};
    auto settings=savedSettings;settings.bloom=true;settings.antialiasing=Native::AntialiasingMode::Off;
    require(Native::setGraphicsSettings(settings),"Final capture graphics settings rejected");
    const auto directory=std::filesystem::temp_directory_path()/
        ("DarkRecomp-final-capture-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    require(!std::filesystem::exists(directory),"Final capture fixture directory already exists");
    Native::configureShadowCapture(directory);
    struct DisableCapture {~DisableCapture(){Native::configureShadowCapture({});}} disable;
    context->ClearState();
    WorldRendererD3D11 ordinary(device,context,scale),inspected(device,context,scale);
    const auto cube=colorGradeCube(false);
    const auto quad=colorGradeQuad(width,height,width,height);
    const auto cubeQuad=colorGradeQuad(324,18,324,18);
    auto decoy=std::make_shared<ColorImage>();decoy->width=decoy->height=1;decoy->pixels={255,0,255,255};
    auto sceneImage=[&](unsigned phase) {
        auto image=std::make_shared<ColorImage>();image->width=width*scale;image->height=height*scale;
        image->authoredMips=true;image->pixels.resize(size_t(image->width)*image->height*4);
        for(unsigned y=0;y<image->height;++y)for(unsigned x=0;x<image->width;++x) {
            auto* p=image->pixels.data()+(size_t(y)*image->width+x)*4;
            p[0]=(x+2*y+phase)%5<2?255:0;
            p[1]=(x/2+y+phase)%3==0?255:0;
            p[2]=(x+y/2+phase)%7<3?255:0;p[3]=255;
        }
        return image;
    };
    auto bloomImageFor=[&](unsigned phase) {
        auto image=std::make_shared<ColorImage>();image->width=bloomWidth*scale;image->height=bloomHeight*scale;
        image->authoredMips=true;image->pixels.resize(size_t(image->width)*image->height*4);
        for(unsigned y=0;y<image->height;++y)for(unsigned x=0;x<image->width;++x) {
            auto* p=image->pixels.data()+(size_t(y)*image->width+x)*4;
            const unsigned rotation=(x/scale+2*(y/scale)+phase)%3;
            const unsigned sample=((y%scale)*scale+x%scale+phase)%4;
            // Each 2x2 block has independently known lane energies 1/4, 1/2,
            // 3/4 in rotating channels. Binary sources avoid rounding ambiguity.
            for(unsigned c=0;c<3;++c)p[c]=sample<1+(c+rotation)%3?255:0;
            p[3]=255;
        }
        return image;
    };
    auto logicalBloom=[&](unsigned phase) {
        BloomImage result{bloomWidth,bloomHeight,{}};result.pixels.resize(bloomWidth*bloomHeight);
        constexpr std::array<double,3> energies{.25,.5,.75};
        for(unsigned y=0;y<bloomHeight;++y)for(unsigned x=0;x<bloomWidth;++x) {
            auto& p=result.pixels[size_t(y)*bloomWidth+x];
            for(unsigned c=0;c<3;++c)p[c]=energies[(c+x+2*y+phase)%3];p[3]=1;
        }
        return result;
    };
    using Pixels=std::array<std::vector<uint8_t>,4>;
    auto render=[&](WorldRendererD3D11& renderer,bool inspect,unsigned phase) {
        context->ClearState();renderer.beginFrame();
        const auto scene=sceneImage(phase),bloom=bloomImageFor(phase);
        const auto sceneTexture=bloomSeed(renderer,scene,scale,5101,5201);
        const auto bloomTexture=bloomSeed(renderer,bloom,scale,5102,5202);
        auto lut=colorGradeDraw(cubeQuad,5103,324,18);
        lut.fragmentName="XREngine_CCFuser";lut.fragmentConstants[0]={1,1,1,1};
        lut.textures[1]=cube;lut.samplers[1]=colorGradeSampler(false,2);bloomDraw(renderer,lut);
        const auto lookup=colorGradeResolve(renderer,lut,5203,324,18);
        auto final=colorGradeDraw(quad,5104,width,height);
        final.fragmentName="XREngine_Final5";final.fragmentFlags=14;
        final.options.modes[1]=0;final.options.coordinates[1]=0;
        final.fragmentConstants[0]={1,1,1,1};final.fragmentConstants[1]={0,0,1,1};
        final.textureObjects[0]=sceneTexture;final.textureObjects[1]=bloomTexture;final.textureObjects[2]=lookup;
        final.samplers[0]=colorGradeSampler(false,2);
        final.samplers[1]=final.samplers[2]=colorGradeSampler(true,2);
        // Optional owned CPU snapshots deliberately disagree with all three
        // resolved bindings. Capturing those snapshots must fail byte checks.
        final.textures[0]=final.textures[1]=final.textures[2]=decoy;
        WorldClear clear;clear.targets=final.targets;clear.viewport=final.viewport;clear.flags=1;renderer.clear(clear);
        if(inspect)renderer.inspectNextFrame();
        auto rejected=final;rejected.geometry={};
        require(!renderer.draw(rejected),"Final capture invalid geometry was accepted");
        require(renderer.draw(final),"Final capture first Final5 rejected");
        Pixels result;result[0]=renderer.readSurface(final,0);
        // With single-sample color these reads only copy/map resources. They
        // do not invalidate bindings, so the repeat exercises the binding cache.
        require(renderer.draw(final),"Final capture cached repeat rejected");result[1]=renderer.readSurface(final,0);
        auto changed=final;changed.fragmentConstants[0]={.3f,.7f,1.5f,1};
        require(renderer.draw(changed),"Final capture subsequent Final5 rejected");result[2]=renderer.readSurface(final,0);
        auto follow=colorGradeDraw(quad,5104,width,height);
        follow.fragmentName="XREngine_CCFuser";follow.fragmentConstants[0]={1,1,1,1};
        follow.textures[1]=scene;follow.samplers[1]=colorGradeSampler(false,2);
        require(renderer.draw(follow),"Final capture following shader draw rejected");result[3]=renderer.readSurface(follow,0);
        renderer.endFrame();
        for(const auto& pixels:result)require(pixels.size()==size_t(width*scale)*height*scale*8,
            "Final capture output has empty or unexpected dimensions");
        require(result[0]==result[1] && result[0]!=result[2],"Final capture repeated or changed draw is vacuous");
        const auto logical=logicalBloom(phase);
        for(unsigned y=0;y<height*scale;++y)for(unsigned x=0;x<width*scale;++x) {
            const auto glow=bloomSample(logical,(x+.5)/(width*scale),(y+.5)/(height*scale));
            const size_t pixel=size_t(y)*width*scale+x;
            for(unsigned c=0;c<4;++c) {
                uint16_t following=0;std::memcpy(&following,result[3].data()+pixel*8+c*2,2);
                require(following==(scene->pixels[pixel*4+c]?0x3c00:0),
                    "Final capture following draw differs from authored binary scene");
                uint16_t actual=0;std::memcpy(&actual,result[0].data()+pixel*8+c*2,2);
                if(c==3) {require(actual==0x3c00,"Final capture lost opaque alpha");continue;}
                // Independent identity-cube oracle: exposure of scene plus
                // bloom, followed by interpolation of quantized LUT endpoints.
                const double s=scene->pixels[pixel*4+c]/255.0;
                const double combined=s*s+glow[c]*glow[c]-std::clamp(s*s*glow[c]*glow[c],0.0,1.0);
                const double coordinate=std::sqrt(1-std::exp(-(std::max)(combined,1e-7)))*17;
                const unsigned lo=unsigned(std::floor(coordinate)),hi=(std::min)(lo+1,17u);
                const double fraction=coordinate-lo;
                const double expected=(std::lround(lo*255.0/17)*(1-fraction)+std::lround(hi*255.0/17)*fraction)/255;
                require(std::abs(materialHalf(actual)-expected)<.004,
                    "Final capture fixture differs from independent scene/bloom/LUT oracle");
            }
        }
        return result;
    };
    auto readBytes=[](const std::filesystem::path& path) {
        std::ifstream input(path,std::ios::binary);require(bool(input),"Final capture binary missing");
        auto bytes=std::vector<uint8_t>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
        require(!input.bad(),"Final capture binary read failed");return bytes;
    };
    auto snapshot=[&] {
        std::vector<std::pair<std::string,std::vector<uint8_t>>> files;
        for(const auto& entry:std::filesystem::directory_iterator(directory))if(entry.is_regular_file())
            files.emplace_back(entry.path().filename().string(),readBytes(entry.path()));
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.first<b.first;});return files;
    };
    auto scalar=[](const std::string& line,const char* field) {
        const auto marker=std::string("\"")+field+"\":";auto first=line.find(marker);
        require(first!=std::string::npos,"Final capture metadata field missing");
        first=line.find_first_not_of(" \t",first+marker.size());
        require(first!=std::string::npos,"Final capture metadata value empty");
        if(line[first]=='\"') {
            const auto last=line.find('\"',first+1);require(last!=std::string::npos,"Final capture string incomplete");
            return line.substr(first+1,last-first-1);
        }
        return line.substr(first,line.find_first_of(",]} \t\r\n",first)-first);
    };
    auto number=[&](const std::string& line,const char* field){return std::stoull(scalar(line,field));};
    const auto uncaptured=render(inspected,false,0);
    require(std::filesystem::is_empty(directory),"Final capture wrote files without explicit inspection");
    require(uncaptured==render(ordinary,false,0),"Final capture ordinary fixture differs");
    for(unsigned inspection=1;inspection<=2;++inspection) {
        const unsigned phase=inspection-1;
        const auto expected=render(ordinary,false,phase),actual=render(inspected,true,phase);
        require(actual==expected,"Final inspection changed first/repeated/subsequent rendered pixels");
        const auto stem="inspection-"+std::to_string(inspection);
        std::ifstream input(directory/(stem+"-commands.jsonl"),std::ios::binary);
        require(bool(input),"Final capture command metadata missing");
        std::vector<std::string> records;std::string line;
        while(std::getline(input,line)) {
            if(!line.empty() && line.back()=='\r')line.pop_back();
            require(line.size()>=2 && line.front()=='{' && line.back()=='}',"Final capture metadata line incomplete");
            records.push_back(line);
        }
        require(input.eof() && !records.empty(),"Final capture command metadata empty or unreadable");
        require(scalar(records.front(),"event")=="begin" && scalar(records.back(),"event")=="end" &&
                number(records.front(),"scale")==scale,"Final capture frame boundaries or render scale differ");
        const auto gpu=readBytes(directory/(stem+"-gpu.bin"));
        require(!gpu.empty() && gpu.size()<128u*1024*1024,"Final capture GPU bytes empty or over budget");
        std::array<std::string,4> textures{};std::array<std::string,3> views{},samplers{};
        constexpr std::array<const char*,4> roles{"final5-scene","final5-bloom","final5-color-lookup","final5-target"};
        unsigned commands=0,textureOrder=0;size_t nextOffset=0;
        for(const auto& record:records) {
            require(number(record,"inspection")==inspection,"Final capture mixed inspection identities");
            const auto event=scalar(record,"event");if(event=="begin" || event=="end")continue;
            if(event!="gpu") {
                require(event=="draw" && number(record,"command")==commands && number(record,"draw")==commands &&
                        number(record,"reason")==unsigned(commands==0?4:0),"Final capture command/draw outcome differs");
                ++commands;continue;
            }
            const auto role=scalar(record,"role");
            const auto found=std::find(roles.begin(),roles.end(),role);
            const bool descriptor=role=="final5-srv-desc" || role=="final5-sampler-desc";
            if(found!=roles.end() || descriptor) {
                require(record.find("\"omitted\"")==std::string::npos,"Small Final capture omitted required GPU evidence");
                require(number(record,"command")==1 && number(record,"draw")==1,
                    "Final capture did not select only the first accepted Final5");
                require(scalar(record,"stage")==((found!=roles.end() && found-roles.begin()==3)?"final5-after":"final5-before"),
                    "Final capture before/after phase differs");
                if(descriptor) {
                    const auto slot=number(record,"slot");require(slot<3,"Final capture descriptor slot differs");
                    auto& destination=role=="final5-srv-desc"?views[slot]:samplers[slot];
                    require(destination.empty(),"Final capture duplicated descriptor");destination=record;
                } else {
                    require(size_t(found-roles.begin())==textureOrder++,"Final capture texture before/after sequence differs");
                    auto& destination=textures[size_t(found-roles.begin())];
                    require(destination.empty(),"Final capture repeated a bounded texture role");destination=record;
                }
            }
            // Include descriptor and any additional diagnostic bytes when
            // checking compact blob offsets; do not assume a total GPU count.
            if(record.find("\"omitted\"")!=std::string::npos)continue;
            const auto offset=number(record,"offset"),bytes=number(record,"bytes");
            require(bytes>0 && offset==nextOffset && offset<=gpu.size() && bytes<=gpu.size()-offset &&
                    bytes==number(record,"rowBytes")*number(record,"height"),"Final capture GPU byte range differs");
            nextOffset+=size_t(bytes);
        }
        require(commands==5 && textureOrder==4 && number(records.back(),"commands")==5 && number(records.back(),"draws")==5 &&
                nextOffset==gpu.size(),"Final capture lost commands or trailing GPU bytes");
        constexpr std::array<unsigned,4> widths{width*scale,bloomWidth,324*scale,width*scale};
        constexpr std::array<unsigned,4> heights{height*scale,bloomHeight,18*scale,height*scale};
        constexpr std::array<unsigned,4> pixelBytes{4,16,4,8};
        constexpr std::array<DXGI_FORMAT,4> formats{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32G32B32A32_FLOAT,
            DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT};
        constexpr std::array<const char*,4> encodings{"RGBA8_UNORM","RGBA32_FLOAT-LE","RGBA8_UNORM","RGBA16_FLOAT-LE"};
        const auto scene=sceneImage(phase);const auto logical=logicalBloom(phase);
        for(unsigned role=0;role<4;++role) {
            const auto& record=textures[role];require(!record.empty(),"Final capture required texture role missing");
            require(number(record,"width")==widths[role] && number(record,"height")==heights[role] &&
                    number(record,"sourceFormat")==formats[role] && number(record,"subresource")==0 &&
                    number(record,"rowBytes")==widths[role]*pixelBytes[role] &&
                    number(record,"bytes")==size_t(widths[role])*heights[role]*pixelBytes[role] &&
                    number(record,"slot")==unsigned(role==3?0:role) && scalar(record,"encoding")==encodings[role],
                    "Final capture actual resource dimensions/format/encoding differ");
            const auto* bytes=gpu.data()+number(record,"offset");
            if(role==0)require(!std::memcmp(bytes,scene->pixels.data(),scene->pixels.size()),
                "Final capture scene differs from authored resolved physical pixels");
            else if(role==1)for(size_t pixel=0;pixel<logical.pixels.size();++pixel)for(unsigned c=0;c<4;++c) {
                uint32_t bits=0;std::memcpy(&bits,bytes+pixel*16+c*4,4);
                require(bits==std::bit_cast<uint32_t>(float(logical.pixels[pixel][c])),
                    "Final capture logical FP32 bloom bytes differ from quarter-energy oracle");
            } else if(role==2) {
                for(unsigned y=0;y<18*scale;++y) {
                    for(unsigned x=0;x<324*scale;++x) {
                        require(!std::memcmp(bytes+(size_t(y)*324*scale+x)*4,
                                            cube->pixels.data()+(size_t(y/scale)*324+x/scale)*4,4),
                            "Final capture LUT differs from authored scaled cube blocks");
                    }
                }
            } else if(role==3)require(!std::memcmp(bytes,expected[0].data(),expected[0].size()),
                "Final capture after-target bytes differ from ordinary first Final5 output");
        }
        for(unsigned slot=0;slot<3;++slot) {
            require(!views[slot].empty() && !samplers[slot].empty(),"Final capture actual SRV/sampler descriptor missing");
            const auto& view=views[slot];const auto& sampler=samplers[slot];
            require(scalar(view,"encoding")=="D3D11_SRV_DESC-LE" && number(view,"bytes")==sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC) &&
                    scalar(sampler,"encoding")=="D3D11_SAMPLER_DESC-LE" && number(sampler,"bytes")==sizeof(D3D11_SAMPLER_DESC),
                "Final capture descriptor encoding/length differs");
            D3D11_SHADER_RESOURCE_VIEW_DESC actualView{};D3D11_SAMPLER_DESC actualSampler{};
            std::memcpy(&actualView,gpu.data()+number(view,"offset"),sizeof(actualView));
            std::memcpy(&actualSampler,gpu.data()+number(sampler,"offset"),sizeof(actualSampler));
            require(actualView.Format==formats[slot] && actualView.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D &&
                    actualView.Texture2D.MostDetailedMip==0 && actualView.Texture2D.MipLevels==1,
                "Final capture SRV does not describe the actual expected texture");
            // MaxAnisotropy is ignored for these declared point/linear filters;
            // the runtime may canonicalize that unused descriptor field to zero.
            // Filter, addressing and LOD still establish the effective sampling.
            require(actualSampler.Filter==(slot?D3D11_FILTER_MIN_MAG_MIP_LINEAR:D3D11_FILTER_MIN_MAG_MIP_POINT) &&
                    actualSampler.AddressU==D3D11_TEXTURE_ADDRESS_CLAMP && actualSampler.AddressV==D3D11_TEXTURE_ADDRESS_CLAMP &&
                    actualSampler.AddressW==D3D11_TEXTURE_ADDRESS_CLAMP &&
                    actualSampler.MipLODBias==0 && actualSampler.MinLOD==0 && actualSampler.MaxLOD==0,
                "Final capture actual sampler differs from point scene/linear bloom and LUT");
        }
        const auto captured=snapshot();
        require(render(ordinary,false,phase+3)==render(inspected,false,phase+3),
            "Final inspection changed the subsequent uninspected frame");
        require(snapshot()==captured,"Final capture wrote outside explicitly inspected frames");
    }
    const auto captured=snapshot();
    require(render(ordinary,false,7)==render(inspected,true,7),"Final capture third inspection changed output");
    require(snapshot()==captured && !std::filesystem::exists(directory/"inspection-3-commands.jsonl"),
        "Final capture exceeded its two-inspection bound");
    context->ClearState();
    std::printf("FinalCapture PASS: scale2 physical scene/LUT, exact logical RGBA32F bloom, actual SRV/samplers, first accepted draw, two-frame bound, bit-identical subsequent draws/frames; artifacts=%s\n",
                directory.string().c_str());
}
