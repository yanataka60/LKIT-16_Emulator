// PANAFACOM L-KIT16 emulator - Visual Studio 2022 / WAV CMT support.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#pragma comment(lib,"winmm.lib")
#endif
#include <sstream>
#include <string>
#include <vector>
#include <utility>
#include "resource.h"


// TVIF manual pp.36-46: lower-byte SCA registers 50H..53H.
class LkitTv {
public:
    // Manual p.42 / p.45: P00..P3F are selected by display codes.
    // CPU uploads follow the article pp.55-56. Normalized bitmaps below
    // connect its 7-bit row bytes to the existing Windows drawing path.
    struct Pattern { unsigned width=0,height=0;std::vector<uint8_t> dots; };
    std::array<Pattern,64> patterns{};
    // Article pp.55-56: each pattern is 10 transfers of 7 dot bits.
    std::array<std::array<uint8_t,10>,64> patternRows{};
    unsigned patternAddress=0,patternLine=0;
    void writePattern(uint8_t v){
        if(v&0x80){patternAddress=v&0x3f;patternLine=0;return;}
        patternRows[patternAddress][patternLine]=v&0x7f;
        std::vector<uint8_t> dots(70);
        for(unsigned y=0;y<10;y++)for(unsigned x=0;x<7;x++)
            dots[y*7+x]=(patternRows[patternAddress][y]>>(6-x))&1;
        definePattern(patternAddress,7,10,dots);
        if(++patternLine==10){patternLine=0;patternAddress=(patternAddress+1)&63;}
    }
    static int patternIndex(uint8_t code){
        if(code>=0x60 && code<=0x7f)return code-0x60;
        if(code>=0xe0)return code-0xe0+32;
        return -1;
    }
    bool definePattern(unsigned index,unsigned width,unsigned height,const std::vector<uint8_t>& dots){
        // Host bounds only; CPU uploads use the article's 7 x 10 geometry.
        if(index>=64 || !width || !height || width>64 || height>64 || dots.size()!=width*height)return false;
        for(uint8_t dot:dots)if(dot>1)return false;
        patterns[index]={width,height,dots};dirty=true;return true;
    }
    bool patternDot(unsigned index,unsigned x,unsigned y)const{
        if(index>=64)return false;
        const Pattern& p=patterns[index];
        return x<p.width && y<p.height && p.dots[y*p.width+x]!=0;
    }
    std::array<uint8_t,640> cells{};
    std::array<bool,640> reverse{};
    std::array<uint8_t,640> colors{}; uint8_t ink=0; bool tone=false;
    unsigned row=0,col=0; uint8_t command=0,pending=0,output=0;
    bool selected=false,type=false,inverse=false,dirty=true;
    std::vector<uint8_t> keys; bool irq=false; uint64_t keyReadyAt=0;
    uint64_t readyAt=0; // Functional handshake; physical latency is not yet measured.
    LkitTv(){reset();}
    void reset(){for(auto& p:patterns)p=Pattern{};for(auto& p:patternRows)p.fill(0);patternAddress=patternLine=0;cells.fill(0x20);reverse.fill(false);colors.fill(0);ink=0;tone=false;row=col=0;command=pending=output=0;selected=type=inverse=false;readyAt=0;keys.clear();irq=false;keyReadyAt=0;dirty=true;}
    void newline(){
        if(type){for(unsigned i=0;i<600;i++){cells[i]=cells[i+40];reverse[i]=reverse[i+40];colors[i]=colors[i+40];}
            for(unsigned i=600;i<640;i++){cells[i]=0x20;reverse[i]=false;colors[i]=0;}row=15;
        }else row=(row+1)%16;
        col=0;dirty=true;
    }
    uint16_t status(uint64_t now)const{if(!selected)return 0;if((command&0x7f)==0)return keys.empty()||now<keyReadyAt?0:0x0041u;return now>=readyAt?uint16_t(0x0002u|(irq?0x80u:0)):0;}
    void input(uint8_t v){if(keys.size()<256)keys.push_back(v);}
    uint16_t readKey(uint64_t now=0){if(keys.empty()||now<keyReadyAt)return 0;uint8_t v=keys.front();keys.erase(keys.begin());keyReadyAt=now+40000;return v;} // Host typeahead pacing, not TVIF hardware timing.
    void writeCommand(uint8_t v){command=v;}
    // Acknowledge an already completed transfer. An early DSR reset must
    // not erase the completion that the article WAIT routine is polling for.
    void acknowledge(uint16_t bits,uint64_t now){if((bits&0x80u)&&now>=readyAt)irq=false;}
    void write(uint8_t v,uint64_t now){
        output=v;
        if((command&0x7f)==0x0e){
            if(v&0x80){selected=false;row=col=0;type=inverse=false;ink=0;tone=false;pending=0;readyAt=0;dirty=true;}
            else {selected=(v&0x1f)==0x10;irq=selected;}
            return;
        }
        if(!selected||((command&0x7f)!=0x08&&(command&0x7f)!=0x0a)||now<readyAt)return;
        irq=true;readyAt=now+32; // provisional 16 us, not a claimed silicon timing.
        if((command&0x7f)==0x0a){writePattern(v);return;}
        if(pending){if(pending==0x80)col=v&63;else row=v&15;pending=0;dirty=true;return;}
        if(v==0)return;
        if(v==0x02){tone=true;return;}
        if(v==0x08){tone=false;return;}
        if(v==0x80||v==0x82){pending=v;return;}
        if(v==0x88){type=false;return;}
        if(v==0x8a){type=true;row=15;col=0;dirty=true;return;}
        if(v>=0x90&&v<=0x9f){inverse=(v&1)!=0;ink=(v>>1)&7;return;}
        if(v==0x0a){if(row<16&&col<40)for(unsigned x=col;x<40;x++){cells[row*40+x]=0x20;reverse[row*40+x]=false;colors[row*40+x]=0;}newline();return;}
        if(!((v>=0x20&&v<=0x5f)||(v>=0xa0&&v<=0xdf)||patternIndex(v)>=0))return;
        if(col>=40)return;
        cells[row*40+col]=v;reverse[row*40+col]=inverse;colors[row*40+col]=ink;dirty=true;
        if(++col==40)newline();
    }
};

// Expanded SCA at 50H: article programs select output with CMR=08H,
// then write 0000H/0001H to OBR=53H. Numeric bit 0 drives the speaker.
class LkitScaSound {
public:
    bool enabled=false; uint16_t command=0,output=0;
    uint64_t phase=0,droppedSamples=0; int32_t previous=-2500,filtered=0;
    std::vector<int16_t> samples;
    void reset(){command=output=0;phase=droppedSamples=0;previous=-2500;filtered=0;samples.clear();}
    void select(bool active){enabled=active;reset();}
    void write(uint8_t port,uint16_t value){
        if(port==0x50)command=value;
        if(port==0x53 && (command&15)==8)output=value;
    }
    uint16_t read(uint8_t port)const{
        if(port==0x50)return command;
        if(port==0x51)return (command&15)==8?2:0;
        if(port==0x53)return output;
        return 0;
    }
    void advance(uint64_t clocks){
        if(!enabled)return;
        phase+=clocks*44100u;
        while(phase>=2000000u){
            phase-=2000000u;
            int32_t raw=(output&1)?2500:-2500;
            // Approximate AC coupling; electrical analog values are not specified.
            filtered=raw-previous+(filtered*32604)/32768;previous=raw;
            if(samples.size()<8820)samples.push_back(int16_t(filtered));else ++droppedSamples;
        }
    }
};

class MN1610 {
public:
    LkitTv tv;
    LkitScaSound scaSound;
    bool basicRomLoaded=false;
    // Stage 36: GUI LED presentation watches the monitor display buffer itself.
    // 001BH..001EH = D4..D1, 001FH..0022H = A4..A1.
    // The MN1610/ROM still executes DSCAN and all port 40H/43H I/O normally;
    // only the Windows presentation is decoupled from dynamic scan timing.
    // GUI physical order is L0..L3 = A4..A1, L4..L7 = D4..D1.
    std::array<uint16_t,8> displayFrame{};
    bool displayDirty=true;
    uint16_t visibleLed(unsigned d) const { return displayFrame[d & 7u]; }
    std::array<uint16_t,8> visibleFrame() const { return displayFrame; }
    bool takeDisplayDirty(){ bool v=displayDirty; displayDirty=false; return v; }
    uint16_t scanSelect=0;
    // Stage 27: physical I/O latches.  Monitor cassette code proves that
    // port 40 architectural bit C (mask 0008H) is CMT OUT and port 41
    // architectural bit D (mask 0004H) is CMT IN.  CMT IN idles high.
    uint16_t port40Latch=0;
    bool cmtIn=true, cmtOut=false, cmtCapture=false;
    uint64_t emuClocks=0; // Stage 28: 2 MHz CPU clock; 1 clock = 0.5 us
    uint64_t emuTimeUs() const { return emuClocks/2; }
    struct CmtEdge { uint64_t clocks; bool level; };
    std::vector<CmtEdge> cmtEdges;
    bool cmtPlayback=false; uint64_t cmtPlayStart=0; size_t cmtPlayIndex=0; bool cmtPlayInitial=true; std::vector<CmtEdge> cmtPlayEdges;
    int pressedKey=-1; // physical key index: row-major, 6 rows x 8 columns
    std::array<uint16_t,65536> mem{};
    std::array<uint16_t,8> r{}; // R0..R4, SP, STR, IC
    bool halted=false, z=false, m=false, l=false;
    // MN1610 architectural bit numbering is MSB-first.
    // E=bit0 (8000H), OVF=bit2 (2000H), M0..M2=bits5..7 (0400H..0100H).
    static constexpr uint16_t STR_E=0x8000u, STR_OVF=0x2000u, STR_MSK=0x0700u;
    // MN1610 hardware probes 02/03/07 (2026-10-06): bits 1,3,4 read as zero.
    static constexpr uint16_t STR_VALID=0xa7ffu;
    void setRegister(unsigned d,uint16_t value){r[d]=d==6?uint16_t(value&STR_VALID):value;}
    // Keep SKD driven by the ALU result; the STR storage mask is applied separately.
    // STR skip cases where masked bits affect Z/M still need a hardware probe.
    void storeResult(unsigned d,uint16_t value){setRegister(d,value);flags(value);}

    bool eFlag() const { return (r[6] & STR_E)!=0; }
    bool ovFlag() const { return (r[6] & STR_OVF)!=0; }
    void setE(bool v){ if(v) r[6]|=STR_E; else r[6]&=uint16_t(~STR_E); }
    void setOV(bool v){ if(v) r[6]|=STR_OVF; else r[6]&=uint16_t(~STR_OVF); }
    // Stage 11 peripheral/interrupt state. OPSWn = 0000+2*n, NPSWn = 0100+2*n.
    uint8_t icmr=0, tobr=0;
    bool intervalEnabled=false, level0Pending=false, level1Pending=false, level2Pending=false, timerIRQActive=false;
    bool stepSwitch=false, singleStepArmed=false, singleStepUserActive=false; // physical AUTO/STEP input and hardware step gate
    uint64_t timerElapsedClocks=0;
    // Debug view follows the monitor save area, never its working registers.
    bool debugUserRunning=false, debugSavePending=true, debugSavedValid=false;
    bool debugStopped=false, debugResumeRequested=false;
    uint64_t debugRunAt=0, debugRunReleaseAt=0;
    bool showSavedRegisters() const {return stepSwitch || (!debugUserRunning && debugStopped);}
    const std::array<uint16_t,8>& displayedRegisters() const {return showSavedRegisters()?debugSaved:r;}
    uint8_t debugSaveMask=0;
    std::array<uint16_t,8> debugSaved{};
    void beginMonitorSave(){debugStopped=debugStopped || debugUserRunning;debugUserRunning=false;debugSavePending=true;debugSaveMask=0;}
    void observeSavedRegister(uint16_t a,uint16_t v){
        if(debugUserRunning || a<0x10 || a>0x17)return;
        if(debugSavePending){
            debugSaveMask|=uint8_t(1u<<(a-0x10));
            if(debugSaveMask==0xff){
                for(unsigned i=0;i<8;i++)debugSaved[i]=mem[0x10+i];
                debugSaved[a-0x10]=v;
                debugSavedValid=true;debugSavePending=false;
            }
        }else if(debugSavedValid)debugSaved[a-0x10]=v;
    }
    uint64_t steps=0;
    static const char* rn(unsigned n){ static const char* s[]={"R0","R1","R2","R3","R4","SP","STR","IC"}; return s[n&7]; }
    static const char* sk(unsigned n){ static const char* s[]={"","SKP","M","PZ","Z","NZ","MZ","P","EZ","ENZ","OZ","ONZ","LMZ","LP","LPZ","LM"}; return s[n&15]; }
    uint16_t& IC(){return r[7];} uint16_t& STR(){return r[6];} uint16_t& SP(){return r[5];}

