/*
 * RoadLapse - Speed up and stabilize your journey videos
 * Copyright (C) 2025  Maciej Bliziński
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 *
 *  This program uses FFmpeg libraries, which are licensed under LGPL 2.1+.
 *  Video stabilization features require libvidstab.
 */

#define _GNU_SOURCE  // For mkstemps
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

typedef struct {
    AVFormatContext *input_fmt_ctx;
    AVFormatContext *output_fmt_ctx;
    AVCodecContext *decoder_ctx;
    AVCodecContext *encoder_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterContext *buffersink_ctx;
    AVFilterGraph *filter_graph;
    int video_stream_index;
} RoadlapseContext;

// Progress callback for mobile apps
typedef void (*ProgressCallback)(float progress, void *user_data);

// Helper function to get input video framerate
double get_input_framerate(AVFormatContext *fmt_ctx, int video_stream_index) {
    AVStream *video_stream = fmt_ctx->streams[video_stream_index];

    if (video_stream->r_frame_rate.den != 0 && video_stream->r_frame_rate.num > 0) {
        return (double)video_stream->r_frame_rate.num / video_stream->r_frame_rate.den;
    }

    if (video_stream->avg_frame_rate.den != 0 && video_stream->avg_frame_rate.num > 0) {
        return (double)video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den;
    }

    // Fallback
    return 25.0;
}

// Determine optimal output framerate based on input framerate
int get_optimal_framerate(double input_fps) {
    int fps_rounded = (int)(input_fps + 0.5);  // Round to nearest integer

    if (fps_rounded <= 25) {
        return 50;  // 25fps or less -> 50fps
    } else if (fps_rounded <= 30) {
        return 60;  // ~30fps -> 60fps
    } else {
        return fps_rounded;  // 50fps+ -> keep original
    }
}

// Build parallel stream speedup filter (like our shell script)
void build_parallel_speedup_filter(char *filter_desc, size_t filter_desc_size,
                                   double input_fps, int speedup_degree) {

    int target_fps = get_optimal_framerate(input_fps);
    int factor = 1 << speedup_degree;  // 2^speedup_degree

    printf("DEBUG: Input framerate: %.2f fps, Target framerate: %d fps\n", input_fps, target_fps);
    printf("DEBUG: Speedup degree: %d, Factor: %d\n", speedup_degree, factor);

    // For small factors, try the complex approach. For testing, let's try simple tmix first
    if (factor <= 8) {
        // Use the parallel stream approach
        // Check if we have enough space for the filter description
        size_t estimated_size = factor * 100 + 1000;
        if (estimated_size > filter_desc_size) {
            fprintf(stderr, "ERROR: Filter description buffer too small. Need ~%zu bytes, have %zu\n",
                    estimated_size, filter_desc_size);
            snprintf(filter_desc, filter_desc_size, "scale=iw:ih");  // Fallback to passthrough
            return;
        }

        // Start building the filter description
        int pos = 0;

        // Split into parallel streams: [in]split=N[s0][s1]...[sN-1];
        pos += snprintf(filter_desc + pos, filter_desc_size - pos, "[in]split=%d", factor);

        for (int i = 0; i < factor; i++) {
            pos += snprintf(filter_desc + pos, filter_desc_size - pos, "[s%d]", i);
        }
        pos += snprintf(filter_desc + pos, filter_desc_size - pos, ";");

        // Generate select filters for each stream: [s0]select='not(mod(n-0,factor))',setpts=N/TB[stream0];
        for (int i = 0; i < factor; i++) {
            pos += snprintf(filter_desc + pos, filter_desc_size - pos,
                           "[s%d]select='not(mod(n-%d,%d))',setpts=N/TB[stream%d];",
                           i, i, factor, i);

            // Safety check to prevent buffer overflow
            if (pos >= filter_desc_size - 200) {
                fprintf(stderr, "ERROR: Filter description buffer overflow during select generation\n");
                snprintf(filter_desc, filter_desc_size, "scale=iw:ih");  // Fallback
                return;
            }
        }

        // Mix all streams and connect to output sink
        for (int i = 0; i < factor; i++) {
            pos += snprintf(filter_desc + pos, filter_desc_size - pos, "[stream%d]", i);
        }

        // Label the final output as "out" to connect to the sink
        pos += snprintf(filter_desc + pos, filter_desc_size - pos, "mix=inputs=%d[out]", factor);

        printf("DEBUG: Using parallel stream mixing for %dx speedup with motion blur\n", factor);
        printf("DEBUG: Filter description length: %d characters\n", pos);

        // Print the actual filter to debug syntax issues
        printf("DEBUG: Complete filter: %s\n", filter_desc);
    } else {
        // For large factors, fall back to simpler tmix approach for now
        printf("DEBUG: Factor %d too large for parallel streams, using simple tmix\n", factor);
        snprintf(filter_desc, filter_desc_size,
                "tmix=frames=%d,setpts=PTS/%d", factor, factor);
        printf("DEBUG: Using simple tmix fallback: %s\n", filter_desc);
    }
}

int setup_decoder(RoadlapseContext *ctx, const char *input_file) {
    int ret;

    // Open input file
    if ((ret = avformat_open_input(&ctx->input_fmt_ctx, input_file, NULL, NULL)) < 0) {
        fprintf(stderr, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ctx->input_fmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Cannot find stream information\n");
        return ret;
    }

    // Find video stream
    for (int i = 0; i < ctx->input_fmt_ctx->nb_streams; i++) {
        if (ctx->input_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ctx->video_stream_index = i;
            break;
        }
    }

    if (ctx->video_stream_index == -1) {
        fprintf(stderr, "Cannot find video stream\n");
        return AVERROR(EINVAL);
    }

    // Setup decoder
    AVCodecParameters *codecpar = ctx->input_fmt_ctx->streams[ctx->video_stream_index]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) {
        fprintf(stderr, "Failed to find decoder\n");
        return AVERROR(EINVAL);
    }

    ctx->decoder_ctx = avcodec_alloc_context3(decoder);
    if (!ctx->decoder_ctx) {
        return AVERROR(ENOMEM);
    }

    if ((ret = avcodec_parameters_to_context(ctx->decoder_ctx, codecpar)) < 0) {
        return ret;
    }

    if ((ret = avcodec_open2(ctx->decoder_ctx, decoder, NULL)) < 0) {
        fprintf(stderr, "Failed to open decoder\n");
        return ret;
    }

    return 0;
}

