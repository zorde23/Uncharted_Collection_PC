/* uc3: libSceVideodec2 (Uncharted 3 cutscenes): H.264 access units decoded by FFmpeg into the
 * game's NV12 frame buffers, as runtime_videodec.c does for libSceVideodec. Structures, sizes,
 * error codes and the picture-info placement follow shadPS4's videodec2 (GPL-2.0-or-later,
 * commit b4e7ae7): the AVC picture info of a frame is kept right after its pixels, in the
 * padding QueryDecoderMemoryInfo adds to the frame buffer size, and GetPictureInfo copies it.
 * Unlike there, a picture that does not fit the game's buffer is an error, never an overflow. */
#include "runtime.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#define ERR_API_FAIL ((int32_t)0x811D0100)
#define ERR_STRUCT_SIZE ((int32_t)0x811D0101)
#define ERR_ARGUMENT_POINTER ((int32_t)0x811D0102)
#define ERR_DECODER_INSTANCE ((int32_t)0x811D0103)
#define ERR_MEMORY_POINTER ((int32_t)0x811D0105)
#define ERR_FRAME_BUFFER_SIZE ((int32_t)0x811D0106)
#define ERR_FRAME_BUFFER_POINTER ((int32_t)0x811D0107)
#define ERR_ACCESS_UNIT_SIZE ((int32_t)0x811D010D)
#define ERR_ACCESS_UNIT_POINTER ((int32_t)0x811D010E)
#define ERR_CONFIG_INFO ((int32_t)0x811D0200)
#define ERR_COMPUTE_PIPE_ID ((int32_t)0x811D0201)
#define ERR_COMPUTE_QUEUE_ID ((int32_t)0x811D0202)
#define ERR_CODEC_TYPE ((int32_t)0x811D0204)
#define CODEC_AVC 1
#define MINIMUM_MEMORY (16u << 20)

typedef struct {
    uint64_t this_size;
    uint32_t resource_type, codec_type, profile, max_level;
    int32_t max_frame_width, max_frame_height, max_dpb_frame_count;
    uint32_t decode_pipeline_depth;
    void *compute_queue;
    uint64_t cpu_affinity_mask;
    int32_t cpu_thread_priority;
    bool optimize_progressive_video, check_memory_type;
    uint8_t reserved0, reserved1;
    void *extra_config_info;
} ConfigInfo;
typedef struct {
    uint64_t this_size, cpu_memory_size;
    void *cpu_memory;
    uint64_t gpu_memory_size;
    void *gpu_memory;
    uint64_t cpu_gpu_memory_size;
    void *cpu_gpu_memory;
    uint64_t max_frame_buffer_size;
    uint32_t frame_buffer_alignment, reserved0;
} MemoryInfo;
typedef struct { uint64_t this_size; void *au_data; uint64_t au_size, pts_data, dts_data, attached_data; } InputData;
typedef struct {
    uint64_t this_size;
    bool is_valid, is_error_frame;
    uint8_t picture_count;
    uint32_t codec_type, frame_width, frame_pitch, frame_height;
    void *frame_buffer;
    uint64_t frame_buffer_size;
    uint32_t frame_format, frame_pitch_in_bytes; /* newer SDKs only (this_size 0x38) */
} OutputInfo;
typedef struct { uint64_t this_size; void *frame_buffer; uint64_t frame_buffer_size; bool is_accepted; } FrameBuffer;
typedef struct { uint64_t this_size, cpu_gpu_memory_size; void *cpu_gpu_memory; } ComputeMemoryInfo;
typedef struct { uint64_t this_size; uint16_t compute_pipe_id, compute_queue_id; bool check_memory_type; uint8_t reserved0; uint16_t reserved1; } ComputeConfigInfo;
typedef struct {
    uint64_t this_size;
    bool is_valid;
    uint64_t pts_data, dts_data, attached_data;
    uint8_t idr_pictureflag, profile_idc, level_idc;
    uint32_t pic_width_in_mbs_minus1, pic_height_in_map_units_minus1;
    uint8_t frame_mbs_only_flag, frame_cropping_flag;
    uint32_t frame_crop_left_offset, frame_crop_right_offset, frame_crop_top_offset, frame_crop_bottom_offset;
    uint8_t aspect_ratio_info_present_flag, aspect_ratio_idc;
    uint16_t sar_width, sar_height;
    uint8_t video_signal_type_present_flag, video_format, video_full_range_flag, colour_description_present_flag,
            colour_primaries, transfer_characteristics, matrix_coefficients, timing_info_present_flag;
    uint32_t num_units_in_tick, time_scale;
    uint8_t fixed_frame_rate_flag, bitstream_restriction_flag, max_dec_frame_buffering, pic_struct_present_flag,
            pic_struct, field_pic_flag, bottom_field_flag, sequence_parameter_set_present_flag,
            picture_parameter_set_present_flag, au_delimiter_present_flag, end_of_sequence_present_flag,
            end_of_stream_present_flag, filler_data_present_flag, picture_timing_sei_present_flag,
            buffering_period_sei_present_flag, constraint_set0_flag, constraint_set1_flag, constraint_set2_flag,
            constraint_set3_flag, constraint_set4_flag, constraint_set5_flag;
} AvcPictureInfo;
_Static_assert(sizeof(ConfigInfo)==0x48 && sizeof(MemoryInfo)==0x48 && sizeof(InputData)==0x30 &&
               sizeof(OutputInfo)==0x38 && sizeof(FrameBuffer)==0x20 && sizeof(ComputeMemoryInfo)==0x18 &&
               sizeof(ComputeConfigInfo)==0x10 && sizeof(AvcPictureInfo)==0x78, "Videodec2 guest layouts");
