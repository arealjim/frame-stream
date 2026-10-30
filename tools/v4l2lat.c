// v4l2lat: how the Frame's hevc_v4l2m2m decoder labels and delays its output.
// For each access unit i (pts = i*11111 us): send, receive immediately (the client's pattern),
// then poll receive every 1 ms for up to POLL_MS without sending more. Prints when each frame
// comes out, with which pts, relative to the submits.
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
static int64_t now_us(void){ return av_gettime_relative(); }
int main(int argc, char** argv){
    const char* path = argc>1?argv[1]:"/tmp/test.hevc";
    int poll_ms = argc>2?atoi(argv[2]):40;
    int set_tb = argc>3?atoi(argv[3]):0;
    int nframes = argc>4?atoi(argv[4]):60;
    AVFormatContext* fmt=NULL; if(avformat_open_input(&fmt,path,NULL,NULL)<0){fprintf(stderr,"open fail\n");return 1;}
    const AVCodec* dec=avcodec_find_decoder_by_name("hevc_v4l2m2m"); if(!dec){fprintf(stderr,"no hevc_v4l2m2m\n");return 1;}
    AVCodecContext* c=avcodec_alloc_context3(dec);
    c->flags|=AV_CODEC_FLAG_LOW_DELAY; c->thread_count=1; c->width=c->coded_width=2752; c->height=c->coded_height=1216;
    if(set_tb){ c->time_base=(AVRational){1,1000000}; c->pkt_timebase=(AVRational){1,1000000}; }
    if(avcodec_open2(c,dec,NULL)<0){fprintf(stderr,"open2 fail\n");return 1;}
    printf("decoder %s, time_base %s, poll %d ms after each submit\n", dec->name, set_tb?"1/1000000":"unset", poll_ms);
    AVPacket* pkt=av_packet_alloc(); AVFrame* fr=av_frame_alloc();
    int i=0, out=0; int64_t t_start=now_us();
    while(i<nframes && av_read_frame(fmt,pkt)>=0){
        pkt->pts = (int64_t)i*11111; pkt->dts=pkt->pts;
        int64_t ts=now_us();
        int r=avcodec_send_packet(c,pkt);
        if(r<0){ printf("send %d: error %d\n",i,r); av_packet_unref(pkt); i++; continue; }
        int got_immediate=0;
        for(;;){ r=avcodec_receive_frame(c,fr); if(r<0)break;
            printf("submit %2d: frame %2d out IMMEDIATELY (+%.1f ms) pts=%lld\n", i, out, (now_us()-ts)/1000.0, (long long)fr->pts); out++; got_immediate=1; av_frame_unref(fr); }
        int64_t t0=now_us(); int got_poll=0;
        while((now_us()-t0)<poll_ms*1000){
            r=avcodec_receive_frame(c,fr);
            if(r>=0){ printf("submit %2d: frame %2d out after POLL (+%.1f ms after submit) pts=%lld\n", i, out, (now_us()-ts)/1000.0, (long long)fr->pts); out++; got_poll=1; av_frame_unref(fr); continue; }
            usleep(1000);
        }
        if(!got_immediate && !got_poll) printf("submit %2d: nothing out within %d ms\n", i, poll_ms);
        av_packet_unref(pkt); i++;
    }
    printf("submitted %d, out %d, held by the decoder at the end: %d  (%.0f ms total)\n", i, out, i-out, (now_us()-t_start)/1000.0);
    return 0;
}
