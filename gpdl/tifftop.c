/* Copyright (C) 2019 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  1305 Grant Avenue - Suite 200, Novato,
   CA 94945, U.S.A., +1(415)492-9861, for further information.
*/

/* tifftop.c */
/* Top-level API implementation of "TIF" Language Interface */

#include "pltop.h"
#include "gserrors.h"
#include "gxdevice.h"
#include "gsstate.h"
#include "strimpl.h"
#include "gscoord.h"
#include "tiffio.h"

/* Forward decls */

/************************************************************/
/******** Language wrapper implementation (see pltop.h) *****/
/************************************************************/

typedef enum
{
    ii_state_identifying = 0,
    ii_state_tiff,
    ii_state_tiff_header,
    ii_state_tiff_decode,
    ii_state_flush
} ii_state;

/*
 * Tiff interpreter instance
 */
typedef struct tiff_interp_instance_s {
    gs_memory_t       *memory;
    gs_memory_t       *cmemory;
    gx_device         *dev;
    gx_device         *nulldev;

    gs_color_space    *gray;
    gs_color_space    *rgb;
    gs_color_space    *cmyk;

    /* Tiff parser state machine */
    ii_state           state;

    int                pages;

    uint32_t           bpp;
    uint32_t           bpc;
    uint32_t           cs;
    uint32_t           width;
    uint32_t           height;
    uint32_t           xresolution;
    uint32_t           yresolution;
    uint32_t           tile_height;
    uint32_t           tile_width;

    uint32_t           num_comps;
    uint32_t           byte_width;
    uint32_t           y;

    uint32_t           bytes_available_on_entry;

    gs_image_t         image;
    gs_image_enum     *penum;
    gs_gstate         *pgs;

    size_t             buffer_full;
    size_t             buffer_max;
    byte              *tiff_buffer;
    size_t             file_pos;
    TIFF              *handle;

    byte              *samples;

} tiff_interp_instance_t;

static int
tiff_detect_language(const char *s, int len)
{
    const byte *hdr = (const byte *)s;
    if (len >= 4) {
        if (hdr[0] == 'I' && hdr[1] == 'I' && hdr[2] == 42 && hdr[3] == 0)
            return 100; /* Intel (LSB) order */
        if (hdr[0] == 'M' && hdr[1] == 'M' && hdr[2] == 0 && hdr[3] == 42)
            return 100; /* Motorola (MSB) order */
    }

    return 0;
}

static const pl_interp_characteristics_t tiff_characteristics = {
    "TIFF",
    tiff_detect_language,
    "Artifex",
    "0.01",
    "18 Nov 2019",
    1 /* minimum input size */
};

/* Get implementation's characteristics */
static const pl_interp_characteristics_t * /* always returns a descriptor */
tiff_impl_characteristics(const pl_interp_implementation_t *impl)     /* implementation of interpreter to alloc */
{
  return &tiff_characteristics;
}

static void
tiff_deallocate(tiff_interp_instance_t *tiff)
{
    if (tiff == NULL)
        return;

    rc_decrement_cs(tiff->gray, "tiff_deallocate");
    rc_decrement_cs(tiff->rgb, "tiff_deallocate");
    rc_decrement_cs(tiff->cmyk, "tiff_deallocate");

    if (tiff->pgs != NULL)
        gs_gstate_free_chain(tiff->pgs);
    gs_free_object(tiff->memory, tiff, "tiff_impl_allocate_interp_instance");
}

/* Deallocate a interpreter instance */
static int
tiff_impl_deallocate_interp_instance(pl_interp_implementation_t *impl)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)impl->interp_client_data;

    tiff_deallocate(tiff);
    impl->interp_client_data = NULL;

    return 0;
}