/* OutputInfo of older SDKs ends before frame_format (0x30). */
static int output_size_ok(const OutputInfo *o) { return (o->this_size|8)==sizeof(OutputInfo); }

typedef struct {
    AVCodecContext *codec; AVPacket *packet;
    AVFrame *frame;      /* the picture being delivered */
    AVFrame *nv12;       /* conversion target for formats other than NV12/YUV420P, kept across pictures */
    struct SwsContext *sws;
    AVFrame *queued[8];  /* pictures set aside when the decoder had more than one ready (see set_aside) */
    int queued_count;
} Decoder;
static uint64_t decoded_frames;

static uint32_t align(uint32_t value, uint32_t to) { return (value+to-1)&~(to-1); }

static ABI int32_t query_compute_memory(ComputeMemoryInfo *info) {
    if (!info) return ERR_ARGUMENT_POINTER;
    if (info->this_size!=sizeof(*info)) return ERR_STRUCT_SIZE;
    info->cpu_gpu_memory=NULL; info->cpu_gpu_memory_size=MINIMUM_MEMORY;
    return 0;
}
/* Decoding runs on the CPU: the queue is a token inside the memory the game supplied. */
static ABI int32_t allocate_compute_queue(const ComputeConfigInfo *config, const ComputeMemoryInfo *memory, void **queue) {
    if (!config || !memory || !queue) return ERR_ARGUMENT_POINTER;
    if (config->this_size!=sizeof(*config) || memory->this_size!=sizeof(*memory)) return ERR_STRUCT_SIZE;
    if (config->reserved0 || config->reserved1) return ERR_CONFIG_INFO;
    if (config->compute_pipe_id>4) return ERR_COMPUTE_PIPE_ID;
    if (config->compute_queue_id>7) return ERR_COMPUTE_QUEUE_ID;
    if (!memory->cpu_gpu_memory) return ERR_MEMORY_POINTER;
    *queue=memory->cpu_gpu_memory;
    return 0;
}
static ABI int32_t release_compute_queue(void *queue) { (void)queue; return 0; }

