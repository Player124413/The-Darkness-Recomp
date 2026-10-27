#pragma once
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace DarkRecomp::TextureMods::Detail {
// Owned-worker I/O only. Never called from the rendering thread. The stop
// event cancels this exact OVERLAPPED and drains completion before any buffer,
// event or file handle can be destroyed or reused.
inline bool transfer(HANDLE file,HANDLE stop,bool writing,uint8_t* bytes,size_t size) {
    struct Event {
        HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        ~Event() {if(value)CloseHandle(value);}
    } event;
    if(!event.value)return false;
    size_t at=0;
    while(at<size) {
        if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0)return false;
        OVERLAPPED operation{}; operation.hEvent=event.value;
        operation.Offset=DWORD(uint64_t(at)); operation.OffsetHigh=DWORD(uint64_t(at)>>32);
        ResetEvent(event.value);
        DWORD count=0;
        const DWORD amount=DWORD((std::min)(size-at,size_t(1024*1024)));
        const BOOL immediate=writing?WriteFile(file,bytes+at,amount,&count,&operation):
            ReadFile(file,bytes+at,amount,&count,&operation);
        if(!immediate) {
            if(GetLastError()!=ERROR_IO_PENDING)return false;
            const HANDLE waits[]{stop,event.value};
            if(WaitForMultipleObjects(2,waits,FALSE,INFINITE)!=WAIT_OBJECT_0+1) {
                CancelIoEx(file,&operation);
                GetOverlappedResult(file,&operation,&count,TRUE);
                return false;
            }
        }
        if(!GetOverlappedResult(file,&operation,&count,FALSE) || !count || count>amount)return false;
        at+=count;
    }
    return true;
}
} // namespace DarkRecomp::TextureMods::Detail
