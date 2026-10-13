/* xinspect2 - read-only X11 observer for the BP-window correlation run.
 *
 * v1 (audit/xinspect.c) walked the tree but decoded ATOM-array properties
 * (_NET_WM_STATE, _NET_WM_WINDOW_TYPE) as text, so those printed empty in every
 * session-20 round, and it never looked at legacy WM_STATE, override-redirect,
 * _NET_ACTIVE_WINDOW or the X input focus. v2 adds all of those, keeps ancestry
 * (parent id per window), stamps every line with CLOCK_MONOTONIC (the SAME clock
 * cdp_probe v7 uses; the guest wall clock is faked and is only echoed as ep=)
 * and gains an --events mode: one root SubstructureNotify+PropertyChange tail
 * that prints CreateNotify/MapNotify/UnmapNotify/ReparentNotify/ConfigureNotify/
 * DestroyNotify/PropertyNotify plus polled _NET_ACTIVE_WINDOW / XGetInputFocus
 * diffs. Nothing is ever written to the server: no map/configure/property sets.
 *
 * usage:  xinspect2                 one tree snapshot (default, v1-compatible role)
 *         xinspect2 --events SEC    root event tail + focus/active-window polling
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static Display *dpy;
static Atom a_pid, a_nname, a_name, a_class, a_nstate, a_wmstate, a_wtype, a_act, a_utf8, a_wn;

static void mono(char *out, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(out, n, "mono=%ld.%02ld ep=%ld", (long)ts.tv_sec, ts.tv_nsec / 10000000, (long)time(NULL));
}

static char *prop_text(Window w, Atom prop) {           /* STRING/UTF8_STRING -> malloc'd or NULL */
    Atom type; int fmt; unsigned long nitems, after; unsigned char *data = NULL;
    char *r = NULL;
    if (XGetWindowProperty(dpy, w, prop, 0, 1024, False, AnyPropertyType,
                           &type, &fmt, &nitems, &after, &data) == Success && data && fmt == 8) {
        r = malloc(nitems + 1);
        if (r) { memcpy(r, data, nitems); r[nitems] = 0; }
    }
    if (data) XFree(data);
    return r;
}

static void prop_atoms(Window w, Atom prop, char *out, size_t n) {  /* ATOM-array -> "A,B" names */
    Atom type; int fmt; unsigned long nitems, after; unsigned char *data = NULL;
    out[0] = 0;
    if (XGetWindowProperty(dpy, w, prop, 0, 64, False, XA_ATOM,
                           &type, &fmt, &nitems, &after, &data) == Success && data && fmt == 32) {
        Atom *av = (Atom *)data;
        size_t used = 0;
        for (unsigned long i = 0; i < nitems && used < n - 2; i++) {
            char *nm = XGetAtomName(dpy, av[i]);
            if (!nm) continue;
            used += (size_t)snprintf(out + used, n - used, "%s%s", i ? "," : "", nm);
            XFree(nm);
        }
    }
    if (data) XFree(data);
}

static void prop_wmstate(Window w, char *out, size_t n) {   /* legacy WM_STATE: state + icon window */
    Atom type; int fmt; unsigned long nitems, after; unsigned char *data = NULL;
    out[0] = 0;
    if (XGetWindowProperty(dpy, w, a_wmstate, 0, 2, False, a_wmstate,
                           &type, &fmt, &nitems, &after, &data) == Success && data &&
        fmt == 32 && nitems >= 1) {
        unsigned long *lv = (unsigned long *)data;
        const char *s = lv[0] == 0 ? "Withdrawn" : lv[0] == 1 ? "Normal" :
                        lv[0] == 2 ? "Inactive"  : lv[0] == 3 ? "Iconic" : "???";
        snprintf(out, n, "%s%s%lx", s, nitems >= 2 ? "+" : "", nitems >= 2 ? lv[1] : 0UL);
    }
    if (data) XFree(data);
}

static long prop_card(Window w, Atom prop) {            /* CARDINAL/ WINDOW single value, -1 absent */
    Atom type; int fmt; unsigned long nitems, after; unsigned char *data = NULL;
    long v = -1;
    if (XGetWindowProperty(dpy, w, prop, 0, 1, False, AnyPropertyType,
                           &type, &fmt, &nitems, &after, &data) == Success && data &&
        fmt == 32 && nitems >= 1)
        v = (long)*(unsigned long *)data;
    if (data) XFree(data);
    return v;
}

