#pragma once

// Issue121 controlled regression: a real snapped Gauss draw, HDR26 /16
// resolve, then Mode01 +4 bilinear upcopy. The oracle uses authored logical
// texels and a discrete 17-tap convolution, never the measured physical
// plateaus. This tests a mechanism; it does not identify the captured halo's
// cause. Run the unchanged renderer first with --post-copy-only.
static constexpr unsigned postCopyWidth=128,postCopyHeight=96;
static constexpr unsigned postCopyActiveWidth=32,postCopyActiveHeight=24;
static constexpr std::array<unsigned,9> postCopyBinomial{
    12870,11440,8008,4368,1820,560,120,16,1};
struct PostCopyRegion {unsigned x,y,width,height;};
struct PostCopyChecks {
    size_t stages=0,failedStages=0,comparisons=0,mismatches=0,insensitiveControls=0;
};
static bool postCopyContains(const PostCopyRegion& r,unsigned x,unsigned y) {
    return x>=r.x && x<r.x+r.width && y>=r.y && y<r.y+r.height;
}
static BloomPixel postCopyAuthored(unsigned x,unsigned y) {
    return {x>=16?1.0:0.0,y>=12?1.0:0.0,
        x>=8 && x<24 && y>=6 && y<18?1.0:0.0,1};
}
static BloomImage postCopyGaussianOracle() {
    BloomImage result{postCopyActiveWidth,postCopyActiveHeight,{}};
    result.pixels.resize(size_t(result.width)*result.height);
    for(unsigned y=0;y<result.height;++y)for(unsigned x=0;x<result.width;++x) {
        auto& pixel=result.pixels[size_t(y)*result.width+x];
        const auto center=postCopyAuthored(x,y);
        for(unsigned c=0;c<4;++c)pixel[c]=center[c]*postCopyBinomial[0]/65536.0;
        for(unsigned tap=1;tap<postCopyBinomial.size();++tap)for(int sign:{-1,1}) {
            const unsigned sy=unsigned(std::clamp(int(y)+sign*int(tap),0,int(result.height)-1));
            const auto source=postCopyAuthored(x,sy);
            for(unsigned c=0;c<3;++c)pixel[c]+=source[c]*postCopyBinomial[tap]/65536.0;
        }
        // The original program convolves RGB; alpha retains the center weight.
        // Bias=0, scale=gamma=1 make its transfer analytically the identity.
    }
    return result;
}
static std::shared_ptr<ColorImage> postCopySource(unsigned scale,const PostCopyRegion& region) {
    auto image=std::make_shared<ColorImage>();
    image->width=postCopyWidth*scale;image->height=postCopyHeight*scale;
    image->authoredMips=true;image->pixels.resize(size_t(image->width)*image->height*4);
    for(unsigned y=0;y<image->height;++y)for(unsigned x=0;x<image->width;++x) {
        const unsigned lx=x/scale,ly=y/scale;
        // No sub-guest detail: every scale-by-scale source block is identical.
        // The source's magenta surroundings also expose an incorrect tap clamp.
        const BloomPixel value=postCopyContains(region,lx,ly)?
            postCopyAuthored(lx-region.x,ly-region.y):BloomPixel{1,0,1,1};
        for(unsigned c=0;c<4;++c)
            image->pixels[(size_t(y)*image->width+x)*4+c]=uint8_t(value[c]*255);
    }
    return image;
}
static std::shared_ptr<StoredGeometry> postCopyQuad(const PostCopyRegion& region,
                                                  const std::array<float,4>& uv) {
    auto geometry=colorGradeQuad(region.width,region.height,postCopyWidth,postCopyHeight);
    const float left=-1+2.0f*region.x/postCopyWidth,right=-1+2.0f*(region.x+region.width)/postCopyWidth;
    const float top=1-2.0f*region.y/postCopyHeight,bottom=1-2.0f*(region.y+region.height)/postCopyHeight;
    const float positions[4][2]{{left,bottom},{left,top},{right,top},{right,bottom}};
    const float coordinates[4][2]{{uv[0],uv[3]},{uv[0],uv[1]},{uv[2],uv[1]},{uv[2],uv[3]}};
    for(unsigned vertex=0;vertex<4;++vertex)for(unsigned c=0;c<2;++c) {
        put(geometry->vertices.data()+vertex*20+c*4,std::bit_cast<uint32_t>(positions[vertex][c]));
        put(geometry->vertices.data()+vertex*20+12+c*4,std::bit_cast<uint32_t>(coordinates[vertex][c]));
    }
    return geometry;
}
static std::array<float,4> postCopyInteriorUvs(const PostCopyRegion& region) {
    // Endpoints are source logical texel centers. All physical output centers
    // lie strictly inside them: no atlas-edge bleed or sampler-clamp ambiguity.
    // A 31x23 logical-center span fills 128x96 output pixels (about a 4x upcopy).
    return {(region.x+.5f)/postCopyWidth,(region.y+.5f)/postCopyHeight,
        (region.x+region.width-.5f)/postCopyWidth,(region.y+region.height-.5f)/postCopyHeight};
}
static std::array<double,2> postCopySampleUv(const std::array<float,4>& uv,
                                            unsigned x,unsigned y,unsigned scale) {
    // Use the actual float endpoints written to the vertices, with the D3D
    // physical sample center. There is deliberately no x/scale or y/scale snap.
    return {double(uv[0])+(double(uv[2])-uv[0])*(x+.5)/(postCopyWidth*scale),
        double(uv[1])+(double(uv[3])-uv[1])*(y+.5)/(postCopyHeight*scale)};
}
static WorldDraw postCopyGaussianDraw(const PostCopyRegion& region,const WorldTexture& source) {
    const std::array<float,4> uv{float(region.x)/postCopyWidth,float(region.y)/postCopyHeight,
        float(region.x+region.width)/postCopyWidth,float(region.y+region.height)/postCopyHeight};
    auto draw=colorGradeDraw(postCopyQuad(region,uv),7002,postCopyWidth,postCopyHeight);
    draw.fragmentName="XREngine_GaussClamped";draw.textureObjects[0]=source;
    draw.samplers[0]=colorGradeSampler(true,2);
    draw.fragmentConstants[1]=draw.fragmentConstants[2]={1,1,1,1};
    const auto interior=postCopyInteriorUvs(region);
    draw.fragmentConstants[3]={interior[0],interior[1],interior[0],interior[1]};
    draw.fragmentConstants[4]={interior[2],interior[3],interior[2],interior[3]};
    draw.fragmentConstants[5]={0,0,0,float(postCopyBinomial[0])/65536};
    for(unsigned n=0;n<8;++n)draw.fragmentConstants[n<4?6:7][n%4]=float(postCopyBinomial[n+1])/65536;
    for(unsigned pair=0;pair<4;++pair)
        draw.fragmentConstants[8+pair]={0,float(pair*2+1)/postCopyHeight,0,float(pair*2+2)/postCopyHeight};
    return draw;
}
static WorldTexture postCopyResolve(WorldRendererD3D11& renderer,const WorldDraw& source,
                                    uint32_t object,const PostCopyRegion& region) {
    // Region edges and offsets are multiples of eight, so guest resolve
    // rectangle expansion cannot silently change which logical texels we copy.
    WorldResolve resolve;resolve.targets=source.targets;resolve.viewport=source.viewport;
    resolve.rectangle={region.x,region.y,region.x+region.width,region.y+region.height};
    resolve.offset={region.x,region.y};
    resolve.destination={object,object*4096u,postCopyWidth,postCopyHeight,26,4};
    resolve.exponent=-4;
    require(renderer.resolve(resolve),"Post-copy HDR26 resolve rejected");
    return resolve.destination;
}
static WorldDraw postCopyFixedDraw(const WorldTexture& source,const std::array<float,4>& uv,
                                   uint32_t target,bool linear=true) {
    auto draw=colorGradeDraw(postCopyQuad({0,0,postCopyWidth,postCopyHeight},uv),
        target,postCopyWidth,postCopyHeight);
    draw.material=WorldMaterial::fixed;draw.fragmentName="MRenderXenon_Attrib_TexEnvMode01";
    // Mode01 c0=0 multiplies the fetch by white VColor, including source alpha.
    draw.fragmentConstants[0]={0,0,0,0};
    draw.textureObjects[0]=source;draw.samplers[0]=colorGradeSampler(linear,2);
    return draw;
}
template<class Oracle>
static void postCopyCompare(WorldRendererD3D11& renderer,const WorldDraw& draw,unsigned scale,
                            const char* label,Oracle oracle,PostCopyChecks& checks,double tolerance=.003) {
    const auto bytes=renderer.readSurface(draw.targets[0],false);
    const unsigned width=postCopyWidth*scale,height=postCopyHeight*scale;
    require(bytes.size()==size_t(width)*height*8,"Post-copy physical surface extent differs");
    size_t mismatches=0,comparisons=0;double maximum=0;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        const auto expected=oracle(x,y);
        for(unsigned c=0;c<4;++c) {
            uint16_t raw=0;std::memcpy(&raw,bytes.data()+(size_t(y)*width+x)*8+c*2,2);
            const double actual=materialHalf(raw),delta=std::abs(actual-expected[c]);++comparisons;
            maximum=(std::max)(maximum,std::isfinite(delta)?delta:std::numeric_limits<double>::infinity());
            if(!std::isfinite(delta) || delta>tolerance) {
                if(mismatches++==0)std::fprintf(stderr,
                    "PostCopyFilter mismatch: %s scale=%u physical=%u,%u lane=%u actual=%.9g expected=%.9g\n",
                    label,scale,x,y,c,actual,expected[c]);
            }
        }
    }
    ++checks.stages;checks.failedStages+=mismatches!=0;
    checks.comparisons+=comparisons;checks.mismatches+=mismatches;
    std::printf("PostCopyFilter: %s scale=%u maxDelta=%.9g tolerance=%g mismatches=%zu/%zu\n",
        label,scale,maximum,tolerance,mismatches,comparisons);
}
static BloomImage postCopyPhysicalAtlas(const BloomImage& logical,unsigned scale) {
    // Expand the independently modeled producer's known repeated blocks,
    // never a measured native plateau.
    BloomImage result{logical.width*scale,logical.height*scale,{}};
    result.pixels.resize(size_t(result.width)*result.height);
    for(unsigned y=0;y<result.height;++y)for(unsigned x=0;x<result.width;++x)
        result.pixels[size_t(y)*result.width+x]=logical.pixels[size_t(y/scale)*logical.width+x/scale];
    return result;
}
static void postCopyPhysicalCheck(WorldRendererD3D11& renderer,unsigned scale,const WorldTexture& atlas,
                                 const std::array<float,4>& uv,const BloomImage& expected,
                                 const char* label,PostCopyChecks& checks,bool unknownConsumer=false) {
    auto copy=postCopyFixedDraw(atlas,uv,7006);
    // Known half gain bounds two-axis physical-filter precision, retaining
    // the existing .005 control tolerance and a strong signal against remapping.
    copy.constants.vectors[10]={.5f,.5f,.5f,.5f};
    if(unknownConsumer) {
        copy.material=WorldMaterial::post;copy.fragmentName="XREngine_CCFuser";
        copy.fragmentConstants[0]={.5f,.5f,.5f,.5f};
        copy.textureObjects[0]={};copy.textureObjects[1]=atlas;
        copy.samplers[1]=colorGradeSampler(true,2);
    }
    bloomDraw(renderer,copy);
    postCopyCompare(renderer,copy,scale,label,[&](unsigned x,unsigned y) {
        const auto sample=postCopySampleUv(uv,x,y,scale);
        auto pixel=bloomSample(expected,sample[0],sample[1]);
        for(auto& lane:pixel)lane*=.5;
        return pixel;
    },checks,.005);
}
static void postCopyAtlasContract(WorldRendererD3D11& renderer,unsigned scale,
                                  const PostCopyRegion& region,PostCopyChecks& checks) {
    const PostCopyRegion full{0,0,postCopyWidth,postCopyHeight};
    const std::string prefix=region.x?"offset-atlas/":"origin-atlas/";
    const auto sourceImage=postCopySource(scale,region);
    const auto source=bloomSeed(renderer,sourceImage,scale,7001,7101);
    const auto authored=bloomImage(*sourceImage);
    auto seed=colorGradeDraw(colorGradeQuad(postCopyWidth,postCopyHeight,postCopyWidth,postCopyHeight),
        7001,postCopyWidth,postCopyHeight);
    postCopyCompare(renderer,seed,scale,(prefix+"replicated-source").c_str(),
        [&](unsigned x,unsigned y){return authored.pixels[size_t(y)*authored.width+x];},checks,.00001);

    auto gaussian=postCopyGaussianDraw(region,source);
    const BloomPixel sentinel{.625,.125,.875,.5};
    WorldClear clear;clear.targets=gaussian.targets;clear.viewport=gaussian.viewport;
    clear.flags=1;clear.color={.625f,.125f,.875f,.5f};renderer.clear(clear);
    // Initialize unrelated resolved atlas texels before the partial Gauss copy.
    postCopyResolve(renderer,gaussian,7102,full);
    require(renderer.draw(gaussian),"Post-copy original Gaussian draw rejected");
    const auto logicalGaussian=postCopyGaussianOracle();
    BloomImage logicalAtlas{postCopyWidth,postCopyHeight,{}};
    logicalAtlas.pixels.assign(size_t(postCopyWidth)*postCopyHeight,sentinel);
    for(unsigned y=0;y<region.height;++y)for(unsigned x=0;x<region.width;++x)
        logicalAtlas.pixels[size_t(region.y+y)*postCopyWidth+region.x+x]=
            logicalGaussian.pixels[size_t(y)*region.width+x];
    postCopyCompare(renderer,gaussian,scale,(prefix+"analytic-Gauss-and-sentinel").c_str(),
        [&](unsigned x,unsigned y){return logicalAtlas.pixels[size_t(y/scale)*postCopyWidth+x/scale];},
        checks,.001);

    // Model FP16 target storage and UNORM16 /16 rounding independently from
    // readback. The +4 fetch restores that known logical image before filtering.
    for(auto& pixel:logicalAtlas.pixels)for(auto& lane:pixel)lane=bloomRetailStored(lane);
    auto atlas=postCopyResolve(renderer,gaussian,7102,region);
    auto point=postCopyFixedDraw(atlas,{0,0,1,1},7003,false);bloomDraw(renderer,point);
    postCopyCompare(renderer,point,scale,(prefix+"HDR26-decode-and-sentinel").c_str(),
        [&](unsigned x,unsigned y){return logicalAtlas.pixels[size_t(y/scale)*postCopyWidth+x/scale];},
        checks,.001);

    const auto uv=postCopyInteriorUvs(region);
    auto upcopy=postCopyFixedDraw(atlas,uv,7004);bloomDraw(renderer,upcopy);
    postCopyCompare(renderer,upcopy,scale,(prefix+"continuous-logical-bilinear-upcopy").c_str(),
        [&](unsigned x,unsigned y) {
            const auto sample=postCopySampleUv(uv,x,y,scale);
            return bloomSample(logicalAtlas,sample[0],sample[1]);
        },checks);
    // .003 is fixed across scales: it covers FP16 storage and subtexel filter
    // precision for these bounded gradients, not multi-physical-pixel plateaus.

    // Positive control: same HDR format, dimensions, exponent, sampler and
    // upcopy geometry, but an ordinary resolved source with genuine physical
    // checker detail. A blanket logical/downsample rule must fail this case.
    const auto checker=bloomFixture(postCopyWidth,postCopyHeight,scale,1);
    // Bound control precision without changing any comparison threshold:
    // two axes with 1/256 weight error at amplitude 128/255 contribute at
    // most 2*(128/255)/256 = .003922; FP16/UNORM storage adds less than .001.
    // Physical-vs-average contrast remains above the existing .02 sensitivity
    // threshold. Keep alpha at 255 and derive the oracle from these authored
    // RGB bytes before seeding, independently of any renderer output.
    for(size_t n=0;n<checker->pixels.size();n+=4)for(unsigned c=0;c<3;++c)
        checker->pixels[n+c]=checker->pixels[n+c]?128:0;
    auto physicalChecker=bloomImage(*checker);
    for(auto& pixel:physicalChecker.pixels)for(auto& lane:pixel)lane=bloomRetailStored(lane);
    const auto ordinary=bloomSeed(renderer,checker,scale,7005,7103,26,4);
    auto control=postCopyFixedDraw(ordinary,uv,7006);bloomDraw(renderer,control);
    const auto averagedChecker=bloomAverage(physicalChecker,scale);
    size_t distinguishable=0;double detailMaximum=0;
    postCopyCompare(renderer,control,scale,(prefix+"ordinary-physical-checker-upcopy").c_str(),
        [&](unsigned x,unsigned y) {
            const auto sample=postCopySampleUv(uv,x,y,scale);
            const auto expected=bloomSample(physicalChecker,sample[0],sample[1]);
            const auto averaged=bloomSample(averagedChecker,sample[0],sample[1]);
            for(unsigned c=0;c<3;++c) {
                const double delta=std::abs(expected[c]-averaged[c]);
                detailMaximum=(std::max)(detailMaximum,delta);distinguishable+=delta>.02;
            }
            return expected;
        },checks,.005); // Two-axis binary checker allows two subtexel rounding steps.
    checks.insensitiveControls+=scale>1 && distinguishable==0;
    std::printf("PostCopyFilter control sensitivity: %s scale=%u maxPhysicalVsAverage=%.9g distinguishingLanes=%zu\n",
        prefix.c_str(),scale,detailMaximum,distinguishable);

    const auto physicalGaussian=postCopyPhysicalAtlas(logicalAtlas,scale);
    postCopyPhysicalCheck(renderer,scale,atlas,uv,physicalGaussian,
        (prefix+"unknown-consumer-linear-clamp-stays-physical").c_str(),checks,true);

    // The ordinary neighbor writes the same source surface, then partially
    // resolves into the SAME destination atlas object. Shared edge, no overlap.
    const PostCopyRegion neighbor{region.x+region.width,region.y,32,24};
    auto writeChecker=[&](const PostCopyRegion& written) {
        const std::array<float,4> sourceUv{float(written.x)/postCopyWidth,float(written.y)/postCopyHeight,
            float(written.x+written.width)/postCopyWidth,float(written.y+written.height)/postCopyHeight};
        auto draw=colorGradeDraw(postCopyQuad(written,sourceUv),7002,postCopyWidth,postCopyHeight);
        draw.fragmentName="XREngine_CCFuser";draw.fragmentConstants[0]={1,1,1,1};
        draw.textures[1]=checker;draw.samplers[1]=colorGradeSampler(false,2);
        require(renderer.draw(draw),"Post-copy ordinary atlas write rejected");
        return draw;
    };
    const auto neighborDraw=writeChecker(neighbor);
    atlas=postCopyResolve(renderer,neighborDraw,7102,neighbor);
    auto mixedAtlas=physicalGaussian;
    for(unsigned y=neighbor.y*scale;y<(neighbor.y+neighbor.height)*scale;++y)
        for(unsigned x=neighbor.x*scale;x<(neighbor.x+neighbor.width)*scale;++x)
            mixedAtlas.pixels[size_t(y)*mixedAtlas.width+x]=physicalChecker.pixels[size_t(y)*physicalChecker.width+x];
    point=postCopyFixedDraw(atlas,{0,0,1,1},7003,false);bloomDraw(renderer,point);
    postCopyCompare(renderer,point,scale,(prefix+"disjoint-neighbor-write-and-preserved-atlas").c_str(),
        [&](unsigned x,unsigned y){return mixedAtlas.pixels[size_t(y)*mixedAtlas.width+x];},checks,.001);
    // Isolate destination retention first, then independently check source
    // retention by re-resolving Gauss after the disjoint source draw.
    for(bool recopy:{false,true}) {
        if(recopy)atlas=postCopyResolve(renderer,gaussian,7102,region);
        upcopy=postCopyFixedDraw(atlas,uv,7004);bloomDraw(renderer,upcopy);
        const auto label=prefix+(recopy?"disjoint-neighbor-then-Gauss-recopy-logical-upcopy":
            "disjoint-neighbor-resolve-logical-upcopy");
        postCopyCompare(renderer,upcopy,scale,label.c_str(),[&](unsigned x,unsigned y) {
            const auto sample=postCopySampleUv(uv,x,y,scale);
            return bloomSample(logicalAtlas,sample[0],sample[1]);
        },checks);
    }

    // Overwrite an aligned 8x8 part of the very same resolved Gauss object
    // with an ordinary physical checker. Check the untouched logical/sentinel
    // texels too; stale producer provenance or whole-atlas conversion is unsafe.
    const PostCopyRegion patch{region.x+8,region.y+8,8,8};
    auto ordinaryDraw=seed;ordinaryDraw.targets[0]=7005;
    atlas=postCopyResolve(renderer,ordinaryDraw,7102,patch);
    point=postCopyFixedDraw(atlas,{0,0,1,1},7003,false);bloomDraw(renderer,point);
    postCopyCompare(renderer,point,scale,(prefix+"partial-overwrite-and-preserved-atlas").c_str(),
        [&](unsigned x,unsigned y) {
            return postCopyContains(patch,x/scale,y/scale)?
                physicalChecker.pixels[size_t(y)*physicalChecker.width+x]:
                mixedAtlas.pixels[size_t(y)*mixedAtlas.width+x];
        },checks,.001);
    const auto patchUv=postCopyInteriorUvs(patch);
    control=postCopyFixedDraw(atlas,patchUv,7006);bloomDraw(renderer,control);
    postCopyCompare(renderer,control,scale,(prefix+"overwritten-region-physical-upcopy").c_str(),
        [&](unsigned x,unsigned y) {
            const auto sample=postCopySampleUv(patchUv,x,y,scale);
            return bloomSample(physicalChecker,sample[0],sample[1]);
        },checks,.005);
    // A source overlap must invalidate proof before a later full active-region
    // resolve could export stale metadata. Model the authored physical write.
    writeChecker(patch);
    atlas=postCopyResolve(renderer,gaussian,7102,region);
    auto sourceWritten=physicalGaussian;
    for(unsigned y=patch.y*scale;y<(patch.y+patch.height)*scale;++y)
        for(unsigned x=patch.x*scale;x<(patch.x+patch.width)*scale;++x)
            sourceWritten.pixels[size_t(y)*sourceWritten.width+x]=physicalChecker.pixels[size_t(y)*physicalChecker.width+x];
    postCopyPhysicalCheck(renderer,scale,atlas,uv,sourceWritten,
        (prefix+"source-overlap-draw-invalidates-proof").c_str(),checks);

    // Re-establish the producer before independently testing clear invalidation.
    require(renderer.draw(gaussian),"Post-copy Gaussian restoration rejected");
    postCopyResolve(renderer,gaussian,7102,region);
    auto overlapClear=clear;overlapClear.color={.5f,.5f,.5f,.5f};
    overlapClear.rectangle=std::array<int32_t,4>{int32_t(patch.x),int32_t(patch.y),
        int32_t(patch.x+patch.width),int32_t(patch.y+patch.height)};
    renderer.clear(overlapClear);
    atlas=postCopyResolve(renderer,gaussian,7102,region);
    auto sourceCleared=physicalGaussian;
    for(unsigned y=patch.y*scale;y<(patch.y+patch.height)*scale;++y)
        for(unsigned x=patch.x*scale;x<(patch.x+patch.width)*scale;++x)
            sourceCleared.pixels[size_t(y)*sourceCleared.width+x].fill(bloomRetailStored(.5));
    postCopyPhysicalCheck(renderer,scale,atlas,uv,sourceCleared,
        (prefix+"source-overlap-clear-invalidates-proof").c_str(),checks);

    // Blended Gauss retains physical checker detail, so it must not establish
    // the opaque producer contract even though its sampler is qualified.
    writeChecker(region);
    auto blended=gaussian;put(blended.attributes.data()+92,0x01100008);
    blended.attributes[144]=5;blended.attributes[145]=6; // SRC_ALPHA, INV_SRC_ALPHA.
    require(renderer.draw(blended),"Post-copy blended Gaussian rejected");
    atlas=postCopyResolve(renderer,blended,7102,region);
    auto blendedExpected=physicalGaussian;
    for(unsigned y=region.y*scale;y<(region.y+region.height)*scale;++y)
        for(unsigned x=region.x*scale;x<(region.x+region.width)*scale;++x) {
            const auto& g=logicalGaussian.pixels[size_t(y/scale-region.y)*region.width+x/scale-region.x];
            auto& pixel=blendedExpected.pixels[size_t(y)*blendedExpected.width+x];
            for(unsigned c=0;c<4;++c) {
                // Authored 0/128 RGB and 255 alpha have independently known
                // FP16 target values 0, 257/512 and 1, before the blend.
                const double background=c==3?1:checker->pixels[(size_t(y)*checker->width+x)*4+c]?257.0/512:0;
                pixel[c]=bloomRetailStored(g[c]*g[3]+background*(1-g[3]));
            }
        }
    postCopyPhysicalCheck(renderer,scale,atlas,uv,blendedExpected,
        (prefix+"blended-Gauss-does-not-establish-proof").c_str(),checks);
}
static void postCopyOverflowDepthContract(WorldRendererD3D11& renderer,unsigned scale,PostCopyChecks& checks) {
    const PostCopyRegion full{0,0,postCopyWidth,postCopyHeight};
    const auto checker=bloomFixture(postCopyWidth,postCopyHeight,scale,1);
    for(size_t n=0;n<checker->pixels.size();n+=4)for(unsigned c=0;c<3;++c)
        checker->pixels[n+c]=checker->pixels[n+c]?128:0;
    auto background=colorGradeDraw(postCopyQuad(full,{0,0,1,1}),7202,postCopyWidth,postCopyHeight);
    background.fragmentName="XREngine_CCFuser";background.fragmentConstants[0]={1,1,1,1};
    background.textures[1]=checker;background.samplers[1]=colorGradeSampler(false,2);
    bloomDraw(renderer,background);

    // A real resolved white source and linear-clamp sampler keep the Gaussian
    // on the snapped path at scales2/3; no CPU-source eligibility shortcut.
    auto white=background;white.targets[0]=7201;
    white.textures[1]=materialSolid({255,255,255,255});bloomDraw(renderer,white);
    const auto source=colorGradeResolve(renderer,white,7301,postCopyWidth,postCopyHeight);
    auto gaussian=postCopyGaussianDraw(full,source);gaussian.targets[0]=7202;
    auto geometry=postCopyQuad(full,{0,0,1,1});
    for(unsigned vertex=0;vertex<4;++vertex)
        put(geometry->vertices.data()+vertex*20+8,std::bit_cast<uint32_t>(1.0f));
    gaussian.geometry={geometry,geometry,0,6};
    gaussian.constants.vectors[2]={0,0,0x1p127f,0x1p127f};
    gaussian.depthRange={.5f,.5f,0,0};
    // Decoded z=w=1 makes float row2 DP4 overflow to Inf. The native depth
    // expression .5*w + 0*Inf is NaN, so rasterization must leave the checker
    // untouched. A finite double qualification must not create blur provenance.
    require(renderer.draw(gaussian),"Post-copy overflowing-depth Gaussian draw rejected");
    auto rawExpected=bloomImage(*checker);
    for(auto& pixel:rawExpected.pixels)for(unsigned c=0;c<3;++c)
        pixel[c]=pixel[c]?257.0/512:0; // Independently known FP16(128/255).
    postCopyCompare(renderer,gaussian,scale,"overflow-depth/no-Gauss-writes",
        [&](unsigned x,unsigned y){return rawExpected.pixels[size_t(y)*rawExpected.width+x];},
        checks,.00001);

    const auto resolved=postCopyResolve(renderer,gaussian,7302,full);
    auto copy=postCopyFixedDraw(resolved,{0,0,1,1},7203);bloomDraw(renderer,copy);
    auto resolvedExpected=bloomImage(*checker);
    for(auto& pixel:resolvedExpected.pixels)for(auto& lane:pixel)lane=bloomRetailStored(lane);
    // Same extent and 0..1 UVs put every physical output sample on its source
    // texel center. This oracle uses authored bytes plus FP16/UNORM rounding,
    // never readback from the overflow draw or an accepted physical plateau.
    postCopyCompare(renderer,copy,scale,"overflow-depth/resolved-checker-stays-physical",
        [&](unsigned x,unsigned y){return resolvedExpected.pixels[size_t(y)*resolvedExpected.width+x];},
        checks,.005);
}
static void postCopyFilterContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    PostCopyChecks checks;
    // Scale1 is collected first. Numerical failures never short-circuit later
    // stages/scales, so a red baseline still reports producer and control data.
    for(unsigned scale:{1u,2u,3u}) {
        context->ClearState();WorldRendererD3D11 renderer(device,context,scale);
        const auto previousStages=checks.stages,previousFailures=checks.failedStages;
        for(const PostCopyRegion region:{PostCopyRegion{0,0,32,24},PostCopyRegion{48,32,32,24}})
            postCopyAtlasContract(renderer,scale,region,checks);
        postCopyOverflowDepthContract(renderer,scale,checks);
        std::printf("PostCopyFilter scale=%u: stages=%zu failedStages=%zu\n",
            scale,checks.stages-previousStages,checks.failedStages-previousFailures);
    }
    context->ClearState();
    std::printf("PostCopyFilter total: stages=%zu failedStages=%zu mismatches=%zu/%zu insensitiveControls=%zu\n",
        checks.stages,checks.failedStages,checks.mismatches,checks.comparisons,checks.insensitiveControls);
    std::fflush(stdout);
    require(checks.insensitiveControls==0,"Post-copy checker control cannot distinguish physical detail from downsampling");
    require(checks.failedStages==0,"Post-copy differs from independent Gaussian/logical bilinear or physical-detail control oracle");
}
