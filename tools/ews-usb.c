/*
 * ews-usb - talk HTTP to an HP printer's Embedded Web Server over USB.
 *
 * HP LaserJet Pro printers expose a vendor-specific USB interface
 * (class 0xff, subclass 0x02, protocol 0x10, named "HP EWS") that carries
 * plain HTTP to the printer's internal web server.  HP's own installers use
 * it to configure Wi-Fi before the printer is on any network.
 *
 * usage: ews-usb [-X METHOD] [-H 'Header: v'] [-d @file | -d @- | -d string] [-v] PATH
 *        -d @- reads the request body from stdin (max 2048 bytes).
 *        -R resets the channel first (SET_INTERFACE + clear halts); -RR also
 *           resets the USB device.  Use if requests start timing out.
 *        Response body goes to stdout, status line + headers to stderr (-v).
 *        Exit status: 0 for HTTP 2xx, 3 for other HTTP statuses, 1 on USB errors.
 *
 * Copyright (c) 2026 Daniel B. Churchill.  GPL-2.0-or-later.
 */
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define HP_VID 0x03f0

static int find_ews(libusb_device_handle **out, int *ifnum,
                    unsigned char *ep_in, unsigned char *ep_out)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(NULL, &list);
    int found = 0;

    for (ssize_t i = 0; i < n && !found; i++) {
        struct libusb_device_descriptor dd;
        struct libusb_config_descriptor *cfg;
        if (libusb_get_device_descriptor(list[i], &dd) || dd.idVendor != HP_VID)
            continue;
        if (libusb_get_active_config_descriptor(list[i], &cfg))
            continue;
        for (int f = 0; f < cfg->bNumInterfaces && !found; f++) {
            const struct libusb_interface *itf = &cfg->interface[f];
            for (int a = 0; a < itf->num_altsetting && !found; a++) {
                const struct libusb_interface_descriptor *d = &itf->altsetting[a];
                if (d->bInterfaceClass != 0xff || d->bInterfaceSubClass != 0x02 ||
                    d->bInterfaceProtocol != 0x10)
                    continue;
                unsigned char in = 0, o = 0;
                for (int e = 0; e < d->bNumEndpoints; e++) {
                    const struct libusb_endpoint_descriptor *ep = &d->endpoint[e];
                    if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
                        continue;
                    if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) in = ep->bEndpointAddress;
                    else o = ep->bEndpointAddress;
                }
                if (in && o && libusb_open(list[i], out) == 0) {
                    *ifnum = d->bInterfaceNumber;
                    *ep_in = in;
                    *ep_out = o;
                    found = 1;
                }
            }
        }
        libusb_free_config_descriptor(cfg);
    }
    libusb_free_device_list(list, 1);
    return found ? 0 : -1;
}

/* Returns 1 once a full HTTP response is in buf. */
static int response_complete(const char *buf, size_t len)
{
    const char *eoh = memmem(buf, len, "\r\n\r\n", 4);
    if (!eoh)
        return 0;
    size_t hlen = (size_t)(eoh - buf) + 4;
    const char *p = buf;
    while (p < eoh) {
        const char *nl = memmem(p, (size_t)(eoh - p) + 2, "\r\n", 2);
        if (!nl) break;
        if (!strncasecmp(p, "Content-Length:", 15))
            return len >= hlen + strtoul(p + 15, NULL, 10);
        if (!strncasecmp(p, "Transfer-Encoding:", 18) && memmem(p, (size_t)(nl - p), "chunked", 7))
            return memmem(buf + hlen, len - hlen, "\r\n0\r\n\r\n", 7) != NULL ||
                   (len - hlen >= 5 && !memcmp(buf + hlen, "0\r\n\r\n", 5));
        p = nl + 2;
    }
    /* No length info: only complete for bodiless status codes. */
    return !strncmp(buf + 9, "204", 3) || !strncmp(buf + 9, "304", 3);
}

static void dechunk_write(const char *body, size_t len)
{
    const char *p = body, *end = body + len;
    while (p < end) {
        char *e;
        unsigned long n = strtoul(p, &e, 16);
        const char *nl = memmem(e, (size_t)(end - e), "\r\n", 2);
        if (!nl || n == 0) break;
        p = nl + 2;
        if ((size_t)(end - p) < n) n = (unsigned long)(end - p);
        fwrite(p, 1, n, stdout);
        p += n + 2;
    }
}