    void reset(){ debugStopped=false;debugResumeRequested=false;debugRunAt=debugRunReleaseAt=0;debugUserRunning=false;debugSavePending=true;debugSavedValid=false;debugSaveMask=0;debugSaved.fill(0); tv.reset();scaSound.reset(); setRegister(6,mem[0x0100]); IC()=mem[0x0101]; halted=false; steps=0; z=m=l=false; icmr=tobr=0; intervalEnabled=level0Pending=level1Pending=level2Pending=timerIRQActive=false; singleStepArmed=false; singleStepUserActive=false; timerElapsedClocks=0; port40Latch=0; scanSelect=0; displayFrame.fill(0); displayDirty=true; cmtIn=true; cmtOut=false; cmtCapture=false; emuClocks=0; cmtEdges.clear(); cmtPlayback=false; cmtPlayStart=0; cmtPlayIndex=0; cmtPlayInitial=true; cmtPlayEdges.clear(); }
    void requestLevel0(){ level0Pending=true; }
    void requestLevel2(){ level2Pending=true; }
    // Lkit-16 STOP/STEP shares the monitor Level-1 entry at NPSW1 (0102/0103).
    void requestStop(){
        debugResumeRequested=false;debugRunAt=0;
        if(debugRunReleaseAt){pressedKey=-1;debugRunReleaseAt=0;}
        timerIRQActive=false;level1Pending=true;halted=false;
    }
    bool canDebugStop() const {return debugUserRunning || debugResumeRequested || debugRunReleaseAt!=0;}
    bool canDebugResume() const {return debugStopped && debugSavedValid && !debugSavePending && !debugUserRunning && !debugResumeRequested && !debugRunReleaseAt;}
    bool canDebugStep() const {return (debugUserRunning || canDebugResume()) && !debugResumeRequested && !debugRunReleaseAt;}
    void requestDebugStop(){
        if(debugUserRunning)requestStop();
        else{debugResumeRequested=false;debugRunAt=0;if(debugRunReleaseAt){pressedKey=-1;debugRunReleaseAt=0;}}
    }
    void requestDebugStep(){
        if(!canDebugStep())return;
        if(debugUserRunning)requestStop();
        stepSwitch=true;debugResumeRequested=true;debugRunAt=0;
    }
    void requestDebugContinue(){
        if(!canDebugResume())return;
        stepSwitch=false;debugResumeRequested=true;debugRunAt=0;
    }
    void serviceDebugControls(){
        if(debugRunReleaseAt && emuClocks>=debugRunReleaseAt){pressedKey=-1;debugRunReleaseAt=0;}
        if(debugResumeRequested && debugStopped && !debugRunReleaseAt && !debugUserRunning && !debugSavePending && debugSavedValid){
            // Allow the monitor to finish its save and return to key scanning.
            if(!debugRunAt)debugRunAt=emuClocks+40000;
            if(emuClocks>=debugRunAt){
                pressedKey=1;debugRunReleaseAt=emuClocks+40000;
                debugResumeRequested=false;debugRunAt=0;
            }
        }
    }
    void enterInterrupt(unsigned level){
        // L-KIT-16 monitor ROM provides NPSW0..NPSW2 at 0100H..0105H.
        // 0106H is executable monitor initialization code, not an NPSW3 vector.
        if(level>2) return;
        if(level==1 && !timerIRQActive)beginMonitorSave();
        const uint16_t opsw=uint16_t(level*2), npsw=uint16_t(0x0100+level*2);
        // MN1610: save current STR/IC to old PSW, then load the new PSW.
        mem[opsw]=STR(); mem[uint16_t(opsw+1)]=IC();
        setRegister(6,mem[npsw]); IC()=mem[uint16_t(npsw+1)]; halted=false;
        // Manual p.107: IRQ acceptance = 0 IUT + 4 EUT, no opcode fetch.
        advanceClocks(memoryEUTTicks(opsw)+memoryEUTTicks(uint16_t(opsw+1))+
            memoryEUTTicks(npsw)+memoryEUTTicks(uint16_t(npsw+1)));
    }
    void serviceLevel0(){ if(!level0Pending)return; level0Pending=false; enterInterrupt(0); }
    void serviceLevel1(){ if(!level1Pending)return; level1Pending=false; enterInterrupt(1); }
    void serviceLevel2(){ if(!level2Pending)return; level2Pending=false; enterInterrupt(2); }
    void loadOldPSW(unsigned level){
        if(level>3) return; const uint16_t a=uint16_t(level*2); setRegister(6,mem[a]); IC()=mem[uint16_t(a+1)]; halted=false;
    }
    uint64_t timerPeriodClocks() const {
        if(!intervalEnabled) return 0;
        const uint64_t unitClocks=(icmr==0x83)?20000u:2000u; // 10 ms / 1 ms at 2 MHz
        const uint64_t count=tobr?tobr:256u;
        return unitClocks*count;
    }
    void advanceClocks(uint64_t clocks){
        scaSound.advance(clocks);
        emuClocks += clocks;
        if(!intervalEnabled) return;
        timerElapsedClocks += clocks;
        const uint64_t period=timerPeriodClocks();
        if(period && timerElapsedClocks>=period){
            timerElapsedClocks%=period; timerIRQActive=true; level1Pending=true;
        }
    }
    // Compatibility helper for explicit peripheral tests. CPU execution itself uses advanceClocks().
    void advanceTimeUs(uint64_t us){ advanceClocks(us*2u); }
    // Manual pp.107-109; clock unit = 0.5 us, IUT = 2 ticks.
    // RAM EUT = 5 ticks (n=2), confirmed by T00-T14 except T03.
    // ROM timing remains provisional; preserve the old ROM fetch/operand values.
    bool timingROM(uint16_t a) const {
        return (a>=0x0100u && a<=0x08ffu) ||
            (basicRomLoaded && a>=0xd800u && a<=0xdfffu);
    }
    unsigned memoryEUTTicks(uint16_t a) const { return timingROM(a)?3u:5u; }
    unsigned fetchEUTTicks(uint16_t a) const {
        // Monitor ROM fetch was 3+2 ticks in v1.22. BASIC ROM fetch is unmeasured.
        if(a>=0x0100u && a<=0x08ffu)return 5u;
        return memoryEUTTicks(a);
    }
    unsigned ioEUTTicks(uint8_t p) const {
        // WT40 total 5.5 us = RAM fetch 2.5 + I/O transfer 3.0 (T08).
        // Other ports have not been measured; retain their prior provisional n=0.
        return p==0x40?6u:3u;
    }
    struct ExecCycles { unsigned iut,eut; };
    ExecCycles execCycles(uint16_t op) const {
        if((op&0xc700)==0x8600 || (op&0xc700)==0xc600) return {3,3}; // DMS/IMS
        if((op&0xc700)==0x8700) return {4,2}; // BAL
        if((op&0xc700)==0xc700) return {2,1}; // B: official manual p.107 (instruction fetch only)
        if((op&0xc000)==0x8000 || (op&0xc000)==0xc000) return {1,2}; // ST/L
        if((op&0xf800)==0x1000 || (op&0xf800)==0x1800) return {0,2}; // WT/RD
        if((op&0xf800)==0x0800) return {1,1}; // MVI
        if((op&0xfffc)==0x2004) return {0,3}; // LPSW
        if(op==0x2000) return {1,1}; // H
        if((op&0xf8ff)==0x2001 || (op&0xf8ff)==0x2002) return {1,2}; // PUSH/POP
        if(op==0x2003) return {2,2}; // RET
        return {1,1}; // register ALU/shift/bit/immediate families
    }
    uint64_t instructionTicks(uint16_t op,uint16_t fetchAddr) const {
        ExecCycles c=execCycles(op);
        // Table EUT count includes the instruction fetch. Split its remaining
        // transfers by target instead of assigning a single EUT to all devices.
        uint64_t ticks=uint64_t(c.iut)*2u + fetchEUTTicks(fetchAddr);
        const bool memoryModify=((op&0xc700)==0x8600)||((op&0xc700)==0xc600);
        const bool branch=(op&0xc700)==0xc700;
        const bool bal=(op&0xc700)==0x8700;
        const bool memoryOp=(op&0xc000)==0x8000 || (op&0xc000)==0xc000;
        if(memoryModify) ticks+=2u*memoryEUTTicks(eaAddr(op,fetchAddr));
        else if(bal) ticks+=memoryEUTTicks(r[5]);
        else if(branch) {} // No target memory read for direct branch.
        else if(memoryOp) ticks+=memoryEUTTicks(eaAddr(op,fetchAddr));
        else if((op&0xf800)==0x1000 || (op&0xf800)==0x1800)
            ticks+=ioEUTTicks(uint8_t(op));
        else if((op&0xfffc)==0x2004){
            const uint16_t a=uint16_t((op&3)*2);
            ticks+=memoryEUTTicks(a)+memoryEUTTicks(uint16_t(a+1));
        }
        else if((op&0xf8ff)==0x2001) ticks+=memoryEUTTicks(r[5]);
        else if((op&0xf8ff)==0x2002 || op==0x2003)
            ticks+=memoryEUTTicks(uint16_t(r[5]+1));
        // Manual p.109 note 4(b): any SKIP condition other than NO Operation adds 1 IUT.
        const bool hasSkipField = ((op&0xf808)==0x5000)||((op&0xf808)==0x5008)||
          ((op&0xf808)==0x5800)||((op&0xf808)==0x5808)||((op&0xf808)==0x6000)||
          ((op&0xf808)==0x6008)||((op&0xf808)==0x6800)||((op&0xf808)==0x6808)||
          ((op&0xf808)==0x7000)||((op&0xf808)==0x7008)||((op&0xf808)==0x7800)||
          ((op&0xf808)==0x7808)||((op&0xf800)==0x2800)||((op&0xf800)==0x3000)||
          ((op&0xf800)==0x3800)||((op&0xf800)==0x4000)||((op&0xf800)==0x4800)||
          ((op&0xf80c)==0x2008)||((op&0xf80c)==0x200c);
        if(hasSkipField && ((op>>4)&15)!=0) ticks+=2u;
        // Manual p.108 note 1 / p.109 note 4(a): address modifier M adds cycles.
        const bool hasEA=((op&0xc700)==0x8600)||((op&0xc700)==0xc600)||((op&0xc700)==0x8700)||
                         ((op&0xc700)==0xc700)||((op&0xc000)==0x8000)||((op&0xc000)==0xc000);
        if(hasEA){ static const unsigned mi[8]={0,0,0,0,1,1,1,1};
            unsigned md=(op>>11)&7; ticks+=uint64_t(mi[md])*2u;
            if(md==2 || md==3 || md==6 || md==7){
                const uint16_t pointer=(md==3)?uint16_t(fetchAddr+int8_t(op&255)):uint16_t(op&255);
                ticks+=memoryEUTTicks(pointer);
            } }
        return ticks;
    }
    bool directLoadBinary(const std::string& path,uint16_t start,std::string* err=nullptr){
        std::ifstream f(std::filesystem::u8path(path),std::ios::binary); if(!f){ if(err)*err="Cannot open BIN"; return false; }
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),{});
        return directLoadBytes(b,start,err);
    }
    bool directLoadBytes(const std::vector<uint8_t>& b,uint16_t start,std::string* err=nullptr){
        if(b.size()>131072u){ if(err)*err="Data exceeds memory"; return false; }
        uint32_t words=(uint32_t(b.size())+1u)/2u;
        if(uint32_t(start)+words>65536u){ if(err)*err="Data exceeds memory"; return false; }
        for(uint32_t i=0;i<words;i++){
            uint16_t a=uint16_t(uint32_t(start)+i);
            if((a>=0x0100u && a<=0x08ffu)||(basicRomLoaded&&a>=0xd800u&&a<=0xdfffu)){ if(err)*err="Data overlaps ROM"; return false; }
        }
        for(uint32_t i=0;i<words;i++){
            uint16_t hi=b[i*2], lo=(i*2+1<b.size())?b[i*2+1]:0;
            write(uint16_t(uint32_t(start)+i),uint16_t((hi<<8)|lo));
        }
        return true;
    }
    bool directLoadBinaryForPanel(const std::string& path,uint16_t start,std::string* err=nullptr){
        if(!directLoadBinary(path,start,err)) return false;
        // MONITOR: FUNC 3 uses B key code 22H + table base 3 = entry 0025H.
        // Update only after a successful Direct BIN Memory load; do not run it here.
        write(0x0025u,start);
        return true;
    }
    bool directSaveBinary(const std::string& path,uint16_t start,uint16_t end,std::string* err=nullptr) const {
        if(end<start){ if(err)*err="End address is below start"; return false; }
        std::ofstream f(std::filesystem::u8path(path),std::ios::binary); if(!f){ if(err)*err="Cannot create BIN"; return false; }
        for(uint32_t a=start;a<=end;a++){ uint16_t v=mem[uint16_t(a)]; f.put(char(v>>8)); f.put(char(v&255)); }
        return bool(f);
    }
    bool directLoadBtkForPanel(const std::string& path,uint16_t* loadedStart=nullptr,std::string* err=nullptr){
        std::ifstream f(std::filesystem::u8path(path),std::ios::binary);
        if(!f){ if(err)*err="Cannot open BTK"; return false; }
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)),{});
        if(file.size()<4u){ if(err)*err="BTK header is incomplete"; return false; }
        const uint16_t start=uint16_t((uint16_t(file[0])<<8)|file[1]);
        const uint16_t end=uint16_t((uint16_t(file[2])<<8)|file[3]);
        // BTK end is the last byte: start + payload byte count - 1.
        // Accept the earlier emulator's exclusive-end BTK files as well.
        const size_t payload=file.size()-4u;
        if(end<start || (payload!=size_t(end-start)+1u && payload!=size_t(end-start))){
            if(err)*err="BTK header length does not match data"; return false;
        }
        std::vector<uint8_t> data(file.begin()+4,file.end());
        if(!directLoadBytes(data,start,err)) return false;
        write(0x0025u,start); // Same FUNC 3 registration as a successful BIN load.
        if(loadedStart)*loadedStart=start;
        return true;
    }
    bool directSaveBtk(const std::string& path,uint16_t start,uint16_t lastWord,std::string* err=nullptr) const {
        if(lastWord<start){ if(err)*err="End address is below start"; return false; }
        const uint32_t bytes=(uint32_t(lastWord)-start+1u)*2u;
        const uint32_t headerEnd=uint32_t(start)+bytes-1u;
        if(headerEnd>0xffffu){ if(err)*err="BTK header end exceeds FFFFH"; return false; }
        std::ofstream f(std::filesystem::u8path(path),std::ios::binary);
        if(!f){ if(err)*err="Cannot create BTK"; return false; }
        f.put(char(start>>8));f.put(char(start&255));
        f.put(char(headerEnd>>8));f.put(char(headerEnd&255));
        for(uint32_t a=start;a<=lastWord;a++){
            const uint16_t v=mem[uint16_t(a)];f.put(char(v>>8));f.put(char(v&255));
        }
        if(!f){ if(err)*err="Cannot write BTK"; return false; }
        return true;
    }
    uint16_t read(uint16_t a) const { return mem[a]; }
    void write(uint16_t a,uint16_t v){
        if((a>=0x0100&&a<=0x08ff)||(basicRomLoaded&&a>=0xd800&&a<=0xdfff)) return;
        observeSavedRegister(a,v);
        if(mem[a]==v) return;
        mem[a]=v;
        if(a>=0x001bu && a<=0x0022u){
            // Monitor buffer -> physical front-panel order.
            // 001F..0022 -> ADDRESS L0..L3, 001B..001E -> DATA L4..L7.
            const unsigned d=(a>=0x001fu)?unsigned(a-0x001fu):unsigned(4u+a-0x001bu);
            displayFrame[d]=v;
            displayDirty=true;
        }
    }
    uint16_t eaAddr(uint16_t op,uint16_t pc) const {
        unsigned mode=(op>>11)&7; uint16_t n=op&0xff; int8_t d=(int8_t)n;
        switch(mode){
        case 0:return n; case 1:return uint16_t(pc+d); case 2:return read(n); case 3:return read(uint16_t(pc+d));
        case 4:return uint16_t(r[3]+n); case 5:return uint16_t(r[4]+n);
        // Zero-page indirect index: add the word at D, not the 8-bit D itself.
        case 6:return uint16_t(r[3]+read(n)); case 7:return uint16_t(r[4]+read(n));
        } return 0;
    }
    std::string eaText(uint16_t op,uint16_t pc) const {
        unsigned md=(op>>11)&7,n=op&255; int8_t d=(int8_t)n; std::ostringstream o;o<<std::hex<<std::uppercase<<std::setfill('0');
        switch(md){case 0:o<<"$"<<std::setw(2)<<n;break;case 1:o<<"$"<<std::setw(4)<<uint16_t(pc+d);break;case 2:o<<"[$"<<std::setw(2)<<n<<"]";break;case 3:o<<"[$"<<std::setw(4)<<uint16_t(pc+d)<<"]";break;case 4:o<<"$"<<std::setw(2)<<n<<"(R3)";break;case 5:o<<"$"<<std::setw(2)<<n<<"(R4)";break;case 6:o<<"[$"<<std::setw(2)<<n<<"]"<<"(R3)";break;case 7:o<<"[$"<<std::setw(2)<<n<<"]"<<"(R4)";break;} return o.str();
    }
    std::string dis(uint16_t pc) const {
        uint16_t op=mem[pc]; unsigned rd=(op>>8)&7,rs=op&7,k=(op>>4)&15; std::ostringstream o;o<<std::hex<<std::uppercase;
        auto rr=[&](const char*n){o<<n<<" "<<rn(rd)<<","<<rn(rs);if(k)o<<","<<sk(k);};
        if((op&0xc700)==0x8600)o<<"DMS "<<eaText(op,pc); else if((op&0xc700)==0xc600)o<<"IMS "<<eaText(op,pc); else if((op&0xc700)==0x8700)o<<"BAL "<<eaText(op,pc); else if((op&0xc700)==0xc700)o<<"B "<<eaText(op,pc);
        else if((op&0xc000)==0x8000)o<<"ST "<<rn(rd)<<","<<eaText(op,pc); else if((op&0xc000)==0xc000)o<<"L "<<rn(rd)<<","<<eaText(op,pc);
        else if((op&0xf808)==0x5000)rr("CB"); else if((op&0xf808)==0x5008)rr("C"); else if((op&0xf808)==0x5800)rr("S"); else if((op&0xf808)==0x5808)rr("A"); else if((op&0xf808)==0x6000)rr("EOR"); else if((op&0xf808)==0x6008)rr("OR"); else if((op&0xf808)==0x6800)rr("LAD"); else if((op&0xf808)==0x6808)rr("AND"); else if((op&0xf808)==0x7000)rr("DSWP"); else if((op&0xf808)==0x7008)rr("BSWP"); else if((op&0xf808)==0x7800)rr("MVB"); else if((op&0xf808)==0x7808)rr("MV");
        else if((op&0xf800)==0x0800)o<<"MVI "<<rn(rd)<<",$"<<std::setw(2)<<std::setfill('0')<<(op&255); else if((op&0xf800)==0x1000)o<<"WT "<<rn(rd)<<",$"<<std::setw(2)<<std::setfill('0')<<(op&255); else if((op&0xf800)==0x1800)o<<"RD "<<rn(rd)<<",$"<<std::setw(2)<<std::setfill('0')<<(op&255);
        else if((op&0xf80c)==0x2008){static const char* ee[]={"","RE","SE","CE"};o<<"SR "<<rn(rd);if(op&3)o<<","<<ee[op&3];if(k)o<<","<<sk(k);}
        else if((op&0xf80c)==0x200c){static const char* ee[]={"","RE","SE","CE"};o<<"SL "<<rn(rd);if(op&3)o<<","<<ee[op&3];if(k)o<<","<<sk(k);} else if(op==0x2000)o<<"H"; else if((op&0xf8ff)==0x2001)o<<"PUSH "<<rn(rd); else if((op&0xf8ff)==0x2002)o<<"POP "<<rn(rd); else if(op==0x2003)o<<"RET"; else if((op&0xfffc)==0x2004)o<<"LPSW "<<(op&3); else if((op&0xf800)==0x2800)o<<"TBIT "<<rn(rd)<<","<<(op&15); else if((op&0xf800)==0x3000)o<<"RBIT "<<rn(rd)<<","<<(op&15); else if((op&0xf800)==0x3800)o<<"SBIT "<<rn(rd)<<","<<(op&15); else if((op&0xf800)==0x4000)o<<"SI "<<rn(rd)<<","<<(op&15); else if((op&0xf800)==0x4800)o<<"AI "<<rn(rd)<<","<<(op&15); else o<<"DW $"<<std::setw(4)<<std::setfill('0')<<op; return o.str();
    }
    void flags(uint16_t v){z=(v==0);m=(v&0x8000)!=0;}
    bool cond(unsigned k) const {
        // MN1610 skip names follow the architectural encoding used by the original ROM/MAME:
        // 0,SKP,M,PZ,Z,NZ,MZ,P,EZ,ENZ,OZ,ONZ,LMZ,LP,LPZ,LM.
        switch(k&15){
        case 0:return false; case 1:return true;
        case 2:return m; case 3:return !m; case 4:return z; case 5:return !z;
        case 6:return m||z; case 7:return !m&&!z;
        case 8:return !eFlag(); case 9:return eFlag(); case 10:return !ovFlag(); case 11:return ovFlag();
        case 12:return !l||z; case 13:return l&&!z; case 14:return l||z; case 15:return !l&&!z;
        } return false;
    }
    void add16(unsigned d,uint16_t a,uint16_t b){
        // L-KIT-16 User's Manual p.107/p.109: A sets E from C0 (carry out
        // of architectural bit 0/MSB) and OVF from C0 xor C1.
        uint32_t q=uint32_t(a)+uint32_t(b); uint16_t v=uint16_t(q);
        setE((q&0x10000u)!=0); l=eFlag(); setOV((~(a^b)&(a^v)&0x8000u)!=0); storeResult(d,v);
    }
    void sub16(unsigned d,uint16_t a,uint16_t b,bool store=true){
        // S is Rd + ~Rs + 1.  The final carry is C0, hence E=1 means no borrow.
        uint16_t v=uint16_t(a-b); setE(a>=b); l=eFlag(); setOV(((a^b)&(a^v)&0x8000u)!=0);
        if(store) setRegister(d,v);
        flags(v);
    }
    void cmp16(uint16_t a,uint16_t b){
        // C only feeds the skip-condition circuit; the manual does not mark
        // E or OVF as modified for C/CB.
        uint16_t v=uint16_t(a-b); l=(a>=b); flags(v);
    }
    uint16_t loadAdjustPart(uint16_t a,uint16_t b){
        // LAD records binary carries out of each 4-bit digit of Rd+Rs.
        // BCD addition first biases one operand with 6666H; LAD itself must
        // therefore detect sum >= 16, not sum >= 10. E and OVF are unchanged.
        uint16_t mask=0; unsigned carry=0;
        for(unsigned digit=0; digit<4; ++digit){
            unsigned shift=digit*4;
            unsigned sum=((a>>shift)&0x0f)+((b>>shift)&0x0f)+carry;
            carry=(sum>=16u)?1u:0u;
            if(carry) mask|=uint16_t(0x6u<<shift);
        }
        return mask;
    }
    uint16_t ioRead(uint8_t p){
        // TVIF owns readback when selected; otherwise expose the sound SCA registers.
        if(scaSound.enabled&&!tv.selected&&p>=0x50&&p<=0x53)return scaSound.read(p);
        if(p==0x50)return tv.command;
        if(p==0x51)return tv.status(emuClocks);
        if(p==0x52)return tv.readKey(emuClocks);
        if(p==0x53)return tv.output;
        // Lkit-16 I/O peripherals used by the monitor ROM.
        if(p==0x40) return port40Latch;
        if(p==0x09) return timerIRQActive ? 0x8000 : 0x0000;
        if(p==0x0c) return icmr;
        if(p==0x0d) return timerIRQActive ? 0x0001 : 0x0000;
        if(p==0x0f) return tobr;
        if(p==0x41) {
            if(cmtPlayback){
                // Stage 37: CMT comparator output is a level, not a pulse.
                // Keep the previous input level until the next recorded edge.
                // Stage 36 incorrectly restored cmtPlayInitial on every RD 41H,
                // so a LOW level survived for only one read and ROM TLOAD lost it.
                const uint64_t rel=emuClocks-cmtPlayStart;
                while(cmtPlayIndex<cmtPlayEdges.size() && cmtPlayEdges[cmtPlayIndex].clocks<=rel){
                    cmtIn=cmtPlayEdges[cmtPlayIndex].level;
                    ++cmtPlayIndex;
                }
            }
            // Port 41 bit 15 (numeric bit 0) is the physical AUTO/STEP switch.
            // The monitor tests it at RUN (0188-018C) and selects 08H/89H for port 40.
            const uint16_t stepBit = stepSwitch ? 0x0001u : 0x0000u;
            // Stage 11: real Lkit-16 6x8 keyboard matrix.  KSCAN writes 08H..0FH
            // to port 40 (one horizontal key position at a time), then reads
            // bits 1..3 of port 41.  0EH means no key.  The six physical rows
            // are encoded as the six non-zero even values used by the ROM.
            uint16_t v=0x000e;
            if(pressedKey >= 0 && pressedKey < 48) {
                const unsigned physicalRow = unsigned(pressedKey) / 8u; // top=0 .. bottom=5
                const unsigned col = unsigned(pressedKey) & 7u;
                if((scanSelect & 0x000f) == (0x0008u + col)) {
                    const unsigned group = 5u - physicalRow; // bottom=00..07, top=28..2F
                    const unsigned decoded = 2u + 2u * group;
                    v=uint16_t(0x000e ^ decoded);
                }
            }
            // CMT IN is architectural bit D = numeric 0004H.  It shares the
            // input port electrically; overwrite only that line, never the
            // keyboard or AUTO/STEP bits.
            // During an active key closure the keyboard matrix drives these
            // shared sense bits; with no key selected, BP=D is the CMT input.
            // This preserves the ROM KSCAN matrix while exposing the cassette
            // comparator to TLOAD, which runs without a key closure.
            if(pressedKey < 0 || pressedKey >= 48) {
                if(cmtIn) v|=0x0004u; else v&=uint16_t(~0x0004u);
            }
            return uint16_t(v | stepBit);
        }
        return 0;
    }
    void ioWrite(uint8_t p,uint16_t v){
        // The sound output taps OBR while TVIF continues receiving the same writes.
        if(scaSound.enabled&&p>=0x50&&p<=0x53)scaSound.write(p,v);
        if(p==0x51){tv.acknowledge(v,emuClocks);return;}
        if(p==0x50){tv.writeCommand(uint8_t(v));return;}
        if(p==0x53){tv.write(uint8_t(v),emuClocks);return;}
        if(p==0x0d){ timerIRQActive=false; }
        if(p==0x0c){ icmr=(uint8_t)v; intervalEnabled=(icmr==0x81 || icmr==0x83); timerElapsedClocks=0; if(!intervalEnabled){timerIRQActive=false; level1Pending=false;} }
        if(p==0x0f){ tobr=(uint8_t)v; timerElapsedClocks=0; }
        if(p==0x40) {
            port40Latch=v;
            scanSelect=v;
            // Cassette output proven by ROM 0455-045F/0475-0477:
            // SBIT/RBIT BP=C around WT 40H.  MN1610 BP=C => mask 0008H.
            const bool newCmtOut=(v & 0x0008u)!=0;
            if(newCmtOut!=cmtOut){ cmtOut=newCmtOut; if(cmtCapture)cmtEdges.push_back({emuClocks,cmtOut}); }
            // Monitor RUN writes 08H in AUTO or 89H in STEP.  Numeric 0080H is
            // the L-KIT16 STEP/break hardware enable.  Do not fire the interrupt
            // immediately here: RUN still has monitor instructions to execute
            // before LPSW 3 transfers control to the saved user PSW.
            singleStepArmed = (v & 0x0080u) != 0;
            if(!singleStepArmed) singleStepUserActive=false;
        }
        if(p==0x43) {
            // Stage 36: DSCAN/port 43H is still executed and accepted as hardware I/O,
            // but GUI presentation is sourced from display-buffer write-watch.
        }
    }



    bool step(){
        serviceDebugControls();
        // Remember whether this instruction itself is the user instruction being
        // single-stepped.  LPSW 3 turns singleStepUserActive on during its own
        // execution, so testing the flag only at the end would incorrectly
        // request Level 1 before any user instruction had executed.
        const bool wasSingleStepUserActive = singleStepUserActive;
        // Also normalize PSW values assigned by host/debugger callers.
        STR()&=STR_VALID;
        if(tv.selected&&(tv.command&0x80)&&tv.irq&&emuClocks>=tv.readyAt)level2Pending=true;
        // Fixed-priority interrupt acceptance: monitor-owned Level 0 first, then Level 1, then user Level 2.
        if(level0Pending && (STR() & 0x0400u)) serviceLevel0();
        else if(level1Pending && (STR() & 0x0200u)) serviceLevel1();
        else if(level2Pending && (STR() & 0x0100u)) serviceLevel2();
        if(halted)return false; uint16_t pc=IC(),op=read(pc); IC()=uint16_t(pc+1); unsigned rd=(op>>8)&7,rs=op&7,k=(op>>4)&15;
        const uint64_t executionTicks=instructionTicks(op,pc);
        auto skip=[&](){if(cond(k))IC()=uint16_t(IC()+1);};
        if((op&0xc700)==0x8600){uint16_t a=eaAddr(op,pc),v=uint16_t(read(a)-1);write(a,v);flags(v);if(v==0)IC()=uint16_t(IC()+1);}
        else if((op&0xc700)==0xc600){uint16_t a=eaAddr(op,pc),v=uint16_t(read(a)+1);write(a,v);flags(v);if(v==0)IC()=uint16_t(IC()+1);}
        else if((op&0xc700)==0xc700){IC()=eaAddr(op,pc);}
        else if((op&0xc700)==0x8700){write(SP(),IC());SP()=uint16_t(SP()-1);IC()=eaAddr(op,pc);}
        else if((op&0xc000)==0x8000){write(eaAddr(op,pc),r[rd]);}
        else if((op&0xc000)==0xc000){storeResult(rd,read(eaAddr(op,pc)));}
        else if((op&0xf800)==0x0800){storeResult(rd,uint16_t((r[rd]&0xff00)|(op&0xff)));}
        else if((op&0xf800)==0x1000){ioWrite(op&255,r[rd]);}
        else if((op&0xf800)==0x1800){storeResult(rd,ioRead(op&255));}
        else if((op&0xf808)==0x6000){storeResult(rd,uint16_t(r[rd]^r[rs]));skip();}
        else if((op&0xf808)==0x6008){storeResult(rd,uint16_t(r[rd]|r[rs]));skip();}
        else if((op&0xf808)==0x6808){storeResult(rd,uint16_t(r[rd]&r[rs]));skip();}
        else if((op&0xf808)==0x5808){add16(rd,r[rd],r[rs]);skip();}
        else if((op&0xf808)==0x5800){sub16(rd,r[rd],r[rs]);skip();}
        else if((op&0xf808)==0x7808){storeResult(rd,r[rs]);skip();}
        else if((op&0xf808)==0x7800){storeResult(rd,uint16_t((r[rd]&0xff00)|(r[rs]&0x00ff)));skip();}
        else if((op&0xf808)==0x6800){uint16_t a=r[rd],b=r[rs];storeResult(rd,loadAdjustPart(a,b));skip();}
        else if((op&0xf808)==0x7008){uint16_t v=r[rs];storeResult(rd,uint16_t((v<<8)|(v>>8)));skip();}
        else if((op&0xf808)==0x7000){uint16_t v=r[rs];storeResult(rd,uint16_t((v&0xf00fu)|((v&0x0f00u)>>4)|((v&0x00f0u)<<4)));skip();}
        else if((op&0xf808)==0x5000){uint16_t a=r[rd]&255u,b=r[rs]&255u,v=uint16_t(a-b);z=((v&0x00ffu)==0);m=(v&0x8000u)!=0;l=(a>=b);skip();}
        else if((op&0xf808)==0x5008){cmp16(r[rd],r[rs]);skip();}
        else if((op&0xf800)==0x4800){uint16_t a=r[rd],b=uint16_t(op&15);l=(uint32_t(a)+b)>0xffffu;storeResult(rd,uint16_t(a+b));skip();} // AI: p.107 leaves E/OVF unchanged
        else if((op&0xf800)==0x4000){uint16_t a=r[rd],b=uint16_t(op&15);l=(a>=b);storeResult(rd,uint16_t(a-b));skip();} // SI: p.107 leaves E/OVF unchanged
        // Hardware probe 06: TBIT feeds the masked 16-bit value to Z/M, not stale M.
        else if((op&0xf800)==0x2800){flags(uint16_t(r[rd]&(0x8000u>>(op&15))));skip();}
        else if((op&0xf800)==0x3000){storeResult(rd,uint16_t(r[rd]&~(0x8000u>>(op&15))));skip();}
        else if((op&0xf800)==0x3800){storeResult(rd,uint16_t(r[rd]|(0x8000u>>(op&15))));skip();}
        // STR destination: store result after the E update (hardware probes 02/03).
        else if((op&0xf80c)==0x2008){unsigned ee=op&3;if(ee==1)setE(false);else if(ee==2)setE(true);else if(ee==3)setE(!eFlag());uint16_t old=r[rd];bool out=(old&1)!=0;uint16_t value=uint16_t((old>>1)|(eFlag()?0x8000:0));setE(out);storeResult(rd,value);skip();}
        else if((op&0xf80c)==0x200c){unsigned ee=op&3;if(ee==1)setE(false);else if(ee==2)setE(true);else if(ee==3)setE(!eFlag());uint16_t old=r[rd];bool out=(old&0x8000)!=0;uint16_t value=uint16_t((old<<1)|(eFlag()?1:0));setE(out);storeResult(rd,value);skip();}
        else if(op==0x2000){halted=true;}
        else if((op&0xf8ff)==0x2001){write(SP(),r[rd]);SP()=uint16_t(SP()-1);}
        else if((op&0xf8ff)==0x2002){SP()=uint16_t(SP()+1);storeResult(rd,read(SP()));}
        else if(op==0x2003){SP()=uint16_t(SP()+1);IC()=read(SP());}
        else if((op&0xfffc)==0x2004){
            const unsigned psw=op&3;
            loadOldPSW(psw);
            // RUN ends with LPSW 3.  At this exact point the monitor has finished
            // its setup and the next fetched instruction is the first user
            // instruction.  This replaces the old, incorrect pc>=1000 shortcut;
            // user programs at 0040H etc. must single-step as on real hardware.
            if(psw==3 || (psw==1 && debugSavePending)){debugUserRunning=true;debugSavePending=false;debugStopped=false;}
            if(psw==3 && singleStepArmed) singleStepUserActive=true;
        }
        else { halted=true; } // MN1610 treats undefined instructions as H.
        ++steps;
        // Hardware STEP request: after LPSW 3 has entered the saved user PSW,
        // execute exactly one user instruction and request Level 1.  The monitor
        // Level-1 routine saves the interrupted PSW/registers and returns to the
        // panel.  Address is deliberately irrelevant (0040H is valid user RAM).
        if(singleStepArmed && wasSingleStepUserActive && !timerIRQActive) {
            singleStepUserActive=false;
            level1Pending=true;
        }
        advanceClocks(executionTicks);
        return true;
    }
};
static std::vector<MN1610::CmtEdge> decodeCmtInput(const std::vector<int16_t>& pcm,uint32_t rate){
    if(!rate) return {};
    // Functional KCS discriminator, not an electrical model of the input circuit.
    // A retriggered 195 us pulse left short HIGH gaps even on a 2400 Hz MARK;
    // the ROM's two-point start test could mistake those gaps for a start bit.
    // Hold the recovered level after two consistent half-cycle measurements.
    // The original ROM still performs every framing/address/data/checksum step.
    std::vector<MN1610::CmtEdge> edges;
    int peak=0;for(int16_t v:pcm) peak=std::max(peak,std::abs(int(v)));
    const int schmitt=std::max(256,peak/40);
    int state=0;double previous=0;
    uint64_t lastCrossing=0;
    bool haveCrossing=false,line=true,candidate=true;
    unsigned stable=0;
    for(size_t i=0;i<pcm.size();++i){
        const uint64_t now=(uint64_t(i)*2000000u)/rate;
        // No carrier for 1.2 ms: restore the idle input level.
        if(haveCrossing && now-lastCrossing>2400u){
            if(!line){line=true;edges.push_back({lastCrossing+2400u,true});}
            haveCrossing=false;stable=0;
        }
        int next=state;
        if(pcm[i]>=schmitt) next=1;else if(pcm[i]<=-schmitt) next=-1;
        if(state && next!=state){
            const double target=next>0?schmitt:-schmitt;
            double fraction=i && double(pcm[i])!=previous?
                (target-previous)/(double(pcm[i])-previous):0;
            fraction=std::max(0.0,std::min(1.0,fraction));
            const uint64_t crossing=uint64_t((double(i?i-1:0)+fraction)*2000000.0/rate+0.5);
            if(haveCrossing){
                const uint64_t half=crossing-lastCrossing;
                if(half>=200u && half<=1300u){
                    // 2 MHz units: nominal half-periods 416.7 (MARK) / 833.3 (SPACE).
                    const bool level=half>=625u;
                    if(stable && level==candidate) ++stable;
                    else{candidate=level;stable=1;}
                    if(stable>=2 && line!=candidate){line=candidate;edges.push_back({crossing,line});}
                }else stable=0;
            }
            lastCrossing=crossing;haveCrossing=true;
        }
        state=next;previous=pcm[i];
    }
    if(haveCrossing && !line) edges.push_back({lastCrossing+2400u,true});
    return edges;
}


