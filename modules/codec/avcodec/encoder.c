/*****************************************************************************
 * encoder.c: video and audio encoder using the libavcodec library
 *****************************************************************************
 * Copyright (C) 1999-2004 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Gildas Bazin <gbazin@videolan.org>
 *          Christophe Massiot <massiot@via.ecp.fr>
 * Part of the file Copyright (C) FFmpeg Project Developers
 * (mpeg4_default matrixes)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_aout.h>
#include <vlc_sout.h>
#include <vlc_codec.h>
#include <vlc_dialog.h>
#include <vlc_avcodec.h>
#include <vlc_cpu.h>

#define HAVE_MMX 1
#include <libavcodec/avcodec.h>

#include "avcodec.h"
#include "avcommon.h"

#define HURRY_UP_GUARD1 (450000)
#define HURRY_UP_GUARD2 (300000)
#define HURRY_UP_GUARD3 (100000)

#define MAX_FRAME_DELAY (FF_MAX_B_FRAMES + 2)

#define RAW_AUDIO_FRAME_SIZE (2048)

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
int  OpenEncoder ( vlc_object_t * );
void CloseEncoder( vlc_object_t * );

static block_t *EncodeVideo( encoder_t *, picture_t * );
static block_t *EncodeAudio( encoder_t *, block_t * );

struct thread_context_t;

/*****************************************************************************
 * thread_context_t : for multithreaded encoding
 *****************************************************************************/
struct thread_context_t
{
    VLC_COMMON_MEMBERS

    AVCodecContext  *p_context;
    int             (* pf_func)(AVCodecContext *c, void *arg);
    void            *arg;
    int             i_ret;

    vlc_mutex_t     lock;
    vlc_cond_t      cond;
    bool            b_work, b_done;
};

/*****************************************************************************
 * encoder_sys_t : libavcodec encoder descriptor
 *****************************************************************************/
struct encoder_sys_t
{
    /*
     * libavcodec properties
     */
    AVCodec         *p_codec;
    AVCodecContext  *p_context;

    /*
     * Common buffer mainly for audio as frame size in there needs usually be constant
     */
    char *p_buffer;
    size_t i_buffer_out;

    /*
     * Video properties
     */
    mtime_t i_last_ref_pts;
    mtime_t i_buggy_pts_detect;
    mtime_t i_last_pts;
    bool    b_inited;

    /*
     * Audio properties
     */
    int i_sample_bytes;
    int i_frame_size;
    int i_samples_delay;
    mtime_t i_pts;

    /* Encoding settings */
    int        i_key_int;
    int        i_b_frames;
    int        i_vtolerance;
    int        i_qmin;
    int        i_qmax;
    int        i_hq;
    int        i_rc_buffer_size;
    float      f_rc_buffer_aggressivity;
    bool       b_pre_me;
    bool       b_hurry_up;
    bool       b_interlace, b_interlace_me;
    float      f_i_quant_factor;
    int        i_noise_reduction;
    bool       b_mpeg4_matrix;
    bool       b_trellis;
    int        i_quality; /* for VBR */
    float      f_lumi_masking, f_dark_masking, f_p_masking, f_border_masking;
    int        i_luma_elim, i_chroma_elim;
    int        i_aac_profile; /* AAC profile to use.*/
};

static const char *const ppsz_enc_options[] = {
    "keyint", "bframes", "vt", "qmin", "qmax", "codec", "hq",
    "rc-buffer-size", "rc-buffer-aggressivity", "pre-me", "hurry-up",
    "interlace", "interlace-me", "i-quant-factor", "noise-reduction", "mpeg4-matrix",
    "trellis", "qscale", "strict", "lumi-masking", "dark-masking",
    "p-masking", "border-masking", "luma-elim-threshold",
    "chroma-elim-threshold",
     "aac-profile",
     NULL
};

static const uint16_t mpa_bitrate_tab[2][15] =
{
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}
};

static const uint16_t mpa_freq_tab[6] =
{ 44100, 48000, 32000, 22050, 24000, 16000 };

static const uint16_t mpeg4_default_intra_matrix[64] = {
  8, 17, 18, 19, 21, 23, 25, 27,
 17, 18, 19, 21, 23, 25, 27, 28,
 20, 21, 22, 23, 24, 26, 28, 30,
 21, 22, 23, 24, 26, 28, 30, 32,
 22, 23, 24, 26, 28, 30, 32, 35,
 23, 24, 26, 28, 30, 32, 35, 38,
 25, 26, 28, 30, 32, 35, 38, 41,
 27, 28, 30, 32, 35, 38, 41, 45,
};

static const uint16_t mpeg4_default_non_intra_matrix[64] = {
 16, 17, 18, 19, 20, 21, 22, 23,
 17, 18, 19, 20, 21, 22, 23, 24,
 18, 19, 20, 21, 22, 23, 24, 25,
 19, 20, 21, 22, 23, 24, 26, 27,
 20, 21, 22, 23, 25, 26, 27, 28,
 21, 22, 23, 24, 26, 27, 28, 30,
 22, 23, 24, 26, 27, 28, 30, 31,
 23, 24, 25, 27, 28, 30, 31, 33,
};

/*****************************************************************************
 * OpenEncoder: probe the encoder
 *****************************************************************************/