static ABI int32_t query_decoder_memory(const ConfigInfo *config, MemoryInfo *out) {
    if (!config || !out) return ERR_ARGUMENT_POINTER;
    if (config->this_size!=sizeof(*config) || out->this_size!=sizeof(*out)) return ERR_STRUCT_SIZE;
    int32_t width=config->max_frame_width, height=config->max_frame_height;
    if (width<=0 || height<=0) { width=config->max_level>=150 ? 3840 : 1920; height=config->max_level>=150 ? 2160 : 1080; }
    uint64_t frame=(uint64_t)align((uint32_t)width,64)*align((uint32_t)height,16)*3/2;
    out->cpu_memory=out->gpu_memory=out->cpu_gpu_memory=NULL;
    out->cpu_memory_size=out->gpu_memory_size=out->cpu_gpu_memory_size=MINIMUM_MEMORY;
    out->max_frame_buffer_size=((frame+255)&~UINT64_C(255))+0x4000; /* room for the picture info */
    out->frame_buffer_alignment=0x100;
    return 0;
}
static ABI int32_t create_decoder(const ConfigInfo *config, const MemoryInfo *memory, Decoder **out) {
    if (!config || !memory || !out) return ERR_ARGUMENT_POINTER;
    if (config->this_size!=sizeof(*config) || memory->this_size!=sizeof(*memory)) return ERR_STRUCT_SIZE;
    if (config->codec_type!=CODEC_AVC) {
        printf("Runtime: Videodec2 codec type %u unsupported (H.264 only)\n",config->codec_type);
        return ERR_CODEC_TYPE;
    }
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);
    Decoder *d=calloc(1,sizeof(*d));
    if (!codec || !d) { free(d); return ERR_API_FAIL; }
    d->codec=avcodec_alloc_context3(codec);
    d->packet=av_packet_alloc();
    d->frame=av_frame_alloc();
    d->nv12=av_frame_alloc();
    if (d->codec) d->codec->flags|=AV_CODEC_FLAG_COPY_OPAQUE; /* attached data follows its picture */
    if (!d->codec || !d->packet || !d->frame || !d->nv12 || avcodec_open2(d->codec,codec,NULL)<0) {
        avcodec_free_context(&d->codec); av_packet_free(&d->packet); av_frame_free(&d->frame); av_frame_free(&d->nv12);
        free(d); return ERR_API_FAIL;
    }
    printf("Runtime: Videodec2 H.264 decoder created (up to %dx%d)\n",config->max_frame_width,config->max_frame_height);
    *out=d;
    return 0;
}
static void drop_queued(Decoder *d) {
    while (d->queued_count) av_frame_free(&d->queued[--d->queued_count]);
}
static ABI int32_t delete_decoder(Decoder *d) {
    if (!d) return ERR_DECODER_INSTANCE;
    drop_queued(d);
    avcodec_free_context(&d->codec); av_packet_free(&d->packet); av_frame_free(&d->frame); av_frame_free(&d->nv12);
    sws_freeContext(d->sws);
    free(d);
    return 0;
}

/* The decoded picture into the game's buffer: luma, then interleaved chroma, a pitch of the width
 * aligned to 64 and the height aligned to 16 (the last rows repeated), then its picture info. */