// A modal transfer loop blocks WM_TIMER/WM_KEYUP; release the FUNC strobe
// before executing the monitor's key-release wait inside TLOAD/TSAVE.
static void releaseCmtFunctionKey(MN1610& cpu){
    if(cpu.pressedKey==2)cpu.pressedKey=-1;
}
// Mounting a cassette arms it; tape time begins when the monitor enters TLOAD.
static void mountCmtPlayback(MN1610& cpu,std::vector<MN1610::CmtEdge> edges){
    cpu.cmtPlayEdges=std::move(edges);cpu.cmtPlayback=false;
    cpu.cmtPlayInitial=true;cpu.cmtIn=true;cpu.cmtPlayIndex=0;cpu.cmtPlayStart=cpu.emuClocks;
}
static void startCmtPlayback(MN1610& cpu){
    cpu.cmtPlayIndex=0;cpu.cmtPlayInitial=true;cpu.cmtIn=true;
    cpu.cmtPlayStart=cpu.emuClocks;cpu.cmtPlayback=true;
}

#ifndef _WIN32
int main(){
    std::cerr << "This project targets Windows / Visual Studio 2022.\n";
    return 0;
}
#else
#include "tv_font_file.h"

static MN1610 g_cpu;
static bool g_loaded=false;
static int g_keyReleaseTicks=0;
static int g_resetVisualTicks=0; // cosmetic only; RESET semantics remain immediate hardware reset

