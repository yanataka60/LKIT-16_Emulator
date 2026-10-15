/* LKIT-16 standard monitor CMT decoder. Visual Studio 2022, C17.
 * Automatic OOK or 1200/2400 Hz FSK; 9 ms bits; 109 ms byte interval.
 * Each byte: start 0, eight LSB-first bits, stop 1 (about 28 ms).
 * Words: high byte first. Header: start/end inclusive/NPSW; sum16 follows data.
 * Reference: supplied user's manual, printed pages 124-128 and ROM OUT routine.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

typedef struct {
    uint32_t rate;
    uint16_t channels, bits, align;
    uint64_t data_offset, data_size;
} Wav;
typedef struct {
    double *levels, dt, threshold;
    unsigned char *on;
    size_t count;
    const char *mode;
} Signal;
typedef struct {
    unsigned char *raw;
    size_t size, expected;
    unsigned int start, end, npsw, checksum;
    double first_time;
} Result;
typedef struct {
    const wchar_t *input, *output;
    unsigned int channel;
    double threshold, speed, leader;
    int little, raw, inspect;
} Options;
static char error_text[1024];

static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error_text, sizeof error_text, fmt, ap);
    va_end(ap);
    return 0;
}
static uint16_t u16(const unsigned char *p) {
    return (uint16_t)((unsigned int)p[0] | ((unsigned int)p[1] << 8));
}
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static unsigned int be16(const unsigned char *p) {
    return ((unsigned int)p[0] << 8) | p[1];
}
/* Matches Python's round-to-even used by the verified reference decoder. */
static size_t nearest(double x) {
    double lo = floor(x), fraction = x - lo;
    return (size_t)(lo + (fraction > .5 || (fraction == .5 && fmod(lo, 2.) != 0.)));
}
static int read_wav_header(FILE *f, Wav *w) {
    unsigned char h[12], chunk[8], fmt[40];
    __int64 file_size;
    uint64_t limit, pos;
    int have_fmt = 0, have_data = 0;
    if (_fseeki64(f, 0, SEEK_END) != 0 || (file_size = _ftelli64(f)) < 12 ||
        _fseeki64(f, 0, SEEK_SET) != 0 || fread(h, 1, 12, f) != 12)
        return fail("WAVヘッダーを読み取れません。");
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return fail("RIFF/WAVE形式のファイルを指定してください。RF64には対応していません。");
    limit = (uint64_t)u32(h + 4) + 8;
    if (limit < 12 || limit > (uint64_t)file_size) return fail("RIFFに記録されたサイズよりファイルが短く、途中で切れています。");
    for (pos = 12; pos < limit;) {
        uint32_t size;
        uint64_t next;
        if (limit - pos < 8 || _fseeki64(f, (__int64)pos, SEEK_SET) != 0 ||
            fread(chunk, 1, 8, f) != 8) return fail("WAVチャンクのヘッダーが途中で切れています。");
        size = u32(chunk + 4);
        next = pos + 8 + size + (size & 1u);
        if (next > limit) return fail("WAVチャンクのサイズがRIFFの範囲を超えています。");
        if (!memcmp(chunk, "fmt ", 4)) {
            uint16_t tag;
            size_t take = size < sizeof fmt ? (size_t)size : sizeof fmt;
            if (have_fmt || size < 16 || fread(fmt, 1, take, f) != take)
                return fail("音声形式情報（fmtチャンク）が不正、または重複しています。");
            tag = u16(fmt);
            w->channels = u16(fmt + 2); w->rate = u32(fmt + 4);
            w->align = u16(fmt + 12); w->bits = u16(fmt + 14);
            if (tag == 0xfffe) {
                static const unsigned char pcm_guid[16] =
                    {1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
                if (size < 40 || u16(fmt + 16) < 22 ||
                    (uint32_t)u16(fmt + 16) > size - 18 ||
                    memcmp(fmt + 24, pcm_guid, 16) || u16(fmt + 18) != w->bits)
                    return fail("拡張WAVの形式または有効ビット数に対応していません。");
            } else if (tag != 1) return fail("整数PCM形式のWAVにのみ対応しています。");
            if (!w->channels || w->rate < 16000 || w->rate > 384000 ||
                (w->bits != 8 && w->bits != 16 && w->bits != 24 && w->bits != 32) ||
                (uint32_t)w->align != (uint32_t)w->channels * (w->bits / 8u) ||
                (uint64_t)u32(fmt + 8) != (uint64_t)w->rate * w->align)
                return fail("PCMの周波数・チャンネル数・ビット数・サイズが非対応、または不整合です。");
            have_fmt = 1;
        } else if (!memcmp(chunk, "data", 4)) {
            if (have_data) return fail("複数のdataチャンクには対応していません。");
            w->data_offset = pos + 8; w->data_size = size; have_data = 1;
        }
        pos = next;
    }
    if (!have_fmt || !have_data || !w->data_size || w->data_size % w->align)
        return fail("音声データがない、空、またはPCMフレームが途中で切れています。");
    return 1;
}
static double pcm_sample(const unsigned char *p, unsigned int bytes) {
    uint64_t value = 0;
    unsigned int i;
    if (bytes == 1) return ((double)p[0] - 128.) / 128.;
    for (i = 0; i < bytes; ++i) value |= (uint64_t)p[i] << (8u * i);
    return (double)((int64_t)value - ((p[bytes-1] & 128u) ?
        (int64_t)(UINT64_C(1) << (8u * bytes)) : 0)) /
        (double)(UINT64_C(1) << (8u * bytes - 1u));
}
static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static int build_signal(FILE *f, const Wav *w, const Options *opt, Signal *s) {
    size_t block = nearest(w->rate * .001), i, j, buffer_size;
    uint64_t count;
    unsigned char *buffer = NULL;
    double *sorted = NULL;
    if (opt->channel >= w->channels) return fail("指定したチャンネルはこのWAVにありません。");
    if (block < 8) block = 8;
    count = (w->data_size / w->align) / block;
    if (!count || count > SIZE_MAX / sizeof(double)) return fail("音声データが短すぎる、または処理できるサイズを超えています。");
    s->count = (size_t)count; s->dt = (double)block / w->rate;
    s->levels = (double *)malloc(s->count * sizeof(double));
    s->on = (unsigned char *)malloc(s->count);
    buffer_size = block * w->align;
    buffer = (unsigned char *)malloc(buffer_size);
    if (!s->levels || !s->on || !buffer) { free(buffer); return fail("処理に必要なメモリを確保できません。"); }
    if (_fseeki64(f, (__int64)w->data_offset, SEEK_SET) != 0) {
        free(buffer); return fail("PCMデータの位置に移動できません。");
    }
    for (i = 0; i < s->count; ++i) {
        double prev, sum = 0.;
        unsigned int width = w->bits / 8u;
        if (fread(buffer, 1, buffer_size, f) != buffer_size) {
            free(buffer); return fail("PCMデータが途中で切れています。");
        }
        prev = pcm_sample(buffer + (size_t)opt->channel * width, width);
        for (j = 1; j < block; ++j) {
            double current = pcm_sample(buffer + j * w->align + (size_t)opt->channel * width, width);
            double delta = current - prev;
            sum += delta * delta; prev = current;
        }
        s->levels[i] = sqrt(sum / (double)(block-1));
    }
    free(buffer);
    s->threshold = opt->threshold;
    if (s->threshold == 0.) {
        sorted = (double *)malloc(s->count * sizeof(double));
        if (!sorted) return fail("処理に必要なメモリを確保できません。");
        memcpy(sorted, s->levels, s->count * sizeof(double));
        qsort(sorted, s->count, sizeof(double), compare_double);
        /* Tape/MP3 transition ringing can exceed the old 0.25 multiplier.
         * 0.3125 preserves bit centers and validates all 18 real recordings.
         * The recovered frame structure and checksum are still mandatory.
         */
        s->threshold = sorted[(size_t)((double)(s->count-1) * .90)] * .3125;
        free(sorted);
    }
    if (s->threshold <= 1e-6) return fail("復元に使える搬送波信号を検出できません。");
    for (i = 0; i < s->count; ++i) s->on[i] = (unsigned char)(s->levels[i] > s->threshold);
    /* Read original neighbors (not already modified neighbors). */
    for (i = 1; i + 1 < s->count; ++i)
        if (s->levels[i-1] > s->threshold && s->levels[i+1] > s->threshold) s->on[i] = 1;
    s->mode = opt->threshold == 0. ? "OOK（搬送波の有無・自動判定）" : "RMS（しきい値の手動指定）";
    return 1;
}
/* Four-millisecond quadrature detectors distinguish tones by energy, not
 * amplitude. A five-block sliding buffer keeps memory independent of WAV size.
 * FSK is attempted only when automatic OOK decoding fails validation.
 */
static int build_fsk_signal(FILE *f, const Wav *w, const Options *opt, Signal *s) {
    size_t block = nearest(w->rate * .001), window, bytes, i, j;
    unsigned int width = w->bits / 8u;
    unsigned char *buffer;
    double *kernel, *samples;
    const double pi = 3.14159265358979323846;
    if (2400. * opt->speed >= w->rate / 2.) return fail("FSK周波数がサンプリング周波数の上限を超えています。");
    if (block < 8) block = 8;
    window = 4 * block; bytes = block * w->align;
    if (s->count < 5) return fail("FSKの復元に必要な録音時間がありません。");
    buffer = (unsigned char *)malloc(5 * bytes);
    kernel = (double *)malloc(4 * window * sizeof(double));
    samples = (double *)malloc(window * sizeof(double));
    if (!buffer || !kernel || !samples) {
        free(buffer); free(kernel); free(samples); return fail("処理に必要なメモリを確保できません。");
    }
    for (j = 0; j < window; ++j) {
        double weight = .5 - .5*cos(2.*pi*j/(double)(window-1));
        double angle = 2.*pi*1200.*opt->speed*j/w->rate;
        kernel[j] = weight*cos(angle);
        kernel[window+j] = weight*sin(angle);
        kernel[2*window+j] = weight*cos(2.*angle);
        kernel[3*window+j] = weight*sin(2.*angle);
    }
    if (_fseeki64(f, (__int64)w->data_offset, SEEK_SET) != 0 ||
        fread(buffer, 1, 5*bytes, f) != 5*bytes) {
        free(buffer); free(kernel); free(samples); return fail("FSKのPCMデータを読み取れません。");
    }
    memset(s->on, 0, s->count);
    for (i = 0; i + 4 < s->count; ++i) {
        double mean = 0., re0 = 0., im0 = 0., re1 = 0., im1 = 0.;
        if (s->levels[i+2] > s->threshold) {
            for (j = 0; j < window; ++j) {
                samples[j] = pcm_sample(buffer + (j+block/2)*w->align +
                    (size_t)opt->channel*width, width);
                mean += samples[j];
            }
            mean /= (double)window;
            for (j = 0; j < window; ++j) {
                double x = samples[j]-mean;
                re0 += x*kernel[j]; im0 += x*kernel[window+j];
                re1 += x*kernel[2*window+j]; im1 += x*kernel[3*window+j];
            }
            s->on[i+2] = (unsigned char)(re1*re1+im1*im1 > re0*re0+im0*im0);
        }
        if (i + 5 < s->count) {
            memmove(buffer, buffer+bytes, 4*bytes);
            if (fread(buffer+4*bytes, 1, bytes, f) != bytes) {
                free(buffer); free(kernel); free(samples); return fail("FSKのPCMデータが途中で切れています。");
            }
        }
    }
    free(buffer); free(kernel); free(samples);
    s->mode = "FSK（1200/2400 Hz・自動判定）";
    return 1;
}
static int decode_candidate(const Signal *s, const Options *opt, size_t first, Result *r) {
    size_t start = first;
    const size_t capacity = 2u * 65536u + 8u;
    r->size = r->expected = 0; r->first_time = first * s->dt;
    if (!r->raw) r->raw = (unsigned char *)malloc(capacity);
    if (!r->raw) return fail("処理に必要なメモリを確保できません。");
    while (!r->expected || r->size < r->expected) {
        double time = start * s->dt;
        unsigned int bits[9], j, value = 0;
        static const double fractions[3] = {.35,.5,.65};
        static const double stops[3] = {.087,.095,.103};
        if (nearest((time + .104/opt->speed)/s->dt) >= s->count)
            return fail("%.6f 秒でバイトまたは停止区間が途中で切れています。", time);
        for (j = 0; j < 9; ++j) {
            size_t indices[3];
            unsigned int k;
            for (k = 0; k < 3; ++k)
                indices[k] = nearest((time + (j + fractions[k]) * .009/opt->speed)/s->dt);
            bits[j] = s->on[indices[0]];
            if (bits[j] != s->on[indices[1]] || bits[j] != s->on[indices[2]])
                return fail("%.6f 秒のビット%uを確実に判定できません。", time, j);
        }
        if (bits[0]) return fail("%.6f 秒の開始ビットが不正です。", time);
        for (j = 0; j < 3; ++j)
            if (!s->on[nearest((time + stops[j]/opt->speed)/s->dt)])
                return fail("%.6f 秒の停止区間が不正です。", time);
        for (j = 0; j < 8; ++j) value |= bits[j+1] << j;
        if (r->size >= capacity) return fail("保存ブロックが処理できるサイズを超えています。");
        r->raw[r->size++] = (unsigned char)value;
        if (r->size == 6) {
            r->start = be16(r->raw); r->end = be16(r->raw+2); r->npsw = be16(r->raw+4);
            if (r->end < r->start) return fail("終了アドレスが開始アドレスより小さい値です。");
            r->expected = 2u * (size_t)(r->end-r->start+1u) + 8u;
        }
        if (r->expected && r->size == r->expected) break;
        {
            double target = time + .109/opt->speed, tolerance = .004/opt->speed;
            double lower = ceil((target-tolerance)/s->dt), upper = floor((target+tolerance)/s->dt);
            double best_distance = HUGE_VAL;
            size_t lo, hi, i, best = SIZE_MAX;
            if (lower >= (double)s->count) return fail("次のバイトがありません（受信済み：%zu バイト）。", r->size);
            lo = lower < 1. ? 1 : (size_t)lower;
            hi = upper >= (double)s->count ? s->count-1 : (size_t)upper;
            for (i = lo; i <= hi; ++i) {
                double distance = fabs(i*s->dt-target);
                if (s->on[i-1] && !s->on[i] && distance < best_distance) {
                    best = i; best_distance = distance;
                }
            }
            if (best == SIZE_MAX) return fail("%.6f 秒付近に次のバイトがありません（受信済み：%zu バイト）。", target, r->size);
            start = best;
        }
    }
    {
        size_t i;
        unsigned int sum = 0, recorded = be16(r->raw+r->size-2);
        for (i = 6; i < r->size-2; i += 2) sum = (sum + be16(r->raw+i)) & 0xffffu;
        if (sum != recorded) return fail("チェックサムが一致しません。計算値：%04X、記録値：%04X。", sum, recorded);
        r->checksum = sum;
    }
    return 1;
}
static int decode(const Signal *s, const Options *opt, Result *r) {
    size_t i, run_start = 0;
    int found = 0;
    for (i = 1; i < s->count; ++i) {
        if (s->on[i] != s->on[i-1]) {
            if (s->on[i-1] && (i-run_start)*s->dt >= opt->leader/opt->speed) {
                found = 1;
                if (decode_candidate(s, opt, i, r)) return 1;
            }
            run_start = i;
        }
    }
    return found ? 0 : fail("リーダー信号に続く開始ビットを検出できません。");
}
static int same_file(const wchar_t *a, const wchar_t *b) {
    wchar_t *pa = _wfullpath(NULL, a, 0), *pb = _wfullpath(NULL, b, 0);
    int same = pa && pb && !_wcsicmp(pa, pb);
    HANDLE ha, hb;
    BY_HANDLE_FILE_INFORMATION ia, ib;
    free(pa); free(pb);
    if (same) return 1;
    ha = CreateFileW(a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    hb = CreateFileW(b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(ha, &ia) && GetFileInformationByHandle(hb, &ib))
        same = ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
            ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return same;
}
static wchar_t *suffix(const wchar_t *path, const wchar_t *extra) {
    size_t length = wcslen(path) + wcslen(extra) + 1;
    wchar_t *out = (wchar_t *)malloc(length * sizeof(wchar_t));
    if (out) { wcscpy_s(out, length, path); wcscat_s(out, length, extra); }
    return out;
}
static int preflight(const Options *opt, const wchar_t *path) {
    DWORD attr;
    if (same_file(opt->input, path)) return fail("入力WAVと同じファイルには出力できません。別の出力名を指定してください。");
    attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return fail("出力先にファイル名を指定してください。フォルダーには書き込めません。");
    return 1;
}
static FILE *open_output(const wchar_t *path) {
    int fd = -1;
    FILE *f;
    if (_wsopen_s(&fd, path, _O_WRONLY | _O_CREAT | _O_BINARY | _O_TRUNC,
        _SH_DENYRW, _S_IREAD | _S_IWRITE) != 0) return NULL;
    f = _fdopen(fd, "wb");
    if (!f) _close(fd);
    return f;
}
static int write_bytes(const wchar_t *path, const unsigned char *data, size_t size) {
    FILE *f = open_output(path);
    int good;
    if (!f) return fail("出力ファイルを作成または上書きできません。");
    good = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) good = 0;
    return good ? 1 : fail("出力ファイルの書き込みを完了できません。");
}
static void print_path(const char *label, const wchar_t *path) {
    int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
    char *text = size > 0 ? (char *)malloc((size_t)size) : NULL;
    if (text && WideCharToMultiByte(CP_UTF8, 0, path, -1, text, size, NULL, NULL))
        printf("%s%s\n", label, text);
    else printf("%s（ファイル名を表示できません）\n", label);
    free(text);
}
static void print_result(const Options *opt, const Wav *w, const Signal *s, const Result *r) {
    puts("変換が完了しました。");
    print_path("入力ファイル：", opt->input);
    print_path("出力ファイル：", opt->output);
    printf("記録形式：LKIT-16 標準モニタ CMT\n"
           "開始アドレス：%04X（語アドレス）\n終了アドレス：%04X（末尾を含む）\n"
           "NPSW：%04X\nデータ量：%u 語（%zu バイト）\n"
           "ヘッダー等を含む受信量：%zu バイト\nチェックサム：%04X（一致）\n"
           "サンプリング周波数：%u Hz\n選択チャンネル：%u（先頭は0）\n"
           "信号判定のしきい値：%.12g\n再生速度比：%.12g\n復調方法：%s\n"
           "先頭バイトの開始時刻：%.9f 秒\n語内バイト順：%s\n",
           r->start, r->end, r->npsw, r->end-r->start+1u, r->size-8, r->size,
           r->checksum, w->rate, opt->channel, s->threshold, opt->speed, s->mode,
           r->first_time, opt->little ? "下位→上位（リトルエンディアン）" : "上位→下位（ビッグエンディアン）");
    if (opt->raw) {
        wchar_t *path = suffix(opt->output, L".raw");
        if (path) { print_path("受信列ファイル：", path); free(path); }
    }
}
static void usage(void) {
    puts("LKIT-16 CMT WAV→バイナリ変換（Visual Studio 2022 / C17）\n"
         "使い方：lkit16_wav2bin 入力.wav 出力.bin [オプション]\n"
         "        lkit16_wav2bin 入力.wav --inspect\n"
         "同名の出力ファイルは上書きします。変換結果は画面に表示します。\n"
         "  --channel N          チャンネルを選択（先頭は0）\n"
         "  --speed R            再生速度比（標準1.0、範囲0.5～2.0）\n"
         "  --threshold R        信号判定のしきい値（省略時は自動）\n"
         "  --leader-min R       最小リーダー秒数（標準1.0）\n"
         "  --byte-order big|little  語内バイト順（標準big＝上位→下位）\n"
         "  --raw                受信列を出力.bin.rawにも保存\n"
         "  --inspect            WAV形式情報のみ表示\n"
         "  --help               この説明を表示\n"
         "終了コード：0＝成功・説明表示、2＝引数・入力・復元・書き込みのエラー");
}
static int number(const wchar_t *text, double *value) {
    wchar_t *end;
    errno = 0; *value = wcstod(text, &end);
    return text != end && !*end && errno != ERANGE && isfinite(*value);
}
int wmain(int argc, wchar_t **argv) {
    Options opt = {0};
    Wav wav = {0}; Signal signal = {0}; Result result = {0};
    FILE *input = NULL;
    wchar_t *raw_path = NULL;
    int i, status = 2;
    /* /utf-8 makes literals UTF-8; redirected output is also UTF-8. */
    SetConsoleOutputCP(CP_UTF8);
    opt.speed = opt.leader = 1.;
    for (i = 1; i < argc; ++i) {
        const wchar_t *arg = argv[i];
        double value;
        if (!wcscmp(arg,L"--help")) { usage(); return 0; }
        if (!wcscmp(arg,L"--raw")) opt.raw = 1;
        else if (!wcscmp(arg,L"--inspect")) opt.inspect = 1;
        else if (!wcscmp(arg,L"--byte-order")) {
            if (++i >= argc || (wcscmp(argv[i],L"big") && wcscmp(argv[i],L"little"))) {
                fail("--byte-orderにはbigまたはlittleを指定してください。"); goto cleanup;
            }
            opt.little = !wcscmp(argv[i],L"little");
        } else if (!wcscmp(arg,L"--channel") || !wcscmp(arg,L"--speed") ||
            !wcscmp(arg,L"--threshold") || !wcscmp(arg,L"--leader-min")) {
            if (++i >= argc || !number(argv[i], &value)) { fail("オプションには有効な数値を指定してください。"); goto cleanup; }
            if (!wcscmp(arg,L"--channel")) {
                if (value < 0 || value > 65535 || floor(value) != value) { fail("チャンネル番号には0～65535の整数を指定してください。"); goto cleanup; }
                opt.channel = (unsigned int)value;
            } else if (!wcscmp(arg,L"--speed")) opt.speed = value;
            else if (!wcscmp(arg,L"--threshold")) {
                if (value <= 0) { fail("しきい値には0より大きい値を指定してください。"); goto cleanup; }
                opt.threshold = value;
            } else opt.leader = value;
        } else if (arg[0] == L'-') { fail("指定したオプションはありません。--helpで使い方を確認してください。"); goto cleanup; }
        else if (!opt.input) opt.input = arg;
        else if (!opt.output) opt.output = arg;
        else { fail("ファイル名の指定が多すぎます。入力と出力の2つを指定してください。"); goto cleanup; }
    }
    if (!opt.input || (!opt.inspect && !opt.output) || opt.speed < .5 || opt.speed > 2 || opt.leader <= 0) {
        usage(); fail("ファイル名が不足しているか、再生速度比・リーダー秒数の指定が不正です。"); goto cleanup;
    }
    if (_wfopen_s(&input, opt.input, L"rb") != 0 || !input) { fail("入力WAVファイルを開けません。"); goto cleanup; }
    if (!read_wav_header(input, &wav)) goto cleanup;
    if (opt.channel >= wav.channels) { fail("指定したチャンネルはこのWAVにありません。"); goto cleanup; }
    if (opt.inspect) {
        printf("WAV形式：整数PCM、%u Hz、%u ビット、%u チャンネル、%.6f 秒\n",
            wav.rate, (unsigned int)wav.bits, (unsigned int)wav.channels,
            (double)(wav.data_size/wav.align)/wav.rate);
        status = 0; goto cleanup;
    }
    if (opt.raw) raw_path = suffix(opt.output,L".raw");
    if (opt.raw && !raw_path) { fail("処理に必要なメモリを確保できません。"); goto cleanup; }
    if (!preflight(&opt,opt.output) ||
        (opt.raw && !preflight(&opt,raw_path))) goto cleanup;
    if (!build_signal(input, &wav, &opt, &signal)) goto cleanup;
    if (!decode(&signal, &opt, &result)) {
        char ook_error[sizeof error_text];
        if (opt.threshold != 0.) goto cleanup;
        memcpy(ook_error, error_text, sizeof ook_error);
        if (!build_fsk_signal(input, &wav, &opt, &signal) || !decode(&signal, &opt, &result)) {
            char fsk_error[sizeof error_text];
            memcpy(fsk_error, error_text, sizeof fsk_error);
            fail("自動復元に失敗しました。\n  OOK判定：%s\n  FSK判定：%s", ook_error, fsk_error);
            goto cleanup;
        }
    }
    if (opt.raw && !write_bytes(raw_path, result.raw, result.size)) goto cleanup;
    if (opt.little) {
        size_t j;
        for (j = 6; j < result.size-2; j += 2) {
            unsigned char tmp = result.raw[j]; result.raw[j] = result.raw[j+1]; result.raw[j+1] = tmp;
        }
    }
    if (!write_bytes(opt.output, result.raw+6, result.size-8)) goto cleanup;
    print_result(&opt, &wav, &signal, &result);
    status = 0;
cleanup:
    if (input) fclose(input);
    free(signal.levels); free(signal.on); free(result.raw); free(raw_path);
    if (status) fprintf(stderr,"エラー：%s\n",error_text);
    return status;
}