static void desc(Window w, char *out, size_t n) {       /* "name='...' class='...'" for any window */
    char *nm = prop_text(w, a_nname);
    if (!nm || !*nm) { free(nm); nm = prop_text(w, a_name); }
    char *cl = prop_text(w, a_class);
    snprintf(out, n, "'%s'/'%s'", nm ? nm : "", cl ? cl : "");
    free(nm); free(cl);
}

static void walk(Window w, Window par, int depth) {
    XWindowAttributes at;
    if (!XGetWindowAttributes(dpy, w, &at)) return;     /* destroyed/unrealized: skip */
    char ms[16], name[300], st[160], ty[160], wst[64], apid[24], m[48], sib[256];
    mono(m, sizeof m);
    snprintf(ms, sizeof ms, at.map_state == IsViewable ? "VIEWABLE" :
                            at.map_state == IsUnmapped ? "unmapped" : "UNVIEWABLE");
    desc(w, sib, sizeof sib);                            /* reuse as name/class scratch */
    prop_atoms(w, a_nstate, st, sizeof st);
    prop_atoms(w, a_wtype, ty, sizeof ty);
    prop_wmstate(w, wst, sizeof wst);
    long pid = prop_card(w, a_pid);
    snprintf(name, sizeof name, "%s", sib);
    for (int i = 0; i < depth && i < 6; i++) printf("  ");
    printf("XW %s 0x%lx par=0x%lx %dx%d+%d+%d d=%d or=%d %s pid=%ld wms=[%s] wmt=[%s] wmst=%s %s\n",
           m, (unsigned long)w, (unsigned long)par, at.width, at.height, at.x, at.y,
           at.depth, at.override_redirect, ms, pid, st, ty, wst[0] ? wst : "-", name);
    Window root, parent, *kids = NULL; unsigned int nk = 0;
    if (XQueryTree(dpy, w, &root, &parent, &kids, &nk) && kids) {
        for (unsigned int i = 0; i < nk; i++) walk(kids[i], w, depth + 1);
        XFree(kids);
    }
}

static void snapshot(void) {
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr), focus = None; int rev = 0;
    char m[48]; mono(m, sizeof m);
    long act = prop_card(root, a_act);
    char ad[256] = "-", fd[256] = "-";
    if (act >= 0) desc((Window)act, ad, sizeof ad);
    XGetInputFocus(dpy, &focus, &rev);
    if (focus != PointerRoot && focus != None) desc(focus, fd, sizeof fd);
    printf("XS %s scr=%dx%d active=0x%lx(%s) focus=0x%lx(%s) revert=%d\n",
           m, DisplayWidth(dpy, scr), DisplayHeight(dpy, scr),
           (unsigned long)(act >= 0 ? act : 0), ad, (unsigned long)focus, fd, rev);
    walk(root, None, 0);
}

