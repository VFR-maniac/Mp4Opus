/*****************************************************************************
 * mp4opusdec.c
 *****************************************************************************
 * Copyright (C) 2014
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license. */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>

#include <lsmash.h>

#include <opus/opus_multistream.h>

typedef struct
{
    int help;
} option_t;

typedef struct
{
    lsmash_sample_t *sample;
    uint8_t         *data;
    uint32_t         size;
} input_packet_t;

typedef struct
{
    lsmash_audio_summary_t  *summary;
    lsmash_codec_specific_t *cs;
} input_summary_t;

typedef struct
{
    input_summary_t *summaries;
    uint32_t         num_summaries;
} input_media_t;

typedef struct
{
#define STATUS_RECOVERY_REQUIRED 0
#define STATUS_RECOVERY_STARTED  1
    int      status;
    uint32_t timescale;
    uint64_t timestamp;
    uint64_t duration;
    int64_t  start_time;
    int32_t  rate;
} presentation_t;

typedef struct
{
    uint32_t      track_ID;
    input_media_t media;
} input_track_t;

typedef struct
{
    input_track_t             track;
    lsmash_movie_parameters_t param;
} input_movie_t;

typedef struct
{
    char                    *name;
    lsmash_file_t           *fh;
    lsmash_file_parameters_t param;
    input_movie_t            movie;
} input_file_t;

typedef struct
{
    lsmash_root_t *root;
    input_file_t   file;
} input_t;

typedef struct
{
    lsmash_audio_summary_t *summary;
    uint8_t                *buffer;
    uint64_t                buffer_offset;
    uint64_t                timestamp;
    uint32_t                sample_entry;
} output_media_t;

typedef struct
{
    uint32_t       track_ID;
    output_media_t media;
} output_track_t;

typedef struct
{
    output_track_t track;
} output_movie_t;

typedef struct
{
    char                    *name;
    lsmash_file_t           *fh;
    lsmash_file_parameters_t param;
    output_movie_t           movie;
} output_file_t;

typedef struct
{
    lsmash_root_t *root;
    output_file_t  file;
} output_t;

typedef struct
{
    OpusMSDecoder *msdec;
} decoder_t;

typedef struct
{
    option_t  opt;
    input_t   input;
    output_t  output;
    decoder_t opus;
} mp4opusdec_t;

#define MP4OPUSDEC_MIN( a, b ) (((a) < (b)) ? (a) : (b))
#define eprintf( ... ) fprintf( stderr, __VA_ARGS__ )
#define ERROR_MSG( ... ) error_message( __VA_ARGS__ )
#define WARNING_MSG( ... ) warning_message( __VA_ARGS__ )
#define MP4OPUSDEC_ERR( ... ) mp4opusdec_error( &dec, __VA_ARGS__ )
#define MP4OPUSDEC_USAGE_ERR() mp4opusdec_usage_error();
#define REFRESH_CONSOLE eprintf( "                                                                               \r" )

#define MAX_OPUS_PACKET_DURATION 5760

static void cleanup_input_movie
(
    input_t *input
)
{
    input_media_t *in_media = &input->file.movie.track.media;
    if( in_media->summaries )
    {
        for( uint32_t i = 0; i < in_media->num_summaries; i++ )
        {
            lsmash_cleanup_summary( (lsmash_summary_t *)in_media->summaries[i].summary );
            lsmash_destroy_codec_specific_data( in_media->summaries[i].cs );
        }
        lsmash_free( in_media->summaries );
    }
    lsmash_close_file( &input->file.param );
    lsmash_destroy_root( input->root );
    input->root = NULL;
}

static void cleanup_output_movie
(
    output_t *output
)
{
    lsmash_cleanup_summary( (lsmash_summary_t *)output->file.movie.track.media.summary );
    lsmash_free( output->file.movie.track.media.buffer );
    lsmash_close_file( &output->file.param );
    lsmash_destroy_root( output->root );
    output->root = NULL;
}

static void cleanup_mp4opusdec
(
    mp4opusdec_t *dec
)
{
    cleanup_input_movie( &dec->input );
    cleanup_output_movie( &dec->output );
    opus_multistream_decoder_destroy( dec->opus.msdec );
}

