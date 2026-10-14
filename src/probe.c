/*
 * probe.c - DRM plane / CRTC / connector / evdev reconnaissance
 *          + live overlay-plane test for the STM32MP157 desktop pet.
 *
 * Cross-compiled (static, armhf). Talks to /dev/dri/card0 with raw ioctls
 * (no libdrm) and to /dev/input/event* with evdev ioctls.
 *
 * What it does:
 *   1. Enumerates DRM planes, formats, connector, CRTC, mode.
 *   2. Probes all /dev/input/eventN devices (name, EV bits, ABS ranges).
 *   3. Picks a plane that is currently unused (crtc_id == 0), creates a
 *      semi-transparent dumb-buffer test pattern, attaches it to the CRTC
 *      with the classic SETPLANE ioctl for a few seconds, then detaches it.
 *      -> proves the "pet floats above the Qt desktop" architecture.
 *
 * NOTE: uses SETPLANE (not ATOMIC) because the host libdrm headers added a
 * 'reserved' field to struct drm_mode_atomic that the board's 5.4 kernel
 * does not know about; struct drm_mode_set_plane is ABI-stable.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

/* newer libdrm headers dropped these legacy defines; the uapi values are fixed */
#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED 1
#define DRM_MODE_DISCONNECTED 0
#endif

static int drm_fd = -1;

static void fourcc_str(uint32_t f, char *out)
{
    out[0] = f & 0xff;
    out[1] = (f >> 8) & 0xff;
    out[2] = (f >> 16) & 0xff;
    out[3] = (f >> 24) & 0xff;
    out[4] = 0;
    for (int i = 0; i < 4; i++)
        if (out[i] < 32 || out[i] > 126)
            out[i] = '?';
}

