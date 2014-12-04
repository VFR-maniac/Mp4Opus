/*****************************************************************************
 * mp4opusenc.c
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
    lsmash_audio_summary_t *summary;
} input_summary_t;

typedef struct
{
    input_summary_t *summaries;
    uint8_t         *buffer;
    uint32_t         buffer_size;
    uint32_t         buffer_pos;
    uint32_t         num_summaries;
    uint64_t         num_samples;
} input_media_t;

typedef struct
{
    uint32_t      track_ID;
    input_media_t media;
} input_track_t;

typedef struct
{
    input_track_t track;
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
    uint32_t                sample_entry;
    uint32_t                priming_samples;
    uint32_t                preroll_distance;
    uint32_t                sample_duration;
    uint64_t                timestamp;
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
    int    application;
    int    complexity;
    int    bitrate;
    int    vbr;
    int    max_bandwidth;
    double frame_size;
} encoder_option_t;

typedef struct
{
    OpusMSEncoder   *msenc;
    encoder_option_t opt;
    int              stream_count;
    int              frame_size;
} encoder_t;

typedef struct
{
    option_t  opt;
    input_t   input;
    output_t  output;
    encoder_t opus;
} mp4opusenc_t;

#define MP4OPUSENC_MIN( a, b ) (((a) < (b)) ? (a) : (b))
#define eprintf( ... ) fprintf( stderr, __VA_ARGS__ )
#define ERROR_MSG( ... ) error_message( __VA_ARGS__ )
#define WARNING_MSG( ... ) warning_message( __VA_ARGS__ )
#define MP4OPUSENC_ERR( ... ) mp4opusenc_error( &enc, __VA_ARGS__ )
#define MP4OPUSENC_USAGE_ERR() mp4opusenc_usage_error();
#define REFRESH_CONSOLE eprintf( "                                                                               \r" )

static void cleanup_input_movie
(
    input_t *input
)
{
    input_media_t *in_media = &input->file.movie.track.media;
    if( in_media->summaries )
    {
        for( uint32_t i = 0; i < in_media->num_summaries; i++ )
            lsmash_cleanup_summary( (lsmash_summary_t *)in_media->summaries[i].summary );
        lsmash_free( in_media->summaries );
    }
    lsmash_free( in_media->buffer );
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
    lsmash_close_file( &output->file.param );
    lsmash_destroy_root( output->root );
    output->root = NULL;
}

static void cleanup_mp4opusenc
(
    mp4opusenc_t *enc
)
{
    cleanup_input_movie( &enc->input );
    cleanup_output_movie( &enc->output );
    opus_multistream_encoder_destroy( enc->opus.msenc );
}

static int mp4opusenc_error
(
    mp4opusenc_t *enc,
    const char   *message,
    ...
)
{
    cleanup_mp4opusenc( enc );
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
        "Usage: mp4opusenc [options] -i input -o output\n"
        "Options:\n"
        "    --help                    Display help\n"
        "    --application <integer>   Specify intended application\n"
        "                                0: Improved speech intelligibility\n"
        "                                1: Faithfulness (default)\n"
        "                                2: Restricted low-delay\n"
        "    --complexity <integer>    Specify encoding complexity\n"
        "                                the range is from 0 to 10 inclusive\n"
        "                                the default value is 10 (slowest)\n"
        "    --bitrate <integer>       Specify bitrate (bits/second)\n"
        "                                6000-256000 per channel are meaningful\n"
        "    --vbr <integer>           Specify VBR mode\n"
        "                                0: Hard CBR\n"
        "                                1: Unconstrained VBR (default)\n"
        "                                2: Constrained VBR\n"
        "    --cutoff <integer>        Specify the maximum bandpass\n"
        "                                0:  4 kHz passband\n"
        "                                1:  6 kHz passband\n"
        "                                2:  8 kHz passband\n"
        "                                3: 12 kHz passband\n"
        "                                4: 20 kHz passband (default)\n"
        "    --framesize <float>       Specify frame size in milliseconds\n"
        "                                2.5, 5, 10, 20, 40 and 60 are available\n"
        "                                the default value is 20\n"
    );
}

static int mp4opusenc_usage_error( void )
{
    display_help();
    return -1;
}

static void default_options
(
    mp4opusenc_t *enc
)
{
    enc->opus.opt.application   = OPUS_APPLICATION_AUDIO;
    enc->opus.opt.complexity    = 10;
    enc->opus.opt.bitrate       = OPUS_AUTO;
    enc->opus.opt.vbr           = 1;
    enc->opus.opt.max_bandwidth = OPUS_BANDWIDTH_FULLBAND;
    enc->opus.opt.frame_size    = 20;
}

static int parse_options
(
    int           argc,
    char        **argv,
    mp4opusenc_t *enc
)
{
    if ( argc < 2 )
        return -1;
    else if( !strcasecmp( argv[1], "-h" ) || !strcasecmp( argv[1], "--help" ) )
    {
        enc->opt.help = 1;
        return 0;
    }
    else if( argc < 5 )
        return -1;
    default_options( enc );
    uint32_t i = 1;
    while( argc > i && *argv[i] == '-' )
    {
#define CHECK_NEXT_ARG if( argc == ++i ) return ERROR_MSG( "%s requires argument.\n", argv[i - 1] );
        if( !strcasecmp( argv[i], "-i" ) || !strcasecmp( argv[i], "--input" ) )
        {
            CHECK_NEXT_ARG;
            enc->input.file.name = argv[i];
        }
        else if( !strcasecmp( argv[i], "-o" ) || !strcasecmp( argv[i], "--output" ) )
        {
            CHECK_NEXT_ARG;
            enc->output.file.name = argv[i];
        }
        else if( !strcasecmp( argv[i], "--application" ) )
        {
            CHECK_NEXT_ARG;
            int index = atoi( argv[i] );
            if( index < 0 || index > 2 )
                return ERROR_MSG( "you specified invalid argument: %s.\n", argv[i] );
            enc->opus.opt.application =
                (int [])
                {
                    OPUS_APPLICATION_VOIP,
                    OPUS_APPLICATION_AUDIO,
                    OPUS_APPLICATION_RESTRICTED_LOWDELAY
                } [index];
        }
        else if( !strcasecmp( argv[i], "--complexity" ) )
        {
            CHECK_NEXT_ARG;
            int complexity = atoi( argv[i] );
            if( complexity < 0 || complexity > 10 )
                return ERROR_MSG( "you specified invalid argument: %s.\n", argv[i] );
            enc->opus.opt.complexity = complexity;
        }
        else if( !strcasecmp( argv[i], "--bitrate" ) )
        {
            CHECK_NEXT_ARG;
            enc->opus.opt.bitrate = atoi( argv[i] );
        }
        else if( !strcasecmp( argv[i], "--vbr" ) )
        {
            CHECK_NEXT_ARG;
            int vbr = atoi( argv[i] );
            if( vbr < 0 || vbr > 2 )
                return ERROR_MSG( "you specified invalid argument: %s.\n", argv[i] );
            enc->opus.opt.vbr = vbr;
        }
        else if( !strcasecmp( argv[i], "--cutoff" ) )
        {
            CHECK_NEXT_ARG;
            int index = atoi( argv[i] );
            if( index < 0 || index > 5 )
                return ERROR_MSG( "you specified invalid argument: %s.\n", argv[i] );
            enc->opus.opt.max_bandwidth =
                (int [])
                {
                    OPUS_BANDWIDTH_NARROWBAND,
                    OPUS_BANDWIDTH_MEDIUMBAND,
                    OPUS_BANDWIDTH_WIDEBAND,
                    OPUS_BANDWIDTH_SUPERWIDEBAND,
                    OPUS_BANDWIDTH_FULLBAND
                } [index];
        }
        else if( !strcasecmp( argv[i], "--framesize" ) )
        {
            CHECK_NEXT_ARG;
            double frame_size = atof( argv[i] );
            if( frame_size != 2.5 && frame_size != 5
             && frame_size != 10  && frame_size != 20
             && frame_size != 40  && frame_size != 60 )
                return ERROR_MSG( "you specified invalid argument: %s.\n", argv[i] );
            enc->opus.opt.frame_size = frame_size;
        }
#undef CHECK_NEXT_ARG
        else
            return ERROR_MSG( "you specified invalid option: %s.\n", argv[i] );
        ++i;
    }
    if( !enc->input.file.name )
        return ERROR_MSG( "input file name is not specified.\n" );
    if( !enc->output.file.name )
        return ERROR_MSG( "output file name is not specified.\n" );
    return 0;
}

static int open_input_file
(
    mp4opusenc_t *enc
)
{
    input_t *input = &enc->input;
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
    lsmash_movie_parameters_t movie_param;
    if( lsmash_get_movie_parameters( input->root, &movie_param ) < 0 )
        return ERROR_MSG( "failed to get movie parameters.\n" );
    input_track_t *in_track = &in_file->movie.track;
    int lpcm_stream_found = 0;
    for( uint32_t i = 0; i < movie_param.number_of_tracks; i++ )
    {
        in_track->track_ID = lsmash_get_track_ID( input->root, i + 1 );
        if( in_track->track_ID == 0 )
            return ERROR_MSG( "failed to get track_ID.\n" );
        /* Check CODEC type.
         * This program has no support of CODECs other than LPCM. */
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
             || !lsmash_check_codec_type_identical( summary->sample_type, QT_CODEC_TYPE_LPCM_AUDIO )
             || (((lsmash_audio_summary_t *)summary)->frequency != 8000
              && ((lsmash_audio_summary_t *)summary)->frequency != 12000
              && ((lsmash_audio_summary_t *)summary)->frequency != 16000
              && ((lsmash_audio_summary_t *)summary)->frequency != 24000
              && ((lsmash_audio_summary_t *)summary)->frequency != 48000)
             || ((lsmash_audio_summary_t *)summary)->channels > 8
             || ((lsmash_audio_summary_t *)summary)->sample_size != 16 )
            {
                lsmash_cleanup_summary( summary );
                continue;
            }
            in_media->summaries[j].summary = (lsmash_audio_summary_t *)summary;
        }
        if( lsmash_construct_timeline( input->root, in_track->track_ID ) < 0 )
        {
            WARNING_MSG( "failed to construct timeline.\n" );
            continue;
        }
        lpcm_stream_found = 1;
        break;
    }
    if( !lpcm_stream_found )
        return ERROR_MSG( "failed to find LPCM stream to encode.\n" );
    lsmash_destroy_children( lsmash_file_as_box( in_file->fh ) );
    return 0;
}