static int mp4opusdec_error
(
    mp4opusdec_t *dec,
    const char   *message,
    ...
)
{
    cleanup_mp4opusdec( dec );
    REFRESH_CONSOLE;
    eprintf( "Error: " );
    va_list args;
    va_start( args, message );
    vfprintf( stderr, message, args );
    va_end( args );
    return -1;
}

static int error_message
(
    const char *message,
    ...
)
{
    REFRESH_CONSOLE;
    eprintf( "Error: " );
    va_list args;
    va_start( args, message );
    vfprintf( stderr, message, args );
    va_end( args );
    return -1;
}

static int warning_message
(
    const char *message,
    ...
)
{
    REFRESH_CONSOLE;
    eprintf( "Warning: " );
    va_list args;
    va_start( args, message );
    vfprintf( stderr, message, args );
    va_end( args );
    return -1;
}

static void display_help( void )
{
    eprintf
    (
        "\n"
        "Usage: mp4opusdec -i input -o output\n"
    );
}

static int mp4opusdec_usage_error( void )
{
    display_help();
    return -1;
}

static int parse_options
(
    int           argc,
    char        **argv,
    mp4opusdec_t *dec
)
{
    if ( argc < 2 )
        return -1;
    else if( !strcasecmp( argv[1], "-h" ) || !strcasecmp( argv[1], "--help" ) )
    {
        dec->opt.help = 1;
        return 0;
    }
    else if( argc < 5 )
        return -1;
    uint32_t i = 1;
    while( argc > i && *argv[i] == '-' )
    {
#define CHECK_NEXT_ARG if( argc == ++i ) return ERROR_MSG( "%s requires argument.\n", argv[i - 1] );
        if( !strcasecmp( argv[i], "-i" ) || !strcasecmp( argv[i], "--input" ) )
        {
            CHECK_NEXT_ARG;
            dec->input.file.name = argv[i];
        }
        else if( !strcasecmp( argv[i], "-o" ) || !strcasecmp( argv[i], "--output" ) )
        {
            CHECK_NEXT_ARG;
            dec->output.file.name = argv[i];
        }
#undef CHECK_NEXT_ARG
        else
            return ERROR_MSG( "you specified invalid option: %s.\n", argv[i] );
        ++i;
    }
    return 0;
}