int main(void)
{
    printf("== DRM ==\n");
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open /dev/dri/card0");
        return 1;
    }
    if (ioctl(drm_fd, DRM_IOCTL_SET_MASTER, 0) == 0)
        printf("drm master: OK\n");
    else
        printf("drm master: FAILED (%s) - continuing\n", strerror(errno));

    /* ---- planes ---- */
    struct drm_mode_get_plane_res pres;
    memset(&pres, 0, sizeof(pres));
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0) {
        perror("GETPLANERESOURCES");
    } else {
        printf("planes: %u\n", pres.count_planes);
        uint32_t *ids = calloc(pres.count_planes ? pres.count_planes : 1, sizeof(uint32_t));
        pres.plane_id_ptr = (uint64_t)(uintptr_t)ids;
        ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres);
        for (uint32_t i = 0; i < pres.count_planes; i++) {
            struct drm_mode_get_plane p;
            memset(&p, 0, sizeof(p));
            p.plane_id = ids[i];
            if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANE, &p) < 0)
                continue;
            uint32_t nf = p.count_format_types;
            uint32_t *fmts = calloc(nf ? nf : 1, sizeof(uint32_t));
            p.format_type_ptr = (uint64_t)(uintptr_t)fmts;
            ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANE, &p);
            printf("plane id=%u crtc_id=%u fb_id=%u possible_crtcs=0x%x formats(%u):",
                   ids[i], p.crtc_id, p.fb_id, p.possible_crtcs, p.count_format_types);
            for (uint32_t j = 0; j < p.count_format_types; j++) {
                char s[5];
                fourcc_str(fmts[j], s);
                printf(" %s", s);
            }
            printf("\n");
            free(fmts);
        }
        free(ids);
    }

    /* ---- connectors / crtcs ---- */
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        perror("GETRESOURCES");
    } else {
        printf("connectors=%u encoders=%u crtcs=%u fbs=%u\n",
               res.count_connectors, res.count_encoders, res.count_crtcs, res.count_fbs);
        uint32_t *cids = calloc(res.count_connectors ? res.count_connectors : 1, sizeof(uint32_t));
        uint32_t *crtcs = calloc(res.count_crtcs ? res.count_crtcs : 1, sizeof(uint32_t));
        res.connector_id_ptr = (uint64_t)(uintptr_t)cids;
        res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
        ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
        for (uint32_t i = 0; i < res.count_connectors; i++) {
            struct drm_mode_get_connector c;
            memset(&c, 0, sizeof(c));
            c.connector_id = cids[i];
            ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
            uint32_t nm = c.count_modes;
            struct drm_mode_modeinfo *modes = calloc(nm ? nm : 1, sizeof(*modes));
            c.modes_ptr = (uint64_t)(uintptr_t)modes;
            ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
            printf("connector id=%u type=%u typeid=%u connection=%u encoder=%u modes=%u\n",
                   cids[i], c.connector_type, c.connector_type_id,
                   c.connection, c.encoder_id, c.count_modes);
            for (uint32_t j = 0; j < c.count_modes; j++) {
                printf("  mode %ux%u@%uHz type=0x%x flags=0x%x name=%s\n",
                       modes[j].hdisplay, modes[j].vdisplay, modes[j].vrefresh,
                       modes[j].type, modes[j].flags, modes[j].name);
            }
            free(modes);
        }
        uint32_t active_crtc = 0;
        for (uint32_t i = 0; i < res.count_crtcs; i++) {
            struct drm_mode_crtc cr;
            memset(&cr, 0, sizeof(cr));
            cr.crtc_id = crtcs[i];
            ioctl(drm_fd, DRM_IOCTL_MODE_GETCRTC, &cr);
            printf("crtc id=%u mode_valid=%u %ux%u\n", crtcs[i], cr.mode_valid,
                   cr.mode.hdisplay, cr.mode.vdisplay);
            if (cr.mode_valid && !active_crtc)
                active_crtc = crtcs[i];
        }
        printf("active_crtc=%u\n", active_crtc);
        free(cids);
        free(crtcs);
    }

    /* ---- evdev ---- */
    printf("== EVDEV ==\n");
    for (int idx = 0; idx < 5; idx++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", idx);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        char name[256] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        printf("%s: \"%s\"\n", path, name);
        unsigned char evbits[EV_CNT / 8 + 1];
        memset(evbits, 0, sizeof(evbits));
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) >= 0) {
            printf("  EV:");
            for (int b = 0; b < EV_CNT; b++)
                if (evbits[b / 8] & (1 << (b % 8)))
                    printf(" %d", b);
            printf("\n");
        }
        unsigned char absbits[ABS_CNT / 8 + 1];
        memset(absbits, 0, sizeof(absbits));
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
            for (int b = 0; b < ABS_CNT; b++) {
                if (!(absbits[b / 8] & (1 << (b % 8))))
                    continue;
                struct input_absinfo ai;
                memset(&ai, 0, sizeof(ai));
                if (ioctl(fd, EVIOCGABS(b), &ai) >= 0)
                    printf("  ABS %d: min=%d max=%d fuzz=%d flat=%d\n",
                           b, ai.minimum, ai.maximum, ai.fuzz, ai.flat);
                else
                    printf("  ABS %d: no-info\n", b);
            }
        }
        unsigned char keybits[KEY_CNT / 8 + 1];
        memset(keybits, 0, sizeof(keybits));
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
            printf("  KEY:");
            for (int b = 0; b < KEY_CNT; b++)
                if (keybits[b / 8] & (1 << (b % 8)))
                    printf(" %d", b);
            printf("\n");
        }
        close(fd);
    }

    /* ---- live overlay-plane test ---- */
    printf("== OVERLAY TEST ==\n");
    struct drm_mode_get_plane_res pr2;
    memset(&pr2, 0, sizeof(pr2));
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr2) < 0 || pr2.count_planes == 0) {
        printf("no planes\n");
        printf("== DONE ==\n");
        return 0;
    }
    uint32_t *pids = calloc(pr2.count_planes, sizeof(uint32_t));
    pr2.plane_id_ptr = (uint64_t)(uintptr_t)pids;
    ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr2);

    uint32_t free_plane = 0;
    for (uint32_t i = 0; i < pr2.count_planes; i++) {
        struct drm_mode_get_plane p;
        memset(&p, 0, sizeof(p));
        p.plane_id = pids[i];
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANE, &p) == 0 && p.crtc_id == 0) {
            free_plane = pids[i];
            break;
        }
    }
    if (!free_plane) {
        printf("no free plane (all attached)\n");
        printf("== DONE ==\n");
        return 0;
    }
    printf("free plane id=%u\n", free_plane);

    struct drm_mode_card_res r2;
    memset(&r2, 0, sizeof(r2));
    ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &r2);
    uint32_t *crtc_ids = calloc(r2.count_crtcs ? r2.count_crtcs : 1, sizeof(uint32_t));
    r2.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
    ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &r2);
    uint32_t crtc_id = 0;
    for (uint32_t i = 0; i < r2.count_crtcs; i++) {
        struct drm_mode_crtc cr;
        memset(&cr, 0, sizeof(cr));
        cr.crtc_id = crtc_ids[i];
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCRTC, &cr) == 0 && cr.mode_valid) {
            crtc_id = crtc_ids[i];
            break;
        }
    }
    if (!crtc_id && r2.count_crtcs)
        crtc_id = crtc_ids[0];
    printf("using crtc id=%u\n", crtc_id);

    const uint32_t W = 240, H = 240;
    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof(cd));
    cd.width = W;
    cd.height = H;
    cd.bpp = 32;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        perror("CREATE_DUMB");
        printf("== DONE ==\n");
        return 1;
    }
    printf("dumb: handle=%u pitch=%u size=%llu\n", cd.handle, cd.pitch,
           (unsigned long long)cd.size);

    struct drm_mode_fb_cmd2 fb;
    memset(&fb, 0, sizeof(fb));
    fb.width = W;
    fb.height = H;
    fb.pixel_format = DRM_FORMAT_ARGB8888;
    fb.handles[0] = cd.handle;
    fb.pitches[0] = cd.pitch;
    fb.offsets[0] = 0;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
        perror("ADDFB2 (ARGB8888)");
        printf("== DONE ==\n");
        return 1;
    }
    printf("fb id=%u\n", fb.fb_id);

    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof(md));
    md.handle = cd.handle;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        perror("MAP_DUMB");
        printf("== DONE ==\n");
        return 1;
    }
    uint32_t *px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, md.offset);
    if (px == MAP_FAILED) {
        perror("mmap");
        printf("== DONE ==\n");
        return 1;
    }

    /* checkerboard of two semi-transparent colors (ARGB8888: bytes B,G,R,A) */
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            uint32_t c = ((x / 30) + (y / 30)) & 1
                ? 0x80FF00FFu   /* B=FF G=00 R=FF A=80 : magenta, 50% */
                : 0x80FFFF00u;  /* B=FF G=FF R=00 A=80 : cyan, 50% */
            px[y * (cd.pitch / 4) + x] = c;
        }
    }

    struct drm_mode_set_plane sp;
    memset(&sp, 0, sizeof(sp));
    sp.plane_id = free_plane;
    sp.crtc_id = crtc_id;
    sp.fb_id = fb.fb_id;
    sp.crtc_x = 50;
    sp.crtc_y = 50;
    sp.crtc_w = W;
    sp.crtc_h = H;
    sp.src_x = 0;
    sp.src_y = 0;
    sp.src_w = W << 16;
    sp.src_h = H << 16;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_SETPLANE, &sp) < 0) {
        perror("SETPLANE (enable plane)");
    } else {
        printf("OVERLAY VISIBLE for 5s (240x240 semi-transparent at 50,50)\n");
        sleep(5);
        memset(&sp, 0, sizeof(sp));
        sp.plane_id = free_plane;   /* fb_id=0, crtc_id=0 -> disable */
        if (ioctl(drm_fd, DRM_IOCTL_MODE_SETPLANE, &sp) < 0)
            perror("SETPLANE (disable plane)");
        else
            printf("OVERLAY HIDDEN again\n");
    }

    munmap(px, cd.size);
    ioctl(drm_fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id);
    struct drm_mode_destroy_dumb dd = { cd.handle };
    ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    ioctl(drm_fd, DRM_IOCTL_DROP_MASTER, 0);
    close(drm_fd);
    printf("== DONE ==\n");
    return 0;
}
