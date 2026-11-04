#include "movies.h"
extern "C" {
#include "hw.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <sys/stat.h>

namespace {
struct Mapping { unsigned disc, lba, frames; int offset; const char *name; };
const Mapping mappings[] = {
#include "movie_map.inc"
};
uint16_t le16(const uint8_t *p) { return unsigned(p[0]) | unsigned(p[1])<<8; }
uint32_t le32(const uint8_t *p) { return le16(p) | uint32_t(le16(p+2))<<16; }
using Frame = HostMovies::Frame;

// Only the worker owns libav*. The guest only posts requests and takes ready frames.
struct Decoder {
    AVFormatContext *format=nullptr;
    AVCodecContext *codec=nullptr;
    AVPacket *packet=av_packet_alloc();
    AVFrame *image=av_frame_alloc();
    SwsContext *scaler=nullptr;
    int stream=-1;
    int64_t first=0;
    bool draining=false;
    struct Source {
        AVFrame *image=nullptr;
        double seconds=0;
        ~Source() { av_frame_free(&image); }
    };
    std::shared_ptr<Source> previous, next, converted;
    std::shared_ptr<Frame> output;
    ~Decoder() {
        sws_freeContext(scaler); av_frame_free(&image); av_packet_free(&packet);
        avcodec_free_context(&codec); avformat_close_input(&format);
    }
    bool open(const std::string &path) {
        if (!image || !packet || avformat_open_input(&format,path.c_str(),nullptr,nullptr)<0 ||
            avformat_find_stream_info(format,nullptr)<0) return false;
        stream=av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
        if (stream<0) return false;
        auto *parameters=format->streams[stream]->codecpar;
        auto *decoder=avcodec_find_decoder(parameters->codec_id);
        if (!decoder || !(codec=avcodec_alloc_context3(decoder)) || avcodec_parameters_to_context(codec,parameters)<0) return false;
        codec->thread_count=1;
        first=format->streams[stream]->start_time;
        if (first==AV_NOPTS_VALUE) first=0;
        return avcodec_open2(codec,decoder,nullptr)>=0;
    }
    std::shared_ptr<Source> read() {
        for (;;) {
            int result=avcodec_receive_frame(codec,image);
            if (result==0) {
                if (image->best_effort_timestamp==AV_NOPTS_VALUE || image->width<=0 || image->height<=0) return {};
                auto out=std::make_shared<Source>();
                out->seconds=(image->best_effort_timestamp-first)*av_q2d(format->streams[stream]->time_base);
                out->image=av_frame_clone(image);
                return out->image ? out : std::shared_ptr<Source>{};
            }
            if (result!=AVERROR(EAGAIN) || draining) return {};
            do {
                result=av_read_frame(format,packet);
                if (result<0) { draining=true; avcodec_send_packet(codec,nullptr); break; }
                if (packet->stream_index==stream) {
                    result=avcodec_send_packet(codec,packet); av_packet_unref(packet);
                    if (result<0) return {};
                    break;
                }
                av_packet_unref(packet);
            } while (true);
        }
    }
    std::shared_ptr<Frame> at(double seconds,unsigned scale) {
        if (!next) next=read();
        while (next && next->seconds<seconds) { previous=next; next=read(); }
        auto selected=next;
        if (!next) selected=previous && seconds-previous->seconds<=1.0/15.0 ? previous : std::shared_ptr<Source>{};
        else if (previous && seconds-previous->seconds<=next->seconds-seconds) selected=previous;
        if (!selected) return {};
        if (selected==converted) return output;
        // Decode dependencies in YUV; convert only the selected guest timestamp.
        auto out=std::make_shared<Frame>();
        out->seconds=selected->seconds; out->width=320*scale; out->height=224*scale;
        out->rgba.resize(size_t(out->width)*out->height*4);
        const AVFrame *source=selected->image;
        scaler=sws_getCachedContext(scaler,source->width,source->height,AVPixelFormat(source->format),
            out->width,out->height,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
        if (!scaler) return {};
        int space=source->colorspace==AVCOL_SPC_UNSPECIFIED ? SWS_CS_DEFAULT : int(source->colorspace);
        const int *coefficients=sws_getCoefficients(space);
        if (sws_setColorspaceDetails(scaler,coefficients,source->color_range==AVCOL_RANGE_JPEG,
                                    coefficients,1,0,1<<16,1<<16)<0) return {};
        uint8_t *planes[]={out->rgba.data(),nullptr,nullptr,nullptr};
        int strides[]={int(out->width*4),0,0,0};
        if (sws_scale(scaler,source->data,source->linesize,0,source->height,planes,strides)!=int(out->height)) return {};
        converted=selected; output=out;
        return out;
    }
};
struct Movies {
    std::string directory;
    unsigned scale=1, disc=1;
    cpu_t *cpu=nullptr;
    const Mapping *stream=nullptr;
    std::array<const Mapping *,16> ring_stream{};
    std::array<unsigned,16> ring_frame{}, ring_base{};
    unsigned incoming_base=0;
    std::shared_ptr<const Frame> current;
    unsigned frame=0;
    bool decode_valid=false;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop=false, unavailable=false;
    const Mapping *requested=nullptr;
    unsigned wanted=0, generation=0;
    struct Ready { unsigned frame; std::shared_ptr<Frame> image; };
    std::deque<Ready> queue;
    std::thread worker;
    FILE *statistics=nullptr;
    uint64_t decoded=0, replaced=0, late=0, missing=0, strips=0;
    ~Movies() {
        { std::lock_guard<std::mutex> lock(mutex); stop=true; }
        wake.notify_one();
        if (worker.joinable()) worker.join();
        if (statistics) std::fclose(statistics);
        std::fprintf(stderr,"Movies: frames=%llu replaced=%llu late=%llu missing=%llu row_spans=%llu\n",
            (unsigned long long)decoded,(unsigned long long)replaced,(unsigned long long)late,
            (unsigned long long)missing,(unsigned long long)strips);
    }
    void request(const Mapping *mapping,unsigned number) {
        std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);
        if (!lock.owns_lock()) return;
        if (requested!=mapping || number<wanted) { requested=mapping; ++generation; queue.clear(); unavailable=false; }
        wanted=number;
        while (!queue.empty() && queue.front().frame<number) queue.pop_front();
        wake.notify_one();
    }
    std::shared_ptr<const Frame> take(const Mapping *mapping,unsigned number,bool &failed) {
        std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);
        if (!lock.owns_lock() || requested!=mapping) return {};
        failed=unavailable;
        for (auto &ready:queue) if (ready.frame==number) { failed=!ready.image; return ready.image; }
        return {};
    }
    void run() {
        unsigned serial=~0u, number=0;
        std::unique_ptr<Decoder> decoder;
        const Mapping *mapping=nullptr;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock,[&] { return stop || (requested && (generation!=serial || number<std::min(requested->frames,wanted+8))); });
                if (stop) return;
                if (generation!=serial) {
                    serial=generation; number=wanted; mapping=requested;
                    decoder.reset();
                }
                if (number<wanted) number=wanted;
            }
            if (!decoder) {
                decoder.reset(new Decoder);
                std::string path=directory+"/"+mapping->name+"h.avi";
                if (!std::strcmp(mapping->name,"disc03_05")) {
                    struct stat info;
                    for (const auto &prefix:{directory+"/lang-en/movies/",directory+"/../lang-en/movies/"}) {
                        std::string localized=prefix+mapping->name+"h.avi";
                        if (!stat(localized.c_str(),&info) && S_ISREG(info.st_mode)) { path=localized; break; }
                    }
                }
                if (!decoder->open(path)) {
                    std::fprintf(stderr,"Movies: unavailable or undecodable %s; using original\n",path.c_str());
                    decoder.reset();
                    // Cache failure for this stream; retry only after a stream/restart change.
                    std::unique_lock<std::mutex> lock(mutex);
                    if (generation==serial) unavailable=true;
                    wake.wait(lock,[&] { return stop || generation!=serial; });
                    if (stop) return;
                    continue;
                }
            }
            double seconds=(double(number)+mapping->offset)/15.0;
            auto image=seconds<0 ? std::shared_ptr<Frame>{} : decoder->at(seconds,scale);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (serial==generation && number>=wanted) {
                    queue.push_back({number,image});
                    while (queue.size()>8) queue.pop_front();
                }
            }
            ++number;
        }
    }
};
std::unique_ptr<Movies> movies;
}
extern "C" int host_movies_start(const char *directory,unsigned scale,const char *statistics) {
    if (!directory) return statistics ? 0 : 1;
    struct stat info;
    if (movies || !scale || scale>8 || stat(directory,&info) || !S_ISDIR(info.st_mode)) return 0;
    try {
        auto m=std::unique_ptr<Movies>(new Movies);
        m->directory=directory; m->scale=scale;
        if (statistics) {
            m->statistics=std::fopen(statistics,"w");
            if (!m->statistics) return 0;
            std::fprintf(m->statistics,"vblank,disc,lba,guest_frame,hd_seconds,status\n");
        }
        movies=std::move(m);
        movies->worker=std::thread([m=movies.get()] { try { m->run(); } catch (...) { std::lock_guard<std::mutex> lock(m->mutex); m->unavailable=true; std::fprintf(stderr,"Movies: decoder worker failed; using original\n"); } });
        return 1;
    } catch (...) { movies.reset(); return 0; }
}
extern "C" void host_movies_bind(cpu_t *c,unsigned disc) {
    if (!movies) return;
    movies->cpu=c; movies->disc=disc; movies->stream=nullptr; movies->decode_valid=false;
    movies->ring_stream.fill(nullptr); movies->incoming_base=0; movies->current.reset();
    movies->request(nullptr,0);
}
extern "C" void host_movies_restore(cpu_t *c,unsigned disc) {
    if (!movies) return;
    host_movies_bind(c,disc);
    auto &m=*movies;
    // Loading is a host boundary: invalidate worker results even if its try-lock
    // was busy during bind. No old CPU pointer is retained by the worker.
    {
        std::lock_guard<std::mutex> lock(m.mutex);
        m.requested=nullptr; ++m.generation; m.queue.clear(); m.unavailable=false;
    }
    const auto *ram=c->ram;
    unsigned count=ram[0x1fa3c4], next=ram[0x1fa3c5], released=le16(ram+0x1fa3c0);
    if (!count || count>16 || next>=count || !released || le32(ram+0x1e8000)!=0x27bdffe0 ||
        !(ram[0x1fa3ca] || ram[0x1fa3cb])) return;
    unsigned frame=le16(ram+0x1fa434+12*((next+count-1)%count)+6);
    if (frame+1<released) return;
    // The saved CD buffer identifies the stream, while the released ring
    // descriptor identifies the displayed decode. Read-ahead never picks N.
    for (const auto &mapping:mappings) if (mapping.disc==disc &&
        c->hw->cd.data_lba>=mapping.lba && c->hw->cd.data_lba<mapping.lba+mapping.frames*10 &&
        frame<mapping.frames) {
        m.incoming_base=frame+1-released;
        for (unsigned i=0;i<count;++i) {
            m.ring_stream[i]=&mapping; m.ring_frame[i]=le16(ram+0x1fa434+12*i+6);
            m.ring_base[i]=m.incoming_base;
        }
        m.stream=&mapping; m.frame=frame; m.decode_valid=true;
        m.request(&mapping,frame);
        return;
    }
}
extern "C" void host_movies_sector(cpu_t *c) {
    if (!movies || movies->cpu!=c) return;
    const auto &d=c->hw->cd;
    unsigned at=d.data_size==2340 ? (d.data[3]==1 ? 4 : 12) : 0;
    const uint8_t *p=d.data+at;
    if (d.data_size<at+8 || std::memcmp(p,"SMJ\1",4) || p[4]!=2 || p[5]!=9) return;
    unsigned frame=le16(p+6), delta=frame*10+2;
    if (d.data_lba<delta) return;
    for (const auto &mapping:mappings) if (mapping.disc==movies->disc && mapping.lba==d.data_lba-delta && frame<mapping.frames) {
        unsigned index=c->ram[0x1fa3c6];
        if (index==0 && !le16(c->ram+0x1fa3c0)) movies->incoming_base=frame;
        if (index<movies->ring_stream.size()) {
            movies->ring_stream[index]=&mapping; movies->ring_frame[index]=frame;
            movies->ring_base[index]=movies->incoming_base;
        }
        // Reading can run ahead of decoding. Prefetch from the last decode, never from this incoming frame.
        movies->request(&mapping,movies->stream==&mapping ? movies->frame : 0);
        return;
    }
    unsigned index=c->ram[0x1fa3c6];
    if (index<movies->ring_stream.size()) movies->ring_stream[index]=nullptr;
}
extern "C" void host_movies_decode(void) {
    if (!movies || !movies->cpu) return;
    auto &m=*movies;
    const uint8_t *ram=m.cpu->ram;
    // FF8 USA movie.ovl: SMJ frame number copied into the ring descriptor.
    // VLC releases its ring slot before DecDCTin consumes the expanded buffer.
    unsigned index=ram[0x1fa3c5], count=ram[0x1fa3c4];
    if (!count || count>16 || index>=count || le32(ram+0x1e8000)!=0x27bdffe0) { m.decode_valid=false; return; }
    index=(index+count-1)%count;
    unsigned frame=le16(ram+0x1fa434+12*index+6), released=le16(ram+0x1fa3c0);
    m.stream=m.ring_stream[index];
    if (!m.stream || !released || frame!=m.ring_base[index]+released-1 || m.ring_frame[index]!=frame || frame>=m.stream->frames) { m.decode_valid=false; return; }
    m.frame=frame; m.decode_valid=true; ++m.decoded;
    bool failed=false;
    m.request(m.stream,frame); m.current=m.take(m.stream,frame,failed);
    const char *status=m.current ? "ready" : failed ? "missing" : "late";
    if (m.current) ++m.replaced; else if (failed) ++m.missing; else ++m.late;
    if (m.statistics) std::fprintf(m.statistics,"%llu,%u,%u,%u,%.9f,%s\n",
        (unsigned long long)m.cpu->hw->vblanks,m.disc,m.stream->lba,frame,m.current ? m.current->seconds : -1,status);
}
namespace HostMovies {
bool span(unsigned x,unsigned y,const uint8_t *data,unsigned words,Patch &out) {
    if (!host_movies_active() || !movies->current) return false;
    auto &m=*movies;
    const uint8_t *ram=m.cpu->ram;
    unsigned buffer=le32(ram+0x1fa3e4)&0x1fffff;
    unsigned width=le16(ram+0x1fa3ec), height=le16(ram+0x1fa3ee);
    bool rgb24=width==24;
    if ((width!=16 && width!=24) || height!=224 || words!=width) return false;
    uintptr_t source=reinterpret_cast<uintptr_t>(data), begin=reinterpret_cast<uintptr_t>(ram)+buffer;
    if (buffer>RAM_SIZE-width*height*2 || source<begin || source>=begin+width*height*2 || (source-begin)%(width*2)) return false;
    unsigned row=unsigned(source-begin)/(width*2);
    unsigned ox=le16(ram+0x1fa3f0), oy=le16(ram+0x1fa3f2);
    unsigned full_width=rgb24 ? 480 : 320;
    if (x<ox || x+words>ox+full_width || y!=oy+row || ox+full_width>1024 || oy+224>512) return false;
    out={x,y,words,1,ox,oy,rgb24,m.current}; ++m.strips;
    return true;
}
}
extern "C" int host_movies_active(void) {
    return movies && movies->cpu && movies->decode_valid &&
        (movies->cpu->ram[0x1fa3ca] || movies->cpu->ram[0x1fa3cb]);
}
extern "C" void host_movies_stop(void) { movies.reset(); }