static void remap_channel_layout
(
    lsmash_summary_t                  *summary,
    lsmash_opus_specific_parameters_t *param,
    uint8_t                            channel_mapping[8]
)
{
    /* SMPTE/USB channel order -> Encoder channel order -> Vorbis channel order */
    static const struct
    {
        uint32_t tag;
        uint32_t bitmap;
        uint8_t  encoder[8];
        uint8_t  vorbis[8];
    } channel_remap_table[8] =
        {
            /* C -> [C] -> C */
            {
                QT_CHANNEL_LAYOUT_MONO,
                QT_CHANNEL_BIT_CENTER,
                { 0 },
                { 0 }
            },
            /* L+R -> [L+R] -> L+R */
            {
                QT_CHANNEL_LAYOUT_STEREO,
                QT_CHANNEL_BIT_LEFT | QT_CHANNEL_BIT_RIGHT,
                { 0, 1 },
                { 0, 1 }
            },
            /* L+R+C -> [L+R]+[C] -> L+C+R */
            {
                QT_CHANNEL_LAYOUT_MPEG_3_0_A,
                QT_CHANNEL_BIT_LEFT | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_CENTER,
                { 0, 1, 2 },
                { 0, 2, 1 }
            },
            /* L+R+BL+BR -> [L+R]+[BL+BR] -> L+R+BL+BR */
            {
                QT_CHANNEL_LAYOUT_QUADRAPHONIC,
                QT_CHANNEL_BIT_LEFT          | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_LEFT_SURROUND | QT_CHANNEL_BIT_RIGHT_SURROUND,
                { 0, 1, 2, 3 },
                { 0, 1, 2, 3 }
            },
            /* L+R+C+BL+BR -> [L+R]+[BL+BR]+[C] -> L+C+R+BL+BR */
            {
                QT_CHANNEL_LAYOUT_MPEG_5_0_A,
                QT_CHANNEL_BIT_LEFT          | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_CENTER
              | QT_CHANNEL_BIT_LEFT_SURROUND | QT_CHANNEL_BIT_RIGHT_SURROUND,
                { 0, 1, 3, 4, 2 },
                { 0, 4, 1, 2, 3 }
            },
            /* L+R+C+LFE+BL+BR -> [L+R]+[BL+BR]+[C]+[LFE] -> L+C+R+BL+BR+LFE */
            {
                QT_CHANNEL_LAYOUT_MPEG_5_1_A,
                QT_CHANNEL_BIT_LEFT          | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_CENTER
              | QT_CHANNEL_BIT_LFE_SCREEN
              | QT_CHANNEL_BIT_LEFT_SURROUND | QT_CHANNEL_BIT_RIGHT_SURROUND,
                { 0, 1, 4, 5, 2, 3 },
                { 0, 4, 1, 2, 3, 5 }
            },
            /* L+R+C+LFE+BC+SL+SR -> [L+R]+[SL+SR]+[C]+[BC]+[LFE] -> L+C+R+SL+SR+BC+LFE */
            {
                QT_CHANNEL_LAYOUT_UNKNOWN | 7,
                QT_CHANNEL_BIT_LEFT                 | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_CENTER
              | QT_CHANNEL_BIT_LFE_SCREEN
              | QT_CHANNEL_BIT_CENTER_SURROUND
              | QT_CHANNEL_BIT_LEFT_SURROUND_DIRECT | QT_CHANNEL_BIT_RIGHT_SURROUND_DIRECT,
                { 0, 1, 5, 6, 2, 4, 3 },
                { 0, 4, 1, 2, 3, 5, 6 }
            },
            /* L+R+C+LFE+BL+BR+SL+SR -> [L+R]+[SL+SR]+[BL+BR]+[C]+[LFE] -> L+C+R+SL+SR+BL+BR+LFE */
            {
                QT_CHANNEL_LAYOUT_UNKNOWN | 8,
                QT_CHANNEL_BIT_LEFT                 | QT_CHANNEL_BIT_RIGHT
              | QT_CHANNEL_BIT_CENTER
              | QT_CHANNEL_BIT_LFE_SCREEN
              | QT_CHANNEL_BIT_LEFT_SURROUND        | QT_CHANNEL_BIT_RIGHT_SURROUND
              | QT_CHANNEL_BIT_LEFT_SURROUND_DIRECT | QT_CHANNEL_BIT_RIGHT_SURROUND_DIRECT,
                { 0, 1, 6, 7, 4, 5, 2, 3 },
                { 0, 6, 1, 2, 3, 4, 5, 7 }
            }
        };
    int channel_layout_found = 0;
    uint32_t cs_count = lsmash_count_codec_specific_data( summary );
    for( uint32_t i = 0; i < cs_count; i++ )
    {
        lsmash_codec_specific_t *cs = lsmash_get_codec_specific_data( summary, i + 1 );
        if( !cs || cs->type != LSMASH_CODEC_SPECIFIC_DATA_TYPE_QT_AUDIO_CHANNEL_LAYOUT )
            continue;
        lsmash_codec_specific_t *conv = lsmash_convert_codec_specific_format( cs, LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
        if( !conv )
            continue;
        lsmash_qt_audio_channel_layout_t *layout = (lsmash_qt_audio_channel_layout_t *)conv->data.structured;
        int index = 8;
        if( layout->channelLayoutTag == QT_CHANNEL_LAYOUT_USE_CHANNEL_BITMAP )
        {
            for( index = 0; index < 8; index++ )
                if( layout->channelBitmap == channel_remap_table[index].bitmap )
                {
                    channel_layout_found = 1;
                    break;
                }
        }
        else if( (layout->channelLayoutTag & QT_CHANNEL_LAYOUT_UNKNOWN) != QT_CHANNEL_LAYOUT_UNKNOWN )
        {
            for( index = 0; index < 8; index++ )
                if( layout->channelLayoutTag == channel_remap_table[index].tag )
                {
                    channel_layout_found = 1;
                    break;
                }
        }
        if( channel_layout_found )
        {
            memcpy( param->ChannelMapping, channel_remap_table[index].vorbis, sizeof(channel_remap_table[index].vorbis) );
            memcpy( channel_mapping, channel_remap_table[index].encoder, sizeof(channel_remap_table[index].encoder) );
        }
        lsmash_destroy_codec_specific_data( conv );
        break;
    }
    if( !channel_layout_found && param->OutputChannelCount < 3 )
    {
        int index = param->OutputChannelCount - 1;
        memcpy( param->ChannelMapping, channel_remap_table[index].vorbis, sizeof(channel_remap_table[index].vorbis) );
        memcpy( channel_mapping, channel_remap_table[index].encoder, sizeof(channel_remap_table[index].encoder) );
    }
}

static int setup_encoder
(
    encoder_t                         *opus,
    lsmash_opus_specific_parameters_t *param,
    uint8_t                            channel_mapping[8]
)
{
    if( opus->opt.frame_size < 10 && opus->opt.application != OPUS_APPLICATION_RESTRICTED_LOWDELAY )
    {
        WARNING_MSG( "framesize < 10ms can only use the MDCT modes.\n"
                     "Switch to restricted low-delay mode.\n" );
        opus->opt.application = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
    }
    int err;
    OpusMSEncoder *msenc = opus_multistream_encoder_create( param->InputSampleRate,
                                                            param->OutputChannelCount,
                                                            param->StreamCount,
                                                            param->CoupledCount,
                                                            channel_mapping,
                                                            opus->opt.application,
                                                            &err );
    if( err != OPUS_OK )
        return ERROR_MSG( "failed to create encoder.\n" );
    opus->msenc = msenc;
#define SET_OPT( opt, ... )                           \
    err = opus_multistream_encoder_ctl( msenc, opt ); \
    if( err != OPUS_OK )                              \
        return ERROR_MSG( __VA_ARGS__ )
    SET_OPT( OPUS_SET_COMPLEXITY( opus->opt.complexity ), "failed to set complexity.\n" );
    SET_OPT( OPUS_SET_BITRATE( opus->opt.bitrate ), "failed to set bitrate.\n" );
    SET_OPT( OPUS_SET_VBR( opus->opt.vbr > 0 ? 1 : 0 ), "failed to set VBR.\n" );
    SET_OPT( OPUS_SET_VBR_CONSTRAINT( opus->opt.vbr == 2 ? 1 : 0 ), "failed to set constraint VBR.\n" );
    SET_OPT( OPUS_SET_MAX_BANDWIDTH( opus->opt.max_bandwidth ), "failed to set maximum bandwidth.\n" );
#undef SET_OPT
    opus->frame_size = param->InputSampleRate * opus->opt.frame_size / 1000;
    /* Get the number of priming samples. */
    int priming_samples;
    err = opus_multistream_encoder_ctl( msenc, OPUS_GET_LOOKAHEAD( &priming_samples ) );
    if( err != OPUS_OK )
        return ERROR_MSG( "failed to get number of priming samples.\n" );
    param->PreSkip = priming_samples * (48000 / param->InputSampleRate);
    return 0;
}

static int prepare_output
(
    mp4opusenc_t *enc
)
{
    output_t      *output   = &enc->output;
    output_file_t *out_file = &output->file;
    /* Initialize L-SMASH muxer */
    output->root = lsmash_create_root();
    if( !output->root )
        return ERROR_MSG( "failed to create ROOT.\n" );
    lsmash_file_parameters_t *file_param = &out_file->param;
    if( lsmash_open_file( out_file->name, 0, file_param ) < 0 )
        return ERROR_MSG( "failed to open an output file.\n" );
    file_param->major_brand   = ISOM_BRAND_TYPE_MP42;
    file_param->brands        = (lsmash_brand_type [2]){ ISOM_BRAND_TYPE_MP42, ISOM_BRAND_TYPE_ISO2 };
    file_param->brand_count   = 2;
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
    media_param.timescale     = 48000;
    media_param.roll_grouping = 1;
    if( lsmash_set_media_parameters( output->root, out_track->track_ID, &media_param ) < 0 )
        return ERROR_MSG( "failed to set media parameters.\n" );
    /* Set up Opus configurations. */
    lsmash_audio_summary_t *in_summary = enc->input.file.movie.track.media.summaries->summary;
    lsmash_audio_summary_t *out_summary = (lsmash_audio_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_AUDIO );
    if( !out_summary )
        return ERROR_MSG( "failed to allocate summary for output.\n" );
    out_track->media.summary = out_summary;
    out_summary->sample_type = ISOM_CODEC_TYPE_OPUS_AUDIO;
    out_summary->frequency   = 48000;
    out_summary->channels    = in_summary->channels;
    out_summary->sample_size = 16;
    lsmash_codec_specific_t *cs = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_AUDIO_OPUS,
                                                                     LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
    if( !cs )
        return ERROR_MSG( "failed to create Opus specific info.\n" );
    lsmash_opus_specific_parameters_t *param = (lsmash_opus_specific_parameters_t *)cs->data.structured;
    param->version              = 0;
    param->flags                = OPUS_DSI_FLAG_PRE_SKIP_PRESENT
                                | OPUS_DSI_FLAG_INPUT_SAMPLE_RATE_PRESENT
                                | OPUS_DSI_FLAG_OUTPUT_GAIN_PRESENT;
    param->OutputChannelCount   = out_summary->channels;
    param->InputSampleRate      = in_summary->frequency;
    param->OutputGain           = 0;
    param->ChannelMappingFamily = out_summary->channels > 2 ? 1 : 0;
    param->CoupledCount         = (int []){ 0, 1, 1, 2, 2, 2, 2, 3 }[ out_summary->channels - 1 ];
    param->StreamCount          = out_summary->channels - param->CoupledCount;
    encoder_t *opus = &enc->opus;
    opus->stream_count = param->StreamCount;
    uint8_t channel_mapping[8];
    remap_channel_layout( (lsmash_summary_t *)in_summary, param, channel_mapping );
    if( setup_encoder( opus, param, channel_mapping ) < 0 )
    {
        lsmash_destroy_codec_specific_data( cs );
        return ERROR_MSG( "failed to set up encoder.\n" );
    }
    input_media_t *in_media = &enc->input.file.movie.track.media;
    uint32_t buffer_size = opus->frame_size * param->OutputChannelCount * 2;
    uint8_t *buffer      = lsmash_malloc_zero( buffer_size );
    if( !buffer )
    {
        lsmash_destroy_codec_specific_data( cs );
        return ERROR_MSG( "failed to allocate sample buffer.\n" );
    }
    in_media->buffer      = buffer;
    in_media->buffer_size = buffer_size;
    out_track->media.priming_samples  = param->PreSkip;
    out_track->media.preroll_distance = (80 - 1) / opus->opt.frame_size + 1;    /* require at least 80ms for pre-roll */
    out_track->media.sample_duration  = 48000 * opus->opt.frame_size / 1000;
    if( lsmash_add_codec_specific_data( (lsmash_summary_t *)out_summary, cs ) < 0 )
    {
        lsmash_destroy_codec_specific_data( cs );
        return ERROR_MSG( "failed to add Opus specific info.\n" );
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
}

static int get_input_packet
(
    lsmash_root_t  *in_root,
    uint32_t        in_track_ID,
    input_media_t  *in_media,
    uint32_t        packet_number,
    input_packet_t *packet
)
{
    lsmash_sample_t *sample = lsmash_get_sample_from_media_timeline( in_root, in_track_ID, packet_number );
    if( !sample )
    {
        if( lsmash_check_sample_existence_in_media_timeline( in_root, in_track_ID, packet_number ) )
            return ERROR_MSG( "failed to get sample.\n" );
        lsmash_sample_t sample_info = { 0 };
        if( lsmash_get_sample_info_from_media_timeline( in_root, in_track_ID, packet_number, &sample_info ) < 0 )
            /* No more samples. So, reached EOF. */
            return 1;
        else
            return ERROR_MSG( "failed to get sample.\n" );
    }
    else
    {
        packet->data = sample->data;
        packet->size = sample->length;
        in_media->num_samples += packet->size / in_media->summaries[0].summary->bytes_per_frame;
    }
    packet->sample = sample;
    return 0;
}

static int feed_packet_to_encoder
(
    encoder_t      *opus,
    lsmash_root_t  *out_root,
    uint32_t        out_track_ID,
    output_media_t *out_media,
    input_media_t  *in_media,
    input_packet_t *packet
)
{
    do
    {
        /* Copy data from input packet to invalid region. */
        uint8_t *invalid      = in_media->buffer      + in_media->buffer_pos;
        uint32_t invalid_size = in_media->buffer_size - in_media->buffer_pos;
        uint32_t padding_size = 0;
        if( packet->data )
        {
            uint32_t consumed_size = MP4OPUSENC_MIN( invalid_size, packet->size );
            memcpy( invalid, packet->data, consumed_size );
            in_media->buffer_pos += consumed_size;
            packet->data         += consumed_size;
            packet->size         -= consumed_size;
        }
        else
        {
            memset( invalid, 0, invalid_size );
            padding_size = invalid_size;
            in_media->buffer_pos = in_media->buffer_size;
        }
        if( in_media->buffer_pos >= in_media->buffer_size )
        {
            in_media->buffer_pos = 0;
            lsmash_sample_t *out_sample = lsmash_create_sample( (1275 * 3 + 7) * opus->stream_count );
            if( !out_sample )
                return ERROR_MSG( "failed to allocate sample.\n" );
            int ret = opus_multistream_encode( opus->msenc,
                                               (const opus_int16 *)in_media->buffer,
                                               opus->frame_size,
                                               out_sample->data,
                                               out_sample->length );
            if( ret < 0 )
            {
                lsmash_delete_sample( out_sample );
                return ERROR_MSG( "failed to encode.\n" );
            }
            else if( ret == 0 )
            {
                lsmash_delete_sample( out_sample );
                continue;
            }
            /* Feed encoded packet to muxer. */
            out_sample->length                 = ret;
            out_sample->dts                    = out_media->timestamp;
            out_sample->cts                    = out_media->timestamp;
            out_sample->index                  = out_media->sample_entry;
            out_sample->prop.ra_flags          = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
            out_sample->prop.pre_roll.distance = out_media->preroll_distance;
            if( lsmash_append_sample( out_root, out_track_ID, out_sample ) < 0 )
            {
                lsmash_delete_sample( out_sample );
                return ERROR_MSG( "failed to append sample.\n" );
            }
            if( padding_size != in_media->buffer_size )
                out_media->timestamp += out_media->sample_duration;
        }
    } while( packet->size );
    return 0;
}

static int flush_encoder
(
    encoder_t      *opus,
    lsmash_root_t  *out_root,
    uint32_t        out_track_ID,
    output_media_t *out_media,
    input_media_t  *in_media
)
{
    input_packet_t packet = { NULL };
    int ret = feed_packet_to_encoder( opus, out_root, out_track_ID, out_media, in_media, &packet );
    if( ret < 0 )
        return ret;
    if( lsmash_flush_pooled_samples( out_root, out_track_ID, out_media->sample_duration ) < 0 )
        return ERROR_MSG( "failed to flush samples.\n" );
    return 0;
}

static int do_encode
(
    mp4opusenc_t *enc
)
{
    input_t  *input     = &enc->input;
    output_t *output    = &enc->output;
    int       eof       = 0;
    for( uint32_t packet_number = 1; !eof; packet_number++ )
    {
        input_packet_t packet = { NULL };
        int ret = get_input_packet( input->root,
                                    input->file.movie.track.track_ID,
                                    &input->file.movie.track.media,
                                    packet_number,
                                    &packet );
        if( ret < 0 )
            return ret;
        eof = ret;
        ret = feed_packet_to_encoder( &enc->opus,
                                      output->root,
                                      output->file.movie.track.track_ID,
                                      &output->file.movie.track.media,
                                      &input->file.movie.track.media,
                                      &packet );
        free_input_packet( &packet );
        if( ret < 0 )
            return ret;
    }
    return flush_encoder( &enc->opus,
                          output->root,
                          output->file.movie.track.track_ID,
                          &output->file.movie.track.media,
                          &input->file.movie.track.media );
}

static int construct_timeline_maps
(
    mp4opusenc_t *enc
)
{
    output_t       *output    = &enc->output;
    output_track_t *out_track = &output->file.movie.track;
    input_media_t  *in_media  = &enc->input.file.movie.track.media;
    lsmash_edit_t edit =
    {
        .duration   = ((double)in_media->num_samples * 48000) / in_media->summaries[0].summary->frequency,
        .start_time = out_track->media.priming_samples,
        .rate       = ISOM_EDIT_MODE_NORMAL
    };
    if( lsmash_create_explicit_timeline_map( output->root, out_track->track_ID, edit ) < 0 )
        return ERROR_MSG( "failed to create explicit timeline map.\n" );
    return 0;
}

static void write_tool_indicator( lsmash_root_t *root )
{
    /* Write a tag in a free space to indicate the output file is written by this tool. */
    char *string = "Mp4OpusEnc: Don't waste your time in order to support this file!";
    int   length = strlen( string );
    lsmash_box_type_t type = lsmash_form_iso_box_type( LSMASH_4CC( 'f', 'r', 'e', 'e' ) );
    lsmash_box_t *free_box = lsmash_create_box( type, (uint8_t *)string, length, LSMASH_BOX_PRECEDENCE_N );
    if( !free_box )
    {
        ERROR_MSG( "failed to allocate the tool specific tag.\n" );
        return;
    }
    if( lsmash_add_box_ex( lsmash_root_as_box( root ), &free_box ) < 0 )
    {
        lsmash_destroy_box( free_box );
        ERROR_MSG( "failed to add the tool specific tag.\n" );
        return;
    }
    if( lsmash_write_top_level_box( free_box ) < 0 )
        ERROR_MSG( "failed to write the tool specific tag.\n" );
}

static int moov_to_front_callback
(
    void    *param,
    uint64_t written_movie_size,
    uint64_t total_movie_size
)
{
    REFRESH_CONSOLE;
    eprintf( "Finalizing: [%5.2lf%%]\r", ((double)written_movie_size / total_movie_size) * 100.0 );
    return 0;
}

static int finish_movie
(
    mp4opusenc_t *enc
)
{
    output_t *output = &enc->output;
    REFRESH_CONSOLE;
    static lsmash_adhoc_remux_t moov_to_front =
    {
        .func        = moov_to_front_callback,
        .buffer_size = 4 * 1024 * 1024, /* 4MiB */
        .param       = NULL
    };
    if( lsmash_finish_movie( output->root, &moov_to_front ) < 0 )
        return ERROR_MSG( "failed to finalize output movie.\n" );
    write_tool_indicator( output->root );
    return 0;
}

int main
(
    int   argc,
    char *argv[]
)
{
    mp4opusenc_t enc = { { 0 } };
    if( parse_options( argc, argv, &enc ) < 0 )
        return MP4OPUSENC_ERR( "failed to parse options.\n" );
    if( enc.opt.help )
    {
        display_help();
        return 0;
    }
    if( open_input_file( &enc ) < 0 )
        return MP4OPUSENC_USAGE_ERR();
    if( prepare_output( &enc ) < 0 )
        return MP4OPUSENC_ERR( "failed to set up preparation for output.\n" );
    if( do_encode( &enc ) < 0 )
        return MP4OPUSENC_ERR( "failed to encode.\n" );
    if( construct_timeline_maps( &enc ) < 0 )
        return MP4OPUSENC_ERR( "failed to construct timeline maps.\n" );
    if( finish_movie( &enc ) < 0 )
        return MP4OPUSENC_ERR( "failed to finish output movie.\n" );
    REFRESH_CONSOLE;
    eprintf( "Encoding completed!\n" );
    cleanup_mp4opusenc( &enc );
    return 0;
}