int setup_encoder(RoadlapseContext *ctx, const char *output_file, double input_fps) {
    int ret;
    int target_fps = get_optimal_framerate(input_fps);

    // Allocate output format context
    avformat_alloc_output_context2(&ctx->output_fmt_ctx, NULL, NULL, output_file);
    if (!ctx->output_fmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }

    // Find H.265 encoder
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    if (!encoder) {
        fprintf(stderr, "H.265 encoder not found\n");
        return AVERROR(EINVAL);
    }

    // Setup encoder context
    ctx->encoder_ctx = avcodec_alloc_context3(encoder);
    if (!ctx->encoder_ctx) {
        return AVERROR(ENOMEM);
    }

    // Configure encoder (matching your script settings)
    ctx->encoder_ctx->codec_id = AV_CODEC_ID_HEVC;
    ctx->encoder_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->encoder_ctx->width = ctx->decoder_ctx->width;
    ctx->encoder_ctx->height = ctx->decoder_ctx->height;

    // FIXED: Set both time_base AND framerate explicitly
    ctx->encoder_ctx->time_base = (AVRational){1, target_fps};
    ctx->encoder_ctx->framerate = (AVRational){target_fps, 1};  // <-- ADD THIS LINE

    ctx->encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->encoder_ctx->bit_rate = 5000000; // 5 Mbps

    printf("DEBUG: Encoder configured with time_base: %d/%d, framerate: %d/%d\n",
           ctx->encoder_ctx->time_base.num, ctx->encoder_ctx->time_base.den,
           ctx->encoder_ctx->framerate.num, ctx->encoder_ctx->framerate.den);

    // H.265 specific options (matching your script)
    av_opt_set(ctx->encoder_ctx->priv_data, "preset", "medium", 0);
    av_opt_set(ctx->encoder_ctx->priv_data, "crf", "20", 0);
    av_opt_set(ctx->encoder_ctx->priv_data, "tag:v", "hvc1", 0);

    if ((ret = avcodec_open2(ctx->encoder_ctx, encoder, NULL)) < 0) {
        fprintf(stderr, "Cannot open video encoder\n");
        return ret;
    }

    // Create output stream
    AVStream *out_stream = avformat_new_stream(ctx->output_fmt_ctx, NULL);
    if (!out_stream) {
        fprintf(stderr, "Failed allocating output stream\n");
        return AVERROR_UNKNOWN;
    }

    if ((ret = avcodec_parameters_from_context(out_stream->codecpar, ctx->encoder_ctx)) < 0) {
        fprintf(stderr, "Failed to copy encoder parameters\n");
        return ret;
    }

    // CRITICAL: Set the stream time_base and framerate metadata AFTER copying codec parameters
    // This ensures our settings aren't overridden
    out_stream->time_base = (AVRational){1, target_fps};           // Force correct time_base
    out_stream->r_frame_rate = (AVRational){target_fps, 1};        // Real framerate
    out_stream->avg_frame_rate = (AVRational){target_fps, 1};      // Average framerate

    printf("DEBUG: Final stream configuration:\n");
    printf("  time_base: %d/%d\n", out_stream->time_base.num, out_stream->time_base.den);
    printf("  r_frame_rate: %d/%d\n", out_stream->r_frame_rate.num, out_stream->r_frame_rate.den);

    if (!(ctx->output_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&ctx->output_fmt_ctx->pb, output_file, AVIO_FLAG_WRITE)) < 0) {
            fprintf(stderr, "Could not open output file '%s'\n", output_file);
            return ret;
        }
    }

    if ((ret = avformat_write_header(ctx->output_fmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        return ret;
    }

    // CRITICAL: After avformat_write_header(), the muxer may have changed our time_base
    // We need to adapt our frame timing to work with the actual time_base
    printf("DEBUG: After avformat_write_header() - actual stream configuration:\n");
    printf("  time_base: %d/%d\n", out_stream->time_base.num, out_stream->time_base.den);
    printf("  r_frame_rate: %d/%d\n", out_stream->r_frame_rate.num, out_stream->r_frame_rate.den);

    return 0;
}

int setup_filter_graph(RoadlapseContext *ctx, const char *filter_desc) {
    char args[512];
    int ret;
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();

    ctx->filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !ctx->filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    // Debug: Print decoder context values before using them
    printf("DEBUG Filter setup:\n");
    printf("  Resolution: %dx%d\n", ctx->decoder_ctx->width, ctx->decoder_ctx->height);
    printf("  Pixel format: %d (%s)\n", ctx->decoder_ctx->pix_fmt, av_get_pix_fmt_name(ctx->decoder_ctx->pix_fmt));
    printf("  Time base: %d/%d\n", ctx->decoder_ctx->time_base.num, ctx->decoder_ctx->time_base.den);
    printf("  Sample aspect ratio: %d/%d\n", ctx->decoder_ctx->sample_aspect_ratio.num, ctx->decoder_ctx->sample_aspect_ratio.den);

    // Fix invalid time_base - use stream time_base as fallback
    AVRational time_base = ctx->decoder_ctx->time_base;
    if (time_base.num == 0 || time_base.den == 0) {
        printf("  WARNING: Invalid decoder time_base, using stream time_base\n");
        time_base = ctx->input_fmt_ctx->streams[ctx->video_stream_index]->time_base;
        printf("  Stream time base: %d/%d\n", time_base.num, time_base.den);

        // If stream time_base is also invalid, use frame rate as fallback
        if (time_base.num == 0 || time_base.den == 0) {
            printf("  WARNING: Invalid stream time_base, using frame rate fallback\n");
            AVRational frame_rate = ctx->input_fmt_ctx->streams[ctx->video_stream_index]->r_frame_rate;
            if (frame_rate.num > 0 && frame_rate.den > 0) {
                time_base = av_inv_q(frame_rate); // invert frame rate to get time base
                printf("  Frame rate derived time base: %d/%d\n", time_base.num, time_base.den);
            } else {
                // Last resort: use 1/25 (25 fps)
                time_base = (AVRational){1, 25};
                printf("  Using fallback time base: %d/%d\n", time_base.num, time_base.den);
            }
        }
    }

    // Fix invalid sample_aspect_ratio
    AVRational sample_aspect_ratio = ctx->decoder_ctx->sample_aspect_ratio;
    if (sample_aspect_ratio.num == 0 || sample_aspect_ratio.den == 0) {
        printf("  WARNING: Invalid sample_aspect_ratio, using 1:1\n");
        sample_aspect_ratio = (AVRational){1, 1};
    }

    // Buffer source
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             ctx->decoder_ctx->width, ctx->decoder_ctx->height,
             ctx->decoder_ctx->pix_fmt,
             time_base.num, time_base.den,
             sample_aspect_ratio.num, sample_aspect_ratio.den);

    printf("  Fixed buffer source args: %s\n", args);

    ret = avfilter_graph_create_filter(&ctx->buffersrc_ctx, buffersrc, "in", args, NULL, ctx->filter_graph);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source: %s\n", errbuf);
        printf("ERROR: Buffer source creation failed: %s\n", errbuf);
        goto end;
    }

    printf("✓ Buffer source created successfully\n");

    // Buffer sink
    ret = avfilter_graph_create_filter(&ctx->buffersink_ctx, buffersink, "out", NULL, NULL, ctx->filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
        goto end;
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = ctx->buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = ctx->buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    printf("  Filter description: %s\n", filter_desc);

    if ((ret = avfilter_graph_parse_ptr(ctx->filter_graph, filter_desc, &inputs, &outputs, NULL)) < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        printf("ERROR: Filter graph parse failed: %s\n", errbuf);
        goto end;
    }

    if ((ret = avfilter_graph_config(ctx->filter_graph, NULL)) < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        printf("ERROR: Filter graph config failed: %s\n", errbuf);
        goto end;
    }

    printf("✓ Filter graph setup successful\n");

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

// Setup decoder for concat demuxer (multiple input files)
int setup_concat_decoder(RoadlapseContext *ctx, char **input_files, int num_files) {
    int ret;

    // Create temporary file for concat list
    char concat_file[] = "/tmp/roadlapse_concat_XXXXXX.txt";
    int fd = mkstemps(concat_file, 4);
    if (fd == -1) {
        fprintf(stderr, "Failed to create temporary concat file\n");
        return -1;
    }

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(concat_file);
        return -1;
    }

    // Write file list in concat format
    for (int i = 0; i < num_files; i++) {
        // For concat demuxer, we need to escape single quotes
        fprintf(fp, "file '");
        for (char *p = input_files[i]; *p; p++) {
            if (*p == '\'') {
                fprintf(fp, "'\\''");
            } else {
                fputc(*p, fp);
            }
        }
        fprintf(fp, "'\n");
    }
    fclose(fp);

    // Open concat demuxer with the file
    const AVInputFormat *concat_demuxer = av_find_input_format("concat");
    AVDictionary *options = NULL;
    av_dict_set(&options, "safe", "0", 0);

    if ((ret = avformat_open_input(&ctx->input_fmt_ctx, concat_file, concat_demuxer, &options)) < 0) {
        fprintf(stderr, "Cannot open concat input\n");
        av_dict_free(&options);
        unlink(concat_file);
        return ret;
    }
    av_dict_free(&options);

    // Remove the temporary file - it's no longer needed
    unlink(concat_file);

    if ((ret = avformat_find_stream_info(ctx->input_fmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Cannot find stream information\n");
        return ret;
    }

    // Find video stream
    ctx->video_stream_index = -1;
    for (int i = 0; i < ctx->input_fmt_ctx->nb_streams; i++) {
        if (ctx->input_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ctx->video_stream_index = i;
            break;
        }
    }

    if (ctx->video_stream_index == -1) {
        fprintf(stderr, "Cannot find video stream\n");
        return AVERROR(EINVAL);
    }

    // Setup decoder
    AVCodecParameters *codecpar = ctx->input_fmt_ctx->streams[ctx->video_stream_index]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) {
        fprintf(stderr, "Failed to find decoder\n");
        return AVERROR(EINVAL);
    }

    ctx->decoder_ctx = avcodec_alloc_context3(decoder);
    if (!ctx->decoder_ctx) {
        return AVERROR(ENOMEM);
    }

    if ((ret = avcodec_parameters_to_context(ctx->decoder_ctx, codecpar)) < 0) {
        return ret;
    }

    if ((ret = avcodec_open2(ctx->decoder_ctx, decoder, NULL)) < 0) {
        fprintf(stderr, "Failed to open decoder\n");
        return ret;
    }

    return 0;
}

// Step 1: Create speedup version with parallel stream processing (supports multiple inputs)
int create_speedup_version(char **input_files, int num_files, const char *output_file,
                          int speedup_degree, ProgressCallback progress_cb, void *user_data) {
    RoadlapseContext ctx = {0};
    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    int ret = 0;
    int factor = 1 << speedup_degree;  // 2^speedup_degree

    printf("=== Step 1: Creating %dx speedup ===\n", factor);

    // Setup decoder (concat for multiple files, regular for single file)
    if (num_files > 1) {
        printf("Concatenating %d input files...\n", num_files);
        if ((ret = setup_concat_decoder(&ctx, input_files, num_files)) < 0) {
            goto cleanup;
        }
    } else {
        if ((ret = setup_decoder(&ctx, input_files[0])) < 0) {
            goto cleanup;
        }
    }

    // Get input framerate to determine optimal filter strategy
    double input_fps = get_input_framerate(ctx.input_fmt_ctx, ctx.video_stream_index);

    // Setup encoder
    if ((ret = setup_encoder(&ctx, output_file, input_fps)) < 0) {
        goto cleanup;
    }

    // Build parallel stream speedup filter based on input
    char speedup_filter[50000];  // Large buffer for complex filter graphs
    build_parallel_speedup_filter(speedup_filter, sizeof(speedup_filter), input_fps, speedup_degree);

    if ((ret = setup_filter_graph(&ctx, speedup_filter)) < 0) {
        goto cleanup;
    }

    // Process frames
    long frame_count = 0;
    long output_frame_number = 0;  // Track output frame number for timestamp calculation
    long total_frames = ctx.input_fmt_ctx->streams[ctx.video_stream_index]->nb_frames;
    if (total_frames <= 0) total_frames = 10000; // Estimate if unknown

    int target_fps = get_optimal_framerate(input_fps);

    while (av_read_frame(ctx.input_fmt_ctx, packet) >= 0) {
        if (packet->stream_index == ctx.video_stream_index) {
            ret = avcodec_send_packet(ctx.decoder_ctx, packet);
            if (ret < 0) break;

            while (ret >= 0) {
                ret = avcodec_receive_frame(ctx.decoder_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) goto cleanup;

                // Push frame through filter
                if (av_buffersrc_add_frame_flags(ctx.buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
                    break;
                }

                // Get filtered frame
                while (1) {
                    ret = av_buffersink_get_frame(ctx.buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) goto cleanup;

                    // Encode frame with corrected timestamps for target framerate
                    filt_frame->pict_type = AV_PICTURE_TYPE_NONE;

                    // CRITICAL: Calculate PTS to achieve target framerate using actual stream time_base
                    // For target_fps=50, each frame should be 1/50=0.02 seconds apart
                    // PTS increment = (1.0 / target_fps) * time_base.den
                    AVStream *out_stream = ctx.output_fmt_ctx->streams[0];
                    int64_t pts_increment = (int64_t)((1.0 / target_fps) * out_stream->time_base.den);
                    filt_frame->pts = output_frame_number * pts_increment;

                    // Debug only first few frames to avoid spam
                    if (output_frame_number < 3) {
                        printf("DEBUG: Frame %ld: PTS=%lld, time_base=%d/%d, increment=%lld\n",
                               output_frame_number, filt_frame->pts,
                               out_stream->time_base.num, out_stream->time_base.den, pts_increment);
                    }

                    output_frame_number++;

                    ret = avcodec_send_frame(ctx.encoder_ctx, filt_frame);
                    if (ret < 0) break;

                    AVPacket *enc_pkt = av_packet_alloc();
                    while (ret >= 0) {
                        ret = avcodec_receive_packet(ctx.encoder_ctx, enc_pkt);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                        if (ret < 0) {
                            av_packet_free(&enc_pkt);
                            goto cleanup;
                        }

                        av_interleaved_write_frame(ctx.output_fmt_ctx, enc_pkt);
                        av_packet_unref(enc_pkt);
                    }
                    av_packet_free(&enc_pkt);
                    av_frame_unref(filt_frame);
                }

                // Progress callback for Step 1 (first third)
                frame_count++;
                if (progress_cb && total_frames > 0) {
                    float progress = (float)frame_count / (total_frames * 3); // Three steps
                    progress_cb(progress, user_data);
                }
            }
        }
        av_packet_unref(packet);
    }

    // Reset return code - main loop ended normally (EOF is expected)
    ret = 0;

    // Finish encoding
    ret = avcodec_send_frame(ctx.encoder_ctx, NULL);
    if (ret < 0) {
        printf("Warning: Error flushing encoder: %d\n", ret);
    }

    AVPacket *enc_pkt = av_packet_alloc();
    while (1) {
        ret = avcodec_receive_packet(ctx.encoder_ctx, enc_pkt);
        if (ret == AVERROR_EOF) {
            ret = 0;  // EOF is expected when flushing
            break;
        }
        if (ret == AVERROR(EAGAIN)) {
            ret = 0;  // No more packets available
            break;
        }
        if (ret < 0) {
            printf("Error receiving packet during flush: %d\n", ret);
            av_packet_free(&enc_pkt);
            goto cleanup;
        }

        ret = av_interleaved_write_frame(ctx.output_fmt_ctx, enc_pkt);
        if (ret < 0) {
            printf("Error writing frame during flush: %d\n", ret);
        }
        av_packet_unref(enc_pkt);
    }
    av_packet_free(&enc_pkt);

    ret = av_write_trailer(ctx.output_fmt_ctx);
    if (ret < 0) {
        printf("Warning: Error writing trailer: %d\n", ret);
        ret = 0;  // Don't fail the whole process for trailer issues
    }

    printf("✓ Step 1 complete: %dx effective speedup saved to %s\n", factor, output_file);
    printf("  Strategy: %.2f fps input → %d fps output using parallel stream mixing\n",
           input_fps, target_fps);

    // Debug: Check what framerate we actually created
    AVFormatContext *verify_ctx = NULL;
    if (avformat_open_input(&verify_ctx, output_file, NULL, NULL) == 0) {
        if (avformat_find_stream_info(verify_ctx, NULL) == 0) {
            for (int i = 0; i < verify_ctx->nb_streams; i++) {
                if (verify_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    AVRational r_fps = verify_ctx->streams[i]->r_frame_rate;
                    AVRational avg_fps = verify_ctx->streams[i]->avg_frame_rate;
                    AVRational tb = verify_ctx->streams[i]->time_base;

                    printf("DEBUG: Step 1 metadata check:\n");
                    printf("  r_frame_rate: %d/%d = %.2f fps\n", r_fps.num, r_fps.den,
                           r_fps.den ? (double)r_fps.num / r_fps.den : 0.0);
                    printf("  avg_frame_rate: %d/%d = %.2f fps\n", avg_fps.num, avg_fps.den,
                           avg_fps.den ? (double)avg_fps.num / avg_fps.den : 0.0);
                    printf("  time_base: %d/%d\n", tb.num, tb.den);

                    if (r_fps.den != 0 && r_fps.num > 0) {
                        double actual_fps = (double)r_fps.num / r_fps.den;
                        printf("DEBUG: Step 1 created video with framerate: %.2f fps (target was %d fps)\n", actual_fps, target_fps);
                    }
                    break;
                }
            }
        }
        avformat_close_input(&verify_ctx);
    }

    printf("DEBUG: Step 1 returning with code: %d\n", ret);

    // Verify the output file was created successfully
    if (access(output_file, R_OK) != 0) {
        fprintf(stderr, "ERROR: Output file was not created or is not readable\n");
        ret = -1;
    }

cleanup:
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    av_packet_free(&packet);

    if (ctx.decoder_ctx) avcodec_free_context(&ctx.decoder_ctx);
    if (ctx.encoder_ctx) avcodec_free_context(&ctx.encoder_ctx);
    if (ctx.input_fmt_ctx) {
        avformat_close_input(&ctx.input_fmt_ctx);
    }
    if (ctx.output_fmt_ctx) {
        if (ctx.output_fmt_ctx->pb && !(ctx.output_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ctx.output_fmt_ctx->pb);
        }
        avformat_free_context(ctx.output_fmt_ctx);
    }
    if (ctx.filter_graph) avfilter_graph_free(&ctx.filter_graph);

    return ret;
}

// Step 2: Detect stabilization data on the sped-up video
int detect_stabilization(const char *speedup_file, const char *trf_file,
                        ProgressCallback progress_cb, void *user_data) {
    RoadlapseContext ctx = {0};
    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    int ret = 0;

    printf("\n=== Step 2: Analyzing motion for stabilization ===\n");

    // Setup decoder for the sped-up file
    if ((ret = setup_decoder(&ctx, speedup_file)) < 0) {
        goto cleanup;
    }

    // Setup filter graph for stabilization detection
    char detect_filter[512];
    snprintf(detect_filter, sizeof(detect_filter), "vidstabdetect=result=%s", trf_file);

    if ((ret = setup_filter_graph(&ctx, detect_filter)) < 0) {
        goto cleanup;
    }

    // Process frames for detection (no encoding needed)
    int64_t frame_count = 0;
    int64_t total_frames = ctx.input_fmt_ctx->streams[ctx.video_stream_index]->nb_frames;
    if (total_frames <= 0) total_frames = 10000; // Estimate if unknown

    while (av_read_frame(ctx.input_fmt_ctx, packet) >= 0) {
        if (packet->stream_index == ctx.video_stream_index) {
            ret = avcodec_send_packet(ctx.decoder_ctx, packet);
            if (ret < 0) break;

            while (ret >= 0) {
                ret = avcodec_receive_frame(ctx.decoder_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) goto cleanup;

                // Push frame through detection filter
                if (av_buffersrc_add_frame_flags(ctx.buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while feeding the detection filtergraph\n");
                    break;
                }

                // Get filtered frame (just for processing, we don't use the output)
                while (1) {
                    ret = av_buffersink_get_frame(ctx.buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) goto cleanup;
                    av_frame_unref(filt_frame);
                }

                // Progress callback for Step 2 (second third)
                frame_count++;
                if (progress_cb && total_frames > 0) {
                    float progress = (1.0f/3.0f) + ((float)frame_count / (total_frames * 3));
                    progress_cb(progress, user_data);
                }
            }
        }
        av_packet_unref(packet);
    }

    // Reset return code - main loop ended normally (EOF is expected)
    ret = 0;

    printf("✓ Step 2 complete: Stabilization data saved to %s\n", trf_file);
    printf("DEBUG: Step 2 returning with code: %d\n", ret);

cleanup:
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    av_packet_free(&packet);

    if (ctx.decoder_ctx) avcodec_free_context(&ctx.decoder_ctx);
    if (ctx.input_fmt_ctx) avformat_close_input(&ctx.input_fmt_ctx);
    if (ctx.filter_graph) avfilter_graph_free(&ctx.filter_graph);

    return ret;
}

// Step 3: Apply stabilization to the sped-up video
int apply_stabilization(const char *speedup_file, const char *output_file, const char *trf_file,
                       ProgressCallback progress_cb, void *user_data) {
    RoadlapseContext ctx = {0};
    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    int ret = 0;

    printf("\n=== Step 3: Applying stabilization ===\n");

    // Setup decoder for the sped-up file
    if ((ret = setup_decoder(&ctx, speedup_file)) < 0) {
        goto cleanup;
    }

    // Get framerate from the speedup file
    double speedup_fps = get_input_framerate(ctx.input_fmt_ctx, ctx.video_stream_index);
    int target_fps = (int)(speedup_fps + 0.5);  // Round to nearest int

    printf("DEBUG: Step 3 using framerate: %d fps\n", target_fps);

    // Setup encoder for final output
    if ((ret = setup_encoder(&ctx, output_file, speedup_fps)) < 0) {
        goto cleanup;
    }

    // Setup filter graph for stabilization transform
    char stabilize_filter[512];
    snprintf(stabilize_filter, sizeof(stabilize_filter),
             "vidstabtransform=input=%s:smoothing=40:crop=keep:zoom=0:optzoom=0:optalgo=avg:relative=1",
             trf_file);

    if ((ret = setup_filter_graph(&ctx, stabilize_filter)) < 0) {
        goto cleanup;
    }

    // Process frames for stabilization
    int64_t frame_count = 0;
    int64_t output_frame_number = 0;  // Track output frame number for timestamp calculation
    int64_t total_frames = ctx.input_fmt_ctx->streams[ctx.video_stream_index]->nb_frames;
    if (total_frames <= 0) total_frames = 10000; // Estimate if unknown

    while (av_read_frame(ctx.input_fmt_ctx, packet) >= 0) {
        if (packet->stream_index == ctx.video_stream_index) {
            ret = avcodec_send_packet(ctx.decoder_ctx, packet);
            if (ret < 0) break;

            while (ret >= 0) {
                ret = avcodec_receive_frame(ctx.decoder_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) goto cleanup;

                // Push frame through stabilization filter
                if (av_buffersrc_add_frame_flags(ctx.buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while feeding the stabilization filtergraph\n");
                    break;
                }

                // Get stabilized frame
                while (1) {
                    ret = av_buffersink_get_frame(ctx.buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) goto cleanup;

                    // Encode stabilized frame with corrected timestamps
                    filt_frame->pict_type = AV_PICTURE_TYPE_NONE;

                    // CRITICAL: Calculate PTS to achieve target framerate using actual stream time_base
                    AVStream *out_stream = ctx.output_fmt_ctx->streams[0];
                    int64_t pts_increment = (int64_t)((1.0 / target_fps) * out_stream->time_base.den);
                    filt_frame->pts = output_frame_number * pts_increment;

                    output_frame_number++;

                    ret = avcodec_send_frame(ctx.encoder_ctx, filt_frame);
                    if (ret < 0) break;

                    AVPacket *enc_pkt = av_packet_alloc();
                    while (ret >= 0) {
                        ret = avcodec_receive_packet(ctx.encoder_ctx, enc_pkt);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                        if (ret < 0) {
                            av_packet_free(&enc_pkt);
                            goto cleanup;
                        }

                        av_interleaved_write_frame(ctx.output_fmt_ctx, enc_pkt);
                        av_packet_unref(enc_pkt);
                    }
                    av_packet_free(&enc_pkt);
                    av_frame_unref(filt_frame);
                }

                // Progress callback for Step 3 (final third)
                frame_count++;
                if (progress_cb && total_frames > 0) {
                    float progress = (2.0f/3.0f) + ((float)frame_count / (total_frames * 3));
                    progress_cb(progress, user_data);
                }
            }
        }
        av_packet_unref(packet);
    }

    // Reset return code - main loop ended normally (EOF is expected)
    ret = 0;

    // Finish encoding
    ret = avcodec_send_frame(ctx.encoder_ctx, NULL);
    if (ret < 0) {
        printf("Warning: Error flushing encoder: %d\n", ret);
    }

    AVPacket *enc_pkt = av_packet_alloc();
    while (1) {
        ret = avcodec_receive_packet(ctx.encoder_ctx, enc_pkt);
        if (ret == AVERROR_EOF) {
            ret = 0;  // EOF is expected when flushing
            break;
        }
        if (ret == AVERROR(EAGAIN)) {
            ret = 0;  // No more packets available
            break;
        }
        if (ret < 0) {
            printf("Error receiving packet during flush: %d\n", ret);
            av_packet_free(&enc_pkt);
            goto cleanup;
        }

        ret = av_interleaved_write_frame(ctx.output_fmt_ctx, enc_pkt);
        if (ret < 0) {
            printf("Error writing frame during flush: %d\n", ret);
        }
        av_packet_unref(enc_pkt);
    }
    av_packet_free(&enc_pkt);

    ret = av_write_trailer(ctx.output_fmt_ctx);
    if (ret < 0) {
        printf("Warning: Error writing trailer: %d\n", ret);
        ret = 0;  // Don't fail the whole process for trailer issues
    }

    printf("✓ Step 3 complete: Final stabilized output saved to %s\n", output_file);

    // Debug: Check what framerate we actually created
    AVFormatContext *verify_ctx = NULL;
    if (avformat_open_input(&verify_ctx, output_file, NULL, NULL) == 0) {
        if (avformat_find_stream_info(verify_ctx, NULL) == 0) {
            for (int i = 0; i < verify_ctx->nb_streams; i++) {
                if (verify_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    AVRational fps = verify_ctx->streams[i]->r_frame_rate;
                    if (fps.den != 0 && fps.num > 0) {
                        double actual_fps = (double)fps.num / fps.den;
                        printf("DEBUG: Step 3 created final video with framerate: %.2f fps\n", actual_fps);
                    } else {
                        // Fallback: try avg_frame_rate
                        fps = verify_ctx->streams[i]->avg_frame_rate;
                        if (fps.den != 0 && fps.num > 0) {
                            double actual_fps = (double)fps.num / fps.den;
                            printf("DEBUG: Step 3 created final video with avg framerate: %.2f fps\n", actual_fps);
                        } else {
                            printf("DEBUG: Step 3 - could not determine framerate from metadata\n");
                        }
                    }
                    break;
                }
            }
        }
        avformat_close_input(&verify_ctx);
    }

    printf("DEBUG: Step 3 returning with code: %d\n", ret);

cleanup:
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    av_packet_free(&packet);

    if (ctx.decoder_ctx) avcodec_free_context(&ctx.decoder_ctx);
    if (ctx.encoder_ctx) avcodec_free_context(&ctx.encoder_ctx);
    if (ctx.input_fmt_ctx) avformat_close_input(&ctx.input_fmt_ctx);
    if (ctx.output_fmt_ctx) {
        if (ctx.output_fmt_ctx->pb && !(ctx.output_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ctx.output_fmt_ctx->pb);
        }
        avformat_free_context(ctx.output_fmt_ctx);
    }
    if (ctx.filter_graph) avfilter_graph_free(&ctx.filter_graph);

    return ret;
}

int process_roadlapse(char **input_files, int num_files, const char *output_file,
                      int speedup_degree, ProgressCallback progress_cb, void *user_data) {
    char trf_temp[] = "/tmp/roadlapse_XXXXXX.trf";
    char shaky_temp[] = "/tmp/roadlapse_shaky_XXXXXX.mp4";
    int ret = 0;
    int factor = 1 << speedup_degree;  // 2^speedup_degree

    // Create temporary files
    int fd = mkstemps(trf_temp, 4);
    if (fd == -1) {
        fprintf(stderr, "Cannot create temporary trf file\n");
        return -1;
    }
    close(fd);

    fd = mkstemps(shaky_temp, 4);
    if (fd == -1) {
        fprintf(stderr, "Cannot create temporary shaky file\n");
        unlink(trf_temp);
        return -1;
    }
    close(fd);

    if (num_files == 1) {
        printf("Processing roadlapse: %s -> %s\n", input_files[0], output_file);
    } else {
        printf("Processing roadlapse: %d input files -> %s\n", num_files, output_file);
        for (int i = 0; i < num_files; i++) {
            printf("  %d. %s\n", i+1, input_files[i]);
        }
    }

    printf("Speedup degree: %d (factor: %dx)\n", speedup_degree, factor);

    // Detect optimal framerate from first input
    AVFormatContext *probe_ctx = NULL;
    if (avformat_open_input(&probe_ctx, input_files[0], NULL, NULL) == 0) {
        if (avformat_find_stream_info(probe_ctx, NULL) == 0) {
            for (int i = 0; i < probe_ctx->nb_streams; i++) {
                if (probe_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    double input_fps = get_input_framerate(probe_ctx, i);
                    int target_fps = get_optimal_framerate(input_fps);
                    printf("Detected input framerate: %.2f fps\n", input_fps);
                    printf("Target output framerate: %d fps\n", target_fps);
                    break;
                }
            }
        }
        avformat_close_input(&probe_ctx);
    }

    printf("Temporary files:\n");
    printf("  Shaky (%dx speedup): %s\n", factor, shaky_temp);
    printf("  Stabilization data: %s\n", trf_temp);
    printf("\n");

    // Step 1: Create speedup version with parallel stream processing
    printf("DEBUG: About to start Step 1\n");
    if ((ret = create_speedup_version(input_files, num_files, shaky_temp, speedup_degree, progress_cb, user_data)) < 0) {
        fprintf(stderr, "Step 1 failed: creating speedup version (error code: %d)\n", ret);
        ret = -1;  // Standardize error code for step 1
        goto cleanup;
    }
    printf("DEBUG: Step 1 completed successfully\n");

    // Step 2: Detect stabilization data on the sped-up video
    printf("DEBUG: About to start Step 2\n");
    if ((ret = detect_stabilization(shaky_temp, trf_temp, progress_cb, user_data)) < 0) {
        fprintf(stderr, "Step 2 failed: detecting stabilization (error code: %d)\n", ret);
        ret = -2;  // Standardize error code for step 2
        goto cleanup;
    }
    printf("DEBUG: Step 2 completed successfully\n");

    // Step 3: Apply stabilization to the sped-up video
    printf("DEBUG: About to start Step 3\n");
    if ((ret = apply_stabilization(shaky_temp, output_file, trf_temp, progress_cb, user_data)) < 0) {
        fprintf(stderr, "Step 3 failed: applying stabilization (error code: %d)\n", ret);
        ret = -3;  // Standardize error code for step 3
        goto cleanup;
    }
    printf("DEBUG: Step 3 completed successfully\n");

    // Success!
    ret = 0;
    printf("DEBUG: All steps completed, setting ret = 0\n");

    // Final progress callback
    if (progress_cb) {
        progress_cb(1.0f, user_data);
    }

    printf("\n🎉 Roadlapse processing completed successfully!\n");
    printf("DEBUG: Final return code: %d\n", ret);

cleanup:
    // Cleanup temporary files
    printf("Cleaning up temporary files...\n");
    if (unlink(trf_temp) != 0) {
        printf("Warning: Could not remove %s\n", trf_temp);
    }
    if (unlink(shaky_temp) != 0) {
        printf("Warning: Could not remove %s\n", shaky_temp);
    }

    if (ret != 0) {
        printf("Process failed at step %s\n",
               ret == -1 ? "1 (speedup)" :
               ret == -2 ? "2 (detection)" : "3 (stabilization)");
    }

    printf("DEBUG: process_roadlapse returning: %d\n", ret);

    return ret;
}

// Diagnostic function to check video file and FFmpeg capabilities
int diagnose_video_file(const char *input_file) {
    AVFormatContext *fmt_ctx = NULL;
    int ret = 0;

    printf("=== Video File Diagnostics ===\n");
    printf("File: %s\n", input_file);

    // Check if file exists and is readable
    if (access(input_file, R_OK) != 0) {
        printf("❌ File is not readable or doesn't exist\n");
        return -1;
    }
    printf("✓ File exists and is readable\n");

    // Try to open with FFmpeg
    if ((ret = avformat_open_input(&fmt_ctx, input_file, NULL, NULL)) < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        printf("❌ Cannot open with FFmpeg: %s\n", errbuf);
        return ret;
    }
    printf("✓ FFmpeg can open the file\n");

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        printf("❌ Cannot find stream information\n");
        avformat_close_input(&fmt_ctx);
        return ret;
    }
    printf("✓ Stream information found\n");

    // Find video stream
    int video_stream_idx = -1;
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx == -1) {
        printf("❌ No video stream found\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    printf("✓ Video stream found at index %d\n", video_stream_idx);

    // Print video details
    AVCodecParameters *codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
    AVStream *video_stream = fmt_ctx->streams[video_stream_idx];

    printf("\n=== Video Details ===\n");
    printf("Codec: %s\n", avcodec_get_name(codecpar->codec_id));
    printf("Resolution: %dx%d\n", codecpar->width, codecpar->height);
    printf("Pixel format: %s\n", av_get_pix_fmt_name(codecpar->format));

    if (video_stream->r_frame_rate.den != 0) {
        double fps = av_q2d(video_stream->r_frame_rate);
        printf("Frame rate: %.2f fps\n", fps);
    }

    if (video_stream->duration != AV_NOPTS_VALUE) {
        double duration = (double)video_stream->duration * av_q2d(video_stream->time_base);
        printf("Duration: %.2f seconds\n", duration);
        printf("Estimated frames: %lld\n", video_stream->nb_frames);
    }

    // Check if decoder is available
    const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) {
        printf("❌ No decoder available for this codec\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    printf("✓ Decoder available: %s\n", decoder->name);

    // Check if H.265 encoder is available
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    if (!encoder) {
        printf("❌ H.265 encoder not available\n");
    } else {
        printf("✓ H.265 encoder available: %s\n", encoder->name);
    }

    // Check if required filters are available
    const AVFilter *split = avfilter_get_by_name("split");
    const AVFilter *select = avfilter_get_by_name("select");
    const AVFilter *mix = avfilter_get_by_name("mix");
    const AVFilter *setpts = avfilter_get_by_name("setpts");
    const AVFilter *fps_filter = avfilter_get_by_name("fps");
    const AVFilter *vidstabdetect = avfilter_get_by_name("vidstabdetect");
    const AVFilter *vidstabtransform = avfilter_get_by_name("vidstabtransform");

    printf("\n=== Filter Availability ===\n");
    printf("split: %s\n", split ? "✓ Available" : "❌ Missing (REQUIRED)");
    printf("select: %s\n", select ? "✓ Available" : "❌ Missing (REQUIRED)");
    printf("mix: %s\n", mix ? "✓ Available" : "❌ Missing (REQUIRED)");
    printf("setpts: %s\n", setpts ? "✓ Available" : "❌ Missing");
    printf("fps: %s\n", fps_filter ? "✓ Available" : "❌ Missing");
    printf("vidstabdetect: %s\n", vidstabdetect ? "✓ Available" : "❌ Missing");
    printf("vidstabtransform: %s\n", vidstabtransform ? "✓ Available" : "❌ Missing");

    if (!split || !select || !mix) {
        printf("\n⚠️  ERROR: Required filters for parallel stream processing not available!\n");
        printf("   This approach requires split, select, and mix filters.\n");
        printf("   Please update FFmpeg to a recent version.\n");
    }

    if (!vidstabdetect || !vidstabtransform) {
        printf("\n⚠️  WARNING: Video stabilization filters not available!\n");
        printf("   You may need to install FFmpeg with libvidstab support.\n");
        printf("   On Ubuntu/Debian: apt install ffmpeg libavfilter-extra\n");
    }

    avformat_close_input(&fmt_ctx);
    printf("\n=== Diagnostics Complete ===\n\n");
    return 0;
}

// Mobile app usage example
void on_progress(float progress, void *user_data) {
    printf("Progress: %.1f%%\r", progress * 100);
    fflush(stdout);
    // Update UI progress bar here
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("RoadLapse - Speed up and stabilize your journey videos\n\n");
        printf("Usage:\n");
        printf("  Single input:    %s [--degree N] output.mp4 input.mp4\n", argv[0]);
        printf("  Multiple inputs: %s [--degree N] output.mp4 input1.mp4 input2.mp4 [...]\n", argv[0]);
        printf("  Diagnostics:     %s --diagnose input.mp4\n", argv[0]);
        printf("\nOptions:\n");
        printf("  --degree N       Speedup degree (default: 2, meaning 4x speedup)\n");
        printf("                   degree=1 → 2x, degree=2 → 4x, degree=3 → 8x, etc.\n");
        printf("\nFramerate is automatically optimized: 25fps→50fps, 30fps→60fps\n");
        return 1;
    }

    // Handle diagnostics mode
    if (strcmp(argv[1], "--diagnose") == 0) {
        if (argc < 3) {
            printf("Usage: %s --diagnose input.mp4\n", argv[0]);
            return 1;
        }
        return diagnose_video_file(argv[2]) == 0 ? 0 : 1;
    }

    // Parse speedup degree option
    int speedup_degree = 2;  // Default to 4x speedup (2^2)
    int arg_offset = 1;

    if (argc > 3 && strcmp(argv[1], "--degree") == 0) {
        speedup_degree = atoi(argv[2]);
        if (speedup_degree < 1 || speedup_degree > 10) {
            fprintf(stderr, "Error: Speedup degree must be between 1 and 10\n");
            return 1;
        }
        arg_offset = 3;
    }

    // Need at least output and one input after processing options
    if (argc < arg_offset + 2) {
        printf("Error: Need at least an output file and one input file\n");
        printf("Usage: %s [--degree N] output.mp4 input1.mp4 [input2.mp4 ...]\n", argv[0]);
        return 1;
    }

    const char *output_file = argv[arg_offset];
    int num_inputs = argc - arg_offset - 1;  // Remaining args after output file
    char **input_files = &argv[arg_offset + 1];
    int factor = 1 << speedup_degree;  // 2^speedup_degree

    printf("RoadLapse Processing Configuration:\n");
    printf("Speedup degree: %d (factor: %dx)\n", speedup_degree, factor);
    printf("Output: %s\n", output_file);
    printf("Input files (%d):\n", num_inputs);
    for (int i = 0; i < num_inputs; i++) {
        printf("  %d. %s\n", i+1, input_files[i]);
    }
    printf("\n");

    // Check all input files exist
    for (int i = 0; i < num_inputs; i++) {
        if (access(input_files[i], R_OK) != 0) {
            fprintf(stderr, "Error: Cannot read input file: %s\n", input_files[i]);
            return 1;
        }
    }

    // Run diagnostics on first file
    printf("Running diagnostics on first input file...\n\n");
    if (diagnose_video_file(input_files[0]) != 0) {
        printf("❌ Diagnostics failed. Cannot proceed.\n");
        return 1;
    }

    // Process all files
    int result = process_roadlapse(input_files, num_inputs, output_file, speedup_degree, on_progress, NULL);

    if (result == 0) {
        printf("\n🎉 RoadLapse processing completed successfully!\n");
        printf("Output saved to: %s\n", output_file);
        printf("Effective speedup: %dx with motion blur\n", factor);
    } else {
        printf("\n❌ RoadLapse processing failed with error code: %d\n", result);
    }

    return result;
}