static int get_opus_specific_info
(
    input_summary_t *in_summary
)
{
    uint32_t cs_count = lsmash_count_codec_specific_data( (lsmash_summary_t *)in_summary->summary );
    for( uint32_t i = 0; i < cs_count; i++ )
    {
        lsmash_codec_specific_t *cs = lsmash_get_codec_specific_data( (lsmash_summary_t *)in_summary->summary, i + 1 );
        if( !cs || cs->type != LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_AUDIO_OPUS )
            continue;
        in_summary->cs = lsmash_convert_codec_specific_format( cs, LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
        if( in_summary->cs )
            return 0;
    }
    return ERROR_MSG( "failed to get Opus specific info.\n" );
}

static int open_input_file
(
    mp4opusdec_t *dec
)
{
    input_t *input = &dec->input;
    input->root = lsmash_create_root();
    if( !input->root )
        return ERROR_MSG( "failed to create ROOT for input file.\n" );
    input_file_t *in_file = &input->file;
    if( lsmash_open_file( in_file->name, 1, &in_file->param ) < 0 )
        return ERROR_MSG( "failed to open input file.\n" );
    in_file->fh = lsmash_set_file( input->root, &in_file->param );
    if( !in_file->fh )
        return ERROR_MSG( "failed to add input file into ROOT.\n" );
    if( lsmash_read_file( in_file->fh, &in_file->param ) < 0 )
        return ERROR_MSG( "failed to read input file\n" );
    if( lsmash_get_movie_parameters( input->root, &in_file->movie.param ) < 0 )
        return ERROR_MSG( "failed to get movie parameters.\n" );
    input_track_t *in_track = &in_file->movie.track;
    int opus_stream_found = 0;
    for( uint32_t i = 0; i < in_file->movie.param.number_of_tracks; i++ )
    {
        in_track->track_ID = lsmash_get_track_ID( input->root, i + 1 );
        if( in_track->track_ID == 0 )
            return ERROR_MSG( "failed to get track_ID.\n" );
        /* Check CODEC type.
         * This program has no support of CODECs other than Opus. */
        uint32_t num_summaries = lsmash_count_summary( input->root, in_track->track_ID );
        if( num_summaries == 0 )
        {
            WARNING_MSG( "failed to find valid summaries.\n" );
            continue;
        }
        else if( num_summaries > 1 )
        {
            WARNING_MSG( "multiple CODEC specific info are not supported yet.\n" );
            continue;
        }
        input_media_t *in_media = &in_track->media;
        in_media->num_summaries = num_summaries;
        in_media->summaries = lsmash_malloc_zero( sizeof(input_summary_t) );
        if( !in_media->summaries )
            return ERROR_MSG( "failed to alloc input summaries.\n" );
        for( uint32_t j = 0; j < num_summaries; j++ )
        {
            lsmash_summary_t *summary = lsmash_get_summary( input->root, in_track->track_ID, j + 1 );
            if( !summary )
            {
                WARNING_MSG( "failed to get summary.\n" );
                continue;
            }
            if( summary->summary_type != LSMASH_SUMMARY_TYPE_AUDIO
             || !lsmash_check_codec_type_identical( summary->sample_type, ISOM_CODEC_TYPE_OPUS_AUDIO )
             || ((lsmash_audio_summary_t *)summary)->frequency != 48000
             || ((lsmash_audio_summary_t *)summary)->channels > 8 )
            {
                lsmash_cleanup_summary( summary );
                continue;
            }
            in_media->summaries[j].summary = (lsmash_audio_summary_t *)summary;
            if( get_opus_specific_info( &in_media->summaries[j] ) < 0 )
            {
                in_media->summaries[j].summary = NULL;
                lsmash_cleanup_summary( summary );
                continue;
            }
        }
        if( lsmash_get_media_timescale( input->root, in_track->track_ID ) != 48000 )
        {
            WARNING_MSG( "media timescale != 48000 is not supported.\n" );
            continue;
        }
        if( lsmash_construct_timeline( input->root, in_track->track_ID ) < 0 )
        {
            WARNING_MSG( "failed to construct timeline.\n" );
            continue;
        }
        opus_stream_found = 1;
        break;
    }
    if( !opus_stream_found )
        return ERROR_MSG( "failed to find Opus stream to decode.\n" );
    lsmash_destroy_children( lsmash_file_as_box( in_file->fh ) );
    return 0;
}

static void remap_channel_layout
(
    lsmash_opus_specific_parameters_t *param,
    lsmash_qt_audio_channel_layout_t  *layout,
    uint8_t                            channel_mapping[8]
)
{
    /* Vorbis channel order -> SMPTE/USB channel order */
    static const struct
    {
        uint32_t bitmap;
        uint8_t  vorbis[8];
    } channel_remap_table[8] =
        {
            /* C -> C */
            {
                QT_CHANNEL_BIT_CENTER,
                { 0 }
            },
            /* L+R -> L+R */
            {
                QT_CHANNEL_BIT_LEFT | QT_CHANNEL_BIT_RIGHT,
                { 0, 1 }
            },
            /* L+C+R -> L+R+C */
            {
                QT_CHANNEL_BIT_LEFT | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_CENTER,
                { 0, 2, 1 }
            },
            /* L+R+BL+BR -> L+R+BL+BR */
            {
                QT_CHANNEL_BIT_LEFT          | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_LEFT_SURROUND | QT_CHANNEL_BIT_RIGHT_SURROUND,
                { 0, 1, 2, 3 }
            },
            /* L+C+R+BL+BR -> L+R+C+BL+BR */
            {
                QT_CHANNEL_BIT_LEFT          | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_CENTER
              | QT_CHANNEL_BIT_LEFT_SURROUND | QT_CHANNEL_BIT_RIGHT_SURROUND,
                { 0, 2, 1, 3, 4 }
            },
            /* L+C+R+BL+BR+LFE -> L+R+C+LFE+BL+BR */
            {
                QT_CHANNEL_BIT_LEFT          | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_CENTER
              | QT_CHANNEL_BIT_LFE_SCREEN
              | QT_CHANNEL_BIT_LEFT_SURROUND | QT_CHANNEL_BIT_RIGHT_SURROUND,
                { 0, 2, 1, 5, 3, 4 }
            },
            /* L+C+R+SL+SR+BC+LFE -> L+R+C+LFE+BC+SL+SR */
            {
                QT_CHANNEL_BIT_LEFT                 | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_CENTER
              | QT_CHANNEL_BIT_LFE_SCREEN
              | QT_CHANNEL_BIT_CENTER_SURROUND
              | QT_CHANNEL_BIT_LEFT_SURROUND_DIRECT | QT_CHANNEL_BIT_RIGHT_SURROUND_DIRECT,
                { 0, 2, 1, 6, 5, 3, 4 }
            },
            /* L+C+R+SL+SR+BL+BR+LFE -> L+R+C+LFE+BL+BR+SL+SR */
            {
                QT_CHANNEL_BIT_LEFT                 | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_CENTER
              | QT_CHANNEL_BIT_LFE_SCREEN
              | QT_CHANNEL_BIT_LEFT_SURROUND        | QT_CHANNEL_BIT_RIGHT_SURROUND
              | QT_CHANNEL_BIT_LEFT_SURROUND_DIRECT | QT_CHANNEL_BIT_RIGHT_SURROUND_DIRECT,
                { 0, 2, 1, 7, 5, 6, 3, 4 }
            }
        };
    /* Coded channel order -> Vorbis channel order */
    uint8_t index = param->OutputChannelCount - 1;
    if( index < 8 )
    {
        layout->channelLayoutTag = QT_CHANNEL_LAYOUT_USE_CHANNEL_BITMAP;
        layout->channelBitmap    = channel_remap_table[index].bitmap;
        uint8_t *opus_channel_mapping = param->ChannelMappingFamily
                                      ? param->ChannelMapping
                                      : (uint8_t [8]){ 0, 1 };
        for( uint8_t i = 0; i < param->OutputChannelCount; i++ )
            channel_mapping[i] = opus_channel_mapping[ channel_remap_table[index].vorbis[i] ];
    }
    else
        layout->channelLayoutTag = QT_CHANNEL_LAYOUT_UNKNOWN | param->OutputChannelCount;
    return;
}

static int setup_decoder
(
    decoder_t                         *opus,
    lsmash_opus_specific_parameters_t *param,
    lsmash_qt_audio_channel_layout_t  *layout
)
{
    uint8_t channel_mapping[8] = { 0 };
    remap_channel_layout( param, layout, channel_mapping );
    int err;
    OpusMSDecoder *msdec = opus_multistream_decoder_create( 48000,
                                                            param->OutputChannelCount,
                                                            param->StreamCount,
                                                            param->CoupledCount,
                                                            channel_mapping,
                                                            &err );
    if( err != OPUS_OK )
        return ERROR_MSG( "failed to create decoder.\n" );
    opus->msdec = msdec;
    err = opus_multistream_decoder_ctl( msdec, OPUS_SET_GAIN( param->OutputGain ) );
    if( err != OPUS_OK )
        return ERROR_MSG( "failed to set output gain.\n" );
    return 0;
}

static int prepare_output
(
    mp4opusdec_t *dec
)
{
    output_t      *output   = &dec->output;
    output_file_t *out_file = &output->file;
    /* Initialize L-SMASH muxer */
    output->root = lsmash_create_root();
    if( !output->root )
        return ERROR_MSG( "failed to create ROOT.\n" );
    lsmash_file_parameters_t *file_param = &out_file->param;
    if( lsmash_open_file( out_file->name, 0, file_param ) < 0 )
        return ERROR_MSG( "failed to open an output file.\n" );
    file_param->major_brand   = ISOM_BRAND_TYPE_QT;
    file_param->brands        = (lsmash_brand_type [1]){ ISOM_BRAND_TYPE_QT };
    file_param->brand_count   = 1;
    file_param->minor_version = 0;
    out_file->fh = lsmash_set_file( output->root, file_param );
    if( !out_file->fh )
        return ERROR_MSG( "failed to add output file into ROOT.\n" );
    /* Initialize movie */
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    movie_param.timescale = 48000;
    if( lsmash_set_movie_parameters( output->root, &movie_param ) < 0 )
        return ERROR_MSG( "failed to set movie parameters.\n" );
    /* Set up track parameters. */
    output_track_t *out_track = &output->file.movie.track;
    out_track->track_ID = lsmash_create_track( output->root, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK );
    if( out_track->track_ID == 0 )
        return ERROR_MSG( "failed to create track.\n" );
    lsmash_track_parameters_t track_param;
    lsmash_initialize_track_parameters( &track_param );
    track_param.mode = ISOM_TRACK_IN_MOVIE | ISOM_TRACK_IN_PREVIEW | ISOM_TRACK_ENABLED;
    if( lsmash_set_track_parameters( output->root, out_track->track_ID, &track_param ) < 0 )
        return ERROR_MSG( "failed to set track parameters.\n" );
    /* Set up media parameters. */
    lsmash_media_parameters_t media_param;
    lsmash_initialize_media_parameters( &media_param );
    media_param.timescale = 48000;
    if( lsmash_set_media_parameters( output->root, out_track->track_ID, &media_param ) < 0 )
        return ERROR_MSG( "failed to set media parameters.\n" );
    /* Set up Opus configurations. */
    input_summary_t *in_summary = &dec->input.file.movie.track.media.summaries[0];
    lsmash_opus_specific_parameters_t *opus_param = (lsmash_opus_specific_parameters_t *)in_summary->cs->data.structured;
    lsmash_audio_summary_t *out_summary = (lsmash_audio_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_AUDIO );
    if( !out_summary )
        return ERROR_MSG( "failed to allocate summary for output.\n" );
    out_track->media.summary = out_summary;
    out_summary->sample_type = QT_CODEC_TYPE_LPCM_AUDIO;
    out_summary->frequency   = 48000;
    out_summary->channels    = opus_param->OutputChannelCount;
    out_summary->sample_size = 16;
    output_media_t *out_media = &dec->output.file.movie.track.media;
    uint32_t buffer_size = MAX_OPUS_PACKET_DURATION * opus_param->OutputChannelCount * 2;
    uint8_t *buffer      = lsmash_malloc_zero( buffer_size );
    if( !buffer )
        return ERROR_MSG( "failed to allocate sample buffer.\n" );
    out_media->buffer = buffer;
    lsmash_codec_specific_t *cs = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_QT_AUDIO_FORMAT_SPECIFIC_FLAGS,
                                                                     LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
    if( !cs )
        return ERROR_MSG( "failed to create LPCM specific info.\n" );
    lsmash_qt_audio_format_specific_flags_t *lpcm_param = (lsmash_qt_audio_format_specific_flags_t *)cs->data.structured;
    lpcm_param->format_flags = QT_AUDIO_FORMAT_FLAG_SIGNED_INTEGER | QT_AUDIO_FORMAT_FLAG_PACKED;
    if( lsmash_add_codec_specific_data( (lsmash_summary_t *)out_summary, cs ) < 0 )
    {
        lsmash_destroy_codec_specific_data( cs );
        return ERROR_MSG( "failed to add LPCM specific info.\n" );
    }
    lsmash_destroy_codec_specific_data( cs );
    cs = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_QT_AUDIO_CHANNEL_LAYOUT,
                                            LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
    if( !cs )
        return ERROR_MSG( "failed to create channel layout info.\n" );
    lsmash_qt_audio_channel_layout_t *layout = (lsmash_qt_audio_channel_layout_t *)cs->data.structured;
    decoder_t *opus = &dec->opus;
    if( setup_decoder( opus, opus_param, layout ) < 0 )
    {
        lsmash_destroy_codec_specific_data( cs );
        return ERROR_MSG( "failed to set up decoder.\n" );
    }
    if( lsmash_add_codec_specific_data( (lsmash_summary_t *)out_summary, cs ) < 0 )
    {
        lsmash_destroy_codec_specific_data( cs );
        return ERROR_MSG( "failed to add channel layout info.\n" );
    }
    lsmash_destroy_codec_specific_data( cs );
    out_track->media.sample_entry = lsmash_add_sample_entry( output->root, out_track->track_ID, out_summary );
    if( !out_track->media.sample_entry )
        return ERROR_MSG( "failed to add sample description entry.\n" );
    return 0;
}

static void free_input_packet
(
    input_packet_t *packet
)
{
    lsmash_delete_sample( packet->sample );
    packet->sample = NULL;
}

static int get_input_packet
(
    lsmash_root_t  *in_root,
    uint32_t        in_track_ID,
    uint32_t       *packet_number,
    input_packet_t *packet,
    presentation_t *presentation
)
{
    do
    {
        if( !lsmash_check_sample_existence_in_media_timeline( in_root, in_track_ID, *packet_number ) )
            return 1;   /* No more samples. So, reached EOF. */
        if( presentation->status == STATUS_RECOVERY_REQUIRED )
        {
            lsmash_sample_t sample_info = { 0 };
            if( lsmash_get_sample_info_from_media_timeline( in_root, in_track_ID, *packet_number, &sample_info ) < 0 )
                return ERROR_MSG( "failed to get sample info.\n" );
            if( sample_info.cts < presentation->start_time )
            {
                *packet_number += 1;
                continue;
            }
            else
            {
                presentation->status = STATUS_RECOVERY_STARTED;
                uint32_t start_from_prev_sample = sample_info.cts > presentation->start_time ? 1 : 0;
                uint32_t pre_roll_distance      = sample_info.prop.pre_roll.distance;
                if( *packet_number <= pre_roll_distance + start_from_prev_sample )
                    *packet_number = 1;
                else
                    *packet_number -= pre_roll_distance + start_from_prev_sample;
                continue;
            }
        }
        lsmash_sample_t *sample = lsmash_get_sample_from_media_timeline( in_root, in_track_ID, *packet_number );
        if( !sample )
            return ERROR_MSG( "failed to get sample.\n" );
        packet->sample = sample;
        packet->data   = sample->data;
        packet->size   = sample->length;
    } while( !packet->sample );
    return 0;
}

static int feed_packet_to_decoder
(
    decoder_t      *opus,
    output_media_t *out_media,
    input_packet_t *packet
)
{
    int num_samples = opus_multistream_decode( opus->msdec,
                                               packet->data,
                                               packet->size,
                                               (opus_int16 *)out_media->buffer,
                                               MAX_OPUS_PACKET_DURATION,
                                               0 );
    if( num_samples < 0 )
        return ERROR_MSG( "failed to decode.\n" );
    return num_samples;
}

static int apply_edit
(
    output_media_t *out_media,
    input_packet_t *packet,
    presentation_t *presentation,
    int             num_samples
)
{
    if( num_samples <= 0 )
        return num_samples;
    int pre_skipped_samples;
    if( packet->sample->cts < presentation->start_time )
    {
        if( packet->sample->cts + num_samples <= presentation->start_time )
            pre_skipped_samples = num_samples;  /* no output samples */
        else
            pre_skipped_samples = presentation->start_time - packet->sample->cts;
        num_samples -= pre_skipped_samples;
    }
    else
        pre_skipped_samples = 0;
    out_media->buffer_offset = (uint64_t)pre_skipped_samples * out_media->summary->channels * 2;
    presentation->timestamp += ((double)num_samples / 48000) * presentation->timescale;
    if( presentation->timestamp > presentation->duration )
        num_samples -= ((double)(presentation->timestamp - presentation->duration) / presentation->timescale) * 48000;
    return num_samples;
}

static int mux_pcm_samples
(
    lsmash_root_t  *out_root,
    uint32_t        out_track_ID,
    output_media_t *out_media,
    int             num_samples
)
{
    if( num_samples <= 0 )
        return num_samples;
    lsmash_sample_t *out_sample = lsmash_create_sample( num_samples * out_media->summary->channels * 2 );
    if( !out_sample )
        return ERROR_MSG( "failed to allocate sample.\n" );
    memcpy( out_sample->data, out_media->buffer + out_media->buffer_offset, out_sample->length );
    out_sample->dts           = out_media->timestamp;
    out_sample->cts           = out_media->timestamp;
    out_sample->index         = out_media->sample_entry;
    out_sample->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
    if( lsmash_append_sample( out_root, out_track_ID, out_sample ) < 0 )
    {
        lsmash_delete_sample( out_sample );
        return ERROR_MSG( "failed to append sample.\n" );
    }
    out_media->timestamp += num_samples;
    return 0;
}

static int flush_decoder
(
    lsmash_root_t *out_root,
    uint32_t       out_track_ID
)
{
    if( lsmash_flush_pooled_samples( out_root, out_track_ID, 1 ) < 0 )
        return ERROR_MSG( "failed to flush samples.\n" );
    return 0;
}

static int do_decode
(
    mp4opusdec_t *dec
)
{
    input_t  *input  = &dec->input;
    output_t *output = &dec->output;
    uint32_t edit_count = lsmash_count_explicit_timeline_map( input->root,
                                                              input->file.movie.track.track_ID );
    for( uint32_t edit_number = 1; edit_number <= edit_count; edit_number++ )
    {
        lsmash_edit_t edit;
        int ret = lsmash_get_explicit_timeline_map( input->root,
                                                    input->file.movie.track.track_ID,
                                                    edit_number,
                                                    &edit );
        if( ret < 0 )
            return ERROR_MSG( "failed to get explicit timeline map.\n" );
        if( edit.start_time == -1 )
        {
            ret = lsmash_create_explicit_timeline_map( output->root,
                                                       output->file.movie.track.track_ID,
                                                       edit );
            if( ret < 0 )
                return ERROR_MSG( "failed to create empty edit.\n" );
            continue;
        }
        presentation_t presentation =
        {
            .status     = STATUS_RECOVERY_REQUIRED,
            .timescale  = input->file.movie.param.timescale,
            .timestamp  = 0,
            .duration   = edit.duration,
            .start_time = edit.start_time,
            .rate       = edit.rate
        };
        edit.start_time = 0;    /* no extra samples within LPCM track */
        if( presentation.duration == 0 )
        {
            uint64_t duration = lsmash_get_media_duration_from_media_timeline( input->root,
                                                                               input->file.movie.track.track_ID );
            presentation.duration = edit.duration = ((double)duration / 48000) * presentation.timescale;
        }
        ret = lsmash_create_explicit_timeline_map( output->root,
                                                   output->file.movie.track.track_ID,
                                                   edit );
        if( ret < 0 )
            return ERROR_MSG( "failed to create explicit timeline map.\n" );
        /* ret == 1 means EOF. */
        for( uint32_t packet_number = 1; ret != 1 && presentation.timestamp < presentation.duration; packet_number++ )
        {
            input_packet_t packet = { NULL };
            ret = get_input_packet( input->root,
                                    input->file.movie.track.track_ID,
                                    &packet_number,
                                    &packet,
                                    &presentation );
            if( ret < 0 )
                return ret;
            if( ret != 1 )
            {
                int num_samples = feed_packet_to_decoder( &dec->opus,
                                                          &output->file.movie.track.media,
                                                          &packet );
                num_samples = apply_edit( &output->file.movie.track.media,
                                          &packet,
                                          &presentation,
                                          num_samples );
                ret = mux_pcm_samples( output->root,
                                       output->file.movie.track.track_ID,
                                       &output->file.movie.track.media,
                                       num_samples );
            }
            free_input_packet( &packet );
            if( ret < 0 )
                return ret;
        }
    }
    return flush_decoder( output->root,
                          output->file.movie.track.track_ID );
}

static int finish_movie
(
    mp4opusdec_t *dec
)
{
    output_t *output = &dec->output;
    if( lsmash_finish_movie( output->root, NULL ) < 0 )
        return ERROR_MSG( "failed to finalize output movie.\n" );
    return 0;
}

int main
(
    int   argc,
    char *argv[]
)
{
    mp4opusdec_t dec = { { 0 } };
    if( parse_options( argc, argv, &dec ) < 0 )
        return MP4OPUSDEC_ERR( "failed to parse options.\n" );
    if( dec.opt.help )
    {
        display_help();
        return 0;
    }
    if( open_input_file( &dec ) < 0 )
        return MP4OPUSDEC_USAGE_ERR();
    if( prepare_output( &dec ) < 0 )
        return MP4OPUSDEC_ERR( "failed to set up preparation for output.\n" );
    if( do_decode( &dec ) < 0 )
        return MP4OPUSDEC_ERR( "failed to decode.\n" );
    if( finish_movie( &dec ) < 0 )
        return MP4OPUSDEC_ERR( "failed to finish output movie.\n" );
    REFRESH_CONSOLE;
    eprintf( "Decoding completed!\n" );
    cleanup_mp4opusdec( &dec );
    return 0;
}