/* Do per-instance interpreter allocation/init. */
static int
tiff_impl_allocate_interp_instance(pl_interp_implementation_t *impl, gs_memory_t *mem)
{
    int code;
    tiff_interp_instance_t *tiff
        = (tiff_interp_instance_t *)gs_alloc_bytes(mem,
                                                  sizeof(tiff_interp_instance_t),
                                                  "tiff_impl_allocate_interp_instance");
    if (!tiff)
        return_error(gs_error_VMerror);
    memset(tiff, 0, sizeof(*tiff));

    tiff->memory = mem;
    tiff->pgs = gs_gstate_alloc(mem);
    if (tiff->pgs == NULL)
        goto failVM;

    /* Push one save level onto the stack to assuage the memory handling */
    code = gs_gsave(tiff->pgs);
    if (code < 0)
        goto fail;

    code = gsicc_init_iccmanager(tiff->pgs);
    if (code < 0)
        goto fail;

    tiff->gray = gs_cspace_new_ICC(mem, tiff->pgs, 1);
    tiff->rgb  = gs_cspace_new_ICC(mem, tiff->pgs, 3);
    tiff->cmyk = gs_cspace_new_ICC(mem, tiff->pgs, 4);

    impl->interp_client_data = tiff;

    return 0;

failVM:
    code = gs_note_error(gs_error_VMerror);
fail:
    (void)tiff_deallocate(tiff);
    return code;
}

/*
 * Get the allocator with which to allocate a device
 */
static gs_memory_t *
tiff_impl_get_device_memory(pl_interp_implementation_t *impl)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)impl->interp_client_data;

    return tiff->dev ? tiff->dev->memory : NULL;
}

#if 0 /* UNUSED */
static int
tiff_impl_set_param(pl_interp_implementation_t *impl,
                   pl_set_param_type           type,
                   const char                 *param,
                   const void                 *val)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)impl->interp_client_data;

    /* No params set here */
    return 0;
}

static int
tiff_impl_add_path(pl_interp_implementation_t *impl,
                  const char                 *path)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)impl->interp_client_data;

    /* No paths to add */
    return 0;
}

static int
tiff_impl_post_args_init(pl_interp_implementation_t *impl)
{
    tiff_interp_instance_t *tiff = (jpg_interp_instance_t *)impl->interp_client_data;

    /* No post args processing */
    return 0;
}
#endif

/* Prepare interp instance for the next "job" */
static int
tiff_impl_init_job(pl_interp_implementation_t *impl,
                  gx_device                  *device)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)impl->interp_client_data;

    tiff->dev = device;
    tiff->state = ii_state_identifying;

    return 0;
}

#if 0 /* UNUSED */
static int
tiff_impl_run_prefix_commands(pl_interp_implementation_t *impl,
                             const char                 *prefix)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)impl->interp_client_data;

    return 0;
}

static int
tiff_impl_process_file(pl_interp_implementation_t *impl, const char *filename)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)impl->interp_client_data;

    return 0;
}
#endif

/* Do any setup for parser per-cursor */
static int                      /* ret 0 or +ve if ok, else -ve error code */
tiff_impl_process_begin(pl_interp_implementation_t * impl)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)impl->interp_client_data;

    return 0;
}

/* Ensure we have 'required' bytes to read, and further ensure
 * that we have no UEL's within those bytes. */
static int
ensure_bytes(tiff_interp_instance_t *jpg, stream_cursor_read *pr, int required)
{
    int n;
    const uint8_t *p = pr->ptr+1;
    const uint8_t *q;
    int avail;

    /* Find out how many bytes we need to check */
    n = pr->limit - pr->ptr;
    if (n > required)
        n = required;

    /* Make sure there are no UELs in that block */
    q = p + n;
    while (p != q) {
        while (p != q && *p != '\033')
            p++;
        if (p == q)
            break;
        avail = pr->limit - pr->ptr;
        if (memcmp(p, "\033%-12345X", min(avail, 9)) == 0) {
            /* At least a partial match to a UEL */
            return avail < 9 ? gs_error_NeedInput : gs_error_InterpreterExit;
        }
        p++;
    }

    /* If we have enough bytes, great, if not, get some more */
    return (n < required) ? gs_error_NeedInput : 0;
}

static int
flush_to_uel(stream_cursor_read *pr)
{
    const uint8_t *p = pr->ptr+1;
    const uint8_t *q = pr->limit+1;
    int avail;

    while (p != q) {
        while (p != q && *p != '\033')
            p++;
        if (p == q)
            break;
        avail = pr->limit - pr->ptr;
        if (memcmp(p, "\033%-12345X", min(avail, 9)) == 0) {
            /* At least a partial match to a UEL. Bin everything to
             * the start of the match. */
            pr->ptr = p-1;
            if (avail == 9) /* Complete match. Exit! */
                return gs_error_InterpreterExit;
            /* Partial match. Get more data. */
            return gs_error_NeedInput;
        }
        p++;
    }

    pr->ptr = pr->limit;

    return 0;
}