static void events(int secs) {
    int fd = ConnectionNumber(dpy);
    Window root = RootWindow(dpy, DefaultScreen(dpy));
    XSelectInput(dpy, root, SubstructureNotifyMask | PropertyChangeMask);
    char m[48]; mono(m, sizeof m);
    printf("XE %s tail-start secs=%d\n", m, secs);
    time_t t0 = time(NULL);
    int printed = 0;
    long last_act = -2; Window last_focus = 1; int last_rev = -1;
    char da[256] = "-", df[256] = "-";
    while (time(NULL) - t0 < secs && printed < 500) {
        struct timeval tv = { 0, 500000 };
        fd_set r; FD_ZERO(&r); FD_SET(fd, &r);
        if (select(fd + 1, &r, NULL, NULL, &tv) > 0)
            while (XPending(dpy)) {
                XEvent e; XNextEvent(dpy, &e);
                mono(m, sizeof m);
                char d1[256] = "-", d2[256] = "-";
                Window ew = None, ew2 = None;
                const char *kind = "?";
                const XConfigureEvent *ce = (const XConfigureEvent *)&e;
                const XCreateWindowEvent *cwe = (const XCreateWindowEvent *)&e;
                const XMapEvent *me = (const XMapEvent *)&e;
                const XPropertyEvent *pe = (const XPropertyEvent *)&e;
                switch (e.xany.type) {
                case CreateNotify:   kind = "CREATE";  ew = cwe->window;
                    printf("XE %s %s 0x%lx par=0x%lx %dx%d+%d+%d d=%d or=%d\n", m, kind,
                           (unsigned long)cwe->window, (unsigned long)cwe->parent,
                           cwe->width, cwe->height, cwe->x, cwe->y, cwe->border_width,
                           cwe->override_redirect); printed++; continue;
                case DestroyNotify:  kind = "DESTROY"; ew = ((XDestroyWindowEvent *)&e)->window; break;
                case UnmapNotify:    kind = "UNMAP";   ew = me->window; break;
                case MapNotify:      kind = "MAP";     ew = me->window; break;
                case ReparentNotify: kind = "REPARENT"; ew = ((XReparentEvent *)&e)->window;
                    ew2 = ((XReparentEvent *)&e)->parent; break;
                case ConfigureNotify:kind = "CONFIG";  ew = ce->window;
                    if (ce->window == root) continue;      /* root resizes: noise */
                    printf("XE %s %s 0x%lx above=0x%lx %dx%d+%d+%d send=%d\n", m, kind,
                           (unsigned long)ce->window, (unsigned long)ce->above,
                           ce->width, ce->height, ce->x, ce->y, ce->send_event); printed++; continue;
                case PropertyNotify: {
                    if (pe->window != root && pe->atom != a_act && pe->atom != a_wmstate &&
                        pe->atom != a_nstate) continue;    /* only correlation-relevant props */
                    char *an = XGetAtomName(dpy, pe->atom);
                    printf("XE %s PROPWIN 0x%lx atom=%s new=%d\n", m,
                           (unsigned long)pe->window, an ? an : "?", pe->state == PropertyNewValue);
                    if (an) XFree(an); printed++; continue;
                }
                case CirculateNotify: kind = "CIRC"; break;
                default: continue;
                }
                if (ew) desc(ew, d1, sizeof d1);
                if (ew2) desc(ew2, d2, sizeof d2);
                printf("XE %s %s 0x%lx(%s)%s%s\n", m, kind, (unsigned long)ew, d1,
                       ew2 ? " ->0x" : "", ew2 ? d2 : "");
                printed++;
            }
        /* idle poll: focus and active-window as SEPARATE observations, print only diffs */
        long act = prop_card(root, a_act);
        Window f = None; int rev = 0;
        XGetInputFocus(dpy, &f, &rev);
        if (act != last_act || f != last_focus || rev != last_rev) {
            if (act >= 0) desc((Window)act, da, sizeof da); else snprintf(da, sizeof da, "-");
            if (f != PointerRoot && f != None) desc(f, df, sizeof df); else snprintf(df, sizeof df, "-");
            mono(m, sizeof m);
            printf("XE %s FOCUSDIFF active=0x%lx(%s) input=0x%lx(%s) revert=%d\n",
                   m, (unsigned long)(act >= 0 ? act : 0), da, (unsigned long)f, df, rev);
            last_act = act; last_focus = f; last_rev = rev; printed++;
        }
    }
    mono(m, sizeof m);
    printf("XE %s tail-end printed=%d %s\n", m, printed, printed >= 500 ? "(truncated)" : "");
    XSelectInput(dpy, root, NoEventMask);
}

int main(int argc, char **argv) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("cannot open display\n"); return 1; }
    a_pid = XInternAtom(dpy, "_NET_WM_PID", False);
    a_nname = XInternAtom(dpy, "_NET_WM_NAME", False);
    a_name = XInternAtom(dpy, "WM_NAME", False);
    a_class = XInternAtom(dpy, "WM_CLASS", False);
    a_nstate = XInternAtom(dpy, "_NET_WM_STATE", False);
    a_wtype = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    a_wmstate = XInternAtom(dpy, "WM_STATE", False);
    a_act = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_wn = XInternAtom(dpy, "WINDOW_NAME", False);
    (void)a_utf8; (void)a_wn;
    if (argc >= 3 && !strcmp(argv[1], "--events")) events(atoi(argv[2]));
    else snapshot();
    XCloseDisplay(dpy);
    return 0;
}