int OpenEncoder( vlc_object_t *p_this )
{
    encoder_t *p_enc = (encoder_t *)p_this;
    encoder_sys_t *p_sys;
    AVCodecContext *p_context;
    AVCodec *p_codec = NULL;
    int i_codec_id, i_cat;
    const char *psz_namecodec;
    float f_val;
    char *psz_val;

    /* Initialization must be done before avcodec_find_encoder() */
    vlc_init_avcodec();

    config_ChainParse( p_enc, ENC_CFG_PREFIX, ppsz_enc_options, p_enc->p_cfg );

    if( p_enc->fmt_out.i_codec == VLC_CODEC_MP3 )
    {
        i_cat = AUDIO_ES;
        i_codec_id = CODEC_ID_MP3;
        psz_namecodec = "MPEG I/II Layer 3";
    }
    else if( p_enc->fmt_out.i_codec == VLC_CODEC_MP2 )
    {
        i_cat = AUDIO_ES;
        i_codec_id = CODEC_ID_MP2;
        psz_namecodec = "MPEG I/II Layer 2";
    }
    else if( p_enc->fmt_out.i_codec == VLC_CODEC_MP1V )
    {
        i_cat = VIDEO_ES;
        i_codec_id = CODEC_ID_MPEG1VIDEO;
        psz_namecodec = "MPEG-1 video";
    }
    else if( !GetFfmpegCodec( p_enc->fmt_out.i_codec, &i_cat, &i_codec_id,
                             &psz_namecodec ) )
    {
        if( FindFfmpegChroma( p_enc->fmt_out.i_codec ) == PIX_FMT_NONE )
            return VLC_EGENERIC; /* handed chroma output */

        i_cat      = VIDEO_ES;
        i_codec_id = CODEC_ID_RAWVIDEO;
        psz_namecodec = "Raw video";
    }

    if( p_enc->fmt_out.i_cat == VIDEO_ES && i_cat != VIDEO_ES )
    {
        msg_Err( p_enc, "\"%s\" is not a video encoder", psz_namecodec );
        dialog_Fatal( p_enc, _("Streaming / Transcoding failed"),
                        _("\"%s\" is no video encoder."), psz_namecodec );
        return VLC_EGENERIC;
    }

    if( p_enc->fmt_out.i_cat == AUDIO_ES && i_cat != AUDIO_ES )
    {
        msg_Err( p_enc, "\"%s\" is not an audio encoder", psz_namecodec );
        dialog_Fatal( p_enc, _("Streaming / Transcoding failed"),
                        _("\"%s\" is no audio encoder."), psz_namecodec );
        return VLC_EGENERIC;
    }

    if( p_enc->fmt_out.i_cat == SPU_ES )
    {
        /* We don't support subtitle encoding */
        return VLC_EGENERIC;
    }

    char *psz_encoder = var_GetString( p_this, ENC_CFG_PREFIX "codec" );
    if( psz_encoder && *psz_encoder )
    {
        p_codec = avcodec_find_encoder_by_name( psz_encoder );
        if( !p_codec )
            msg_Err( p_this, "Encoder `%s' not found", psz_encoder );
        else if( p_codec->id != i_codec_id )
        {
            msg_Err( p_this, "Encoder `%s' can't handle %4.4s",
                    psz_encoder, (char*)&p_enc->fmt_out.i_codec );
            p_codec = NULL;
        }
    }
    free( psz_encoder );
    if( !p_codec )
        p_codec = avcodec_find_encoder( i_codec_id );
    if( !p_codec )
    {
        msg_Err( p_enc, "cannot find encoder %s\n"
"*** Your Libav/FFmpeg installation is crippled.   ***\n"
"*** Please check with your Libav/FFmpeg packager. ***\n"
"*** This is NOT a VLC media player issue.   ***", psz_namecodec );

        dialog_Fatal( p_enc, _("Streaming / Transcoding failed"), _(
/* I have had enough of all these MPEG-3 transcoding bug reports.
 * Downstream packager, you had better not patch this out, or I will be really
 * annoyed. Think about it - you don't want to fork the VLC translation files,
 * do you? -- Courmisch, 2008-10-22 */
"It seems your Libav/FFmpeg (libavcodec) installation lacks the following encoder:\n"
"%s.\n"
"If you don't know how to fix this, ask for support from your distribution.\n"
"\n"
"This is not an error inside VLC media player.\n"
"Do not contact the VideoLAN project about this issue.\n"),
            psz_namecodec );
        return VLC_EGENERIC;
    }

    /* Allocate the memory needed to store the encoder's structure */
    if( ( p_sys = calloc( 1, sizeof(encoder_sys_t) ) ) == NULL )
        return VLC_ENOMEM;
    p_enc->p_sys = p_sys;
    p_sys->i_samples_delay = 0;
    p_sys->p_codec = p_codec;

    p_sys->p_buffer = NULL;
    p_sys->i_buffer_out = 0;

#if LIBAVCODEC_VERSION_MAJOR < 54
    p_context = avcodec_alloc_context();
#else
    p_context = avcodec_alloc_context3(p_codec);
#endif
    p_sys->p_context = p_context;
    p_sys->p_context->codec_id = p_sys->p_codec->id;
    p_context->debug = var_InheritInteger( p_enc, "avcodec-debug" );
    p_context->opaque = (void *)p_this;

    /* set CPU capabilities */
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT( 51, 25, 0 )
    av_set_cpu_flags_mask( INT_MAX & ~GetVlcDspMask() );
#else
    p_context->dsp_mask = GetVlcDspMask();
#endif

    p_sys->i_key_int = var_GetInteger( p_enc, ENC_CFG_PREFIX "keyint" );
    p_sys->i_b_frames = var_GetInteger( p_enc, ENC_CFG_PREFIX "bframes" );
    p_sys->i_vtolerance = var_GetInteger( p_enc, ENC_CFG_PREFIX "vt" ) * 1000;
    p_sys->b_interlace = var_GetBool( p_enc, ENC_CFG_PREFIX "interlace" );
    p_sys->b_interlace_me = var_GetBool( p_enc, ENC_CFG_PREFIX "interlace-me" );
    p_sys->b_pre_me = var_GetBool( p_enc, ENC_CFG_PREFIX "pre-me" );
    p_sys->b_hurry_up = var_GetBool( p_enc, ENC_CFG_PREFIX "hurry-up" );

    if( p_sys->b_hurry_up )
    {
        /* hurry up mode needs noise reduction, even small */
        p_sys->i_noise_reduction = 1;
    }

    p_sys->i_rc_buffer_size = var_GetInteger( p_enc, ENC_CFG_PREFIX "rc-buffer-size" );
    p_sys->f_rc_buffer_aggressivity = var_GetFloat( p_enc, ENC_CFG_PREFIX "rc-buffer-aggressivity" );
    p_sys->f_i_quant_factor = var_GetFloat( p_enc, ENC_CFG_PREFIX "i-quant-factor" );
    p_sys->i_noise_reduction = var_GetInteger( p_enc, ENC_CFG_PREFIX "noise-reduction" );
    p_sys->b_mpeg4_matrix = var_GetBool( p_enc, ENC_CFG_PREFIX "mpeg4-matrix" );

    f_val = var_GetFloat( p_enc, ENC_CFG_PREFIX "qscale" );
    if( f_val < 0.01 || f_val > 255.0 ) f_val = 0;
    p_sys->i_quality = (int)(FF_QP2LAMBDA * f_val + 0.5);

    psz_val = var_GetString( p_enc, ENC_CFG_PREFIX "hq" );
    p_sys->i_hq = FF_MB_DECISION_RD;
    if( psz_val && *psz_val )
    {
        if( !strcmp( psz_val, "rd" ) )
            p_sys->i_hq = FF_MB_DECISION_RD;
        else if( !strcmp( psz_val, "bits" ) )
            p_sys->i_hq = FF_MB_DECISION_BITS;
        else if( !strcmp( psz_val, "simple" ) )
            p_sys->i_hq = FF_MB_DECISION_SIMPLE;
        else
            p_sys->i_hq = FF_MB_DECISION_RD;
    }
    else
        p_sys->i_hq = FF_MB_DECISION_RD;
    free( psz_val );

    p_sys->i_qmin = var_GetInteger( p_enc, ENC_CFG_PREFIX "qmin" );
    p_sys->i_qmax = var_GetInteger( p_enc, ENC_CFG_PREFIX "qmax" );
    p_sys->b_trellis = var_GetBool( p_enc, ENC_CFG_PREFIX "trellis" );

    p_context->strict_std_compliance = var_GetInteger( p_enc, ENC_CFG_PREFIX "strict" );

    p_sys->f_lumi_masking = var_GetFloat( p_enc, ENC_CFG_PREFIX "lumi-masking" );
    p_sys->f_dark_masking = var_GetFloat( p_enc, ENC_CFG_PREFIX "dark-masking" );
    p_sys->f_p_masking = var_GetFloat( p_enc, ENC_CFG_PREFIX "p-masking" );
    p_sys->f_border_masking = var_GetFloat( p_enc, ENC_CFG_PREFIX "border-masking" );
    p_sys->i_luma_elim = var_GetInteger( p_enc, ENC_CFG_PREFIX "luma-elim-threshold" );
    p_sys->i_chroma_elim = var_GetInteger( p_enc, ENC_CFG_PREFIX "chroma-elim-threshold" );

    psz_val = var_GetString( p_enc, ENC_CFG_PREFIX "aac-profile" );
    /* libavcodec uses faac encoder atm, and it has issues with
     * other than low-complexity profile, so default to that */
    p_sys->i_aac_profile = FF_PROFILE_AAC_LOW;
    if( psz_val && *psz_val )
    {
        if( !strncmp( psz_val, "main", 4 ) )
            p_sys->i_aac_profile = FF_PROFILE_AAC_MAIN;
        else if( !strncmp( psz_val, "low", 3 ) )
            p_sys->i_aac_profile = FF_PROFILE_AAC_LOW;
#if 0    /* Not supported by FAAC encoder */
        else if( !strncmp( psz_val, "ssr", 3 ) )
            p_sys->i_aac_profile = FF_PROFILE_AAC_SSR;
#endif
        else if( !strncmp( psz_val, "ltp", 3 ) )
            p_sys->i_aac_profile = FF_PROFILE_AAC_LTP;
#if LIBAVCODEC_VERSION_CHECK( 54, 19, 0, 35, 100 )
/* These require libavcodec with libfdk-aac */
        else if( !strncmp( psz_val, "hev2", 4 ) )
            p_sys->i_aac_profile = FF_PROFILE_AAC_HE_V2;
        else if( !strncmp( psz_val, "hev1", 4 ) )
            p_sys->i_aac_profile = FF_PROFILE_AAC_HE;
#endif
        else
        {
            msg_Warn( p_enc, "unknown AAC profile requested, setting it to low" );
            p_sys->i_aac_profile = FF_PROFILE_AAC_LOW;
        }
    }
    free( psz_val );

    if( p_enc->fmt_in.i_cat == VIDEO_ES )
    {
        if( !p_enc->fmt_in.video.i_width || !p_enc->fmt_in.video.i_height )
        {
            msg_Warn( p_enc, "invalid size %ix%i", p_enc->fmt_in.video.i_width,
                      p_enc->fmt_in.video.i_height );
            free( p_sys );
            return VLC_EGENERIC;
        }

        p_context->codec_type = AVMEDIA_TYPE_VIDEO;

        p_context->width = p_enc->fmt_in.video.i_width;
        p_context->height = p_enc->fmt_in.video.i_height;

        p_context->time_base.num = p_enc->fmt_in.video.i_frame_rate_base;
        p_context->time_base.den = p_enc->fmt_in.video.i_frame_rate;
        if( p_codec->supported_framerates )
        {
            AVRational target = {
                .num = p_enc->fmt_in.video.i_frame_rate,
                .den = p_enc->fmt_in.video.i_frame_rate_base,
            };
            int idx = av_find_nearest_q_idx(target, p_codec->supported_framerates);

            p_context->time_base.num = p_codec->supported_framerates[idx].den;
            p_context->time_base.den = p_codec->supported_framerates[idx].num;
        }

        /* Defaults from ffmpeg.c */
        p_context->qblur = 0.5;
        p_context->qcompress = 0.5;
        p_context->b_quant_offset = 1.25;
        p_context->b_quant_factor = 1.25;
        p_context->i_quant_offset = 0.0;
        p_context->i_quant_factor = -0.8;

        p_context->lumi_masking = p_sys->f_lumi_masking;
        p_context->dark_masking = p_sys->f_dark_masking;
        p_context->p_masking = p_sys->f_p_masking;
        p_context->border_masking = p_sys->f_border_masking;
        p_context->luma_elim_threshold = p_sys->i_luma_elim;
        p_context->chroma_elim_threshold = p_sys->i_chroma_elim;

        if( p_sys->i_key_int > 0 )
            p_context->gop_size = p_sys->i_key_int;
        p_context->max_b_frames =
            VLC_CLIP( p_sys->i_b_frames, 0, FF_MAX_B_FRAMES );
        p_context->b_frame_strategy = 0;
        if( !p_context->max_b_frames  &&
            (  p_enc->fmt_out.i_codec == VLC_CODEC_MPGV ||
               p_enc->fmt_out.i_codec == VLC_CODEC_MP2V ) )
            p_context->flags |= CODEC_FLAG_LOW_DELAY;

        av_reduce( &p_context->sample_aspect_ratio.num,
                   &p_context->sample_aspect_ratio.den,
                   p_enc->fmt_in.video.i_sar_num,
                   p_enc->fmt_in.video.i_sar_den, 1 << 30 );


        p_enc->fmt_in.i_codec = VLC_CODEC_I420;
        p_enc->fmt_in.video.i_chroma = p_enc->fmt_in.i_codec;
        GetFfmpegChroma( &p_context->pix_fmt, &p_enc->fmt_in.video );

        if( p_codec->pix_fmts )
        {
            const enum PixelFormat *p = p_codec->pix_fmts;
            for( ; *p != -1; p++ )
            {
                if( *p == p_context->pix_fmt ) break;
            }
            if( *p == -1 ) p_context->pix_fmt = p_codec->pix_fmts[0];
            GetVlcChroma( &p_enc->fmt_in.video, p_context->pix_fmt );
            p_enc->fmt_in.i_codec = p_enc->fmt_in.video.i_chroma;
        }


        if ( p_sys->f_i_quant_factor != 0.0 )
            p_context->i_quant_factor = p_sys->f_i_quant_factor;

        p_context->noise_reduction = p_sys->i_noise_reduction;

        if ( p_sys->b_mpeg4_matrix )
        {
            p_context->intra_matrix = mpeg4_default_intra_matrix;
            p_context->inter_matrix = mpeg4_default_non_intra_matrix;
        }

        if ( p_sys->b_pre_me )
        {
            p_context->pre_me = 1;
            p_context->me_pre_cmp = FF_CMP_CHROMA;
        }

        if ( p_sys->b_interlace )
        {
            if ( p_context->height <= 280 )
            {
                if ( p_context->height != 16 || p_context->width != 16 )
                    msg_Warn( p_enc,
                        "disabling interlaced video because height=%d <= 280",
                        p_context->height );
            }
            else
            {
                p_context->flags |= CODEC_FLAG_INTERLACED_DCT;
                if ( p_sys->b_interlace_me )
                    p_context->flags |= CODEC_FLAG_INTERLACED_ME;
            }
        }

        p_context->trellis = p_sys->b_trellis;

        if ( p_sys->i_qmin > 0 && p_sys->i_qmin == p_sys->i_qmax )
            p_context->flags |= CODEC_FLAG_QSCALE;
        /* These codecs cause libavcodec to exit if thread_count is > 1.
           See libavcodec/mpegvideo_enc.c:MPV_encode_init and
           libavcodec/svq3.c , WMV2 calls MPV_encode_init also.
         */
        if ( i_codec_id == CODEC_ID_FLV1 ||
             i_codec_id == CODEC_ID_H261 ||
             i_codec_id == CODEC_ID_LJPEG ||
             i_codec_id == CODEC_ID_MJPEG ||
             i_codec_id == CODEC_ID_H263 ||
             i_codec_id == CODEC_ID_H263P ||
             i_codec_id == CODEC_ID_MSMPEG4V1 ||
             i_codec_id == CODEC_ID_MSMPEG4V2 ||
             i_codec_id == CODEC_ID_MSMPEG4V3 ||
             i_codec_id == CODEC_ID_WMV1 ||
             i_codec_id == CODEC_ID_WMV2 ||
             i_codec_id == CODEC_ID_RV10 ||
             i_codec_id == CODEC_ID_RV20 ||
             i_codec_id == CODEC_ID_SVQ3 )
            p_enc->i_threads = 1;

        if( p_sys->i_vtolerance > 0 )
            p_context->bit_rate_tolerance = p_sys->i_vtolerance;

        /* usually if someone sets bitrate, he likes more to get that bitrate
         * over quality should help 'normal' user to get asked bitrate
         */
        if( p_enc->fmt_out.i_bitrate > 0 && p_sys->i_qmax == 0 && p_sys->i_qmin == 0 )
        {
            p_sys->i_qmax = 51;
            p_sys->i_qmin = 3;
        }

        if( p_sys->i_qmin > 0 )
        {
            p_context->qmin = p_sys->i_qmin;
            p_context->mb_lmin = p_context->lmin = p_sys->i_qmin * FF_QP2LAMBDA;
        }
        if( p_sys->i_qmax > 0 )
        {
            p_context->qmax = p_sys->i_qmax;
            p_context->mb_lmax = p_context->lmax = p_sys->i_qmax * FF_QP2LAMBDA;
        }
        p_context->max_qdiff = 3;

        p_context->mb_decision = p_sys->i_hq;

        if( p_sys->i_quality )
        {
            p_context->flags |= CODEC_FLAG_QSCALE;
            p_context->global_quality = p_sys->i_quality;
        }
        else
        {
            p_context->rc_qsquish = 1.0;
            if( p_sys->i_rc_buffer_size )
            {
                p_context->rc_max_rate = p_enc->fmt_out.i_bitrate;
                p_context->rc_min_rate = p_enc->fmt_out.i_bitrate;
            }
            p_context->rc_buffer_size = p_sys->i_rc_buffer_size;
            /* This is from ffmpeg's ffmpeg.c : */
            p_context->rc_initial_buffer_occupancy
                = p_sys->i_rc_buffer_size * 3/4;
            p_context->rc_buffer_aggressivity = p_sys->f_rc_buffer_aggressivity;
        }
    }
    else if( p_enc->fmt_in.i_cat == AUDIO_ES )
    {
        /* work around bug in libmp3lame encoding */
        if( i_codec_id == CODEC_ID_MP3 && p_enc->fmt_in.audio.i_channels > 2 )
            p_enc->fmt_in.audio.i_channels = 2;

        p_context->codec_type  = AVMEDIA_TYPE_AUDIO;
        p_context->sample_fmt  = p_codec->sample_fmts ?
                                    p_codec->sample_fmts[0] :
                                    AV_SAMPLE_FMT_S16;

        // Try if we can use interleaved format for codec input as VLC doesn't really do planar audio yet
        // FIXME: Remove when planar/interleaved audio in vlc is equally supported
        if( av_sample_fmt_is_planar( p_context->sample_fmt ) )
        {
            msg_Dbg( p_enc, "Trying to find packet sample format instead of %s", av_get_sample_fmt_name( p_context->sample_fmt ) );
            for( unsigned int i=0; p_codec->sample_fmts[i] != -1; i++ )
            {
                if( !av_sample_fmt_is_planar( p_codec->sample_fmts[i] ) )
                {
                    p_context->sample_fmt = p_codec->sample_fmts[i];
                    msg_Dbg( p_enc, "Using %s as new sample format", av_get_sample_fmt_name( p_context->sample_fmt ) );
                    break;
                }
            }
        }
        p_enc->fmt_in.i_codec  = GetVlcAudioFormat( p_context->sample_fmt );

        p_context->sample_rate = p_enc->fmt_out.audio.i_rate;
        p_context->time_base.num = 1;
        p_context->time_base.den = p_context->sample_rate;
        p_context->channels    = p_enc->fmt_out.audio.i_channels;

        if ( p_enc->fmt_out.i_codec == VLC_CODEC_MP4A )
        {
            /* XXX: FAAC does resample only when setting the INPUT samplerate
             * to the desired value (-R option of the faac frontend)
            p_enc->fmt_in.audio.i_rate = p_context->sample_rate;*/
            /* vlc should default to low-complexity profile, faac encoder
             * has bug and aac audio has issues otherwise atm */
            p_context->profile = p_sys->i_aac_profile;
        }
    }

    /* Misc parameters */
    p_context->bit_rate = p_enc->fmt_out.i_bitrate;

    /* Set reasonable defaults to VP8, based on
       libvpx-720p preset from libvpx ffmpeg-patch */
    if( i_codec_id == CODEC_ID_VP8 )
    {
        /* Lets give bitrate tolerance */
        p_context->bit_rate_tolerance = __MAX(2 * (int)p_enc->fmt_out.i_bitrate, p_sys->i_vtolerance );
        /* default to 120 frames between keyframe */
        if( !var_GetInteger( p_enc, ENC_CFG_PREFIX "keyint" ) )
            p_context->gop_size = 120;
        /* Don't set rc-values atm, they were from time before
           libvpx was officially in FFmpeg */
        //p_context->rc_max_rate = 24 * 1000 * 1000; //24M
        //p_context->rc_min_rate = 40 * 1000; // 40k
        /* seems that FFmpeg presets have 720p as divider for buffers */
        if( p_enc->fmt_out.video.i_height >= 720 )
        {
            /* Check that we don't overrun users qmin/qmax values */
            if( !var_GetInteger( p_enc, ENC_CFG_PREFIX "qmin" ) )
            {
                p_context->qmin = 10;
                p_context->mb_lmin = p_context->lmin = 10 * FF_QP2LAMBDA;
            }

            if( !var_GetInteger( p_enc, ENC_CFG_PREFIX "qmax" ) )
            {
                p_context->qmax = 42;
                p_context->mb_lmax = p_context->lmax = 42 * FF_QP2LAMBDA;
            }

            } else {
            if( !var_GetInteger( p_enc, ENC_CFG_PREFIX "qmin" ) )
            {
                p_context->qmin = 1;
                p_context->mb_lmin = p_context->lmin = FF_QP2LAMBDA;
            }
        }


#if 0 /* enable when/if vp8 encoder is accepted in libavcodec */
        p_context->lag = 16;
        p_context->level = 216;
        p_context->profile = 0;
        p_context->rc_buffer_aggressivity = 0.95;
        p_context->token_partitions = 4;
        p_context->mb_static_threshold = 0;
#endif
    }

    if( i_codec_id == CODEC_ID_RAWVIDEO )
    {
        /* XXX: hack: Force same codec (will be handled by transcode) */
        p_enc->fmt_in.video.i_chroma = p_enc->fmt_in.i_codec = p_enc->fmt_out.i_codec;
        GetFfmpegChroma( &p_context->pix_fmt, &p_enc->fmt_in.video );
    }

    /* Make sure we get extradata filled by the encoder */
    p_context->extradata_size = 0;
    p_context->extradata = NULL;
    p_context->flags |= CODEC_FLAG_GLOBAL_HEADER;

    if( p_enc->i_threads >= 1)
        p_context->thread_count = p_enc->i_threads;
    else
        p_context->thread_count = vlc_GetCPUCount();

    int ret;
    vlc_avcodec_lock();
#if LIBAVCODEC_VERSION_MAJOR < 54
    ret = avcodec_open( p_context, p_codec );
#else
    ret = avcodec_open2( p_context, p_codec, NULL /* options */ );
#endif
    vlc_avcodec_unlock();
    if( ret )
    {
        if( p_enc->fmt_in.i_cat == AUDIO_ES &&
             (p_context->channels > 2 || i_codec_id == CODEC_ID_MP2
               || i_codec_id == CODEC_ID_MP3) )
        {
            if( p_context->channels > 2 )
            {
                p_context->channels = 2;
                p_enc->fmt_in.audio.i_channels = 2; // FIXME
                msg_Warn( p_enc, "stereo mode selected (codec limitation)" );
            }

            if( i_codec_id == CODEC_ID_MP2 || i_codec_id == CODEC_ID_MP3 )
            {
                int i_frequency, i;

                for ( i_frequency = 0; i_frequency < 6; i_frequency++ )
                {
                    if ( p_enc->fmt_out.audio.i_rate
                            == mpa_freq_tab[i_frequency] )
                        break;
                }
                if ( i_frequency == 6 )
                {
                    msg_Err( p_enc, "MPEG audio doesn't support frequency=%d",
                             p_enc->fmt_out.audio.i_rate );
                    free( p_sys );
                    return VLC_EGENERIC;
                }

                for ( i = 1; i < 14; i++ )
                {
                    if ( p_enc->fmt_out.i_bitrate / 1000
                          <= mpa_bitrate_tab[i_frequency / 3][i] )
                        break;
                }
                if ( p_enc->fmt_out.i_bitrate / 1000
                      != mpa_bitrate_tab[i_frequency / 3][i] )
                {
                    msg_Warn( p_enc,
                              "MPEG audio doesn't support bitrate=%d, using %d",
                              p_enc->fmt_out.i_bitrate,
                              mpa_bitrate_tab[i_frequency / 3][i] * 1000 );
                    p_enc->fmt_out.i_bitrate =
                        mpa_bitrate_tab[i_frequency / 3][i] * 1000;
                    p_context->bit_rate = p_enc->fmt_out.i_bitrate;
                }
            }

            p_context->codec = NULL;
            vlc_avcodec_lock();
#if LIBAVCODEC_VERSION_MAJOR < 54
            ret = avcodec_open( p_context, p_codec );
#else
            ret = avcodec_open2( p_context, p_codec, NULL /* options */ );
#endif
            vlc_avcodec_unlock();
            if( ret )
            {
                msg_Err( p_enc, "cannot open encoder" );
                dialog_Fatal( p_enc,
                                _("Streaming / Transcoding failed"),
                                "%s", _("VLC could not open the encoder.") );
                free( p_sys );
                return VLC_EGENERIC;
            }
        }
        else
        {
            msg_Err( p_enc, "cannot open encoder" );
            dialog_Fatal( p_enc, _("Streaming / Transcoding failed"),
                            "%s", _("VLC could not open the encoder.") );
            free( p_sys );
            return VLC_EGENERIC;
        }
    }

    if( i_codec_id == CODEC_ID_FLAC )
    {
        p_enc->fmt_out.i_extra = 4 + 1 + 3 + p_context->extradata_size;
        p_enc->fmt_out.p_extra = malloc( p_enc->fmt_out.i_extra );
        if( p_enc->fmt_out.p_extra )
        {
            uint8_t *p = p_enc->fmt_out.p_extra;
            p[0] = 0x66;    /* f */
            p[1] = 0x4C;    /* L */
            p[2] = 0x61;    /* a */
            p[3] = 0x43;    /* C */
            p[4] = 0x80;    /* streaminfo block, last block before audio */
            p[5] = ( p_context->extradata_size >> 16 ) & 0xff;
            p[6] = ( p_context->extradata_size >>  8 ) & 0xff;
            p[7] = ( p_context->extradata_size       ) & 0xff;
            memcpy( &p[8], p_context->extradata, p_context->extradata_size );
        }
        else
        {
            p_enc->fmt_out.i_extra = 0;
        }
    }
    else
    {
        p_enc->fmt_out.i_extra = p_context->extradata_size;
        if( p_enc->fmt_out.i_extra )
        {
            p_enc->fmt_out.p_extra = malloc( p_enc->fmt_out.i_extra );
            if ( p_enc->fmt_out.p_extra == NULL )
            {
                goto error;
            }
            memcpy( p_enc->fmt_out.p_extra, p_context->extradata,
                    p_enc->fmt_out.i_extra );
        }
    }

    p_context->flags &= ~CODEC_FLAG_GLOBAL_HEADER;

    if( p_enc->fmt_in.i_cat == AUDIO_ES )
    {
        p_enc->fmt_in.i_codec = GetVlcAudioFormat( p_sys->p_context->sample_fmt );
        p_enc->fmt_in.audio.i_bitspersample = aout_BitsPerSample( p_enc->fmt_in.i_codec );

        p_sys->i_sample_bytes = (p_enc->fmt_in.audio.i_bitspersample / 8) *
                                p_context->channels;
        p_sys->i_frame_size = p_context->frame_size > 1 ?
                                    p_context->frame_size :
                                    RAW_AUDIO_FRAME_SIZE;
        p_sys->p_buffer = malloc( p_sys->i_frame_size * p_sys->i_sample_bytes );
        if ( p_sys->p_buffer == NULL )
        {
            goto error;
        }
        p_enc->fmt_out.audio.i_blockalign = p_context->block_align;
        p_enc->fmt_out.audio.i_bitspersample = aout_BitsPerSample( p_enc->fmt_out.i_codec );

        if( p_context->frame_size > 1 )
            p_sys->i_buffer_out = 8 * AVCODEC_MAX_AUDIO_FRAME_SIZE;
        else
            p_sys->i_buffer_out = p_sys->i_frame_size * p_sys->i_sample_bytes;
    }

    msg_Dbg( p_enc, "found encoder %s", psz_namecodec );
    p_enc->pf_encode_video = EncodeVideo;
    p_enc->pf_encode_audio = EncodeAudio;


    return VLC_SUCCESS;
error:
    free( p_enc->fmt_out.p_extra );
    free( p_sys->p_buffer );
    free( p_sys );
    return VLC_ENOMEM;
}

