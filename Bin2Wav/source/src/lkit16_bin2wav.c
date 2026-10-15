/* LKIT-16 BIN/BTK -> OOK WAV. Visual Studio 2022 / C17 / UTF-8.
 * BTK: BE16 start, BE16 last-byte endpoint (start+payload_bytes-1), payload.
 * CMT: BE16 start word, end word inclusive, NPSW, data words, sum16.
 * 3078-Hz carrier ON=1/OFF=0, 9-ms bits, 109-ms bytes, LSB-first.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <wchar.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

static char error_text[768];
static int fail(const char *fmt,...) {
    va_list ap;va_start(ap,fmt);vsnprintf(error_text,sizeof error_text,fmt,ap);va_end(ap);return 0;
}
static unsigned be16(const unsigned char *p){return ((unsigned)p[0]<<8)|p[1];}
static void store16(unsigned char *p,unsigned v){p[0]=(unsigned char)(v>>8);p[1]=(unsigned char)v;}
static void le16(unsigned char *p,uint32_t v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void le32(unsigned char *p,uint32_t v){le16(p,v);le16(p+2,v>>16);}
static int hex16(const wchar_t *text,unsigned *value){
    wchar_t *end;unsigned long n;
    if(!*text||*text==L'-'||*text==L'+'||*text==L' ')return 0;
    n=wcstoul(text,&end,16);
    if(*end||end==text||n>0xfffful)return 0;
    *value=(unsigned)n;return 1;
}
static int same_file(const wchar_t *a,const wchar_t *b){
    wchar_t *pa=_wfullpath(NULL,a,0),*pb=_wfullpath(NULL,b,0);
    int same=pa&&pb&&!_wcsicmp(pa,pb);
    HANDLE ha,hb;BY_HANDLE_FILE_INFORMATION ia,ib;
    free(pa);free(pb);if(same)return 1;
    ha=CreateFileW(a,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    hb=CreateFileW(b,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(ha!=INVALID_HANDLE_VALUE&&hb!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(ha,&ia)&&GetFileInformationByHandle(hb,&ib))
        same=ia.dwVolumeSerialNumber==ib.dwVolumeSerialNumber&&ia.nFileIndexHigh==ib.nFileIndexHigh&&ia.nFileIndexLow==ib.nFileIndexLow;
    if(ha!=INVALID_HANDLE_VALUE)CloseHandle(ha);
    if(hb!=INVALID_HANDLE_VALUE)CloseHandle(hb);
    return same;
}
static void print_path(const char *label,const wchar_t *path){
    int n=WideCharToMultiByte(CP_UTF8,0,path,-1,NULL,0,NULL,NULL);
    char *s=n>0?(char*)malloc((size_t)n):NULL;
    if(s&&WideCharToMultiByte(CP_UTF8,0,path,-1,s,n,NULL,NULL))printf("%s%s\n",label,s);
    else printf("%s（表示できません）\n",label);
    free(s);
}
static int filename_range(const wchar_t *input,unsigned *start,unsigned *end){
    const wchar_t *base=input,*p,*dot,*open;
    wchar_t a[5],b[5];
    for(p=input;*p;p++)if(*p==L'\\'||*p==L'/')base=p+1;
    dot=wcsrchr(base,L'.');if(!dot)dot=base+wcslen(base);
    if(dot-base<11)return 0;
    open=dot-11;
    if(open[0]!=L'('||open[5]!=L'-'||open[10]!=L')')return 0;
    wmemcpy(a,open+1,4);a[4]=0;wmemcpy(b,open+6,4);b[4]=0;
    return hex16(a,start)&&hex16(b,end)&&*end>=*start;
}
typedef struct{
    FILE *file;
    uint64_t samples,ms;
    double phase;
    unsigned char bytes[8192];size_t used;
    int ok;
} WaveWriter;
static int flush_pcm(WaveWriter *w){
    if(w->used&&fwrite(w->bytes,1,w->used,w->file)!=w->used)w->ok=0;
    w->used=0;return w->ok;
}
static int segment(WaveWriter *w,unsigned duration_ms,int carrier){
    uint64_t target;
    w->ms+=duration_ms;target=(w->ms*44100u+500u)/1000u;
    while(w->samples<target){
        int16_t sample;
        w->phase+=3078.0/44100.0;if(w->phase>=1.)w->phase-=1.;
        sample=carrier?(int16_t)lround(18000.*sin(6.28318530717958647692*w->phase)):0;
        le16(w->bytes+w->used,(uint16_t)sample);w->used+=2;w->samples++;
        if(w->used==sizeof w->bytes&&!flush_pcm(w))return 0;
    }
    return 1;
}
static int emit_byte(WaveWriter *w,unsigned char byte){
    unsigned j;
    if(!segment(w,9,0))return 0;
    for(j=0;j<8;j++)if(!segment(w,9,(byte>>j)&1))return 0;
    return segment(w,28,1);
}
static int write_wav(const wchar_t *path,const unsigned char *raw,size_t size,double *seconds){
    unsigned char header[44]={0};
    uint64_t samples=((uint64_t)(400u+12288u+700u)+size*109u)*44100u;
    uint32_t data_size;
    FILE *f;int fd=-1,ok;size_t i;WaveWriter writer={0};
    samples=(samples+500u)/1000u;
    if(samples>(UINT32_MAX-36u)/2u)return fail("生成するWAVがRIFFのサイズ上限を超えています。");
    data_size=(uint32_t)(samples*2u);
    memcpy(header,"RIFF",4);le32(header+4,data_size+36u);memcpy(header+8,"WAVEfmt ",8);
    le32(header+16,16);le16(header+20,1);le16(header+22,1);le32(header+24,44100);
    le32(header+28,88200);le16(header+32,2);le16(header+34,16);memcpy(header+36,"data",4);le32(header+40,data_size);
    if(_wsopen_s(&fd,path,_O_WRONLY|_O_CREAT|_O_TRUNC|_O_BINARY,_SH_DENYRW,_S_IREAD|_S_IWRITE))
        return fail("出力WAVを作成または上書きできません。");
    f=_fdopen(fd,"wb");if(!f){_close(fd);return fail("出力WAVを開けません。");}
    writer.file=f;writer.ok=1;
    ok=fwrite(header,1,sizeof header,f)==sizeof header;
    if(ok)ok=segment(&writer,400,0)&&segment(&writer,12288,1);
    for(i=0;ok&&i<size;i++)ok=emit_byte(&writer,raw[i]);
    if(ok)ok=segment(&writer,700,0)&&flush_pcm(&writer);
    if(writer.samples!=samples)ok=0;
    if(fclose(f))ok=0;
    if(!ok)return fail("WAVの書き込みを完了できません。途中までの出力が残る場合があります。");
    *seconds=(double)samples/44100.;return 1;
}
static void usage(void){
    puts("LKIT-16 BIN／BTK→WAV変換（Visual Studio 2022 / C17）\n"
         "使い方：lkit16_bin2wav 入力.btk 出力.wav\n"
         "        lkit16_bin2wav 入力.bin 出力.wav --start 0040\n"
         "        lkit16_bin2wav \"LED LOOP(0040-0091).bin\" 出力.wav\n"
         "  --start HHHH  BINの開始語アドレス（16進数）。BTKでは指定不可\n"
         "  --npsw HHHH   保存するNPSW（16進数、省略時0000）\n"
         "  --help        この説明を表示\n"
         "BINは上位バイト→下位バイト。開始番地の指定がなければファイル名末尾の範囲を使用。\n"
         "BTKは先頭4バイトのアドレス情報を使用。拡張子の大文字・小文字は区別しません。\n"
         "出力はOOK、3078Hz、44.1kHz／16ビット／モノラル。同名の出力は上書きします。\n"
         "終了コード：0＝成功、2＝エラー。JSONは生成しません。");
}
int wmain(int argc,wchar_t **argv){
    const wchar_t *input=NULL,*output=NULL,*ext;
    unsigned start=0,end=0,npsw=0,checksum=0,header_end=0;
    int have_start=0,btk=0,status=2,i;
    unsigned char *file=NULL,*raw=NULL,*payload;
    size_t size=0,payload_size,raw_size,j;__int64 length;
    FILE *f=NULL;double seconds;
    SetConsoleOutputCP(CP_UTF8);
    for(i=1;i<argc;i++){
        if(!wcscmp(argv[i],L"--help")){usage();return 0;}
        if(!wcscmp(argv[i],L"--start")||!wcscmp(argv[i],L"--npsw")){
            int is_start=!wcscmp(argv[i],L"--start");unsigned value;
            if(++i>=argc||!hex16(argv[i],&value)){fail("アドレス／NPSWには0000～FFFFの16進数を指定してください。");goto done;}
            if(is_start){start=value;have_start=1;}else npsw=value;
        }else if(argv[i][0]==L'-'){fail("指定したオプションはありません。--helpで確認してください。");goto done;}
        else if(!input)input=argv[i];else if(!output)output=argv[i];
        else{fail("ファイル名は入力と出力の2つを指定してください。");goto done;}
    }
    if(!input||!output){usage();fail("入力ファイルと出力WAVを指定してください。");goto done;}
    ext=wcsrchr(input,L'.');
    if(!ext||(_wcsicmp(ext,L".bin")&&_wcsicmp(ext,L".btk"))){fail("入力拡張子はBINまたはBTKを指定してください。");goto done;}
    btk=!_wcsicmp(ext,L".btk");
    if(btk&&have_start){fail("BTKはヘッダーの開始アドレスを使用します。--startは指定しないでください。");goto done;}
    if(same_file(input,output)){fail("入力と同じファイルには出力できません。");goto done;}
    if(_wfopen_s(&f,input,L"rb")||!f){fail("入力ファイルを開けません。");goto done;}
    if(_fseeki64(f,0,SEEK_END)||(length=_ftelli64(f))<=0||length>131076||_fseeki64(f,0,SEEK_SET)){
        fail("入力が空、読み取り不能、または最大サイズを超えています。");goto done;
    }
    size=(size_t)length;file=(unsigned char*)malloc(size);
    if(!file){fail("メモリを確保できません。");goto done;}
    if(fread(file,1,size,f)!=size){fail("入力を最後まで読み取れません。");goto done;}
    fclose(f);f=NULL;
    payload=file;payload_size=size;
    if(btk){
        if(size<6){fail("BTKのアドレスヘッダーまたはデータが不足しています。");goto done;}
        start=be16(file);header_end=be16(file+2);payload=file+4;payload_size=size-4;
        if(header_end<start||(size_t)(header_end-start+1u)!=payload_size){
            fail("BTKヘッダーの終了値とデータ長が一致しません（終了値は開始値＋データバイト数－1）。");goto done;
        }
    }
    if(payload_size%2||payload_size<2||payload_size>131072){fail("メモリデータは偶数バイト（1語＝2バイト）で指定してください。");goto done;}
    if(!btk&&!have_start){
        if(!filename_range(input,&start,&end)){fail("BINの開始アドレスを--startで指定するか、ファイル名を名前(HHHH-HHHH).binにしてください。");goto done;}
        if((size_t)(end-start+1u)*2u!=payload_size){fail("BINのファイル名にある保存範囲とデータ長が一致しません。");goto done;}
    }
    if((uint64_t)start+payload_size/2u-1u>0xffffu){fail("保存範囲がFFFF番地を超えています。");goto done;}
    end=start+(unsigned)(payload_size/2u)-1u;
    for(j=0;j<payload_size;j+=2)checksum=(checksum+be16(payload+j))&0xffffu;
    raw_size=payload_size+8u;raw=(unsigned char*)malloc(raw_size);
    if(!raw){fail("メモリを確保できません。");goto done;}
    store16(raw,start);store16(raw+2,end);store16(raw+4,npsw);memcpy(raw+6,payload,payload_size);store16(raw+6+payload_size,checksum);
    if(!write_wav(output,raw,raw_size,&seconds))goto done;
    puts("WAVへの変換が完了しました。");print_path("入力ファイル：",input);print_path("出力ファイル：",output);
    printf("入力形式：%s\n",btk?"BTK（先頭4バイトにアドレス情報）":"BIN（ヘッダーなし）");
    if(btk)printf("BTKヘッダー：開始%04X、バイト単位の終了値%04X\n",start,header_end);
    printf("保存範囲：%04X～%04X（語アドレス、末尾を含む）\nデータ量：%zu語（%zuバイト）\n"
           "NPSW：%04X\nチェックサム：%04X\nテープ列：%zuバイト\n"
           "記録方式：OOK（搬送波あり＝1、なし＝0）\n搬送波：3078Hz\n"
           "音声形式：44.1kHz／16ビット整数PCM／モノラル\n録音時間：%.3f秒\n",
           start,end,payload_size/2,payload_size,npsw,checksum,raw_size,seconds);
    status=0;
done:
    if(f)fclose(f);free(file);free(raw);
    if(status)fprintf(stderr,"エラー：%s\n",error_text);
    return status;
}
