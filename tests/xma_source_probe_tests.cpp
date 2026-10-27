#include "runtime/native/xma_source_probe.h"
#include "ppc_context.h" // Exact frozen Native11 include directory supplied by private CMake.
#include "ppc_recomp_shared.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

void ObserveXmaSourceSliceMidAsmHook(PPCRegister&, PPCRegister&, PPCRegister&,
    PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&);

using DarkRecomp::Native::XmaSourceProbe;
namespace Source = DarkRecomp::Native::XmaSourceIdentity;
namespace {
int checks = 0, failures = 0, originalCalls = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
}
using Body = void(*)(PPCContext&, uint8_t*);
Body body = nullptr;
struct Guest {
    static constexpr size_t Size = 0x100000;
    uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Guest() { if (!base) std::abort(); }
    ~Guest() { VirtualFree(base, 0, MEM_RELEASE); }
    void word(uint32_t at, uint64_t value, uint32_t size = 4) {
        if (uint64_t(at)+size > Size) std::abort();
        for (uint32_t i=0; i<size; ++i) base[at+i]=uint8_t(value >> (8*(size-1-i)));
    }
};
struct Fixture {
    Guest& memory;
    uint32_t voice, frame, container, outer, stream, slice, file, group, records, backing;
    int32_t index;
    std::unique_ptr<PPCContext> ownedContext = std::make_unique<PPCContext>();
    PPCContext& ctx = *ownedContext;
    std::string path;
    explicit Fixture(Guest& g, uint32_t shift=0) : memory(g),
        voice(0x11000+shift), frame(0x12000+shift), container(0x13000+shift),
        outer(0x14000+shift), stream(outer+8), slice(0x15000+shift),
        file(0x16000+shift), group(0x17000+shift), records(0x18000+shift),
        backing(0x19000+shift), index(shift ? 71 : 37),
        path(shift ? "Content\\Sound\\Inner.xwc" : "Content\\Sound\\TV\"News.xwc") {
        ctx.r1.u64=frame+320; ctx.r3.u64=voice;
        ctx.r8.u64=0x1122334455667788ull; ctx.lr=0x827D8FA0;
        memory.word(voice+64, voice);
        memory.word(frame+88, outer);
        memory.word(outer, 0x82096E68);
        memory.word(stream, 0x820658FC);
        memory.word(stream+20, 0); memory.word(stream+24, 1); memory.word(stream+16, slice);
        memory.word(slice, 0x8209DBA8); memory.word(slice+12, file);
        memory.word(slice+16, 0x123456, 8); memory.word(slice+24, 0x45678, 8);
        memory.word(file, 0x820658FC); memory.word(file+8, 0x82065568);
        memory.word(file+12, backing); memory.word(backing, 0x4001, 2);
        std::memcpy(memory.base+backing+2, path.c_str(), path.size()+1);
        memory.word(group, 2); memory.word(group+8, records);
    }
    void enter() { ctx.r1.u64=frame+320; ctx.r3.u64=voice; ctx.lr=0x827D8FA0; }
    void allocation(PPCContext& at) const {
        at.r1.u64=frame-400-208-160;
        at.r16.u64=voice+64; at.r17.u64=voice; at.r24.u64=1;
        at.r26.u64=group; at.r30.u64=records+96;
        at.r31.u64=records+160; at.r3.u64=records+160; at.lr=0x828B1898;
    }
    void observe(bool wrongOutput=false, bool registerMismatch=false) const {
        std::array<PPCRegister,8> args{};
        args[0].u64=container; args[1].s64=index; args[2].u64=slice;
        args[3].u64=slice+(registerMismatch ? 4u : 0u);
        args[4].u64=file; args[5].u64=0x123456; args[6].u64=0x45678;
        args[7].u64=frame+104+(wrongOutput ? 4u : 0u);
        const auto before=args;
        ObserveXmaSourceSliceMidAsmHook(args[0],args[1],args[2],args[3],
                                       args[4],args[5],args[6],args[7]);
        check(std::memcmp(before.data(),args.data(),sizeof(args))==0,
              "observer preserves original input registers");
    }
};
struct Sink {
    FILE* file=std::tmpfile();
    Sink() { if (!file) std::abort(); }
    ~Sink() { std::fclose(file); }
    std::vector<std::string> lines() const {
        std::fflush(file); std::rewind(file);
        std::vector<std::string> out;
        char line[4096]{};
        while (std::fgets(line,sizeof(line),file)) out.emplace_back(line);
        std::fseek(file,0,SEEK_END);
        return out;
    }
};
uint64_t number(const std::string& line, const char* name) {
    const auto key=std::string(name)+"=";
    const auto at=line.find(key);
    if (at==std::string::npos) { check(false,"diagnostic numeric key exists"); return 0; }
    return std::strtoull(line.c_str()+at+key.size(),nullptr,10);
}
Fixture* activeFixture=nullptr;
XmaSourceProbe* probe=nullptr;
uint32_t slot=4, contextAddress=0x60010100;
bool sameContext(const PPCContext& a, const PPCContext& b) {
    return std::memcmp(&a,&b,sizeof(PPCContext))==0;
}
void validBody(PPCContext& ctx, uint8_t* base) {
    activeFixture->observe();
    activeFixture->allocation(ctx);
    PPCContext before{};
    std::memcpy(&before,&ctx,sizeof(ctx));
    probe->created(base,slot,contextAddress);
    check(sameContext(before,ctx),"created does not modify PPC context");
}
void invoke(Fixture& fixture, XmaSourceProbe& p, Body action) {
    activeFixture=&fixture; probe=&p; body=action; fixture.enter();
    const int before=originalCalls;
    sub_827D92E0(fixture.ctx,fixture.memory.base);
    check(originalCalls==before+1,"original loader invoked exactly once");
}
void stubEffects(PPCContext& ctx, uint8_t*) {
    ctx.r3.u64=0xfeed12345678ull; ctx.r8.u64^=0xabcddcbaull;
    ctx.lr=0x12345678; ctx.f3.u64=0x4008000000000000ull;
    ctx.trace[17]=0xabcdef01; ++ctx.traceIndex;
}
void wrapperEffects() {
    Guest guest; Fixture fixture(guest); Sink sink;
    auto p=std::make_unique<XmaSourceProbe>(sink.file);
    PPCContext expected{};
    std::memcpy(&expected,&fixture.ctx,sizeof(expected));
    stubEffects(expected,guest.base);
    const int before=originalCalls;
    body=stubEffects;
    sub_827D92E0(fixture.ctx,guest.base);
    check(originalCalls==before+1,"off/on wrapper calls original exactly once");
    check(sameContext(expected,fixture.ctx),"wrapper preserves exactly original register effects");
    fixture.observe(); // No scope after wrapper return.
    p->created(guest.base,1,0x60010040);
    check(sink.lines().empty(),"scope restored after loader exits");
}
void validAndQuietUnrelated() {
    Guest guest; Fixture fixture(guest); Sink sink;
    auto p=std::make_unique<XmaSourceProbe>(sink.file);
    for (unsigned i=0;i<256;++i) p->created(guest.base,7,0x600101c0);
    const std::vector<uint8_t> before(guest.base,guest.base+Guest::Size);
    slot=4; contextAddress=0x60010100;
    invoke(fixture,*p,validBody);
    check(std::equal(before.begin(),before.end(),guest.base),"probe writes no guest bytes");
    const auto lines=sink.lines();
    check(lines.size()==1,"unrelated allocations do not exhaust log budget");
    if (lines.empty()) return;
    check(number(lines[0],"bound")==1,"enabled matching source binds");
    check(number(lines[0],"generation")==257,"unrelated creates still advance generation");
    check(number(lines[0],"wave")==37 && number(lines[0],"stream")==1 &&
          number(lines[0],"offset")==0x123456 && number(lines[0],"bytes")==0x45678,
          "binding describes logical source");
    check(lines[0].find("bank=\"Content\\\\Sound\\\\TV\\\"News.xwc\"")!=std::string::npos,
          "path diagnostic escapes quotes/backslashes");
}
void lifecycleAndReuse() {
    Guest guest; Fixture fixture(guest); Sink sink;
    auto p=std::make_unique<XmaSourceProbe>(sink.file);
    slot=6; contextAddress=0x60010180;
    invoke(fixture,*p,validBody);
    p->reset(slot,contextAddress+64); p->freed(slot,contextAddress+64);
    p->reset(slot,contextAddress); p->freed(slot,contextAddress);
    p->reset(slot,contextAddress); p->freed(slot,contextAddress);
    p->created(guest.base,slot,contextAddress); // No scope: fresh unbound generation.
    p->reset(slot,contextAddress); p->freed(slot,contextAddress);
    invoke(fixture,*p,validBody);
    const auto lines=sink.lines();
    check(lines.size()==4,"only bound generation emits lifecycle diagnostics");
    if (lines.size()!=4) return;
    check(lines[1].find("phase=reset")!=std::string::npos &&
          lines[2].find("phase=free")!=std::string::npos,
          "reset and free are distinct events");
    const auto first=number(lines[0],"generation");
    check(number(lines[1],"generation")==first && number(lines[2],"generation")==first,
          "reset/free refer to the same allocation generation");
    check(number(lines[3],"generation")==first+2 && number(lines[3],"bound")==1,
          "same-address reuse never inherits old generation");
}
Fixture* outerFixture=nullptr;
Fixture* innerFixture=nullptr;
void innerBody(PPCContext& ctx, uint8_t* base) {
    innerFixture->observe(); innerFixture->allocation(ctx);
    probe->created(base,2,0x60010080);
}
void nestingBody(PPCContext& ctx, uint8_t* base) {
    outerFixture->observe(); outerFixture->allocation(ctx);
    probe->created(base,1,0x60010040);
    const auto oldBody=body;
    body=innerBody; innerFixture->enter();
    const int before=originalCalls;
    sub_827D92E0(innerFixture->ctx,base);
    check(originalCalls==before+1,"nested original invoked once");
    body=oldBody;
    outerFixture->allocation(ctx);
    probe->created(base,3,0x600100c0); // Reuses the restored outer observation.
}
void nestedScope() {
    Guest guest; Fixture outer(guest), inner(guest,0x40000); Sink sink;
    auto p=std::make_unique<XmaSourceProbe>(sink.file);
    outerFixture=&outer; innerFixture=&inner; probe=p.get(); body=nestingBody; outer.enter();
    const int before=originalCalls;
    sub_827D92E0(outer.ctx,guest.base);
    check(originalCalls==before+2,"outer plus inner original calls only");
    const auto lines=sink.lines();
    check(lines.size()==3,"nested scope produces three source bindings");
    if (lines.size()!=3) return;
    check(number(lines[0],"bound")==1 && number(lines[1],"bound")==1 &&
          number(lines[2],"bound")==1,"all nested sources validate");
    check(number(lines[0],"epoch")==number(lines[2],"epoch") &&
          number(lines[0],"epoch")!=number(lines[1],"epoch"),
          "nested epoch restored exactly");
    check(number(lines[0],"wave")==37 && number(lines[1],"wave")==71 &&
          number(lines[2],"wave")==37,"nested source does not leak into outer");
}
void ignoredOutputBody(PPCContext& ctx, uint8_t* base) {
    activeFixture->observe(true,true); // Must be ignored BEFORE register mismatch.
    validBody(ctx,base);
}
void stickyBody(PPCContext& ctx, uint8_t* base) {
    activeFixture->observe();
    activeFixture->observe(false,true);
    activeFixture->observe();
    activeFixture->allocation(ctx);
    probe->created(base,4,0x60010100);
}
void hookGuards() {
    Guest guest; Fixture fixture(guest); Sink goodSink, badSink;
    auto good=std::make_unique<XmaSourceProbe>(goodSink.file);
    auto bad=std::make_unique<XmaSourceProbe>(badSink.file);
    invoke(fixture,*good,ignoredOutputBody);
    auto lines=goodSink.lines();
    check(lines.size()==1 && number(lines[0],"bound")==1,
          "nonmatching opener output ignored before POD validation");
    invoke(fixture,*bad,stickyBody);
    lines=badSink.lines();
    check(lines.size()==1 && number(lines[0],"bound")==0,
          "register mismatch cannot bind or heal");
    if (!lines.empty()) check(number(lines[0],"rejection")==
        unsigned(Source::Rejection::InvalidPod),
        "r3/r27 mismatch has sticky InvalidPod even after prior observation");
}
void unreadableSource() {
    Guest guest; Fixture fixture(guest); Sink sink;
    auto p=std::make_unique<XmaSourceProbe>(sink.file);
    DWORD previous=0, restored=0;
    check(VirtualProtect(guest.base+fixture.file,4096,PAGE_NOACCESS,&previous)!=0,
          "create inaccessible source fixture");
    invoke(fixture,*p,validBody);
    check(VirtualProtect(guest.base+fixture.file,4096,previous,&restored)!=0,
          "restore test-owned memory permission");
    const auto lines=sink.lines();
    check(lines.size()==1 && number(lines[0],"bound")==0,"unreadable source stays unbound");
    if (!lines.empty()) check(number(lines[0],"rejection")==unsigned(Source::Rejection::ReadFailed),
                              "VirtualQuery reader rejects inaccessible page");
}
void budgetBody(PPCContext& ctx, uint8_t* base) {
    activeFixture->observe(); activeFixture->allocation(ctx);
    for (unsigned i=0;i<150;++i) probe->created(base,4,0x60010100);
}
void eventBudget() {
    Guest guest; Fixture fixture(guest); Sink sink;
    auto p=std::make_unique<XmaSourceProbe>(sink.file);
    invoke(fixture,*p,budgetBody);
    p->reset(4,0x60010100); p->freed(4,0x60010100);
    check(sink.lines().size()==128,"scoped diagnostics stop at bounded 128-event budget");
}
} // namespace

extern "C" PPC_FUNC(__imp__sub_827D92E0) {
    ++originalCalls;
    if (body) body(ctx,base);
}

int main(int argc, char** argv) {
    if (argc!=2 || (std::string(argv[1])!="--enabled" && std::string(argv[1])!="--disabled")) return 2;
    const bool expected=std::string(argv[1])=="--enabled";
    check(DarkRecomp::Native::xmaSourceProbeEnabled()==expected,"process env enable state");
    // Verify the documented cached policy in a separate process for each mode.
    _putenv_s("DARK_XMA_ORIGIN_PROBE",expected ? "0" : "1");
    check(DarkRecomp::Native::xmaSourceProbeEnabled()==expected,"enable policy cached per process");
    wrapperEffects();
    if (expected) {
        validAndQuietUnrelated();
        lifecycleAndReuse();
        nestedScope();
        hookGuards();
        unreadableSource();
        eventBudget();
    }
    std::printf("XmaSourceProbe %s: %d checks, %d failures, original calls=%d\n",
                expected ? "on" : "off",checks,failures,originalCalls);
    return failures ? 1 : 0;
}