/****************************************************************************
 * EncodeVideo: the whole thing
 ****************************************************************************/
static block_t *EncodeVideo( encoder_t *p_enc, picture_t *p_pict )
{
    encoder_sys_t *p_sys = p_enc->p_sys;
    int i_out, i_plane;

    /* Initialize the video output buffer the first time.
     * This is done here instead of OpenEncoder() because we need the actual
     * bits_per_pixel value, without having to assume anything.
     */
    const int bytesPerPixel = p_enc->fmt_out.video.i_bits_per_pixel ?
                         p_enc->fmt_out.video.i_bits_per_pixel / 8 : 3;
    const int blocksize = __MAX( FF_MIN_BUFFER_SIZE,bytesPerPixel * p_sys->p_context->height * p_sys->p_context->width + 200 );
    block_t *p_block = block_Alloc( blocksize );

    if( likely(p_pict) ) {
        AVFrame *frame;
        frame = avcodec_alloc_frame();
        for( i_plane = 0; i_plane < p_pict->i_planes; i_plane++ )
        {
            frame->data[i_plane] = p_pict->p[i_plane].p_pixels;
            frame->linesize[i_plane] = p_pict->p[i_plane].i_pitch;
        }

        /* Let libavcodec select the frame type */
        frame->pict_type = 0;

        frame->repeat_pict = p_pict->i_nb_fields - 2;
        frame->interlaced_frame = !p_pict->b_progressive;
        frame->top_field_first = !!p_pict->b_top_field_first;

        /* Set the pts of the frame being encoded */
        frame->pts = p_pict->date ? p_pict->date : (int64_t)AV_NOPTS_VALUE;

        if ( p_sys->b_hurry_up && frame->pts != (int64_t)AV_NOPTS_VALUE )
        {
            mtime_t current_date = mdate();

            if ( current_date + HURRY_UP_GUARD3 > frame->pts )
            {
                p_sys->p_context->mb_decision = FF_MB_DECISION_SIMPLE;
                p_sys->p_context->trellis = 0;
                msg_Dbg( p_enc, "hurry up mode 3" );
            }
            else
            {
                p_sys->p_context->mb_decision = p_sys->i_hq;

                if ( current_date + HURRY_UP_GUARD2 > frame->pts )
                {
                    p_sys->p_context->trellis = 0;
                    p_sys->p_context->noise_reduction = p_sys->i_noise_reduction
                        + (HURRY_UP_GUARD2 + current_date - frame->pts) / 500;
                    msg_Dbg( p_enc, "hurry up mode 2" );
                }
                else
                {
                    p_sys->p_context->trellis = p_sys->b_trellis;

                    p_sys->p_context->noise_reduction =
                       p_sys->i_noise_reduction;
                }
            }

            if ( current_date + HURRY_UP_GUARD1 > frame->pts )
            {
                frame->pict_type = AV_PICTURE_TYPE_P;
                /* msg_Dbg( p_enc, "hurry up mode 1 %lld", current_date + HURRY_UP_GUARD1 - frame.pts ); */
            }
        }

        if ( frame->pts != (int64_t)AV_NOPTS_VALUE && frame->pts != 0 )
        {
            if ( p_sys->i_last_pts == frame->pts )
            {
                msg_Warn( p_enc, "almost fed libavcodec with two frames with the "
                         "same PTS (%"PRId64 ")", frame->pts );
                av_freep( &frame );
                return NULL;
            }
            else if ( p_sys->i_last_pts > frame->pts )
            {
                msg_Warn( p_enc, "almost fed libavcodec with a frame in the "
                         "past (current: %"PRId64 ", last: %"PRId64")",
                         frame->pts, p_sys->i_last_pts );
                av_freep( &frame );
                return NULL;
            }
            else
            {
                p_sys->i_last_pts = frame->pts;
            }
        }

        frame->quality = p_sys->i_quality;

        i_out = avcodec_encode_video( p_sys->p_context, p_block->p_buffer,
                                     p_block->i_buffer, frame );
        av_freep( &frame );
    }
    else
    {
        i_out = avcodec_encode_video( p_sys->p_context, p_block->p_buffer,
                                     p_block->i_buffer, NULL);
    }

    if( i_out <= 0 )
    {
        block_Release( p_block );
        return NULL;
    }

    p_block->i_buffer = i_out;

    /* FIXME, 3-2 pulldown is not handled correctly */
    p_block->i_length = INT64_C(1000000) *
        p_enc->fmt_in.video.i_frame_rate_base /
            p_enc->fmt_in.video.i_frame_rate;

    if( !p_sys->p_context->max_b_frames || !p_sys->p_context->delay )
    {
        /* No delay -> output pts == input pts */
        if( p_pict )
            p_block->i_dts = p_pict->date;
        p_block->i_pts = p_block->i_dts;
    }
    else if( p_sys->p_context->coded_frame->pts != (int64_t)AV_NOPTS_VALUE &&
        p_sys->p_context->coded_frame->pts != 0 &&
        p_sys->i_buggy_pts_detect != p_sys->p_context->coded_frame->pts )
    {
        p_sys->i_buggy_pts_detect = p_sys->p_context->coded_frame->pts;
        p_block->i_pts = p_sys->p_context->coded_frame->pts;

        if( p_sys->p_context->coded_frame->pict_type != AV_PICTURE_TYPE_I &&
            p_sys->p_context->coded_frame->pict_type != AV_PICTURE_TYPE_P )
        {
            p_block->i_dts = p_block->i_pts;
        }
        else
        {
            if( p_sys->i_last_ref_pts )
            {
                p_block->i_dts = p_sys->i_last_ref_pts;
            }
            else
            {
                /* Let's put something sensible */
                p_block->i_dts = p_block->i_pts;
            }

            p_sys->i_last_ref_pts = p_block->i_pts;
        }
    }
    else if( p_pict )
    {
        /* Buggy libavcodec which doesn't update coded_frame->pts
         * correctly */
        p_block->i_dts = p_block->i_pts = p_pict->date;
    }

    switch ( p_sys->p_context->coded_frame->pict_type )
    {
    case AV_PICTURE_TYPE_I:
        p_block->i_flags |= BLOCK_FLAG_TYPE_I;
        break;
    case AV_PICTURE_TYPE_P:
        p_block->i_flags |= BLOCK_FLAG_TYPE_P;
        break;
    case AV_PICTURE_TYPE_B:
        p_block->i_flags |= BLOCK_FLAG_TYPE_B;
        break;

    }

    return p_block;
}