static int
bytes_until_uel(const stream_cursor_read *pr)
{
    const uint8_t *p = pr->ptr+1;
    const uint8_t *q = pr->limit+1;
    int avail;

    while (p != q) {
        while (p != q && *p != '\033')
            p++;
        if (p == q)
            break;
        avail = pr->limit - pr->ptr;
        if (memcmp(p, "\033%-12345X", min(avail, 9)) == 0) {
            /* At least a partial match to a UEL. Everything up to
             * the start of the match is up for grabs. */
            return p - (pr->ptr+1);
        }
        p++;
    }

    return pr->limit - pr->ptr;
}

static int
get32be(stream_cursor_read *pr)
{
    int v = pr->ptr[1] << 24;
    v |= pr->ptr[2] << 16;
    v |= pr->ptr[3] << 8;
    v |= pr->ptr[4];
    pr->ptr += 4;

    return v;
}

static int
get8(stream_cursor_read *pr)
{
    return *++(pr->ptr);
}

static tmsize_t tifsReadProc(thandle_t  tiff_,
                             void      *buf,
                             tmsize_t   size)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)tiff_;
    tmsize_t available = tiff->buffer_full - tiff->file_pos;
    if (available > size)
        available = size;

    memcpy(buf, &tiff->tiff_buffer[tiff->file_pos], available);
    tiff->file_pos += available;

    return available;
}

static tmsize_t tifsWriteProc(thandle_t  tiff_,
                              void      *buf,
                              tmsize_t   size)
{
    return 0;
}

static toff_t tifsSeekProc(thandle_t tiff_, toff_t offset, int whence)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)tiff_;
    size_t pos = tiff->file_pos;

    if (whence == 1) { /* SEEK_CURR */
        offset += pos;
    } else if (whence == 2) { /* SEEK_END */
        offset += tiff->buffer_full;
    }
    /* else assume SEEK_SET */

    /* Clamp */
    if (offset < 0)
        offset = 0;
    else if (offset > tiff->buffer_full)
        offset = tiff->buffer_full;

    tiff->file_pos = offset;

    return offset;
}

static tifsCloseProc(thandle_t tiff_)
{
    return 0;
}

static toff_t tifsSizeProc(thandle_t tiff_)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)tiff_;

    return tiff->buffer_full;
}