static int g_pcKeyboardPhysicalKey=-1;
static HWND g_editLoadAddr=nullptr,g_editSaveStart=nullptr,g_editSaveEnd=nullptr;
static HWND g_binWnd=nullptr,g_mainWnd=nullptr,g_regWnd=nullptr,g_tvWnd=nullptr;
static constexpr UINT IDM_DIRECT_BIN=2001;
static constexpr UINT IDM_CMT_LOAD_WAV=2002;
static constexpr UINT IDM_CMT_SAVE_WAV=2003;
static constexpr UINT IDM_CPU_REGISTERS=2004;
static constexpr UINT IDM_TV=2005,IDM_BASIC_START=2006;
static constexpr UINT IDM_TV_COLOR=2007,IDM_TV_SOUND=2008,IDM_TV_BG=2010;
static constexpr UINT IDM_SCA_SOUND=2020,IDM_TV_FONT_CREATE=2021,IDM_TV_FONT_RELOAD=2022;
static LkitTvFont g_tvFont;
static bool g_tvFontExternal=false;
static bool g_tvColor=false,g_tvSound=true; static unsigned g_tvBackground=7;
static bool g_cmtSaveArmed=false; static bool g_cmtSaveEntered=false; static uint64_t g_cmtSaveStart=0; static bool g_cmtSaveInitial=false; static std::wstring g_cmtSavePath;
// Cassette transfers use virtual time only. The monitor ROM still
// sees its normal timer/WAIT behaviour, but CMT LOAD/SAVE is fast-forwarded
// computationally; no wall-clock waiting is used to create or consume WAV.
static bool g_cmtLoadMounted=false; static bool g_cmtLoadEntered=false;
static uint16_t editHex(HWND e,uint16_t def){ wchar_t b[32]{}; GetWindowTextW(e,b,31); wchar_t* ep=nullptr; unsigned long v=wcstoul(b,&ep,16); return (ep==b||v>0xffff)?def:uint16_t(v); }
static std::string narrow(const wchar_t* w){ int n=WideCharToMultiByte(CP_UTF8,0,w,-1,nullptr,0,nullptr,nullptr); std::string s(n?size_t(n-1):0,'\0'); if(n>1) WideCharToMultiByte(CP_UTF8,0,w,int(wcslen(w)),s.data(),n-1,nullptr,nullptr); return s; }
static bool chooseBin(HWND h,bool save,std::string& path,bool btk=false){ wchar_t fn[MAX_PATH]=L""; OPENFILENAMEW o{}; o.lStructSize=sizeof(o);o.hwndOwner=h;o.lpstrFilter=btk?L"BTKファイル (*.btk)\0*.btk\0すべてのファイル (*.*)\0*.*\0\0":L"BINファイル (*.bin)\0*.bin\0すべてのファイル (*.*)\0*.*\0\0";o.lpstrFile=fn;o.nMaxFile=MAX_PATH;o.lpstrTitle=save?L"保存先ファイルを選択":L"読み込むファイルを選択";o.lpstrDefExt=btk?L"btk":L"bin";o.Flags=OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST); bool ok=save?GetSaveFileNameW(&o):GetOpenFileNameW(&o); if(ok) path=narrow(fn); return ok; }

// PC keyboard -> physical L-KIT-16 key index.  This only closes the same
// key-matrix contact as the front-panel button; monitor semantics remain in ROM.
// Main Enter is STORE.  Numpad Enter is distinguished by the extended-key bit
// and is mapped to hexadecimal E as requested.
static int pcKeyToPhysical(WPARAM vk, LPARAM lp){
    if(vk>='0' && vk<='9'){
        unsigned v=unsigned(vk-'0');
        return v<8 ? int(40+v) : int(32+(v-8));
    }
    if(vk>='A' && vk<='F') return int(34+(vk-'A'));
    if(vk>=VK_NUMPAD0 && vk<=VK_NUMPAD9){
        unsigned v=unsigned(vk-VK_NUMPAD0);
        return v<8 ? int(40+v) : int(32+(v-8));
    }
    switch(vk){
      case VK_F1:  return 8;  // L/R0
      case VK_F2:  return 9;  // ST/R1
      case VK_F3:  return 10; // B/R2
      case VK_F4:  return 11; // BAL/X0
      case VK_F5:  return 0;  // STOP
      case VK_F6:  return 1;  // RUN
      case VK_F7:  return 2;  // FUNC
      case VK_F8:  return 3;  // CANCL
      case VK_F9:  return 4;  // SET ADRS
      case VK_F10: return 5;  // ASMBL
      case VK_F11: return 6;  // STORE (main Enter also remains STORE)
      case VK_F12: return 7;  // DISP
      case VK_DIVIDE:   return 34; // numpad / -> A
      case VK_MULTIPLY: return 35; // numpad * -> B
      case VK_SUBTRACT: return 36; // numpad - -> C
      case VK_ADD:      return 37; // numpad + -> D
      case VK_DECIMAL:  return 39; // numpad . -> F
      case VK_RETURN:
        return (lp & (1LL<<24)) ? 38 : 6; // numpad Enter -> E, main Enter -> STORE
      default: return -1;
    }
}


// CMT file I/O is WAV-only. No MP3/Media Foundation path is used.
// WAV is converted only to the electrical waveform presented at port 41H.D;
// the original monitor TLOAD at 0494H performs all framing/address/data/checks.
static uint16_t rd16le(std::ifstream& f){ unsigned char b[2]{}; f.read((char*)b,2); return uint16_t(b[0]|(uint16_t(b[1])<<8)); }
static uint32_t rd32le(std::ifstream& f){ unsigned char b[4]{}; f.read((char*)b,4); return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24); }
static bool decodeWavMono16(const std::filesystem::path& path,std::vector<int16_t>& pcm,UINT32& rate,std::wstring& er){
    pcm.clear(); rate=0;
    std::ifstream f(path,std::ios::binary); if(!f){er=L"WAVファイルを開けません。";return false;}
    char id[4]{}; f.read(id,4); (void)rd32le(f); char wave[4]{}; f.read(wave,4);
    if(std::string(id,4)!="RIFF" || std::string(wave,4)!="WAVE"){er=L"RIFF/WAVE形式のファイルではありません。";return false;}
    uint16_t fmt=0,ch=0,bits=0,align=0; uint32_t hz=0; std::vector<unsigned char> data;
    while(f && (!fmt || data.empty())){
        char cid[4]{}; if(!f.read(cid,4))break; uint32_t n=rd32le(f); std::streampos next=f.tellg(); next+=std::streamoff(n+(n&1u));
        std::string c(cid,4);
        if(c=="fmt " && n>=16){ fmt=rd16le(f); ch=rd16le(f); hz=rd32le(f); (void)rd32le(f); align=rd16le(f); bits=rd16le(f); }
        else if(c=="data"){ data.resize(n); if(n)f.read((char*)data.data(),n); }
        f.seekg(next);
    }
    if((fmt!=1 && fmt!=3)||ch<1||hz==0||align==0||data.empty()){er=L"対応していないWAV形式です。PCMまたはIEEE浮動小数点形式をご使用ください。";return false;}
    if(fmt==1 && !(bits==8||bits==16||bits==24||bits==32)){er=L"対応していないPCMのビット数です。";return false;}
    if(fmt==3 && bits!=32){er=L"IEEE浮動小数点形式は32ビットのWAVのみ対応しています。";return false;}
    size_t frames=data.size()/align; pcm.reserve(frames); rate=hz;
    auto clamp16=[](double v){ if(v>32767.0)v=32767.0; if(v<-32768.0)v=-32768.0; return int16_t(v); };
    for(size_t i=0;i<frames;i++){
        const unsigned char* q=data.data()+i*align; double sum=0.0;
        for(unsigned c=0;c<ch;c++){
            const unsigned char* x=q+c*(bits/8); double v=0;
            if(fmt==3){ float z=0; std::memcpy(&z,x,4); v=double(z)*32767.0; }
            else if(bits==8) v=(int(x[0])-128)*256.0;
            else if(bits==16) v=int16_t(uint16_t(x[0])|(uint16_t(x[1])<<8));
            else if(bits==24){ int32_t z=int32_t(x[0])|(int32_t(x[1])<<8)|(int32_t(x[2])<<16); if(z&0x800000)z|=~0xffffff; v=z/256.0; }
            else { int32_t z=int32_t(uint32_t(x[0])|(uint32_t(x[1])<<8)|(uint32_t(x[2])<<16)|(uint32_t(x[3])<<24)); v=z/65536.0; }
            sum+=v;
        }
        pcm.push_back(clamp16(sum/double(ch)));
    }
    if(pcm.empty()){er=L"WAVファイルに音声データがありません。";return false;} return true;
}
static bool chooseWav(HWND owner,std::wstring& path){
    wchar_t f[MAX_PATH]=L""; OPENFILENAMEW o{}; o.lStructSize=sizeof(o);o.hwndOwner=owner;o.lpstrFile=f;o.nMaxFile=MAX_PATH;
    o.lpstrFilter=L"カセットWAVファイル (*.wav)\0*.wav\0\0";o.lpstrTitle=L"読み込むカセットWAVを選択";o.lpstrDefExt=L"wav";o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST; if(!GetOpenFileNameW(&o))return false; path=f;return true;
}
static bool mountRealCassetteWav(HWND owner){
    std::wstring path,er; if(!chooseWav(owner,path))return false; std::vector<int16_t> pcm; UINT32 rate=0;
    if(!decodeWavMono16(path,pcm,rate,er)){MessageBoxW(owner,er.c_str(),L"カセットWAV読込",MB_OK|MB_ICONERROR);return false;}
    // Decode only the external modem level. The unmodified ROM still handles
    // start/stop bits, addresses, data, and checksum in instruction time.
    auto e=decodeCmtInput(pcm,rate);
    mountCmtPlayback(g_cpu,std::move(e));
    g_cmtLoadMounted=true;g_cmtLoadEntered=false;
    std::wstringstream ss;ss<<L"カセットWAVのロード待機に入りました。\nサンプル数："<<pcm.size()<<L"　サンプリング周波数："<<rate<<L" Hz\n\nL/R0 → FUNC（FUNC 1）でロードを開始してください。";
    MessageBoxW(owner,ss.str().c_str(),L"カセットWAV読込",MB_OK|MB_ICONINFORMATION); return true;
}

// L-KIT16 cassette SAVE uses nominal KCS 1200/2400 Hz.
// the nominal modulation frequencies.  Do not calibrate the specification from recording FFT peaks;
// tape speed, recorder response and recording/codec effects may shift/weight measured peaks.
// The real ROM TSAVE still supplies the logical 40H.C timeline and all framing/data.
// The ROM TSAVE supplies the complete logical 40H.C timeline. The host only
// converts that timeline to KCS carrier; it never rebuilds the tape format.
static bool writeCmtWav(const std::filesystem::path& path,uint64_t startClock,uint64_t endClock,bool initialLevel,const std::vector<MN1610::CmtEdge>& edges){
    if(endClock<=startClock) return false;
    constexpr uint32_t RATE=44100;
    constexpr double KCS_SPACE_HZ=1200.0, KCS_MARK_HZ=2400.0;
    constexpr int AMP=18000;
    // Preserve the observed recording lead/trail silence only as file padding; it is not
    // used to redefine KCS carrier frequencies.
    constexpr double PRE=0.40, POST=0.70;
    uint64_t total=endClock-startClock; uint64_t core=(total*RATE+1000000u)/2000000u;
    uint64_t pre=uint64_t(PRE*RATE+0.5), post=uint64_t(POST*RATE+0.5), ns=pre+core+post;
    if(ns>uint64_t(RATE)*1800u) return false;
    std::vector<int16_t> pcm((size_t)ns,0); size_t ei=0; bool lev=initialLevel; double phase=0.0;
    while(ei<edges.size() && edges[ei].clocks<=startClock){lev=edges[ei].level;++ei;}
    for(uint64_t j=0;j<core;j++){
        uint64_t clk=startClock+(j*2000000u)/RATE;
        while(ei<edges.size() && edges[ei].clocks<=clk){lev=edges[ei].level;++ei;}
        // 40H.C LOW is MARK (2400 Hz), HIGH is SPACE (1200 Hz).  This is only
        // the external KCS modem polarity; the monitor ROM still creates all
        // start/stop/data/checksum framing.
        const double hz=lev?KCS_SPACE_HZ:KCS_MARK_HZ;
        phase+=hz/double(RATE); if(phase>=1.0) phase-=1.0;
        pcm[(size_t)(pre+j)]=(phase<0.5)?AMP:-AMP;
    }
    std::ofstream o(path,std::ios::binary); if(!o)return false;
    uint32_t dataBytes=(uint32_t)(pcm.size()*2), riff=36+dataBytes; uint16_t fmt=1,ch=1,bps=16,align=2; uint32_t br=RATE*2,fs=16;
    o.write("RIFF",4);o.write((char*)&riff,4);o.write("WAVEfmt ",8);o.write((char*)&fs,4);o.write((char*)&fmt,2);o.write((char*)&ch,2);o.write((char*)&RATE,4);o.write((char*)&br,4);o.write((char*)&align,2);o.write((char*)&bps,2);o.write("data",4);o.write((char*)&dataBytes,4);o.write((char*)pcm.data(),dataBytes); return !!o;
}
static bool armCmtSave(HWND owner){
    wchar_t f[MAX_PATH]=L"LKIT16_SAVE.wav"; OPENFILENAMEW o{}; o.lStructSize=sizeof(o);o.hwndOwner=owner;o.lpstrFile=f;o.nMaxFile=MAX_PATH;
    o.lpstrFilter=L"WAVファイル (*.wav)\0*.wav\0\0";o.lpstrTitle=L"カセットWAVの保存先を指定";o.lpstrDefExt=L"wav";o.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
    if(!GetSaveFileNameW(&o))return false; g_cmtSavePath=f; g_cpu.cmtEdges.clear(); g_cpu.cmtCapture=false; g_cmtSaveStart=0; g_cmtSaveInitial=false; g_cmtSaveArmed=true; g_cmtSaveEntered=false;
    MessageBoxW(owner,L"保存待機に入りました。\nMONITORで保存範囲を設定し、ST/R1 → FUNC（FUNC 2）を押してください。\n保存が完了するとWAVファイルを書き出します。",L"カセットWAV保存",MB_OK|MB_ICONINFORMATION); return true;
}
static void pollCmtTransfer(HWND owner){
    // LOAD: once the real ROM enters TLOAD, execute it as fast as the host can.
    // emuClocks still advances instruction-by-instruction, so ROM timer waits
    // and the WAV-derived 41H.D timeline retain their virtual timing.
    if(g_cmtLoadMounted){
        uint16_t pc=g_cpu.IC();
        if(!g_cmtLoadEntered && pc>=0x0494 && pc<=0x04fb){
            releaseCmtFunctionKey(g_cpu);g_keyReleaseTicks=0;
            startCmtPlayback(g_cpu);g_cmtLoadEntered=true;
        }
        if(g_cmtLoadEntered){
            bool back=false,loadError=false;
            for(unsigned n=0;n<200000000u && !g_cpu.halted;n++){
                if(!g_cpu.step()) break;
                uint16_t q=g_cpu.IC();
                if(q==0x04b5)loadError=true; // Native TLOAD error path.
                if(q>=0x014b && q<=0x0153){ back=true; break; }
            }
            g_cmtLoadMounted=false;g_cmtLoadEntered=false;g_cpu.cmtPlayback=false;
            if(loadError) MessageBoxW(owner,L"MONITORがカセットの読込エラーを検出しました。\nWAVファイルとファイルIDの指定を確認してください。",L"カセットWAV読込",MB_OK|MB_ICONWARNING);
            else if(back) MessageBoxW(owner,L"カセットWAVのロードが完了しました。",L"カセットWAV読込",MB_OK|MB_ICONINFORMATION);
            else MessageBoxW(owner,L"ロード処理が制限時間内に完了しませんでした。\nWAVファイルとロード操作を確認してください。",L"カセットWAV読込",MB_OK|MB_ICONWARNING);
        }
    }

    // SAVE: likewise, enter the real TSAVE and fast-forward it.  Capture the
    // complete 40H.C virtual-time waveform first, then synthesize the WAV in
    // one offline pass.  WAV generation itself never waits on a timer.
    if(!g_cmtSaveArmed) return;
    uint16_t pc=g_cpu.IC();
    if(!g_cmtSaveEntered && pc>=0x0416 && pc<=0x0493){
        releaseCmtFunctionKey(g_cpu);g_keyReleaseTicks=0;
        g_cmtSaveEntered=true; g_cmtSaveStart=g_cpu.emuClocks; g_cmtSaveInitial=g_cpu.cmtOut;
        g_cpu.cmtEdges.clear(); g_cpu.cmtCapture=true;
    }
    if(g_cmtSaveEntered){
        bool back=false;
        for(unsigned n=0;n<200000000u && !g_cpu.halted;n++){
            if(!g_cpu.step()) break;
            uint16_t q=g_cpu.IC();
            if(q>=0x014b && q<=0x0153){ back=true; break; }
        }
        uint64_t end=g_cpu.emuClocks; g_cpu.cmtCapture=false;
        bool ok=back && g_cpu.cmtEdges.size()>8 && writeCmtWav(g_cmtSavePath,g_cmtSaveStart,end,g_cmtSaveInitial,g_cpu.cmtEdges);
        g_cmtSaveArmed=false;
        MessageBoxW(owner,ok?L"カセットWAVの保存が完了しました。":L"カセットWAVを保存できませんでした。",L"カセットWAV保存",MB_OK|(ok?MB_ICONINFORMATION:MB_ICONERROR));
    }
}