/****************************************************************************
 * EncodeAudio: the whole thing
 ****************************************************************************/
static block_t *EncodeAudio( encoder_t *p_enc, block_t *p_aout_buf )
{
    encoder_sys_t *p_sys = p_enc->p_sys;

    block_t *p_block, *p_chain = NULL;
    AVFrame *frame=NULL;
    int got_packet,i_out,i_samples_left=0,i_data_offset = 0;

    //i_samples_left is amount of samples we get
    i_samples_left = p_aout_buf ? p_aout_buf->i_nb_samples : 0;

    if( p_sys->i_samples_delay > 0 )
    {
        AVPacket packet;
        //How much we need to copy from new packet
        const int leftover = __MAX(0,__MIN(i_samples_left, (p_sys->i_frame_size - p_sys->i_samples_delay)));

        frame = avcodec_alloc_frame();
        frame->nb_samples = p_sys->i_samples_delay + leftover;
        frame->format     = p_sys->p_context->sample_fmt;

        //Copy samples from new packet to buffer to get frame size
        if( likely( leftover ) )
        {
            if( av_sample_fmt_is_planar( p_sys->p_context->sample_fmt ) )
                aout_Deinterleave( p_sys->p_buffer+(p_sys->i_samples_delay*p_sys->i_sample_bytes*p_enc->fmt_in.audio.i_channels ),
                                  p_aout_buf->p_buffer, leftover, p_enc->fmt_in.audio.i_channels, p_enc->fmt_in.i_codec );
            else
                memcpy( p_sys->p_buffer+(p_sys->i_samples_delay*p_sys->i_sample_bytes),
                        p_aout_buf->p_buffer,
                        leftover * p_sys->i_sample_bytes * p_enc->fmt_in.audio.i_channels
                       );
        }

        if( avcodec_fill_audio_frame( frame, p_enc->fmt_in.audio.i_channels,
                              p_sys->p_context->sample_fmt,
                              p_sys->p_buffer,
                              (p_sys->i_samples_delay + leftover) * p_sys->i_sample_bytes * p_enc->fmt_in.audio.i_channels,
                              0) < 0 )
            msg_Err( p_enc, "Filling on leftovers error i_leftover %d i_samples_left %d samples_delay %d frame size %d", leftover, i_samples_left, p_sys->i_samples_delay, p_sys->i_frame_size );

        if( likely( p_aout_buf ) )
            frame->pts = p_aout_buf->i_pts -
                     (mtime_t)1000000 * (mtime_t)p_sys->i_samples_delay /
                     (mtime_t)p_enc->fmt_in.audio.i_rate;

        p_sys->i_samples_delay = 0;
        i_data_offset += leftover * p_sys->i_sample_bytes * p_enc->fmt_in.audio.i_channels;
        i_samples_left -= leftover;

        p_block = block_Alloc( p_sys->i_buffer_out );
        av_init_packet( &packet );
        packet.data = p_block->p_buffer;
        packet.size = p_block->i_buffer;

        i_out = avcodec_encode_audio2( p_sys->p_context, &packet, frame, &got_packet );
        p_block->i_buffer = packet.size;


        /*FIXME: same as avcodec_free_frame, but we don't require so new avcodec that has it*/
        if( frame->extended_data != frame->data )
           av_freep( frame->extended_data );
        av_freep( &frame );
        if( unlikely( !got_packet || ( i_out < 0 ) ) )
        {
            if( i_out < 0 )
            {
                msg_Err( p_enc,"Encoding problem...");
                return p_chain;
            }
        } else {
            p_block->i_buffer = packet.size;

            p_block->i_length = (mtime_t)1000000 *
             (mtime_t)p_sys->i_frame_size /
             (mtime_t)p_sys->p_context->sample_rate;

            p_block->i_dts = p_block->i_pts = packet.pts;

            block_ChainAppend( &p_chain, p_block );
        }
    }

    if( unlikely( !p_aout_buf ) )
    {
        msg_Dbg(p_enc,"Flushing..");
        do {
            AVPacket packet;
            p_block = block_Alloc( p_sys->i_buffer_out );
            av_init_packet( &packet );
            packet.data = p_block->p_buffer;
            packet.size = p_block->i_buffer;

            i_out = avcodec_encode_audio2( p_sys->p_context, &packet, NULL, &got_packet );
            p_block->i_buffer = packet.size;

            p_block->i_length = (mtime_t)1000000 *
             (mtime_t)p_sys->i_frame_size /
             (mtime_t)p_sys->p_context->sample_rate;

            p_block->i_dts = p_block->i_pts = packet.pts;

            if( !i_out && got_packet )
                block_ChainAppend( &p_chain, p_block );
        } while( got_packet && !i_out );
        return p_chain;
    }

    while( i_samples_left >= p_sys->i_frame_size )
    {
        AVPacket packet = {0};
        frame = avcodec_alloc_frame();

        frame->nb_samples = p_sys->i_frame_size;
        frame->format     = p_sys->p_context->sample_fmt;

        if( av_sample_fmt_is_planar( p_sys->p_context->sample_fmt ) )
        {
            aout_Deinterleave( p_sys->p_buffer, p_aout_buf->p_buffer+i_data_offset,
                              p_sys->i_frame_size, p_enc->fmt_in.audio.i_channels, p_enc->fmt_in.i_codec );
            if( avcodec_fill_audio_frame( frame, p_enc->fmt_in.audio.i_channels,
                                    p_sys->p_context->sample_fmt,
                                    p_sys->p_buffer,
                                    p_sys->i_frame_size * p_sys->i_sample_bytes * p_enc->fmt_in.audio.i_channels,
                                    0) < 0 )
                 msg_Err( p_enc, "filling error on encode" );
        } else {
            if( avcodec_fill_audio_frame( frame, p_enc->fmt_in.audio.i_channels,
                                    p_sys->p_context->sample_fmt,
                                    p_aout_buf->p_buffer+i_data_offset,
                                    p_sys->i_frame_size * p_sys->i_sample_bytes * p_enc->fmt_in.audio.i_channels,
                                    0) < 0 )
                 msg_Err( p_enc, "filling error on encode" );
        }

        i_samples_left -= p_sys->i_frame_size;
        i_data_offset += p_sys->i_frame_size * p_sys->i_sample_bytes * p_enc->fmt_in.audio.i_channels;

        frame->pts = p_aout_buf->i_pts;

        p_block = block_Alloc( p_sys->i_buffer_out );
        av_init_packet( &packet );
        packet.data = p_block->p_buffer;
        packet.size = p_block->i_buffer;

        i_out = avcodec_encode_audio2( p_sys->p_context, &packet, frame, &got_packet );
        p_block->i_buffer = packet.size;

        if( frame->extended_data != frame->data )
           av_freep( frame->extended_data );
        av_freep( &frame );
        if( unlikely( !got_packet || ( i_out < 0 ) ) )
        {
            if( i_out < 0 )
            {
                msg_Err( p_enc,"Encoding problem..");
                return p_chain;
            }
            block_Release( p_block );
            continue;
        }

        p_block->i_buffer = packet.size;

        p_block->i_length = (mtime_t)1000000 *
            (mtime_t)p_sys->i_frame_size /
            (mtime_t)p_sys->p_context->sample_rate;

        p_block->i_dts = p_block->i_pts = packet.pts;

        block_ChainAppend( &p_chain, p_block );
    }
    if( i_samples_left < 0 )
        msg_Err( p_enc, "I_data_left overflow");

    // We have leftover samples that don't fill frame_size, and libavcodec doesn't seem to like
    // that frame has more data than p_sys->i_frame_size most of the cases currently.
    if( i_samples_left > 0 )
    {
        if( av_sample_fmt_is_planar( p_sys->p_context->sample_fmt ) )
            aout_Deinterleave( p_sys->p_buffer, p_aout_buf->p_buffer+i_data_offset, i_samples_left, p_enc->fmt_in.audio.i_channels, p_enc->fmt_in.i_codec );
        else
            memcpy( p_sys->p_buffer, p_aout_buf->p_buffer+i_data_offset , i_samples_left*p_sys->i_sample_bytes*p_enc->fmt_in.audio.i_channels);
        p_sys->i_samples_delay = i_samples_left;
    }

    return p_chain;
}

/*****************************************************************************
 * CloseEncoder: libavcodec encoder destruction
 *****************************************************************************/
void CloseEncoder( vlc_object_t *p_this )
{
    encoder_t *p_enc = (encoder_t *)p_this;
    encoder_sys_t *p_sys = p_enc->p_sys;

    vlc_avcodec_lock();
    avcodec_close( p_sys->p_context );
    vlc_avcodec_unlock();
    av_free( p_sys->p_context );

    free( p_sys->p_buffer );

    free( p_sys );
}