static int
do_impl_process(pl_interp_implementation_t * impl, stream_cursor_read * pr, int eof)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)impl->interp_client_data;
    int code = 0;
    ii_state ostate = (ii_state)-1;
    int bytes_left = 0;

    while (tiff->state != ostate || pr->limit - pr->ptr != bytes_left)
    {
        ostate = tiff->state;
        bytes_left = pr->limit - pr->ptr;
        switch(tiff->state)
        {
        case ii_state_identifying:
        {
            const byte *hdr;
            /* Try and get us 4 bytes */
            code = ensure_bytes(tiff, pr, 4);
            if (code < 0)
                return code;
            hdr = pr->ptr+1;
            if (hdr[0] == 'I' && hdr[1] == 'I' && hdr[2] == 42 && hdr[3] == 0) {
                tiff->state = ii_state_tiff;
                break;
            }
            if (hdr[0] == 'M' && hdr[1] == 'M' && hdr[2] == 0 && hdr[3] == 42) {
                tiff->state = ii_state_tiff;
                break;
            }
            tiff->state = ii_state_flush;
            break;
        }
        case ii_state_tiff:
        {
            /* Gather data into a buffer */
            int bytes = bytes_until_uel(pr);

            if (bytes == 0 && pr->limit - pr->ptr > 9) {
                /* No bytes until UEL, and there is space for a UEL in the buffer */
                tiff->state = ii_state_tiff_decode;
                tiff->file_pos = 0;
                break;
            }
            if (bytes == 0 && eof) {
                /* No bytes until UEL, and we are at eof */
                tiff->state = ii_state_tiff_decode;
                tiff->file_pos = 0;
                break;
            }

            if (tiff->buffer_full + bytes > tiff->buffer_max) {
                /* Need to expand our buffer */
                size_t proposed = tiff->buffer_full*2;
                if (proposed == 0)
                    proposed = 32768;
                while (proposed < tiff->buffer_full + bytes)
                    proposed *= 2;

                if (tiff->tiff_buffer == NULL) {
                    tiff->tiff_buffer = gs_alloc_bytes(tiff->memory, proposed, "tiff_buffer");
                    if (tiff->tiff_buffer == NULL) {
                        tiff->state = ii_state_flush;
                        break;
                    }
                } else {
                    void *new_buf = gs_resize_object(tiff->memory, tiff->tiff_buffer, proposed, "tiff_buffer");
                    if (new_buf == NULL) {
                        tiff->state = ii_state_flush;
                        break;
                    }
                    tiff->tiff_buffer = new_buf;
                }
                tiff->buffer_max = proposed;
            }

            memcpy(&tiff->tiff_buffer[tiff->buffer_full], pr->ptr+1, bytes);
            tiff->buffer_full += bytes;
            pr->ptr += bytes;
            break;
        }
        case ii_state_tiff_decode:
        {
            int tx, ty;
            short planar;
            float f, scale;
            gs_color_space *cs;
            unsigned int used;

            tiff->handle = TIFFClientOpen("dummy", "rm",
                                          (thandle_t)tiff,
                                          tifsReadProc,
                                          tifsWriteProc,
                                          tifsSeekProc,
                                          tifsCloseProc,
                                          tifsSizeProc,
                                          NULL,
                                          NULL);
            if (tiff->handle == NULL) {
                tiff->state = ii_state_flush;
                break;
            }

            TIFFGetField(tiff->handle, TIFFTAG_IMAGEWIDTH, &tiff->width);
            TIFFGetField(tiff->handle, TIFFTAG_IMAGELENGTH, &tiff->height);
            TIFFGetField(tiff->handle, TIFFTAG_TILEWIDTH, &tiff->tile_width);
            TIFFGetField(tiff->handle, TIFFTAG_TILELENGTH, &tiff->tile_height);
            TIFFGetField(tiff->handle, TIFFTAG_BITSPERSAMPLE, &tiff->bpc);
            TIFFGetField(tiff->handle, TIFFTAG_SAMPLESPERPIXEL, &tiff->num_comps);
            TIFFGetField(tiff->handle, TIFFTAG_PLANARCONFIG, &planar);
            TIFFGetField(tiff->handle, TIFFTAG_XRESOLUTION, &f);
            tiff->xresolution = (uint32_t)(f+0.5);
            TIFFGetField(tiff->handle, TIFFTAG_YRESOLUTION, &f);
            tiff->yresolution = (uint32_t)(f+0.5);
            
            if (TIFFIsTiled(tiff->handle)) {
                int x, y;
                tiff->samples = gs_alloc_bytes(tiff->memory, TIFFTileSize(tiff->handle), "tiff_tile");
            } else {
                tiff->tile_width = tiff->width;
                tiff->tile_height = tiff->height;
                tiff->samples = gs_alloc_bytes(tiff->memory, TIFFScanlineSize(tiff->handle), "tiff_scan");
            }
            if (tiff->samples == NULL) {
                TIFFClose(tiff->handle);
                tiff->state = ii_state_flush;
                break;
            }

            /* FIXME: Need to allow for LAB, YUV etc too */
            tiff->bpp = tiff->bpc * tiff->num_comps;
            switch(tiff->num_comps) {
            default:
            case 1:
                cs = tiff->gray;
                break;
            case 3:
                cs = tiff->rgb;
                break;
            case 4:
                cs = tiff->cmyk;
                break;
            }

            /* Scale to fit, if too large. */
            scale = 1.0f;
            if (tiff->width * tiff->dev->HWResolution[0] > tiff->dev->width * tiff->xresolution)
                scale = ((float)tiff->dev->width * tiff->xresolution) / (tiff->width * tiff->dev->HWResolution[0]);
            if (scale * tiff->height * tiff->dev->HWResolution[1] > tiff->dev->height * tiff->yresolution)
                scale = ((float)tiff->dev->height * tiff->yresolution) / (tiff->height * tiff->dev->HWResolution[1]);

            tiff->nulldev = gs_currentdevice(tiff->pgs);
            rc_increment(tiff->nulldev);
            code = gs_setdevice_no_erase(tiff->pgs, tiff->dev);
            if (code < 0) {
                tiff->state = ii_state_flush;
                break;
            }

            if (TIFFIsTiled(tiff->handle) || planar == PLANARCONFIG_CONTIG) {
                tiff->byte_width = ((tiff->bpc * tiff->num_comps * tiff->tile_width + 7)>>3);
            } else {
                tiff->byte_width = ((tiff->bpc * tiff->tile_width + 7)>>3) * tiff->num_comps;
            }
            code = gs_erasepage(tiff->pgs);
            if (code < 0) {
                tiff->state = ii_state_flush;
                return code;
            }
            for (ty = 0; ty < tiff->height; ty += tiff->tile_height)
                for (tx = 0; tx < tiff->width; tx += tiff->tile_width) {
                    int y, s;
                    byte *row;
                    float xext, xoffset, yext, yoffset;

                    tiff->penum = gs_image_enum_alloc(tiff->memory, "tiff_impl_process(penum)");
                    if (tiff->penum == NULL) {
                        code = gs_note_error(gs_error_VMerror);
                        tiff->state = ii_state_flush;
                        return code;
                    }

                    /* Centre - Extents and offsets are all calculated in points (1/72 of an inch) */
                    xext = (((float)tiff->width - tx * 2) * 72 * scale / tiff->xresolution);
                    xoffset = (tiff->dev->width * 72 / tiff->dev->HWResolution[0] - xext)/2;
                    yext = (((float)tiff->height - ty * 2) * 72 * scale / tiff->yresolution);
                    yoffset = (tiff->dev->height * 72 / tiff->dev->HWResolution[1] - yext)/2;

                    gs_initmatrix(tiff->pgs);

                    /* By default the ctm is set to:
                     *   xres/72   0
                     *   0         -yres/72
                     *   0         dev->height * yres/72
                     * i.e. it moves the origin from being top right to being bottom left.
                     * We want to move it back, as without this, the image will be displayed
                     * upside down.
                     */
                    code = gs_translate(tiff->pgs, 0.0, tiff->dev->height * 72 / tiff->dev->HWResolution[1]);
                    if (code >= 0)
                        code = gs_translate(tiff->pgs, xoffset, -yoffset);
                    if (code >= 0)
                        code = gs_scale(tiff->pgs, scale, -scale);
                    if (code < 0) {
                        tiff->state = ii_state_flush;
                        break;
                    }

                    memset(&tiff->image, 0, sizeof(tiff->image));
                    gs_image_t_init(&tiff->image, cs);
                    tiff->image.BitsPerComponent = tiff->bpp/tiff->num_comps;
                    tiff->image.Width = tiff->tile_width;
                    tiff->image.Height = tiff->tile_height;

                    tiff->image.ImageMatrix.xx = tiff->xresolution / 72.0f;
                    tiff->image.ImageMatrix.yy = tiff->yresolution / 72.0f;

                    code = gs_image_init(tiff->penum,
                                         &tiff->image,
                                         false,
                                         false,
                                         tiff->pgs);
                    if (code < 0) {
                        tiff->state = ii_state_flush;
                        return code;
                    }

                    if (TIFFIsTiled(tiff->handle)) {
                        TIFFReadTile(tiff->handle, tiff->samples, tx, ty, 0, 0);
                    }

                    for (y = 0; y < tiff->tile_height; y++) {
                        if (TIFFIsTiled(tiff->handle)) {
                            row = tiff->samples + tiff->byte_width * y;
                        } else if (planar == PLANARCONFIG_CONTIG) {
                            row = tiff->samples;
                            TIFFReadScanline(tiff->handle, tiff->samples, ty+y, 0);
                        } else {
                            int span = tiff->byte_width / tiff->num_comps; /* FIXME: Avoid division */
                            row = tiff->samples;
                            for (s = 0; s < tiff->num_comps; s++)
                                TIFFReadScanline(tiff->handle, tiff->samples + span*s, ty+y, s);
                        }

                        code = gs_image_next(tiff->penum, row, tiff->byte_width, &used);
                        if (code < 0) {
                            tiff->state = ii_state_flush;
                            break;
                        }
                    }
                    code = gs_image_cleanup_and_free_enum(tiff->penum, tiff->pgs);
                    tiff->penum = NULL;
                    if (code < 0) {
                        tiff->state = ii_state_flush;
                        break;
                    }
            }
            tiff->state = ii_state_flush;
            (void)pl_finish_page(tiff->memory->gs_lib_ctx->top_of_system,
                                 tiff->pgs, 1, true);
            break;
        }
        default:
        case ii_state_flush:

            if (tiff->handle) {
                TIFFClose(tiff->handle);
                tiff->handle = NULL;
            }

            if (tiff->penum) {
                (void)gs_image_cleanup_and_free_enum(tiff->penum, tiff->pgs);
                tiff->penum = NULL;
            }

            if (tiff->samples) {
                gs_free_object(tiff->memory, tiff->samples, "tiff_impl_process(samples)");
                tiff->samples = NULL;
            }

            if (tiff->tiff_buffer) {
                gs_free_object(tiff->memory, tiff->tiff_buffer, "tiff_impl_process(tiff_buffer)");
                tiff->tiff_buffer = NULL;
            }
            /* We want to bin any data we get up to, but not including
             * a UEL. */
            return flush_to_uel(pr);
        }
    }

    return code;
}