static int32_t picture(Decoder *d, FrameBuffer *buffer, OutputInfo *out) {
    AVFrame *f=d->frame;
    /* The software H.264 decoder gives planar YUV 4:2:0: its chroma is interleaved below while
     * copying, with no conversion pass. Any other format goes through swscale into an NV12
     * frame kept from one picture to the next. */
    const int planar=f->format==AV_PIX_FMT_YUV420P || f->format==AV_PIX_FMT_YUVJ420P;
    if (f->format!=AV_PIX_FMT_NV12 && !planar) {
        if (!d->nv12->data[0] || d->nv12->width!=f->width || d->nv12->height!=f->height) {
            av_frame_unref(d->nv12);
            d->nv12->format=AV_PIX_FMT_NV12; d->nv12->width=f->width; d->nv12->height=f->height;
            if (av_frame_get_buffer(d->nv12,0)<0) return ERR_API_FAIL;
        }
        d->sws=sws_getCachedContext(d->sws,f->width,f->height,(enum AVPixelFormat)f->format,
                                    f->width,f->height,AV_PIX_FMT_NV12,SWS_FAST_BILINEAR,NULL,NULL,NULL);
        if (!d->sws ||
            sws_scale(d->sws,(const uint8_t *const *)f->data,f->linesize,0,f->height,d->nv12->data,d->nv12->linesize)<0)
            return ERR_API_FAIL;
        f=d->nv12;
    }
    uint32_t width=(uint32_t)f->width, height=(uint32_t)f->height;
    uint32_t pitch=align(width,64), rows=align(height,16);
    uint64_t size=(uint64_t)pitch*rows*3/2;
    if (size+sizeof(AvcPictureInfo)>buffer->frame_buffer_size) return ERR_FRAME_BUFFER_SIZE;
    unsigned char *luma=buffer->frame_buffer, *chroma=luma+(size_t)pitch*rows;
    for (uint32_t y=0;y<rows;++y) {
        uint32_t from=y<height ? y : height-1;
        memcpy(luma+(size_t)y*pitch,f->data[0]+(size_t)from*(size_t)f->linesize[0],width);
        if (y<rows/2) {
            uint32_t c=y<height/2 ? y : height/2-1;
            unsigned char *row=chroma+(size_t)y*pitch;
            if (!planar) memcpy(row,f->data[1]+(size_t)c*(size_t)f->linesize[1],width);
            else {
                const unsigned char *u=f->data[1]+(size_t)c*(size_t)f->linesize[1], *v=f->data[2]+(size_t)c*(size_t)f->linesize[2];
                for (uint32_t x=0;x<width/2;++x) { row[2*x]=u[x]; row[2*x+1]=v[x]; }
                if (width&1) row[width-1]=u[width/2];
            }
        }
    }
    buffer->is_accepted=true;
    out->is_valid=true; out->is_error_frame=false; out->picture_count=1;
    out->codec_type=CODEC_AVC;
    out->frame_width=align(width,16); out->frame_pitch=pitch; out->frame_height=rows;
    out->frame_buffer=buffer->frame_buffer; out->frame_buffer_size=size;
    if (out->this_size==sizeof(OutputInfo)) { out->frame_format=0; out->frame_pitch_in_bytes=pitch; }
    AvcPictureInfo info;
    memset(&info,0,sizeof(info));
    info.this_size=sizeof(info);
    info.is_valid=true;
    info.pts_data=(uint64_t)d->frame->pts;
    info.dts_data=(uint64_t)(d->frame->pkt_dts<0 ? 0 : d->frame->pkt_dts);
    info.attached_data=(uint64_t)(uintptr_t)d->frame->opaque;
    info.frame_cropping_flag=pitch!=width || rows!=height;
    info.frame_crop_right_offset=pitch-width;
    info.frame_crop_bottom_offset=rows-height;
    info.colour_primaries=(uint8_t)d->frame->color_primaries;
    memcpy(luma+size,&info,sizeof(info));
    /* uc3: GOW3_VIDEO_INVALIDATE=1 tells the texture cache that the picture changed, as shadPS4's
     * AvPlayer does after each frame. A friend's UC2 (RTX 3070, 1440p, the texture cache over
     * budget) once kept showing one picture of a video while its sound went on: the write faults
     * alone likely stopped marking the video image dirty after evictions (not reproduced). */
    static int invalidate=-1;
    if (invalidate<0) { const char *e=getenv("GOW3_VIDEO_INVALIDATE"); invalidate=e && e[0]=='1'; }
    if (invalidate) runtime_memory_invalidate((uintptr_t)luma,size+sizeof(info));
    if (!decoded_frames++) printf("Runtime: Videodec2 first picture %ux%u (pitch %u), game buffer %p, %llu bytes\n",width,height,pitch,
                                  buffer->frame_buffer,(unsigned long long)buffer->frame_buffer_size);
    return 0;
}
static int32_t receive(Decoder *d, FrameBuffer *buffer, OutputInfo *out) {
    av_frame_unref(d->frame);
    if (d->queued_count) { /* pictures set aside come out first, in order */
        av_frame_move_ref(d->frame,d->queued[0]);
        av_frame_free(&d->queued[0]);
        memmove(d->queued,d->queued+1,(size_t)--d->queued_count*sizeof(*d->queued));
        return picture(d,buffer,out);
    }
    int r=avcodec_receive_frame(d->codec,d->frame);
    if (r==AVERROR(EAGAIN) || r==AVERROR_EOF) return 0;
    return r<0 ? ERR_API_FAIL : picture(d,buffer,out);
}
/* After avcodec_send_packet refused an access unit (EAGAIN: pictures are waiting to be taken,
 * an access unit that produced two), sets them aside so the unit is accepted; 0 when it can
 * be sent again. One picture still leaves per call. */