static bool handlePcKeyboardMessage(const MSG& m){
    HWND f=GetFocus();
    if(g_tvWnd&&f&&(f==g_tvWnd||IsChild(g_tvWnd,f)))return false;
    if(g_binWnd && f && (f==g_binWnd || IsChild(g_binWnd,f))) return false;
    if(m.message!=WM_KEYDOWN && m.message!=WM_SYSKEYDOWN &&
       m.message!=WM_KEYUP && m.message!=WM_SYSKEYUP) return false;
    int physical=pcKeyToPhysical(m.wParam,m.lParam);
    if(physical<0) return false;
    if(m.message==WM_KEYDOWN || m.message==WM_SYSKEYDOWN){
        g_pcKeyboardPhysicalKey=physical;
        if(physical==0)g_cpu.requestStop();
        else g_cpu.pressedKey=physical;
        g_keyReleaseTicks=0;
    } else if(g_pcKeyboardPhysicalKey==physical){
        if(g_cpu.pressedKey==physical) g_cpu.pressedKey=-1;
        g_pcKeyboardPhysicalKey=-1;
    }
    return true;
}

static const wchar_t* keyNames[48] = {
 L"STOP",L"RUN",L"FUNC",L"CANCL",L"SET ADRS",L"ASMBL",L"STORE",L"DISP",
 L"L|R0",L"ST|R1",L"B|R2",L"BAL|X0",L"IMS|X1",L"DMS|SP",L"BSWP|STR",L"DSWP|IC",
 L"MV|hh",L"MVB|*",L"MVI|(hh)",L"AND|(+*)",L"OR|hh(X0)",L"EOR|hh(X1)",L"C|(hh)(X0)",L"CB|(hh)(X1)",
 L"A|",L"AI|RE",L"S|SE",L"SI|CE",L"SL|+",L"SR|-",L"PUSH|",L"POP|",
 L"LAD|EZ|8",L"SBIT|ENZ|9",L"RBIT|OZ|A",L"TBIT|ONZ|B",L"RD|LMZ|C",L"WT|LP|D",L"LPSW|LPZ|E",L"RET|LM|F",
 L"H||0",L"NOP|SKP|1",L"SKIP|M|2",L"CLEAR|PZ|3",L"|Z|4",L"|NZ|5",L"|MZ|6",L"DC|P|7"};

static void splitKeyLabel(const wchar_t* src,std::wstring& a,std::wstring& b,std::wstring& c){
    std::wstring v=src?src:L""; size_t p1=v.find(L'|');
    if(p1==std::wstring::npos){ a=v; b.clear(); c.clear(); return; }
    a=v.substr(0,p1); size_t p2=v.find(L'|',p1+1);
    if(p2==std::wstring::npos){ b.clear(); c=v.substr(p1+1); return; } // two labels => rows 1 and 3
    b=v.substr(p1+1,p2-p1-1); c=v.substr(p2+1);
}
static void centerText(HDC dc,const RECT& r,const std::wstring& t,HFONT f){
    if(t.empty()) return; HGDIOBJ of=SelectObject(dc,f); RECT q=r;
    DrawTextW(dc,t.c_str(),(int)t.size(),&q,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    SelectObject(dc,of);
}
static void drawKeyTop(const DRAWITEMSTRUCT* d){
    HDC dc=d->hDC; RECT r=d->rcItem; bool down=(d->itemState&ODS_SELECTED)!=0;
    HBRUSH edge=CreateSolidBrush(RGB(105,101,88)); FillRect(dc,&r,edge); DeleteObject(edge);
    InflateRect(&r,-3,-3); HBRUSH face=CreateSolidBrush(down?RGB(214,209,185):RGB(239,235,211)); FillRect(dc,&r,face); DeleteObject(face);
    HPEN hi=CreatePen(PS_SOLID,1,RGB(255,255,244)), lo=CreatePen(PS_SOLID,1,RGB(92,88,78));
    HGDIOBJ op=SelectObject(dc,down?lo:hi); MoveToEx(dc,r.left,r.bottom-1,nullptr);LineTo(dc,r.left,r.top);LineTo(dc,r.right-1,r.top);
    SelectObject(dc,down?hi:lo); LineTo(dc,r.right-1,r.bottom-1);LineTo(dc,r.left,r.bottom-1); SelectObject(dc,op);DeleteObject(hi);DeleteObject(lo);
    int id=(int)d->CtlID-1000; if(id<0||id>=48)return;
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(78,72,60));
    HFONT normal=CreateFontW(17,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Arial");
    HFONT hex=CreateFontW(24,0,0,0,FW_HEAVY,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_MODERN,L"Arial Black");
    HFONT ctl=CreateFontW(18,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Arial");
    if(id<8){
        // The real panel uses orange legends on the eight control keytops.
        SetTextColor(dc,RGB(214,104,24));
        // Control keys are NOT three-row keytops.  Draw the complete legend as
        // one block and calculate its height explicitly so it is visually
        // centred in the physical key, including the two-line SET ADRS key.
        std::wstring t=keyNames[id]; if(id==4)t=L"SET\nADRS";
        HGDIOBJ of=SelectObject(dc,ctl);
        RECT measure{0,0,r.right-r.left-4,0};
        DrawTextW(dc,t.c_str(),-1,&measure,DT_CALCRECT|DT_CENTER|DT_WORDBREAK|DT_NOPREFIX);
        int th=measure.bottom-measure.top;
        RECT q{r.left+2, r.top+((r.bottom-r.top)-th)/2, r.right-2, r.top+((r.bottom-r.top)-th)/2+th};
        DrawTextW(dc,t.c_str(),-1,&q,DT_CENTER|DT_WORDBREAK|DT_NOPREFIX);
        SelectObject(dc,of);
    }
    else { std::wstring a,b,c;splitKeyLabel(keyNames[id],a,b,c); int h=(r.bottom-r.top)/3; RECT r1{r.left,r.top,r.right,r.top+h},r2{r.left,r.top+h,r.right,r.top+2*h},r3{r.left,r.top+2*h,r.right,r.bottom}; centerText(dc,r1,a,normal); centerText(dc,r2,b,normal); centerText(dc,r3,c,(id>=32)?hex:normal); }
    DeleteObject(normal);DeleteObject(hex);DeleteObject(ctl);
}
static void drawToggle(HDC dc,int x,int y,bool down){
    // Repaint the whole owner-draw surface first.  This prevents the stock
    // BUTTON background and old lever positions from remaining as white debris.
    RECT all{0,0,46,64}; HBRUSH panel=CreateSolidBrush(RGB(218,218,207)); FillRect(dc,&all,panel); DeleteObject(panel);
    const int cx=x+19, cy=y+22;
    // soft shadow behind the mounting washer
    HBRUSH sh=CreateSolidBrush(RGB(150,150,141)); HGDIOBJ ob=SelectObject(dc,sh); Ellipse(dc,cx-13,cy-11,cx+15,cy+17); SelectObject(dc,ob); DeleteObject(sh);
    // dark outer bezel and brushed-metal washer
    HBRUSH rim=CreateSolidBrush(RGB(75,75,70)); ob=SelectObject(dc,rim); Ellipse(dc,cx-14,cy-14,cx+14,cy+14); SelectObject(dc,ob); DeleteObject(rim);
    HBRUSH washer=CreateSolidBrush(RGB(205,204,192)); ob=SelectObject(dc,washer); Ellipse(dc,cx-11,cy-11,cx+11,cy+11); SelectObject(dc,ob); DeleteObject(washer);
    HPEN shine=CreatePen(PS_SOLID,2,RGB(246,245,231)); HGDIOBJ op=SelectObject(dc,shine); Arc(dc,cx-10,cy-10,cx+10,cy+10,cx-8,cy+5,cx+7,cy-7); SelectObject(dc,op); DeleteObject(shine);
    HBRUSH hole=CreateSolidBrush(RGB(48,48,45)); ob=SelectObject(dc,hole); Ellipse(dc,cx-5,cy-5,cx+6,cy+6); SelectObject(dc,ob); DeleteObject(hole);
    // Lever resembles the real spring toggle: long diagonal metal shaft with a rounded end.
    const int ex=cx+13, ey=cy+(down?17:-17);
    HPEN shaftShadow=CreatePen(PS_SOLID,7,RGB(70,70,66)); op=SelectObject(dc,shaftShadow); MoveToEx(dc,cx+1,cy+1,nullptr); LineTo(dc,ex+1,ey+1); SelectObject(dc,op); DeleteObject(shaftShadow);
    HPEN shaft=CreatePen(PS_SOLID,5,RGB(222,221,208)); op=SelectObject(dc,shaft); MoveToEx(dc,cx,cy,nullptr); LineTo(dc,ex,ey); SelectObject(dc,op); DeleteObject(shaft);
    HPEN shaftHi=CreatePen(PS_SOLID,1,RGB(255,255,245)); op=SelectObject(dc,shaftHi); MoveToEx(dc,cx-1,cy-1,nullptr); LineTo(dc,ex-1,ey-1); SelectObject(dc,op); DeleteObject(shaftHi);
    HBRUSH tip=CreateSolidBrush(RGB(226,225,212)); ob=SelectObject(dc,tip); Ellipse(dc,ex-5,ey-5,ex+6,ey+6); SelectObject(dc,ob); DeleteObject(tip);
}

static void drawPanelLogo(HDC dc,int x,int y){
    // Original vector artwork inspired by the real PANAFACOM LKIT-16 nameplate.
    // No bitmap/photo is used: the PF monogram and lettering are drawn directly.
    const COLORREF ink=RGB(39,39,34);
    SetBkMode(dc,TRANSPARENT);

    // PF monogram: compact, heavy interlocking mark without a surrounding box.
    // P body / bowl.
    HBRUSH br=CreateSolidBrush(ink); HGDIOBJ oldBr=SelectObject(dc,br);
    RECT r1{x+1,y+3,x+7,y+32}; FillRect(dc,&r1,br);
    RECT r2{x+6,y+3,x+19,y+8}; FillRect(dc,&r2,br);
    RECT r3{x+14,y+7,x+20,y+18}; FillRect(dc,&r3,br);
    RECT r4{x+6,y+15,x+17,y+20}; FillRect(dc,&r4,br);
    // F shares the central mass and extends to the right.
    RECT r5{x+20,y+3,x+26,y+32}; FillRect(dc,&r5,br);
    RECT r6{x+25,y+3,x+37,y+8}; FillRect(dc,&r6,br);
    RECT r7{x+25,y+15,x+35,y+20}; FillRect(dc,&r7,br);
    // small counter in the P bowl gives the distinctive open centre.
    HBRUSH panel=CreateSolidBrush(RGB(218,218,207));
    RECT cut{x+7,y+8,x+14,y+15}; FillRect(dc,&cut,panel);
    DeleteObject(panel); SelectObject(dc,oldBr); DeleteObject(br);

    // Heavy industrial sans lettering.  Condense horizontally to match the plate.
    LOGFONTW lf{}; lf.lfHeight=-31; lf.lfWeight=FW_HEAVY; lf.lfQuality=CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily=FF_SWISS; wcscpy_s(lf.lfFaceName,L"Arial Black");
    HFONT f=CreateFontIndirectW(&lf); HGDIOBJ oldF=SelectObject(dc,f);
    SetTextColor(dc,ink); SetTextCharacterExtra(dc,-2);
    XFORM oldXf{}; int gm=SetGraphicsMode(dc,GM_ADVANCED); GetWorldTransform(dc,&oldXf);
    XFORM xf{0.88f,0,0,1.0f,(FLOAT)(x+42),(FLOAT)y}; SetWorldTransform(dc,&xf);
    TextOutW(dc,0,0,L"LKIT-16",7);
    SetWorldTransform(dc,&oldXf); SetGraphicsMode(dc,gm); SetTextCharacterExtra(dc,0);
    SelectObject(dc,oldF); DeleteObject(f);
}
static void drawSegPoly(HDC dc,const POINT* pt,int n,bool on){
    HBRUSH b=CreateSolidBrush(on?RGB(255,45,25):RGB(58,16,13));
    HPEN pen=CreatePen(PS_NULL,0,RGB(0,0,0));
    HGDIOBJ ob=SelectObject(dc,b),op=SelectObject(dc,pen);
    Polygon(dc,pt,n);
    SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(pen); DeleteObject(b);
}
static void draw7(HDC dc,int x,int y,int w,int h,uint16_t seg){
    // Lkit-16 monitor ROM table at 027D: 0=00FC, 1=0060, ... F=008E.
    // Therefore: a=0080 b=0040 c=0020 d=0010 e=0008 f=0004 g=0002 dp=0001.
    const int t=6, q=3, mid=y+h/2;
    POINT a[6]={{x+q,y},{x+w-q,y},{x+w,y+q},{x+w-q,y+t},{x+q,y+t},{x,y+q}};
    POINT g[6]={{x+q,mid-t/2},{x+w-q,mid-t/2},{x+w,mid},{x+w-q,mid+t/2},{x+q,mid+t/2},{x,mid}};
    POINT d[6]={{x+q,y+h-t},{x+w-q,y+h-t},{x+w,y+h-q},{x+w-q,y+h},{x+q,y+h},{x,y+h-q}};
    POINT f[6]={{x,y+q},{x+q,y},{x+t,y+q},{x+t,mid-q},{x+q,mid},{x,mid-q}};
    POINT b[6]={{x+w-t,y+q},{x+w-q,y},{x+w,y+q},{x+w,mid-q},{x+w-q,mid},{x+w-t,mid-q}};
    POINT e[6]={{x,y+mid-y+q},{x+q,mid},{x+t,mid+q},{x+t,y+h-q},{x+q,y+h},{x,y+h-q}};
    POINT c[6]={{x+w-t,mid+q},{x+w-q,mid},{x+w,mid+q},{x+w,y+h-q},{x+w-q,y+h},{x+w-t,y+h-q}};
    drawSegPoly(dc,a,6,(seg&0x0080u)!=0);
    drawSegPoly(dc,b,6,(seg&0x0040u)!=0);
    drawSegPoly(dc,c,6,(seg&0x0020u)!=0);
    drawSegPoly(dc,d,6,(seg&0x0010u)!=0);
    drawSegPoly(dc,e,6,(seg&0x0008u)!=0);
    drawSegPoly(dc,f,6,(seg&0x0004u)!=0);
    drawSegPoly(dc,g,6,(seg&0x0002u)!=0);
    if(seg&0x0001u){ HBRUSH br=CreateSolidBrush(RGB(255,45,25)); HGDIOBJ old=SelectObject(dc,br); Ellipse(dc,x+w+4,y+h-7,x+w+10,y+h-1); SelectObject(dc,old); DeleteObject(br); }
}
static bool validateSevenSegEncoding(){
    // Directly derived from the monitor ROM's table at 027D-028C.
    static const uint16_t romHex[16]={0x00FC,0x0060,0x00DA,0x00F2,0x0066,0x00B6,0x00BE,0x00E4,
                                     0x00FE,0x00F6,0x00EE,0x003E,0x009C,0x007A,0x009E,0x008E};
    return romHex[0]==(0x80|0x40|0x20|0x10|0x08|0x04) &&
           romHex[1]==(0x40|0x20) && romHex[8]==0x00FE &&
           romHex[15]==(0x80|0x08|0x04|0x02);
}
static std::string exeDirectory(){
    wchar_t w[MAX_PATH]{}; DWORD n=GetModuleFileNameW(nullptr,w,MAX_PATH);
    if(!n || n>=MAX_PATH) return std::string();
    std::wstring ws(w,n); size_t pos=ws.find_last_of(L"\\/");
    if(pos==std::wstring::npos) return std::string();
    ws.resize(pos+1);
    int bytes=WideCharToMultiByte(CP_UTF8,0,ws.c_str(),(int)ws.size(),nullptr,0,nullptr,nullptr);
    std::string out(bytes,'\0');
    if(bytes) WideCharToMultiByte(CP_UTF8,0,ws.c_str(),(int)ws.size(),out.data(),bytes,nullptr,nullptr);
    return out;
}

static void updateTvFontTitle(){
    if(g_tvWnd)SetWindowTextW(g_tvWnd,g_tvFontExternal?
        L"L-KIT-16 テレビ画面（TVIF・外部フォント）":L"L-KIT-16 テレビ画面（TVIF・Windows疑似フォント）");
}
static bool makeWindowsTvFont(LOGFONTW lf,LkitTvFont& out){
    // Draw through the user's installed font. Do not read/copy TTF or TTC files.
    lf.lfHeight=-70;lf.lfWidth=0;lf.lfEscapement=lf.lfOrientation=0;
    lf.lfQuality=ANTIALIASED_QUALITY;lf.lfCharSet=DEFAULT_CHARSET;
    HDC dc=CreateCompatibleDC(nullptr);if(!dc)return false;
    HFONT font=CreateFontIndirectW(&lf);if(!font){DeleteDC(dc);return false;}
    HGDIOBJ prevFont=SelectObject(dc,font);
    TEXTMETRICW tm{};
    if(!GetTextMetricsW(dc,&tm)||tm.tmHeight<=0||tm.tmHeight>512){
        SelectObject(dc,prevFont);DeleteObject(font);DeleteDC(dc);return false;
    }
    // Measure every requested character so halfwidth kana cannot be clipped.
    int width=1;bool ok=true;
    for(unsigned glyph=0;glyph<128;glyph++){
        wchar_t c=wchar_t(LkitTvFont::unicode(glyph));SIZE size{};
        if(!GetTextExtentPoint32W(dc,&c,1,&size)){ok=false;break;}
        width=(std::max<int>)(width,size.cx);
    }
    const int height=tm.tmHeight;
    if(!ok||width>512){SelectObject(dc,prevFont);DeleteObject(font);DeleteDC(dc);return false;}
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=width;bi.bmiHeader.biHeight=-height;
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HBITMAP bmp=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!bmp||!bits){if(bmp)DeleteObject(bmp);SelectObject(dc,prevFont);DeleteObject(font);DeleteDC(dc);return false;}
    HGDIOBJ prevBmp=SelectObject(dc,bmp);
    SetTextColor(dc,RGB(255,255,255));SetBkColor(dc,RGB(0,0,0));
    SetBkMode(dc,TRANSPARENT);SetTextAlign(dc,TA_TOP|TA_LEFT|TA_NOUPDATECP);
    LkitTvFont next;
    for(unsigned glyph=0;glyph<128;glyph++){
        wchar_t c=wchar_t(LkitTvFont::unicode(glyph));SIZE size{};
        if(!GetTextExtentPoint32W(dc,&c,1,&size)||!PatBlt(dc,0,0,width,height,BLACKNESS)||
            !TextOutW(dc,(width-size.cx)/2,0,&c,1)||!GdiFlush()){ok=false;break;}
        // Space and A0H are always blank, regardless of selected font.
        if(glyph==0||glyph==64)continue;
        for(unsigned y=0;y<7;y++)next.bytes[glyph*7+y]=
            LkitTvFont::sampleRow(static_cast<const uint32_t*>(bits),width,width,height,y);
    }
    SelectObject(dc,prevBmp);SelectObject(dc,prevFont);
    DeleteObject(bmp);DeleteObject(font);DeleteDC(dc);
    if(ok)out=next;return ok;
}
static LOGFONTW defaultTvLogFont(){
    LOGFONTW lf{};lf.lfWeight=FW_NORMAL;
    wcscpy_s(lf.lfFaceName,L"ＭＳ ゴシック");return lf;
}
static bool loadTvFontAtStartup(HWND owner){
    const std::string dir=exeDirectory();std::string err;
    g_tvFontExternal=!dir.empty()&&g_tvFont.load(std::filesystem::u8path(dir+"TVFONT.BIN"),&err);
    if(g_tvFontExternal)return true;
    if(!makeWindowsTvFont(defaultTvLogFont(),g_tvFont)){
        MessageBoxW(owner,L"テレビ用の疑似フォントを作成できませんでした。TVFONT.BINをEXEと同じフォルダに配置してください。",
            L"テレビフォントエラー",MB_OK|MB_ICONERROR);return false;
    }
    if(err!="open"&&!dir.empty())MessageBoxW(owner,
        L"TVFONT.BINを読み込めませんでした。896バイトで、各バイトの上位3ビットが0のファイルが必要です。\n今回はWindowsの疑似フォントを使用します。元のファイルは変更しません。",
        L"テレビフォント",MB_OK|MB_ICONWARNING);
    return true;
}
static void createTvFontFile(HWND owner){
    LOGFONTW lf=defaultTvLogFont();CHOOSEFONTW cf{};cf.lStructSize=sizeof(cf);
    cf.hwndOwner=owner;cf.lpLogFont=&lf;
    cf.Flags=CF_SCREENFONTS|CF_INITTOLOGFONTSTRUCT;cf.iPointSize=120;
    if(!ChooseFontW(&cf))return;
    LkitTvFont next;
    if(!makeWindowsTvFont(lf,next)){
        MessageBoxW(owner,L"選択したWindowsフォントから疑似フォントを作成できませんでした。",
            L"テレビフォントエラー",MB_OK|MB_ICONERROR);return;
    }
    wchar_t filename[MAX_PATH]=L"TVFONT.BIN";
    const auto directory=std::filesystem::u8path(exeDirectory()).wstring();
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=owner;
    ofn.lpstrFilter=L"テレビフォント（*.BIN）\0*.BIN\0すべてのファイル\0*.*\0\0";
    ofn.lpstrFile=filename;ofn.nMaxFile=MAX_PATH;ofn.lpstrDefExt=L"BIN";
    ofn.lpstrInitialDir=directory.empty()?nullptr:directory.c_str();
    ofn.lpstrTitle=L"Windows疑似テレビフォントを保存（896バイト）";
    ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetSaveFileNameW(&ofn))return;
    if(!next.save(std::filesystem::path(filename))){
        MessageBoxW(owner,L"疑似フォントファイルを保存できませんでした。保存先と書き込み権限を確認してください。",
            L"テレビフォントエラー",MB_OK|MB_ICONERROR);return;
    }
    g_tvFont=next;g_tvFontExternal=false;updateTvFontTitle();
    if(g_tvWnd)InvalidateRect(g_tvWnd,nullptr,FALSE);
    std::wstring message=L"Windowsフォント「";message+=lf.lfFaceName;
    message+=L"」から5×7ドットの疑似フォントを保存し、表示に適用しました。\n\n";
    message+=filename;message+=L"\n\n次回起動時にも使用するには、EXEと同じフォルダにTVFONT.BINという名前で配置してください。\n実機の字形とは異なります。";
    MessageBoxW(owner,message.c_str(),L"テレビフォント保存完了",MB_OK|MB_ICONINFORMATION);
}
static void reloadTvFontFile(HWND owner){
    const std::string dir=exeDirectory();LkitTvFont next;
    if(dir.empty()||!next.load(std::filesystem::u8path(dir+"TVFONT.BIN"))){
        MessageBoxW(owner,L"EXEと同じフォルダのTVFONT.BINを読み込めませんでした。\n896バイトで、各バイトの上位3ビットが0のファイルを配置してください。現在の表示フォントは維持します。",
            L"テレビフォントエラー",MB_OK|MB_ICONERROR);return;
    }
    g_tvFont=next;g_tvFontExternal=true;updateTvFontTitle();
    if(g_tvWnd)InvalidateRect(g_tvWnd,nullptr,FALSE);
}