static int
tiff_impl_process(pl_interp_implementation_t * impl, stream_cursor_read * pr) {
    return do_impl_process(impl, pr, 0);
}

static int
tiff_impl_process_end(pl_interp_implementation_t * impl)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)impl->interp_client_data;
    int code = 0;

    /* FIXME: */
    if (code == gs_error_InterpreterExit || code == gs_error_NeedInput)
        code = 0;

    return code;
}

/* Not implemented */
static int
tiff_impl_flush_to_eoj(pl_interp_implementation_t *impl, stream_cursor_read *cursor)
{
    const byte *p = cursor->ptr;
    const byte *rlimit = cursor->limit;

    /* Skip to, but leave UEL in buffer for PJL to find later */
    for (; p < rlimit; ++p)
        if (p[1] == '\033') {
            uint avail = rlimit - p;

            if (memcmp(p + 1, "\033%-12345X", min(avail, 9)))
                continue;
            if (avail < 9)
                break;
            cursor->ptr = p;
            return 1;           /* found eoj */
        }
    cursor->ptr = p;
    return 0;                   /* need more */
}

/* Parser action for end-of-file */
static int
tiff_impl_process_eof(pl_interp_implementation_t *impl)
{
    stream_cursor_read r;

    r.ptr = NULL;
    r.limit = NULL;
    return do_impl_process(impl, &r, 1);
}

