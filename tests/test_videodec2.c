/* uc3: libSceVideodec2 (src/runtime_videodec2.c): H.264 access units to NV12 pictures, the
 * picture info after the pixels and sceVideodec2GetPictureInfo (tests/data/tiny.h264: 6 frames
 * of 64x48, as tests/test_videodec.c). Windows: ninja -C out/gpu videodec2-test. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_videodec2.c"
#include <assert.h>
#include <stdio.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
void runtime_memory_invalidate(uintptr_t address, uint64_t size) { (void)address; (void)size; }

static unsigned char frame_buffer[1<<16];
static int pictures, first_luma_sum, seen;

static void check_picture(const OutputInfo *o, const FrameBuffer *b) {
    if (!o->is_valid) { assert(!o->picture_count); return; }
    assert(b->is_accepted && o->picture_count==1 && o->codec_type==CODEC_AVC && !o->is_error_frame);
    assert(o->frame_width==64 && o->frame_pitch==64 && o->frame_height==48);
    assert(o->frame_buffer==frame_buffer && o->frame_buffer_size==64*48*3/2);
    assert(o->frame_format==0 && o->frame_pitch_in_bytes==64);
    /* The picture info as the game reads it: its own size field stays. */
    AvcPictureInfo info; memset(&info,0xEE,sizeof(info)); info.this_size=sizeof(info);
    assert(get_picture_info(o,&info,NULL)==0);
    assert(info.this_size==sizeof(info) && info.is_valid && !info.frame_crop_right_offset && !info.frame_crop_bottom_offset);
    /* B-frames: pictures come in display order, each with its own access unit's data. */
    assert(info.attached_data>=1000 && info.attached_data<1006 && !(seen>>(info.attached_data-1000)&1));
    seen|=1<<(info.attached_data-1000);
    if (!pictures++) for (int i=0;i<64*48;++i) first_luma_sum+=frame_buffer[i];
}

int main(int argc, char **argv) {
    FILE *f=fopen(argc>1 ? argv[1] : "tests/data/tiny.h264","rb");
    assert(f);
    static unsigned char stream[1<<16];
    size_t size=fread(stream,1,sizeof(stream),f);
    fclose(f);

    ComputeMemoryInfo compute={sizeof(ComputeMemoryInfo),0,NULL};
    assert(query_compute_memory(&compute)==0 && compute.cpu_gpu_memory_size);
    static unsigned char compute_memory[256];
    compute.cpu_gpu_memory=compute_memory;
    ComputeConfigInfo compute_config={sizeof(ComputeConfigInfo),0,0,false,0,0};
    void *queue=NULL;
    assert(allocate_compute_queue(&compute_config,&compute,&queue)==0 && queue==compute_memory);
    compute_config.compute_pipe_id=9;
    assert(allocate_compute_queue(&compute_config,&compute,&queue)==ERR_COMPUTE_PIPE_ID);

    ConfigInfo config; memset(&config,0,sizeof(config));
    config.this_size=sizeof(config); config.codec_type=CODEC_AVC;
    config.max_frame_width=64; config.max_frame_height=48; config.max_dpb_frame_count=4; config.compute_queue=queue;
    MemoryInfo memory; memset(&memory,0,sizeof(memory)); memory.this_size=sizeof(memory);
    assert(query_decoder_memory(&config,&memory)==0);
    assert(memory.max_frame_buffer_size>=64*48*3/2+sizeof(AvcPictureInfo) && memory.frame_buffer_alignment==0x100);
    assert(query_decoder_memory(NULL,&memory)==ERR_ARGUMENT_POINTER);
    config.this_size=8;
    assert(query_decoder_memory(&config,&memory)==ERR_STRUCT_SIZE);
    config.this_size=sizeof(config);
    config.codec_type=974921; /* HEVC */
    Decoder *d=NULL;
    assert(create_decoder(&config,&memory,&d)==ERR_CODEC_TYPE && !d);
    config.codec_type=CODEC_AVC;
    assert(create_decoder(&config,&memory,&d)==0 && d);

    FrameBuffer buffer={sizeof(FrameBuffer),frame_buffer,sizeof(frame_buffer),false};
    OutputInfo out; memset(&out,0,sizeof(out)); out.this_size=sizeof(out);

    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecParserContext *parser=av_parser_init(AV_CODEC_ID_H264);
    AVCodecContext *parse_context=avcodec_alloc_context3(codec);
    const unsigned char *p=stream;
    int units=0;
    for (int left=(int)size;;) {
        uint8_t *au; int au_size, flushing=!left;
        int used=av_parser_parse2(parser,parse_context,&au,&au_size,p,left,AV_NOPTS_VALUE,AV_NOPTS_VALUE,0);
        p+=used; left-=used;
        if (au_size) {
            InputData input={sizeof(InputData),au,(uint64_t)au_size,(uint64_t)units,(uint64_t)units,1000+(uint64_t)units};
            assert(decode(d,&input,&buffer,&out)==0);
            check_picture(&out,&buffer);
            ++units;
        }
        if (flushing && !au_size) break;
    }
    av_parser_close(parser);
    avcodec_free_context(&parse_context);
    assert(units==6);
    /* Older SDKs' OutputInfo (0x30) is accepted too. */
    out.this_size=0x30;
    for (int i=0;i<8;++i) {
        out.frame_format=0; out.frame_pitch_in_bytes=64;
        assert(flush(d,&buffer,&out)==0);
        check_picture(&out,&buffer);
    }
    assert(pictures==6 && seen==0x3f && first_luma_sum>0);
    out.this_size=sizeof(out);

    /* Errors are codes, not crashes. */
    InputData empty={sizeof(InputData),stream,0,0,0,0};
    assert(decode(d,&empty,&buffer,&out)==ERR_ACCESS_UNIT_SIZE);
    empty.au_data=NULL; empty.au_size=4;
    assert(decode(d,&empty,&buffer,&out)==ERR_ACCESS_UNIT_POINTER);
    assert(decode(NULL,&empty,&buffer,&out)==ERR_DECODER_INSTANCE);
    out.this_size=16;
    assert(get_picture_info(&out,frame_buffer,NULL)==ERR_STRUCT_SIZE);
    out.this_size=sizeof(out);
    /* A buffer with room for the pixels but not the picture info is refused. */
    FrameBuffer small={sizeof(FrameBuffer),frame_buffer,64*48*3/2,false};
    assert(reset(d)==0);
    InputData whole={sizeof(InputData),stream,(uint64_t)size,0,0,0};
    int32_t r=decode(d,&whole,&small,&out);
    assert(r==0 || r==ERR_FRAME_BUFFER_SIZE);
    assert(!(r==0 && out.is_valid) && !small.is_accepted);

    assert(delete_decoder(d)==0);
    assert(delete_decoder(NULL)==ERR_DECODER_INSTANCE);
    assert(release_compute_queue(queue)==0);
    puts("videodec2 test: ok");
    return 0;
}