static int set_aside(Decoder *d) {
    while (d->queued_count<(int)(sizeof(d->queued)/sizeof(*d->queued))) {
        AVFrame *f=av_frame_alloc();
        if (!f) return -1;
        int r=avcodec_receive_frame(d->codec,f);
        if (r<0) { av_frame_free(&f); return r==AVERROR(EAGAIN) || r==AVERROR_EOF ? 0 : -1; }
        d->queued[d->queued_count++]=f;
    }
    return 0;
}
static int32_t check(Decoder *d, FrameBuffer *buffer, OutputInfo *out) {
    if (!d) return ERR_DECODER_INSTANCE;
    if (!buffer || !out) return ERR_ARGUMENT_POINTER;
    if (buffer->this_size!=sizeof(*buffer) || !output_size_ok(out)) return ERR_STRUCT_SIZE;
    if (!buffer->frame_buffer) return ERR_FRAME_BUFFER_POINTER;
    buffer->is_accepted=false;
    out->is_valid=false; out->picture_count=0;
    return 0;
}
static ABI int32_t decode(Decoder *d, const InputData *input, FrameBuffer *buffer, OutputInfo *out) {
    if (!d) return ERR_DECODER_INSTANCE;
    if (!input) return ERR_ARGUMENT_POINTER;
    if (input->this_size!=sizeof(*input)) return ERR_STRUCT_SIZE;
    int32_t r=check(d,buffer,out);
    if (r) return r;
    if (!input->au_data) return ERR_ACCESS_UNIT_POINTER;
    if (!input->au_size || input->au_size>INT32_MAX) return ERR_ACCESS_UNIT_SIZE;
    d->packet->data=input->au_data; d->packet->size=(int)input->au_size;
    d->packet->pts=(int64_t)input->pts_data; d->packet->dts=(int64_t)input->dts_data;
    d->packet->opaque=(void *)(uintptr_t)input->attached_data;
    int sent=avcodec_send_packet(d->codec,d->packet);
    if (sent==AVERROR(EAGAIN) && !set_aside(d)) sent=avcodec_send_packet(d->codec,d->packet);
    if (sent==AVERROR_EOF) { avcodec_flush_buffers(d->codec); sent=avcodec_send_packet(d->codec,d->packet); } /* after a flush */
    d->packet->data=NULL; d->packet->size=0;
    return sent<0 ? ERR_API_FAIL : receive(d,buffer,out);
}
/* One buffered picture per call; the game calls it until no picture comes back. */
static ABI int32_t flush(Decoder *d, FrameBuffer *buffer, OutputInfo *out) {
    int32_t r=check(d,buffer,out);
    if (r) return r;
    avcodec_send_packet(d->codec,NULL); /* AVERROR_EOF once draining: the rest still comes out */
    return receive(d,buffer,out);
}
static ABI int32_t reset(Decoder *d) {
    if (!d) return ERR_DECODER_INSTANCE;
    drop_queued(d);
    avcodec_flush_buffers(d->codec);
    return 0;
}
/* The info kept after the picture's pixels, without its size field (the caller's stays). */
static ABI int32_t get_picture_info(const OutputInfo *out, void *first, void *second) {
    (void)second; /* progressive video: one picture per frame */
    if (!out) return ERR_ARGUMENT_POINTER;
    if (!output_size_ok(out)) return ERR_STRUCT_SIZE;
    if (!out->picture_count || !first) return 0;
    uint64_t size;
    memcpy(&size,first,8);
    if (size>sizeof(AvcPictureInfo)) size=sizeof(AvcPictureInfo);
    if (size>8) memcpy((unsigned char *)first+8,(const unsigned char *)out->frame_buffer+out->frame_buffer_size+8,size-8);
    return 0;
}

static const RuntimeExport exports[]={
    {"sceVideodec2QueryComputeMemoryInfo",query_compute_memory}, {"sceVideodec2AllocateComputeQueue",allocate_compute_queue},
    {"sceVideodec2ReleaseComputeQueue",release_compute_queue}, {"sceVideodec2QueryDecoderMemoryInfo",query_decoder_memory},
    {"sceVideodec2CreateDecoder",create_decoder}, {"sceVideodec2DeleteDecoder",delete_decoder},
    {"sceVideodec2Decode",decode}, {"sceVideodec2Flush",flush}, {"sceVideodec2Reset",reset},
    {"sceVideodec2GetPictureInfo",get_picture_info},
};
uintptr_t runtime_videodec2_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