// Monitor words are stored at 0100H..04FFH. A combined image is big-endian.
static bool readMonitorRom(const std::string& dir, std::array<uint16_t,1024>& words){
 const auto merged=std::filesystem::u8path(dir+"LKIT-16 ROM.bin");
 const auto cwdMerged=std::filesystem::u8path("LKIT-16 ROM.bin");
 std::filesystem::path selected;
 if(!dir.empty() && std::filesystem::exists(merged)) selected=merged;
 else if(std::filesystem::exists(cwdMerged)) selected=cwdMerged;
 if(!selected.empty()){
     std::ifstream f(selected,std::ios::binary);
     if(!f)return false;
     std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),{});
     if(data.size()!=2048)return false;
     for(unsigned i=0;i<1024;i++)words[i]=uint16_t((uint16_t(data[2*i])<<8)|data[2*i+1]);
     return true;
 }
 return false;
}

static bool readBasicRom(const std::string& dir, const char* name, std::vector<uint16_t>& words, unsigned count){
 if(dir.empty())return false;
 std::ifstream f(std::filesystem::u8path(dir+name),std::ios::binary);
 if(!f)return false;
 std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),{});
 if(data.size()!=count*2)return false;
 words.resize(count);
 for(unsigned i=0;i<count;i++)words[i]=uint16_t((uint16_t(data[2*i])<<8)|data[2*i+1]);
 return true;
}
static bool loadRom(){
 const std::string dir=exeDirectory();
 std::array<uint16_t,1024> words{};
 if(!readMonitorRom(dir,words))return false;
 for(unsigned i=0;i<1024;i++)g_cpu.mem[0x100+i]=words[i];
 std::vector<uint16_t> basic1,basic2;
 const bool b1=readBasicRom(dir,"BASIC-ROM1.bin",basic1,1024);
 const bool b2=readBasicRom(dir,"BASIC-ROM2.bin",basic2,2048);
 if(b1)for(unsigned i=0;i<1024;i++)g_cpu.mem[0x500+i]=basic1[i];
 if(b2)for(unsigned i=0;i<2048;i++)g_cpu.mem[0xd800+i]=basic2[i];
 g_cpu.basicRomLoaded=b1&&b2;
 g_cpu.reset();return true;
}
static void syncBinMenuCheck(bool shown){
    if(!g_mainWnd) return;
    HMENU m=GetMenu(g_mainWnd); if(m) CheckMenuItem(m,IDM_DIRECT_BIN,MF_BYCOMMAND|(shown?MF_CHECKED:MF_UNCHECKED));
}
static std::wstring fileErrorJapanese(const std::string& error){
    static const std::pair<const char*,const wchar_t*> messages[]={
        {"Cannot open BIN",L"BINファイルを開けません。"},
        {"Cannot create BIN",L"BINファイルを作成できません。"},
        {"Cannot open BTK",L"BTKファイルを開けません。"},
        {"Cannot create BTK",L"BTKファイルを作成できません。"},
        {"Cannot write BTK",L"BTKファイルへの書込みに失敗しました。"},
        {"Data exceeds memory",L"ロードするデータがメモリ範囲を超えています。"},
        {"Data overlaps ROM",L"ロード先がROM領域と重なっています。"},
        {"End address is below start",L"終了アドレスが開始アドレスより小さくなっています。"},
        {"BTK header is incomplete",L"BTKのヘッダが不足しています（4バイト必要です）。"},
        {"BTK header length does not match data",L"BTKのヘッダに指定された範囲と、実データのバイト数が一致しません。"},
        {"BTK header end exceeds FFFFH",L"BTKヘッダの終了値がFFFFHを超えます。保存範囲を小さくしてください。"}
    };
    for(const auto& entry:messages)if(error==entry.first)return entry.second;
    return L"ファイルの読込または保存に失敗しました。";
}
static BOOL CALLBACK setControlGuiFont(HWND child,LPARAM font){
    SendMessageW(child,WM_SETFONT,WPARAM(font),TRUE);return TRUE;
}
static LRESULT CALLBACK BinWndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
 switch(msg){
 case WM_CREATE:
   CreateWindowW(L"STATIC",L"BIN 読込 開始（16進）",WS_CHILD|WS_VISIBLE,20,24,190,20,h,nullptr,GetModuleHandleW(nullptr),nullptr);
   g_editLoadAddr=CreateWindowW(L"EDIT",L"1000",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,215,20,90,25,h,(HMENU)(INT_PTR)1200,GetModuleHandleW(nullptr),nullptr);
   CreateWindowW(L"BUTTON",L"BIN 読込...",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|WS_TABSTOP,325,18,115,30,h,(HMENU)(INT_PTR)1201,GetModuleHandleW(nullptr),nullptr);
   CreateWindowW(L"STATIC",L"BTK 読込：開始アドレス自動",WS_CHILD|WS_VISIBLE,20,78,290,20,h,nullptr,GetModuleHandleW(nullptr),nullptr);
   CreateWindowW(L"BUTTON",L"BTK 読込...",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|WS_TABSTOP,325,72,115,30,h,(HMENU)(INT_PTR)1205,GetModuleHandleW(nullptr),nullptr);
   CreateWindowW(L"STATIC",L"アドレスの入力は不要です。",WS_CHILD|WS_VISIBLE,455,78,230,20,h,nullptr,GetModuleHandleW(nullptr),nullptr);
   CreateWindowW(L"STATIC",L"保存範囲（開始／終了・16進）",WS_CHILD|WS_VISIBLE,20,132,220,20,h,nullptr,GetModuleHandleW(nullptr),nullptr);
   g_editSaveStart=CreateWindowW(L"EDIT",L"1000",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,245,128,90,25,h,(HMENU)(INT_PTR)1202,GetModuleHandleW(nullptr),nullptr);
   g_editSaveEnd=CreateWindowW(L"EDIT",L"10FF",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,345,128,90,25,h,(HMENU)(INT_PTR)1203,GetModuleHandleW(nullptr),nullptr);
   CreateWindowW(L"BUTTON",L"BIN 保存...",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|WS_TABSTOP,455,126,115,30,h,(HMENU)(INT_PTR)1204,GetModuleHandleW(nullptr),nullptr);
   CreateWindowW(L"BUTTON",L"BTK 保存...",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|WS_TABSTOP,575,126,115,30,h,(HMENU)(INT_PTR)1206,GetModuleHandleW(nullptr),nullptr);
   CreateWindowW(L"STATIC",L"BTKの開始アドレスは、選択したファイルから自動設定されます。",WS_CHILD|WS_VISIBLE,20,180,680,22,h,nullptr,GetModuleHandleW(nullptr),nullptr);
   CreateWindowW(L"STATIC",L"保存範囲の終了アドレスに指定したワードも保存されます。",WS_CHILD|WS_VISIBLE,20,205,680,22,h,nullptr,GetModuleHandleW(nullptr),nullptr);
   EnumChildWindows(h,setControlGuiFont,(LPARAM)GetStockObject(DEFAULT_GUI_FONT));
   return 0;
 case WM_COMMAND:{
   int id=LOWORD(wp);
   if(id==1201){ std::string path,er; if(chooseBin(h,false,path)){ uint16_t a=editHex(g_editLoadAddr,0x1000); if(g_cpu.directLoadBinaryForPanel(path,a,&er)) MessageBoxW(h,L"BINをメモリに読み込みました。\n読み込んだ開始アドレスをFUNC 3に登録しました。",L"BIN読込",MB_OK|MB_ICONINFORMATION); else { std::wstring w=fileErrorJapanese(er); MessageBoxW(h,w.c_str(),L"BIN読込エラー",MB_OK|MB_ICONERROR); } } return 0; }
   if(id==1204){ std::string path,er; if(chooseBin(h,true,path)){ uint16_t a=editHex(g_editSaveStart,0x1000),e=editHex(g_editSaveEnd,0x10ff); if(g_cpu.directSaveBinary(path,a,e,&er)) MessageBoxW(h,L"メモリの内容をBINファイルに保存しました。",L"BIN保存",MB_OK|MB_ICONINFORMATION); else { std::wstring w=fileErrorJapanese(er); MessageBoxW(h,w.c_str(),L"BIN保存エラー",MB_OK|MB_ICONERROR); } } return 0; }
   if(id==1205){
       std::string path,er;uint16_t start=0;
       if(chooseBin(h,false,path,true)){
           if(g_cpu.directLoadBtkForPanel(path,&start,&er)){
               wchar_t message[160];swprintf_s(message,L"BTKを%04XHに読み込みました（開始アドレスはファイルから取得）。\n読み込んだ開始アドレスをFUNC 3に登録しました。",unsigned(start));
               MessageBoxW(h,message,L"BTK読込",MB_OK|MB_ICONINFORMATION);
           }else{std::wstring w=fileErrorJapanese(er);MessageBoxW(h,w.c_str(),L"BTK読込エラー",MB_OK|MB_ICONERROR);}
       }return 0;
   }
   if(id==1206){
       std::string path,er;
       if(chooseBin(h,true,path,true)){
           const uint16_t start=editHex(g_editSaveStart,0x1000),end=editHex(g_editSaveEnd,0x10ff);
           if(g_cpu.directSaveBtk(path,start,end,&er))MessageBoxW(h,L"開始・終了アドレスの4バイトヘッダを付けて、BTKファイルに保存しました。",L"BTK保存",MB_OK|MB_ICONINFORMATION);
           else{std::wstring w=fileErrorJapanese(er);MessageBoxW(h,w.c_str(),L"BTK保存エラー",MB_OK|MB_ICONERROR);}
       }return 0;
   }
   break; }
 case WM_CLOSE: ShowWindow(h,SW_HIDE); syncBinMenuCheck(false); return 0;
 case WM_SHOWWINDOW: syncBinMenuCheck(wp!=0); break;
 }
 return DefWindowProcW(h,msg,wp,lp);
}
static void showDirectBinWindow(){
 if(!g_binWnd) return;
 ShowWindow(g_binWnd,SW_SHOW); SetForegroundWindow(g_binWnd); syncBinMenuCheck(true);
}