/* Report any errors after running a job */
static int
tiff_impl_report_errors(pl_interp_implementation_t *impl,          /* interp instance to wrap up job in */
                        int                         code,          /* prev termination status */
                        long                        file_position, /* file position of error, -1 if unknown */
                        bool                        force_to_cout  /* force errors to cout */
)
{
    return 0;
}

/* Wrap up interp instance after a "job" */
static int
tiff_impl_dnit_job(pl_interp_implementation_t *impl)
{
    tiff_interp_instance_t *tiff = (tiff_interp_instance_t *)impl->interp_client_data;

    if (tiff->nulldev) {
        int code = gs_setdevice(tiff->pgs, tiff->nulldev);
        tiff->dev = NULL;
        rc_decrement(tiff->nulldev, "tiff_impl_dnit_job(nulldevice)");
        tiff->nulldev = NULL;
        return code;
    }
    return 0;
}

/* Parser implementation descriptor */
const pl_interp_implementation_t tiff_implementation = {
  tiff_impl_characteristics,
  tiff_impl_allocate_interp_instance,
  tiff_impl_get_device_memory,
  NULL, /* tiff_impl_set_param */
  NULL, /* tiff_impl_add_path */
  NULL, /* tiff_impl_post_args_init */
  tiff_impl_init_job,
  NULL, /* tiff_impl_run_prefix_commands */
  NULL, /* tiff_impl_process_file */
  tiff_impl_process_begin,
  tiff_impl_process,
  tiff_impl_process_end,
  tiff_impl_flush_to_eoj,
  tiff_impl_process_eof,
  tiff_impl_report_errors,
  tiff_impl_dnit_job,
  tiff_impl_deallocate_interp_instance,
  NULL
};