int main(int argc, char **argv)
{
    const char *method = "GET", *data = NULL;
    char *body = NULL;
    size_t blen = 0;
    char headers[4096] = "";
    int verbose = 0, reset = 0, c;

    while ((c = getopt(argc, argv, "X:H:d:vR")) != -1) {
        switch (c) {
        case 'R': reset++; break;
        case 'X': method = optarg; break;
        case 'H': strlcat(headers, optarg, sizeof(headers)); strlcat(headers, "\r\n", sizeof(headers)); break;
        case 'd': data = optarg; break;
        case 'v': verbose = 1; break;
        default: goto usage;
        }
    }
    if (optind != argc - 1) {
usage:
        fprintf(stderr, "usage: %s [-X METHOD] [-H header] [-d @file|@-|data] [-v] [-R] PATH\n", argv[0]);
        return 2;
    }
    if (data) {
        if (!strcmp(data, "@-")) {
            /* Body from stdin: keeps secrets out of argv and off disk. */
            size_t cap = 4096;
            body = malloc(cap);
            size_t r;
            while ((r = fread(body + blen, 1, cap - blen, stdin)) > 0)
                if ((blen += r) == cap)
                    body = realloc(body, cap *= 2);
        } else if (data[0] == '@') {
            FILE *f = fopen(data + 1, "rb");
            if (!f) { perror(data + 1); return 2; }
            fseek(f, 0, SEEK_END); blen = (size_t)ftell(f); rewind(f);
            body = malloc(blen + 1);
            if (fread(body, 1, blen, f) != blen) { perror("read"); return 2; }
            fclose(f);
        } else {
            body = strdup(data);
            blen = strlen(data);
        }
    }

    /* The printer's EWS stops reading when it receives a large body it
     * doesn't consume (tested: 10 KB to a 404 URL wedged the channel until
     * the printer restarted itself).  Real requests are well under 1 KB. */
    if (blen > 2048) {
        fprintf(stderr, "ews-usb: request body too large (%zu bytes, max 2048)\n", blen);
        return 2;
    }

    libusb_device_handle *h = NULL;
    int ifnum;
    unsigned char ep_in, ep_out;
    if (libusb_init(NULL) || find_ews(&h, &ifnum, &ep_in, &ep_out)) {
        fprintf(stderr, "ews-usb: no HP printer with an EWS-over-USB interface found\n");
        return 1;
    }
    int rc;
    if (reset >= 2) {
        /* Full USB port reset; the printer re-enumerates. */
        rc = libusb_reset_device(h);
        fprintf(stderr, "ews-usb: USB reset: %s\n", rc ? libusb_error_name(rc) : "ok");
        libusb_close(h);
        h = NULL;
        sleep(3);
        if (find_ews(&h, &ifnum, &ep_in, &ep_out)) {
            fprintf(stderr, "ews-usb: printer did not come back after reset\n");
            return 1;
        }
    }
    rc = libusb_claim_interface(h, ifnum);
    if (rc) {
        fprintf(stderr, "ews-usb: cannot claim interface %d: %s (is a print job running?)\n",
                ifnum, libusb_error_name(rc));
        return 1;
    }
    if (reset >= 1) {
        /* SET_INTERFACE + CLEAR_FEATURE(HALT) restart the channel. */
        int a = libusb_set_interface_alt_setting(h, ifnum, 0);
        int o = libusb_clear_halt(h, ep_out);
        int i = libusb_clear_halt(h, ep_in);
        fprintf(stderr, "ews-usb: channel reset: set-interface %s, clear-halt out %s, in %s\n",
                a ? libusb_error_name(a) : "ok", o ? libusb_error_name(o) : "ok",
                i ? libusb_error_name(i) : "ok");
    }

    /* Drain anything stale left over from an earlier, interrupted request. */
    unsigned char junk[16384];
    int got;
    while (libusb_bulk_transfer(h, ep_in, junk, sizeof(junk), &got, 100) == 0 && got > 0)
        ;

    char clen[48] = "";
    if (body)
        snprintf(clen, sizeof(clen), "Content-Length: %zu\r\n", blen);
    char *req;
    int rlen = asprintf(&req,
        "%s %s HTTP/1.1\r\nHost: localhost\r\nUser-Agent: ews-usb\r\n%s%s"
        "Connection: keep-alive\r\n\r\n",
        method, argv[optind], headers, clen);
    int sent;
    if ((rc = libusb_bulk_transfer(h, ep_out, (unsigned char *)req, rlen, &sent, 5000)) ||
        (body && blen && (rc = libusb_bulk_transfer(h, ep_out, (unsigned char *)body, (int)blen, &sent, 5000)))) {
        fprintf(stderr, "ews-usb: write failed: %s\n", libusb_error_name(rc));
        return 1;
    }

    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap + 1);
    int idle = 0;
    while (!response_complete(buf, len) && idle < 30) {
        if (cap - len < 16384) buf = realloc(buf, (cap *= 2) + 1);
        rc = libusb_bulk_transfer(h, ep_in, (unsigned char *)buf + len, 16384, &got, 200);
        if (rc && rc != LIBUSB_ERROR_TIMEOUT) {
            fprintf(stderr, "ews-usb: read failed: %s\n", libusb_error_name(rc));
            break;
        }
        if (got > 0) { len += (size_t)got; idle = 0; } else idle++;
    }
    buf[len] = 0;

    libusb_release_interface(h, ifnum);
    libusb_close(h);
    libusb_exit(NULL);

    char *eoh = memmem(buf, len, "\r\n\r\n", 4);
    if (!eoh) {
        fprintf(stderr, "ews-usb: no HTTP response (%zu bytes)\n", len);
        fwrite(buf, 1, len, stderr);
        return 1;
    }
    if (verbose)
        fwrite(buf, 1, (size_t)(eoh - buf) + 2, stderr);
    char *b = eoh + 4;
    size_t bl = len - (size_t)(b - buf);
    if (strcasestr(buf, "Transfer-Encoding: chunked") && strcasestr(buf, "chunked") < eoh)
        dechunk_write(b, bl);
    else
        fwrite(b, 1, bl, stdout);
    int status = len > 12 ? atoi(buf + 9) : 0;
    return status >= 200 && status < 300 ? 0 : 3;
}