static void syncRegisterMenuCheck(bool shown){
    if(!g_mainWnd) return;
    HMENU m=GetMenu(g_mainWnd); if(m) CheckMenuItem(m,IDM_CPU_REGISTERS,MF_BYCOMMAND|(shown?MF_CHECKED:MF_UNCHECKED));
}
struct RegisterViewSnapshot {
    std::array<uint16_t,8> regs{};
    bool saved=false,valid=false;
    unsigned state=0;
    bool operator==(const RegisterViewSnapshot& other) const {
        return regs==other.regs && saved==other.saved && valid==other.valid && state==other.state;
    }
};
static RegisterViewSnapshot captureRegisterView(){
    RegisterViewSnapshot view;
    view.saved=g_cpu.showSavedRegisters();
    view.valid=!view.saved || g_cpu.debugSavedValid;
    if(view.valid)view.regs=g_cpu.displayedRegisters();
    view.state=g_cpu.debugUserRunning?(g_cpu.halted?1u:0u):
        (g_cpu.debugSavePending?2u:(g_cpu.stepSwitch?3u:4u));
    return view;
}
struct RegisterWindowResources {
    HFONT title=nullptr,mono=nullptr;
    HBRUSH background=nullptr;
    RegisterViewSnapshot painted;
    bool hasPainted=false;
    RegisterWindowResources(){
        title=CreateFontW(20,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN,L"ＭＳ ゴシック");
        mono=CreateFontW(19,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN,L"ＭＳ ゴシック");
        background=CreateSolidBrush(RGB(244,244,240));
    }
    ~RegisterWindowResources(){if(title)DeleteObject(title);if(mono)DeleteObject(mono);if(background)DeleteObject(background);}
};
static RegisterWindowResources* registerResources(HWND h){
    return reinterpret_cast<RegisterWindowResources*>(GetWindowLongPtrW(h,GWLP_USERDATA));
}
static void updateRegisterButtons(HWND h){
    const bool enabled[]={g_loaded&&g_cpu.canDebugStop(),g_loaded&&g_cpu.canDebugStep(),g_loaded&&g_cpu.canDebugResume()};
    for(unsigned i=0;i<3;i++){
        HWND button=GetDlgItem(h,1301+i);
        if(button && (IsWindowEnabled(button)!=FALSE)!=enabled[i])EnableWindow(button,enabled[i]);
    }
}
static void paintRegisterWindow(HWND h){
    updateRegisterButtons(h);
    auto* resources=registerResources(h);
    if(!resources || !resources->hasPainted || !(captureRegisterView()==resources->painted))
        InvalidateRect(h,nullptr,FALSE);
}
static LRESULT CALLBACK RegisterWndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:
        SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(new RegisterWindowResources));
        CreateWindowW(L"BUTTON",L"STOP",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,18,278,100,30,h,(HMENU)1301,GetModuleHandleW(nullptr),nullptr);
        CreateWindowW(L"BUTTON",L"STEP",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,132,278,100,30,h,(HMENU)1302,GetModuleHandleW(nullptr),nullptr);
        CreateWindowW(L"BUTTON",L"CONT",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,246,278,100,30,h,(HMENU)1303,GetModuleHandleW(nullptr),nullptr);
        updateRegisterButtons(h);
        return 0;
    case WM_COMMAND:
        if(HIWORD(wp)==BN_CLICKED && (LOWORD(wp)==1301 || LOWORD(wp)==1302 || LOWORD(wp)==1303)){
            if(g_loaded){
                if(LOWORD(wp)==1301)g_cpu.requestDebugStop();
                else if(LOWORD(wp)==1302)g_cpu.requestDebugStep();
                else g_cpu.requestDebugContinue();
                InvalidateRect(g_mainWnd,nullptr,FALSE);paintRegisterWindow(h);
            }
            return 0;
        }
        break;
    case WM_NCDESTROY:
        delete registerResources(h);SetWindowLongPtrW(h,GWLP_USERDATA,0);break;
    case WM_CLOSE: ShowWindow(h,SW_HIDE); syncRegisterMenuCheck(false); return 0;
    case WM_SHOWWINDOW: syncRegisterMenuCheck(wp!=0); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT rc; GetClientRect(h,&rc);
        auto* resources=registerResources(h);
        if(!resources){EndPaint(h,&ps);return 0;}
        const auto view=captureRegisterView();
        FillRect(dc,&rc,resources->background?resources->background:(HBRUSH)GetStockObject(WHITE_BRUSH));
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(25,25,25));
        HGDIOBJ old=SelectObject(dc,resources->title?resources->title:GetStockObject(DEFAULT_GUI_FONT));
        const wchar_t* heading=L"MN1610 CPUレジスタ";TextOutW(dc,18,14,heading,(int)wcslen(heading));
        SelectObject(dc,resources->mono?resources->mono:GetStockObject(DEFAULT_GUI_FONT));
        wchar_t line[256]; int y=50;
        const bool saved=view.saved;
        const bool valid=view.valid;
        const auto& regs=view.regs;
        for(int i=0;i<8;i+=2){
            if(valid)
                swprintf_s(line,L"%-3S = %04X        %-3S = %04X",MN1610::rn(i),regs[i],MN1610::rn(i+1),regs[i+1]);
            else swprintf_s(line,L"%-3S = ----        %-3S = ----",MN1610::rn(i),MN1610::rn(i+1));
            TextOutW(dc,18,y,line,(int)wcslen(line)); y+=30;
        }
        y+=4;
        if(valid){
            const uint16_t str=regs[6];
            swprintf_s(line,L"STR: E=%d  OVF=%d",(str&MN1610::STR_E)?1:0,(str&MN1610::STR_OVF)?1:0);
        }else swprintf_s(line,L"保存STR: 未確定");
        TextOutW(dc,18,y,line,(int)wcslen(line)); y+=30;
        static const wchar_t* states[]={L"ユーザープログラム実行中",L"ユーザーCPU停止（HALT）",
            L"モニタ実行中（レジスタ保存中）",L"モニタ実行中（STEP待機）",L"モニタ実行中（操作待ち）"};
        const wchar_t* state=states[view.state];
        TextOutW(dc,18,y,state,(int)wcslen(state)); y+=30;
        const wchar_t* source=saved?(valid?L"表示: モニタ保存値 0010H～0017H":L"表示: モニタの保存完了待ち"):
            L"表示: CPU現在値（常時更新）";
        TextOutW(dc,18,y,source,(int)wcslen(source));
        SelectObject(dc,old);EndPaint(h,&ps);resources->painted=view;resources->hasPainted=true;return 0; }
    }
    return DefWindowProcW(h,msg,wp,lp);
}
static void showRegisterWindow(){
    if(!g_regWnd) return;
    ShowWindow(g_regWnd,SW_SHOW); SetForegroundWindow(g_regWnd); syncRegisterMenuCheck(true); paintRegisterWindow(g_regWnd);
}


// Fixed 900 Hz TVIFOP tone, 100 ms PCM buffer (90 complete periods).
static const std::vector<uint8_t>& tvToneWave(){
    static const std::vector<uint8_t> wave=[](){
        std::vector<uint8_t> w(44+4410*2,0);
        auto put16=[&](unsigned p,unsigned v){w[p]=uint8_t(v);w[p+1]=uint8_t(v>>8);};
        auto put32=[&](unsigned p,unsigned v){for(unsigned k=0;k<4;k++)w[p+k]=uint8_t(v>>(8*k));};
        std::memcpy(w.data(),"RIFF",4);put32(4,unsigned(w.size()-8));std::memcpy(w.data()+8,"WAVEfmt ",8);
        put32(16,16);put16(20,1);put16(22,1);put32(24,44100);put32(28,88200);put16(32,2);put16(34,16);
        std::memcpy(w.data()+36,"data",4);put32(40,8820);
        for(unsigned i=0;i<4410;i++){int16_t v=(i%49<24)?2500:-2500;put16(44+i*2,uint16_t(v));}
        return w;
    }();return wave;
}
static void updateTvTone(){
    static bool playing=false; bool want=g_tvSound&&g_cpu.tv.tone;
    if(want==playing)return;
    if(want)playing=PlaySoundA(reinterpret_cast<LPCSTR>(tvToneWave().data()),nullptr,SND_MEMORY|SND_ASYNC|SND_LOOP|SND_NODEFAULT)!=FALSE;
    else {PlaySoundA(nullptr,nullptr,0);playing=false;}
}
// Generate missing PCM by executing the actual CPU, preserving every WT edge.
static bool fillScaPcm(size_t count){
    auto& source=g_cpu.scaSound;
    unsigned guard=200000;
    while(source.samples.size()<count&&g_loaded&&!g_cpu.halted&&guard--){if(!g_cpu.step())break;}
    if(source.samples.size()<count&&(!g_loaded||g_cpu.halted)){
        // A halted CPU holds the output level; continue its AC decay without
        // fetching instructions or substituting a repeated audio block.
        uint64_t missing=count-source.samples.size();
        source.advance((missing*2000000u+44099u)/44100u);
    }
    return source.samples.size()>=count;
}
// Continuous PCM stream. Buffers stay alive and prepared until reset/close.
class ScaSoundPlayer {
    HWAVEOUT device=nullptr;bool preciseTimer=false;
    std::array<WAVEHDR,6> headers{};
    std::array<std::array<int16_t,882>,6> data{};
bool paused=false;
public:
    void close(){
        if(device){
            waveOutReset(device);
            for(auto& h:headers)waveOutUnprepareHeader(device,&h,sizeof(h));
            waveOutClose(device);device=nullptr;
        }
        if(preciseTimer){timeEndPeriod(1);preciseTimer=false;}paused=false;
    }
    void pump(){
        auto& source=g_cpu.scaSound;
        if(!source.enabled){close();source.samples.clear();return;}
        if(!device){
            preciseTimer=timeBeginPeriod(1)==TIMERR_NOERROR;
            WAVEFORMATEX fmt{};fmt.wFormatTag=WAVE_FORMAT_PCM;fmt.nChannels=1;
            fmt.nSamplesPerSec=44100;fmt.wBitsPerSample=16;fmt.nBlockAlign=2;fmt.nAvgBytesPerSec=88200;
            if(waveOutOpen(&device,WAVE_MAPPER,&fmt,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR){device=nullptr;close();source.samples.clear();return;}
            for(unsigned i=0;i<headers.size();i++){
                headers[i]={};headers[i].lpData=reinterpret_cast<LPSTR>(data[i].data());headers[i].dwBufferLength=DWORD(sizeof(data[i]));
                if(waveOutPrepareHeader(device,&headers[i],sizeof(WAVEHDR))!=MMSYSERR_NOERROR){close();source.samples.clear();return;}
            }
            // Prime real emulated samples before starting playback.
            if(waveOutPause(device)!=MMSYSERR_NOERROR){close();return;}paused=true;
        }
        unsigned queued=0;for(const auto& h:headers)if(h.dwFlags&WHDR_INQUEUE)++queued;
        // Playback consumption, rather than WM_TIMER count, sets CPU progress.
        for(unsigned i=0;i<headers.size()&&queued<5;i++){
            if(headers[i].dwFlags&WHDR_INQUEUE)continue;
            if(!fillScaPcm(882))break;
            std::copy_n(source.samples.begin(),882,data[i].begin());
            source.samples.erase(source.samples.begin(),source.samples.begin()+882);
            if(waveOutWrite(device,&headers[i],sizeof(WAVEHDR))!=MMSYSERR_NOERROR){close();source.samples.clear();return;}
            ++queued;
        }
        if(paused&&queued>=3){if(waveOutRestart(device)!=MMSYSERR_NOERROR){close();return;}paused=false;}

    }
};
static ScaSoundPlayer g_scaPlayer;
static void selectScaSound(bool active){
    g_scaPlayer.close();g_cpu.scaSound.select(active);
    // Enabling/disabling the speaker must preserve TVIF registers and display.
    updateTvTone();
    CheckMenuItem(GetMenu(g_mainWnd),IDM_SCA_SOUND,MF_BYCOMMAND|(active?MF_CHECKED:MF_UNCHECKED));
}
static COLORREF tvColor(unsigned c){static const COLORREF palette[8]={RGB(0,0,0),RGB(0,255,0),RGB(0,0,255),RGB(0,255,255),RGB(255,0,0),RGB(255,255,0),RGB(255,0,255),RGB(255,255,255)};return palette[c&7];}

static LRESULT CALLBACK TvWndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_CHAR){unsigned ch=unsigned(wp);if(ch>=32&&ch<=126){if(ch>='a'&&ch<='z')ch-=32;g_cpu.tv.input(uint8_t(ch));}else if(ch==13||ch==8)g_cpu.tv.input(uint8_t(ch));return 0;}
    if(msg==WM_CLOSE){ShowWindow(h,SW_HIDE);CheckMenuItem(GetMenu(g_mainWnd),IDM_TV,MF_BYCOMMAND|MF_UNCHECKED);return 0;}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_PAINT){
        PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT rc;GetClientRect(h,&rc);
        HDC back=CreateCompatibleDC(dc);HBITMAP bmp=CreateCompatibleBitmap(dc,rc.right,rc.bottom);HGDIOBJ oldBmp=SelectObject(back,bmp);
        HBRUSH screen=CreateSolidBrush(g_tvColor?tvColor(g_tvBackground):RGB(0,0,0));FillRect(back,&rc,screen);DeleteObject(screen);
        int cw=(std::max<int>)(1,int((rc.right-24)/40)),ch=(std::max<int>)(1,int((rc.bottom-24)/16));
        // Draw the external/runtime 5x7 font in a 6x8 cell.
        // Integer dot scaling avoids dependence on installed Windows fonts.
        int sx=(std::max<int>)(1,cw/6),sy=(std::max<int>)(1,ch/8);
        HBRUSH brushes[8];for(unsigned c=0;c<8;c++)brushes[c]=CreateSolidBrush(tvColor(c));
        for(unsigned i=0;i<640;i++){
            uint8_t code=g_cpu.tv.cells[i];
            unsigned glyph=LkitTvFont::glyphIndex(code);
            int left=12+int(i%40)*cw,top=12+int(i/40)*ch;
            RECT cell{left,top,left+cw,top+ch};
            HBRUSH fg=brushes[g_tvColor?g_cpu.tv.colors[i]:7],bg=brushes[g_tvColor?g_tvBackground:0];
            FillRect(back,&cell,g_cpu.tv.reverse[i]?fg:bg);
            const int pattern=LkitTv::patternIndex(code);
            if(pattern>=0){
                const auto& p=g_cpu.tv.patterns[unsigned(pattern)];
                // Draw normalized pattern data across the whole cell. Undefined
                // slots stay blank; do not substitute guessed ROM artwork.
                if(p.width && p.height){
                    for(unsigned y=0;y<p.height;y++)for(unsigned x=0;x<p.width;x++){
                        if(!g_cpu.tv.patternDot(unsigned(pattern),x,y))continue;
                        RECT dot{left+int(x*cw/p.width),top+int(y*ch/p.height),
                            left+int((x+1)*cw/p.width),top+int((y+1)*ch/p.height)};
                        FillRect(back,&dot,g_cpu.tv.reverse[i]?bg:fg);
                    }
                }
                continue;
            }
            int x0=left+(cw-6*sx)/2,y0=top+(ch-8*sy)/2;
            for(unsigned y=0;y<7;y++)for(unsigned x=0;x<5;x++){
                if(g_tvFont.row(glyph,y)&(0x10u>>x)){
                    RECT dot{x0+int(x)*sx,y0+int(y)*sy,x0+int(x+1)*sx,y0+int(y+1)*sy};
                    RECT clipped;if(IntersectRect(&clipped,&dot,&cell))FillRect(back,&clipped,g_cpu.tv.reverse[i]?bg:fg);
                }
            }
        }
        if(g_cpu.tv.col<40&&g_cpu.tv.row<16&&(GetTickCount64()/500)%2==0){RECT cursor{12+int(g_cpu.tv.col)*cw,12+int(g_cpu.tv.row+1)*ch-2,12+int(g_cpu.tv.col+1)*cw,12+int(g_cpu.tv.row+1)*ch};FillRect(back,&cursor,brushes[g_tvColor?g_cpu.tv.ink:7]);}
        for(auto brush:brushes)DeleteObject(brush);
        BitBlt(dc,0,0,rc.right,rc.bottom,back,0,0,SRCCOPY);SelectObject(back,oldBmp);DeleteObject(bmp);DeleteDC(back);EndPaint(h,&ps);return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}

static LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
 switch(msg){
 case WM_CREATE:{
   loadTvFontAtStartup(h);
   g_loaded=loadRom();
   for(int i=0;i<48;i++){
     int row=i/8,col=i%8;
     // The real L-KIT-16 has a visibly larger gap between the command row
     // and the five instruction/data-key rows below it.
     int y = (row==0) ? 220 : (338 + (row-1)*78);
     CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                   72+col*82,y,72,72,h,(HMENU)(INT_PTR)(1000+i),GetModuleHandleW(nullptr),nullptr);
   }
   CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,15,420,46,64,h,(HMENU)(INT_PTR)1100,GetModuleHandleW(nullptr),nullptr);
   CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,15,576,46,64,h,(HMENU)(INT_PTR)1102,GetModuleHandleW(nullptr),nullptr);
   SetTimer(h,1,10,nullptr); return 0;}
 case WM_COMMAND:{int id=LOWORD(wp);
   // Owner-drawn controls can notify for reasons other than a click.
   // Focus/redraw/double-click notifications must not operate panel keys.
   if(((id>=1000&&id<1048)||id==1100||id==1102) && HIWORD(wp)!=BN_CLICKED) return 0;
   if(id>=1000&&id<1048){ if(id==1000){ g_cpu.requestStop(); } else { g_cpu.pressedKey=id-1000; g_keyReleaseTicks=12; } InvalidateRect(h,nullptr,FALSE); }
   else if(id==1100){ if(g_loaded){ g_cmtLoadMounted=false;g_cmtLoadEntered=false;g_scaPlayer.close();g_cpu.reset(); g_cpu.pressedKey=-1; g_resetVisualTicks=8; InvalidateRect(h,nullptr,FALSE); } }
   else if(id==1102){ g_cpu.stepSwitch=!g_cpu.stepSwitch; InvalidateRect(GetDlgItem(h,1102),nullptr,FALSE); InvalidateRect(h,nullptr,FALSE); }
   else if(id==IDM_CMT_LOAD_WAV){ mountRealCassetteWav(h); }
   else if(id==IDM_CMT_SAVE_WAV){ armCmtSave(h); }
   else if(id==IDM_DIRECT_BIN){ if(g_binWnd && IsWindowVisible(g_binWnd)){ ShowWindow(g_binWnd,SW_HIDE); syncBinMenuCheck(false); } else showDirectBinWindow(); }
   else if(id==IDM_BASIC_START){if(!g_cpu.basicRomLoaded){MessageBoxW(h,L"BASIC-ROM1.bin（2048バイト）とBASIC-ROM2.bin（4096バイト）を、エミュレータのEXEと同じフォルダに配置してください。",L"BASIC ROMエラー",MB_OK|MB_ICONERROR);}else{selectScaSound(false);g_cpu.reset();for(unsigned i=0;i<100000&&!g_cpu.halted;i++)g_cpu.step();g_cpu.SP()=0x10ff;g_cpu.STR()=0x0700;g_cpu.IC()=0xd800;g_cpu.debugUserRunning=true;g_cpu.debugSavePending=false;g_cpu.halted=false;ShowWindow(g_tvWnd,SW_SHOW);SetFocus(g_tvWnd);CheckMenuItem(GetMenu(h),IDM_TV,MF_BYCOMMAND|MF_CHECKED);}}
   else if(id==IDM_TV_FONT_CREATE){createTvFontFile(h);}
   else if(id==IDM_TV_FONT_RELOAD){reloadTvFontFile(h);}
   else if(id==IDM_SCA_SOUND){selectScaSound(!g_cpu.scaSound.enabled);}
   else if(id==IDM_TV_COLOR){g_tvColor=!g_tvColor;CheckMenuItem(GetMenu(h),id,MF_BYCOMMAND|(g_tvColor?MF_CHECKED:MF_UNCHECKED));InvalidateRect(g_tvWnd,nullptr,FALSE);}
   else if(id==IDM_TV_SOUND){g_tvSound=!g_tvSound;CheckMenuItem(GetMenu(h),id,MF_BYCOMMAND|(g_tvSound?MF_CHECKED:MF_UNCHECKED));updateTvTone();}
   else if(id>=IDM_TV_BG&&id<IDM_TV_BG+8){g_tvBackground=id-IDM_TV_BG;for(unsigned c=0;c<8;c++)CheckMenuItem(GetMenu(h),IDM_TV_BG+c,MF_BYCOMMAND|(c==g_tvBackground?MF_CHECKED:MF_UNCHECKED));InvalidateRect(g_tvWnd,nullptr,FALSE);}
   else if(id==IDM_TV){bool visible=IsWindowVisible(g_tvWnd)!=0;if(!visible)selectScaSound(false);ShowWindow(g_tvWnd,visible?SW_HIDE:SW_SHOW);CheckMenuItem(GetMenu(h),IDM_TV,MF_BYCOMMAND|(visible?MF_UNCHECKED:MF_CHECKED));}
   else if(id==IDM_CPU_REGISTERS){ if(g_regWnd && IsWindowVisible(g_regWnd)){ ShowWindow(g_regWnd,SW_HIDE); syncRegisterMenuCheck(false); } else showRegisterWindow(); }
   return 0;}
 case WM_LBUTTONUP: g_cpu.pressedKey=-1; return 0;
 case WM_DRAWITEM:{ const DRAWITEMSTRUCT* d=(const DRAWITEMSTRUCT*)lp; if(d->CtlID>=1000&&d->CtlID<1048){drawKeyTop(d);return TRUE;} if(d->CtlID==1100){drawToggle(d->hDC,2,8,(d->itemState&ODS_SELECTED)!=0);return TRUE;} if(d->CtlID==1102){drawToggle(d->hDC,2,10,g_cpu.stepSwitch);return TRUE;} break; }
 case WM_TIMER:{
   // Refill audio before synchronous panel drawing can delay this message.
   if(g_cpu.scaSound.enabled)g_scaPlayer.pump();
   static uint64_t nextVisual=0;uint64_t now=GetTickCount64();
   bool drawNow=!g_cpu.scaSound.enabled||now>=nextVisual;
   if(drawNow){nextVisual=now+33;if(g_tvWnd&&IsWindowVisible(g_tvWnd))InvalidateRect(g_tvWnd,nullptr,FALSE);if(g_regWnd&&IsWindowVisible(g_regWnd))paintRegisterWindow(g_regWnd);}
   if(g_keyReleaseTicks>0&&--g_keyReleaseTicks==0){g_cpu.pressedKey=-1;InvalidateRect(h,nullptr,FALSE);}
   if(g_resetVisualTicks>0&&--g_resetVisualTicks==0)InvalidateRect(h,nullptr,FALSE);
   if(g_loaded){
       if(!g_cpu.scaSound.enabled){uint64_t target=g_cpu.emuClocks+20000u;int guard=50000;while(g_cpu.emuClocks<target&&guard-->0&&!g_cpu.halted)if(!g_cpu.step())break;}
       if(g_cpu.takeDisplayDirty()){RECT ledRc{145,62,700,190};InvalidateRect(h,&ledRc,FALSE);UpdateWindow(h);}
       pollCmtTransfer(h);
   }
   updateTvTone();if(!g_cpu.scaSound.enabled)g_scaPlayer.pump();return 0;}

 case WM_ERASEBKGND: return 1;
 case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT rc;GetClientRect(h,&rc);
   // Draw the complete client area off-screen, then present it in one BitBlt.
   // This removes GDI background/segment tearing independently of CPU timing.
   HDC back=CreateCompatibleDC(dc); HBITMAP bmp=CreateCompatibleBitmap(dc,rc.right-rc.left,rc.bottom-rc.top); HGDIOBJ oldBmp=SelectObject(back,bmp);
   HBRUSH bg=CreateSolidBrush(RGB(218,218,207));FillRect(back,&rc,bg);DeleteObject(bg);
   SetBkMode(back,TRANSPARENT); SetTextColor(back,RGB(30,30,26));
   // Original vector-style panel logo.  No photograph or logo bitmap is pasted.
   drawPanelLogo(back,235,12);
   if(!g_loaded){SetTextColor(back,RGB(180,0,0));const wchar_t* warning=L"ROM読込エラー：LKIT-16 ROM.bin（2048バイト）を確認してください。";TextOutW(back,160,45,warning,(int)wcslen(warning));}
   // Two separate black LED windows, as on the original front panel.
   RECT ap{160,82,405,188}, dp{425,82,670,188};
   HBRUSH pb=CreateSolidBrush(RGB(24,24,22));FillRect(back,&ap,pb);FillRect(back,&dp,pb);DeleteObject(pb);
   SetTextColor(back,RGB(45,45,40)); TextOutW(back,244,61,L"ADDRESS",7); TextOutW(back,522,61,L"DATA",4);
   // Draw one immutable snapshot maintained by the 001BH..0022H write-watch.
   const auto ledFrame=g_cpu.visibleFrame();
   // Physical display order L0..L3 = ADDRESS A4..A1, L4..L7 = DATA D4..D1.
   for(int i=0;i<4;i++) draw7(back,177+i*55,101,31,62,ledFrame[i]);
   for(int i=0;i<4;i++) draw7(back,442+i*55,101,31,62,ledFrame[4+i]);
   // Front-panel legends and connector outlines (cosmetic only).
   SetTextColor(back,RGB(45,45,40)); TextOutW(back,12,205,L"OUT",3); TextOutW(back,15,253,L"IN",2); TextOutW(back,5,402,L"RESET",5);
   TextOutW(back,5,554,L"AUTO",4); TextOutW(back,5,646,L"STEP",4);
   // Physical-looking toggle controls are owner-drawn child buttons above.
   HFONT footFont=CreateFontW(18,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Georgia"); HGDIOBJ off=SelectObject(back,footFont); RECT footRc{0,842,740,870}; DrawTextW(back,L"PANAFACOM LIMITED",-1,&footRc,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX); SelectObject(back,off); DeleteObject(footFont);
   HPEN jackPen=CreatePen(PS_SOLID,2,RGB(80,80,72)); HGDIOBJ oldPen=SelectObject(back,jackPen); HBRUSH jackBr=CreateSolidBrush(RGB(235,235,224)); HGDIOBJ oldBr=SelectObject(back,jackBr);
   Ellipse(back,20,222,42,244); Ellipse(back,20,270,42,292);
   SelectObject(back,oldBr);SelectObject(back,oldPen);DeleteObject(jackBr);DeleteObject(jackPen);
   BitBlt(dc,0,0,rc.right-rc.left,rc.bottom-rc.top,back,0,0,SRCCOPY);
   SelectObject(back,oldBmp);DeleteObject(bmp);DeleteDC(back);EndPaint(h,&ps);return 0;}
 case WM_DESTROY:g_scaPlayer.close();PlaySoundA(nullptr,nullptr,0);PostQuitMessage(0);return 0;} return DefWindowProcW(h,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int show){
 HICON hIconLarge=(HICON)LoadImageW(hi,MAKEINTRESOURCEW(IDI_LKIT16),IMAGE_ICON,32,32,LR_DEFAULTCOLOR);
 HICON hIconSmall=(HICON)LoadImageW(hi,MAKEINTRESOURCEW(IDI_LKIT16),IMAGE_ICON,16,16,LR_DEFAULTCOLOR);
 WNDCLASSW wc{};wc.lpfnWndProc=WndProc;wc.hInstance=hi;wc.lpszClassName=L"Lkit16EmulatorMain";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hIcon=hIconLarge;RegisterClassW(&wc);
 WNDCLASSW bw{};bw.lpfnWndProc=BinWndProc;bw.hInstance=hi;bw.lpszClassName=L"Lkit16DirectBinTool";bw.hCursor=LoadCursor(nullptr,IDC_ARROW);bw.hIcon=hIconLarge;bw.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);RegisterClassW(&bw);
 WNDCLASSW rw{};rw.lpfnWndProc=RegisterWndProc;rw.hInstance=hi;rw.lpszClassName=L"Lkit16CpuRegisters";rw.hCursor=LoadCursor(nullptr,IDC_ARROW);rw.hIcon=hIconLarge;rw.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);RegisterClassW(&rw);
 WNDCLASSW tw{};tw.lpfnWndProc=TvWndProc;tw.hInstance=hi;tw.lpszClassName=L"Lkit16Television";tw.hCursor=LoadCursor(nullptr,IDC_ARROW);tw.hIcon=hIconLarge;RegisterClassW(&tw);
 HMENU menu=CreateMenu(),tools=CreatePopupMenu(),view=CreatePopupMenu(); AppendMenuW(tools,MF_STRING,IDM_DIRECT_BIN,L"BIN／BTK メモリ入出力..."); AppendMenuW(tools,MF_STRING,IDM_CMT_LOAD_WAV,L"カセットWAV読込..."); AppendMenuW(tools,MF_STRING,IDM_CMT_SAVE_WAV,L"カセットWAV保存..."); AppendMenuW(tools,MF_STRING,IDM_BASIC_START,L"BASIC起動（D800H）");AppendMenuW(tools,MF_STRING,IDM_SCA_SOUND,L"拡張SCAサウンド（50H～53H共用）");AppendMenuW(tools,MF_SEPARATOR,0,nullptr);AppendMenuW(tools,MF_STRING,IDM_TV_FONT_CREATE,L"Windowsフォントからテレビフォントを作成...");AppendMenuW(tools,MF_STRING,IDM_TV_FONT_RELOAD,L"テレビフォントを再読込（TVFONT.BIN）");AppendMenuW(menu,MF_POPUP,(UINT_PTR)tools,L"ツール"); AppendMenuW(view,MF_STRING,IDM_CPU_REGISTERS,L"CPUレジスタ"); AppendMenuW(view,MF_STRING,IDM_TV,L"テレビ画面（TVIF）");AppendMenuW(view,MF_STRING,IDM_TV_COLOR,L"カラー表示（TVIFOP）");AppendMenuW(view,MF_STRING|MF_CHECKED,IDM_TV_SOUND,L"テレビ音声（900Hz）");
 HMENU bg=CreatePopupMenu();const wchar_t* names[8]={L"黒",L"緑",L"青",L"水色",L"赤",L"黄",L"赤紫",L"白"};for(unsigned c=0;c<8;c++)AppendMenuW(bg,MF_STRING|(c==g_tvBackground?MF_CHECKED:MF_UNCHECKED),IDM_TV_BG+c,names[c]);AppendMenuW(view,MF_POPUP,(UINT_PTR)bg,L"テレビの背景色");
 AppendMenuW(menu,MF_POPUP,(UINT_PTR)view,L"表示");
 g_mainWnd=CreateWindowW(wc.lpszClassName,L"PANAFACOM LKIT-16 エミュレータ v1.31a",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,760,930,nullptr,menu,hi,nullptr);
 SendMessageW(g_mainWnd,WM_SETICON,ICON_BIG,(LPARAM)hIconLarge);
 SendMessageW(g_mainWnd,WM_SETICON,ICON_SMALL,(LPARAM)hIconSmall);
 g_binWnd=CreateWindowW(bw.lpszClassName,L"L-KIT-16 BIN／BTK メモリ入出力",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,730,290,g_mainWnd,nullptr,hi,nullptr);
 g_regWnd=CreateWindowW(rw.lpszClassName,L"L-KIT-16 CPUレジスタ",WS_CLIPCHILDREN|WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,480,370,g_mainWnd,nullptr,hi,nullptr);
 g_tvWnd=CreateWindowW(tw.lpszClassName,L"L-KIT-16 テレビ画面（TVIF）",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,760,440,g_mainWnd,nullptr,hi,nullptr);
 updateTvFontTitle();
 ShowWindow(g_mainWnd,show); MSG m; while(GetMessageW(&m,nullptr,0,0)){
   if(g_binWnd && IsWindowVisible(g_binWnd) && IsDialogMessageW(g_binWnd,&m)) continue;
   if(g_regWnd && IsWindowVisible(g_regWnd) && IsDialogMessageW(g_regWnd,&m)) continue;
   if(handlePcKeyboardMessage(m)) continue;
   TranslateMessage(&m);DispatchMessageW(&m);
 } return (int)m.wParam;
}
#endif
