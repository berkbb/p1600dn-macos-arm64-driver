/*
 * HP LaserJet Professional P1600dn Series — PAPPL Printer Application Adapter
 *
 * This module implements a modern, driverless OpenPrinting PAPPL (Printer Application)
 * interface for the HP LaserJet Professional P1600dn series (P1606dn, P1607dn, etc.).
 *
 * It bridges PAPPL's IPP/AirPrint raster pipeline to our native ZjStream and
 * JBIG encoder (zjs_encoder.c, jbig.c), allowing the printer to function as a
 * native, driverless IPP Everywhere / AirPrint network printer on CUPS 3.0,
 * modern Linux distributions, and embedded print servers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifdef HAVE_PAPPL
#include <pappl/pappl.h>
#include <cups/raster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zjs.h"
#include "zjs_encoder.h"

/* Job context passed between PAPPL print callbacks */
typedef struct {
    ZjsJobOptions job;     /* PJL/START_DOC settings */
    ZjsPageOptions page;   /* START_PAGE settings for the current page */
    bool duplex;
    FILE *dev_fp;
    char *stream;
    size_t stream_size;
} p1600dn_pappl_job_t;

/* Driver callback: registers supported P1600dn hardware capabilities */
static bool p1600dn_driver_cb(
    pappl_system_t         *system,
    const char             *driver_name,
    const char             *device_uri,
    const char             *device_id,
    pappl_pr_driver_data_t *driver_data,
    ipp_t                  **attrs,
    void                   *data) {

    (void)system;
    (void)driver_name;
    (void)device_uri;
    (void)device_id;
    (void)attrs;
    (void)data;

    /* Hardware Identification */
    driver_data->make_and_model = "HP LaserJet Professional P1606dn";
    driver_data->device_id      = "MFG:Hewlett-Packard;MDL:HP LaserJet Professional P1606dn;CMD:ZJS,PJL;";

    /* Resolutions: 600x600 dpi and FastRes 1200 (1200x600 dpi) */
    driver_data->num_resolution = 2;
    driver_data->x_resolution[0] = 600;
    driver_data->y_resolution[0] = 600;
    driver_data->x_resolution[1] = 1200;
    driver_data->y_resolution[1] = 600;
    driver_data->x_default = 1200;
    driver_data->y_default = 600;

    /* Duplex capabilities: hardware automatic duplex */
    driver_data->duplex = PAPPL_DUPLEX_TWO_SIDED_LONG_EDGE | PAPPL_DUPLEX_TWO_SIDED_SHORT_EDGE;

    /* Color mode: monochrome one-bit DeviceK */
    driver_data->color_supported = PAPPL_COLOR_MODE_MONOCHROME;
    driver_data->raster_types    = PAPPL_PWG_RASTER_TYPE_BLACK_1;

    /* Hardware margins: 12pt (1/6 inch = 4.23mm) */
    driver_data->left_margin   = 423;
    driver_data->right_margin  = 423;
    driver_data->top_margin    = 423;
    driver_data->bottom_margin = 423;

    /* Supported Media Sizes */
    driver_data->num_media = 7;
    driver_data->media[0] = "iso_a4_210x297mm";
    driver_data->media[1] = "na_letter_8.5x11in";
    driver_data->media[2] = "na_legal_8.5x14in";
    driver_data->media[3] = "na_executive_7.25x10.5in";
    driver_data->media[4] = "iso_a5_148x210mm";
    driver_data->media[5] = "jis_b5_182x257mm";
    driver_data->media[6] = "iso_dl_110x220mm";
    driver_data->media_default = "iso_a4_210x297mm";

    return true;
}

/* Callback: Start of print job */
static bool p1600dn_rstartjob_cb(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device) {
    p1600dn_pappl_job_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return false;

    (void)device;
    ctx->job.job_name   = papplJobGetName(job);
    ctx->job.density    = 3;
    ctx->page.copies    = options->copies > 0 ? (uint32_t)options->copies : 1U;
    ctx->page.source    = DMBIN_MANUAL;
    ctx->page.media     = DMMEDIA_PLAIN;
    ctx->page.bpp       = options->printer_resolution[0] > 600 ? 2U : 1U;
    ctx->duplex         = options->duplex == PAPPL_DUPLEX_TWO_SIDED_LONG_EDGE ||
                          options->duplex == PAPPL_DUPLEX_TWO_SIDED_SHORT_EDGE;

    /* Buffer the ZjStream; pages are screened and encoded like rastertop1600dn. */
    ctx->dev_fp = open_memstream(&ctx->stream, &ctx->stream_size);
    papplJobSetData(job, ctx);

    return true;
}

/* Callback: End of print job */
static bool p1600dn_rendjob_cb(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device) {
    (void)options;
    (void)device;
    p1600dn_pappl_job_t *ctx = (p1600dn_pappl_job_t *)papplJobGetData(job);
    if (ctx) {
        if (ctx->dev_fp) fclose(ctx->dev_fp);
        free(ctx->stream);
        free(ctx);
        papplJobSetData(job, NULL);
    }
    return true;
}

/* Main PAPPL application loop */
int main(int argc, char *argv[]) {
    static const char * const drivers[] = {
        "p1600dn",
        NULL
    };

    return papplMainloop(
        argc, argv,
        "6.0.0",
        "HP LaserJet Professional P1600dn Series Printer Application",
        (int)(sizeof(drivers)/sizeof(drivers[0]) - 1),
        drivers,
        p1600dn_driver_cb,
        /* autoadd */ NULL,
        /* subcmd */ NULL,
        /* system_cb */ NULL,
        /* usage_cb */ NULL,
        /* data */ NULL
    );
}

#else

/* Standalone stub when compiled without libpappl on macOS */
#include <stdio.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    fprintf(stderr,
        "HP LaserJet Professional P1600dn Series — PAPPL Printer Application\n"
        "------------------------------------------------------------------\n"
        "This executable was built without libpappl.\n"
        "To compile with live OpenPrinting PAPPL support:\n"
        "  1. Install libpappl and pkg-config (e.g. on Linux or from Michael Sweet's repository).\n"
        "  2. Run: make pappl\n\n"
        "Note: The native macOS CUPS driver ('rastertop1600dn') runs natively\n"
        "and does not require PAPPL.\n");
    return 0;
}

#endif
