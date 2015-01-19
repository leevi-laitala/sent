/* See LICENSE for licence details. */
#include <errno.h>
#include <math.h>
#include <png.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "arg.h"

char *argv0;

/* macros */
#define LEN(a)     (sizeof(a) / sizeof(a)[0])
#define LIMIT(x, a, b)    (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)

typedef enum {
	NONE = 0,
	LOADED = 1,
	SCALED = 2,
	DRAWN = 4
} imgstate;

struct image {
	unsigned char *buf;
	unsigned int bufwidth, bufheight;
	imgstate state;
	XImage *ximg;
	FILE *f;
	png_structp png_ptr;
	png_infop info_ptr;
	int numpasses;
};

typedef struct {
	char *text;
	struct image *img;
} Slide;

/* Purely graphic info */
typedef struct {
	Display *dpy;
	Window win;
	Atom wmdeletewin, netwmname;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	int w, h;
	int uw, uh; /* usable dimensions for drawing text and images */
} XWindow;

/* Drawing Context linked list*/
struct DC{
	XFontStruct *font;
	GC gc;
	struct DC *next;
};

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int b;
	void (*func)(const Arg *);
	const Arg arg;
} Mousekey;

typedef struct {
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Shortcut;

static struct image *pngopen(char *filename);
static int pngread(struct image *img);
static int pngprepare(struct image *img);
static void pngscale(struct image *img);
static void pngdraw(struct image *img);

static Bool xfontisscalable(char *name);
static XFontStruct *xloadqueryscalablefont(char *name, int size);
static struct DC *getfontsize(char *str, size_t len, int *width, int *height);
static void cleanup(struct DC *cur);
static void eprintf(const char *, ...);
static void load(FILE *fp);
static void advance(const Arg *arg);
static void quit(const Arg *arg);
static void resize(int width, int height);
static void run();
static void usage();
static void xdraw();
static void xhints();
static void xinit();
static void xloadfonts(char *);

static void bpress(XEvent *);
static void cmessage(XEvent *);
static void expose(XEvent *);
static void kpress(XEvent *);
static void configure(XEvent *);

/* config.h for applying patches and the configuration. */
#include "config.h"

/* Globals */
static Slide *slides = NULL;
static int idx = 0;
static int slidecount = 0;
static XWindow xw;
static struct DC dc;
static int running = 1;
static char *opt_font = NULL;

static void (*handler[LASTEvent])(XEvent *) = {
	[ButtonPress] = bpress,
	[ClientMessage] = cmessage,
	[ConfigureNotify] = configure,
	[Expose] = expose,
	[KeyPress] = kpress,
};

struct image *pngopen(char *filename)
{
	FILE *f;
	unsigned char buf[8];
	struct image *img;

	if (!(f = fopen(filename, "rb"))) {
		eprintf("could not open file %s:", filename);
		return NULL;
	}

	if (fread(buf, 1, 8, f) != 8 || png_sig_cmp(buf, 1, 8))
		return NULL;

	img = malloc(sizeof(struct image));
	if (!(img->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
					NULL, NULL))) {
		free(img);
		return NULL;
	}
	if (!(img->info_ptr = png_create_info_struct(img->png_ptr))) {
		png_destroy_read_struct(&img->png_ptr, NULL, NULL);
		free(img);
		return NULL;
	}
	if (setjmp(png_jmpbuf(img->png_ptr))) {
		png_destroy_read_struct(&img->png_ptr, &img->info_ptr, NULL);
		free(img);
		return NULL;
	}

	img->f = f;
	rewind(f);
	png_init_io(img->png_ptr, f);
	png_read_info(img->png_ptr, img->info_ptr);
	img->bufwidth = png_get_image_width(img->png_ptr, img->info_ptr);
	img->bufheight = png_get_image_height(img->png_ptr, img->info_ptr);

	return img;
}

int pngread(struct image *img)
{
	unsigned int y;
	png_bytepp row_pointers;

	if (!img)
		return 0;

	if (img->state & LOADED)
		return 2;

	if (img->buf)
		free(img->buf);
	if (!(img->buf = malloc(3 * img->bufwidth * img->bufheight)))
		return 0;

	if (setjmp(png_jmpbuf(img->png_ptr))) {
		png_destroy_read_struct(&img->png_ptr, &img->info_ptr, NULL);
		return 0;
	}

	{
		int color_type = png_get_color_type(img->png_ptr, img->info_ptr);
		int bit_depth = png_get_bit_depth(img->png_ptr, img->info_ptr);
		if (color_type == PNG_COLOR_TYPE_PALETTE)
			png_set_expand(img->png_ptr);
		if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
			png_set_expand(img->png_ptr);
		if (png_get_valid(img->png_ptr, img->info_ptr, PNG_INFO_tRNS))
			png_set_expand(img->png_ptr);
		if (bit_depth == 16)
			png_set_strip_16(img->png_ptr);
		if (color_type == PNG_COLOR_TYPE_GRAY
				|| color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
			png_set_gray_to_rgb(img->png_ptr);

		png_color_16 my_background = {.red = 0xff, .green = 0xff, .blue = 0xff};
		png_color_16p image_background;

		if (png_get_bKGD(img->png_ptr, img->info_ptr, &image_background))
			png_set_background(img->png_ptr, image_background, PNG_BACKGROUND_GAMMA_FILE, 1, 1.0);
		else
			png_set_background(img->png_ptr, &my_background, PNG_BACKGROUND_GAMMA_SCREEN, 2, 1.0);

		if (png_get_interlace_type(img->png_ptr, img->info_ptr) == PNG_INTERLACE_ADAM7)
			img->numpasses = png_set_interlace_handling(img->png_ptr);
		else
			img->numpasses = 1;
		png_read_update_info(img->png_ptr, img->info_ptr);
	}

	row_pointers = (png_bytepp)malloc(img->bufheight * sizeof(png_bytep));
	for (y = 0; y < img->bufheight; y++)
		row_pointers[y] = img->buf + y * img->bufwidth * 3;

	png_read_image(img->png_ptr, row_pointers);
	free(row_pointers);

	png_destroy_read_struct(&img->png_ptr, &img->info_ptr, NULL);
	fclose(img->f);
	img->state |= LOADED;

	return 1;
}

int pngprepare(struct image *img)
{
	int depth = DefaultDepth(xw.dpy, xw.scr);
	int width = xw.uw;
	int height = xw.uh;

	if (xw.uw * img->bufheight > xw.uh * img->bufwidth)
		width = img->bufwidth * xw.uh / img->bufheight;
	else
		height = img->bufheight * xw.uw / img->bufwidth;

	if (depth < 24) {
		eprintf("display depths <24 not supported.");
		return 0;
	}

	if (!(img->ximg = XCreateImage(xw.dpy, CopyFromParent, depth, ZPixmap, 0,
				NULL, width, height, 32, 0))) {
		eprintf("could not create XImage");
		return 0;
	}

	if (!(img->ximg->data = malloc(img->ximg->bytes_per_line * height))) {
		eprintf("could not alloc data section for XImage");
		XDestroyImage(img->ximg);
		img->ximg = NULL;
		return 0;
	}

	if (!XInitImage(img->ximg)) {
		eprintf("could not init XImage");
		free(img->ximg->data);
		XDestroyImage(img->ximg);
		img->ximg = NULL;
		return 0;
	}

	pngscale(img);
	img->state |= SCALED;
	return 1;
}

void pngscale(struct image *img)
{
	unsigned int x, y;
	unsigned int width = img->ximg->width;
	unsigned int height = img->ximg->height;
	char* __restrict__ newBuf = img->ximg->data;
	unsigned char * __restrict__ ibuf;
	unsigned int jdy = img->ximg->bytes_per_line / 4 - width;
	unsigned int dx = (img->bufwidth << 10) / width;

	for (y = 0; y < height; y++) {
		unsigned int bufx = img->bufwidth / width;
		ibuf = &img->buf[y * img->bufheight / height * img->bufwidth * 3];

		for (x = 0; x < width; x++) {
			*newBuf++ = (ibuf[(bufx >> 10)*3+2]);
			*newBuf++ = (ibuf[(bufx >> 10)*3+1]);
			*newBuf++ = (ibuf[(bufx >> 10)*3+0]);
			newBuf++;
			bufx += dx;
		}
		newBuf += jdy;
	}
}

void pngdraw(struct image *img)
{
	int xoffset = (xw.w - img->ximg->width) / 2;
	int yoffset = (xw.h - img->ximg->height) / 2;
	XPutImage(xw.dpy, xw.win, dc.gc, img->ximg, 0, 0,
			xoffset, yoffset, img->ximg->width, img->ximg->height);
	XFlush(xw.dpy);
	img->state |= DRAWN;
}

Bool xfontisscalable(char *name)
{
	int i, field;

	if (!name || name[0] != '-')
		return False;

	for (i = field = 0; name[i] != '\0'; i++) {
		if (name[i] == '-') {
			field++;
			if ((field == 7) || (field == 8) || (field == 12))
				if ((name[i+1] != '0') || (name[i+2] != '-'))
					return False;
		}
	}
	return field == 14;
}

XFontStruct *xloadqueryscalablefont(char *name, int size)
{
	int i, j, field;
	char newname[500];
	int resx, resy;

	if (!name || name[0] != '-')
		return NULL;
	/* calculate our screen resolution in dots per inch. 25.4mm = 1 inch */
	resx = DisplayWidth(xw.dpy, xw.scr)/(DisplayWidthMM(xw.dpy, xw.scr)/25.4);
	resy = DisplayHeight(xw.dpy, xw.scr)/(DisplayHeightMM(xw.dpy, xw.scr)/25.4);
	/* copy the font name, changing the scalable fields as we do so */
	for (i = j = field = 0; name[i] != '\0' && field <= 14; i++) {
		newname[j++] = name[i];
		if (name[i] == '-') {
			field++;
			switch (field) {
				case 7:  /* pixel size */
				case 12: /* average width */
					/* change from "-0-" to "-*-" */
					newname[j] = '*';
					j++;
					if (name[i+1] != '\0') i++;
					break;
				case 8:  /* point size */
					/* change from "-0-" to "-<size>-" */
					sprintf(&newname[j], "%d", size);
					while (newname[j] != '\0') j++;
					if (name[i+1] != '\0') i++;
					break;
				case 9:  /* x-resolution */
				case 10: /* y-resolution */
					/* change from an unspecified resolution to resx or resy */
					sprintf(&newname[j], "%d", (field == 9) ? resx : resy);
					while (newname[j] != '\0') j++;
					while ((name[i+1] != '-') && (name[i+1] != '\0')) i++;
					break;
			}
		}
	}
	newname[j] = '\0';
	return (field != 14) ? NULL : XLoadQueryFont(xw.dpy, newname);
}

struct DC *getfontsize(char *str, size_t len, int *width, int *height)
{
	XCharStruct info;
	int unused;
	struct DC *pre = &dc;
	struct DC *cur = &dc;

	do {
		XTextExtents(cur->font, str, len, &unused, &unused, &unused, &info);
		if (info.width > xw.uw || info.ascent + info.descent > xw.uh)
			break;
		pre = cur;
	} while ((cur = cur->next));

	XTextExtents(pre->font, "o", 1, &unused, &unused, &unused, &info);
	*height = info.ascent;
	*width = XTextWidth(pre->font, str, len);
	return pre;
}

void cleanup(struct DC *cur)
{
	XFreeFont(xw.dpy, cur->font);
	XFreeGC(xw.dpy, cur->gc);

	if (cur->next) {
		cleanup(cur->next);
		cur->next = NULL;
	}

	if (cur != &dc) {
		free(cur);
		return;
	}

	XDestroyWindow(xw.dpy, xw.win);
	XSync(xw.dpy, False);
	XCloseDisplay(xw.dpy);
	if (slides) {
		free(slides);
		slides = NULL;
	}
}

void eprintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] != '\0' && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}
	exit(EXIT_FAILURE);
}

void load(FILE *fp)
{
	static size_t size = 0;
	char buf[BUFSIZ], *p;
	size_t i;

	/* read each line from stdin and add it to the item list */
	for (i = slidecount; fgets(buf, sizeof(buf), fp); i++) {
		if ((i+1) * sizeof(*slides) >= size)
			if (!(slides = realloc(slides, (size += BUFSIZ))))
				eprintf("cannot realloc %u bytes:", size);
		if ((p = strchr(buf, '\n')))
			*p = '\0';
		if (!(slides[i].text = strdup(buf)))
			eprintf("cannot strdup %u bytes:", strlen(buf)+1);
		if (slides[i].text[0] == '@')
			slides[i].img = pngopen(slides[i].text + 1);
	}
	if (slides)
		slides[i].text = NULL;
	slidecount = i;
}

void advance(const Arg *arg)
{
	int new_idx = idx + arg->i;
	LIMIT(new_idx, 0, slidecount-1);
	if (new_idx != idx) {
		if (slides[idx].img)
			slides[idx].img->state &= ~(DRAWN | SCALED);
		idx = new_idx;
		xdraw();
		if (slidecount > idx + 1 && slides[idx + 1].img && !pngread(slides[idx + 1].img))
			eprintf("could not read image %s", slides[idx + 1].text + 1);
		if (0 < idx && slides[idx - 1].img && !pngread(slides[idx - 1].img))
			eprintf("could not read image %s", slides[idx - 1].text + 1);
	}
}

void quit(const Arg *arg)
{
	running = 0;
}

void resize(int width, int height)
{
	xw.w = width;
	xw.h = height;
	xw.uw = usablewidth * width;
	xw.uh = usableheight * height;
}

void run()
{
	XEvent ev;

	/* Waiting for window mapping */
	while (1) {
		XNextEvent(xw.dpy, &ev);
		if (ev.type == ConfigureNotify) {
			resize(ev.xconfigure.width, ev.xconfigure.height);
		} else if (ev.type == MapNotify) {
			break;
		}
	}

	while (running) {
		XNextEvent(xw.dpy, &ev);
		if (handler[ev.type])
			(handler[ev.type])(&ev);
	}
}

void usage()
{
	eprintf("sent " VERSION " (c) 2014 markus.teich@stusta.mhn.de\n" \
	"usage: sent [-f font] FILE1 [FILE2 ...]", argv0);
}

void xdraw()
{
	int line_len = strlen(slides[idx].text);
	int height;
	int width;
	struct DC *dc = getfontsize(slides[idx].text, line_len, &width, &height);
	struct image *im = slides[idx].img;

	XClearWindow(xw.dpy, xw.win);

	if (!im)
		XDrawString(xw.dpy, xw.win, dc->gc, (xw.w - width)/2, (xw.h + height)/2,
				slides[idx].text, line_len);
	else if (!(im->state & LOADED) && !pngread(im))
		eprintf("could not read image %s", slides[idx].text + 1);
	else if (!(im->state & SCALED) && !pngprepare(im))
		eprintf("could not prepare image %s for drawing", slides[idx].text + 1);
	else if (!(im->state & DRAWN))
		pngdraw(im);
}

void xhints()
{
	XClassHint class = {.res_name = "sent", .res_class = "presenter"};
	XWMHints wm = {.flags = InputHint, .input = True};
	XSizeHints *sizeh = NULL;

	if (!(sizeh = XAllocSizeHints()))
		eprintf("sent: Could not alloc size hints");

	sizeh->flags = PSize;
	sizeh->height = xw.h;
	sizeh->width = xw.w;

	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm, &class);
	XFree(sizeh);
}

void xinit()
{
	XTextProperty prop;

	if (!(xw.dpy = XOpenDisplay(NULL)))
		eprintf("Can't open display.");
	xw.scr = XDefaultScreen(xw.dpy);
	xw.vis = XDefaultVisual(xw.dpy, xw.scr);
	xw.w = DisplayWidth(xw.dpy, xw.scr);
	xw.h = DisplayHeight(xw.dpy, xw.scr);

	xw.attrs.background_pixel = WhitePixel(xw.dpy, xw.scr);
	xw.attrs.bit_gravity = CenterGravity;
	xw.attrs.event_mask = KeyPressMask | ExposureMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask;

	xw.win = XCreateWindow(xw.dpy, XRootWindow(xw.dpy, xw.scr), 0, 0,
			xw.w, xw.h, 0, XDefaultDepth(xw.dpy, xw.scr), InputOutput, xw.vis,
			CWBackPixel | CWBitGravity | CWEventMask, &xw.attrs);

	xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
	xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
	XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

	xloadfonts(opt_font ? opt_font : font);

	XStringListToTextProperty(&argv0, 1, &prop);
	XSetWMName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmname);
	XFree(prop.value);
	XMapWindow(xw.dpy, xw.win);
	xhints();
	XSync(xw.dpy, False);
}

void xloadfonts(char *fontstr)
{
	int count = 0;
	int i = 0;
	XFontStruct *fnt;
	XGCValues gcvalues;
	struct DC *cur = &dc;
	char **fstr = XListFonts(xw.dpy, fontstr, 42, &count);

	while (count-- && !xfontisscalable(fstr[count]))
		; /* nothing, just get first scalable font result */

	if (count < 0)
		eprintf("sent: could not find a scalable font matching %s", fontstr);

	memset(&gcvalues, 0, sizeof(gcvalues));

	do {
		if (!(fnt = xloadqueryscalablefont(fstr[count], FONTSZ(i)))) {
			i++;
			continue;
		}

		cur->gc = XCreateGC(xw.dpy, XRootWindow(xw.dpy, xw.scr), 0, &gcvalues);
		cur->font = fnt;
		XSetFont(xw.dpy, cur->gc, fnt->fid);
		XSetForeground(xw.dpy, cur->gc, BlackPixel(xw.dpy, xw.scr));
		cur->next = (++i < NUMFONTS) ? malloc(sizeof(struct DC)) : NULL;
		cur = cur->next;
	} while (cur && i < NUMFONTS);

	if (cur == &dc)
		eprintf("sent: could not load fonts.");

	XFreeFontNames(fstr);
}

void bpress(XEvent *e)
{
	unsigned int i;

	for (i = 0; i < LEN(mshortcuts); i++)
		if (e->xbutton.button == mshortcuts[i].b && mshortcuts[i].func)
			mshortcuts[i].func(&(mshortcuts[i].arg));
}

void cmessage(XEvent *e)
{
	if (e->xclient.data.l[0] == xw.wmdeletewin)
		running = 0;
}

void expose(XEvent *e)
{
	if (0 == e->xexpose.count)
		xdraw();
}

void kpress(XEvent *e)
{
	unsigned int i;
	KeySym sym;

	sym = XkbKeycodeToKeysym(xw.dpy, (KeyCode)e->xkey.keycode, 0, 0);
	for (i = 0; i < LEN(shortcuts); i++)
		if (sym == shortcuts[i].keysym && shortcuts[i].func)
			shortcuts[i].func(&(shortcuts[i].arg));
}

void configure(XEvent *e)
{
	resize(e->xconfigure.width, e->xconfigure.height);
	if (slides[idx].img)
		slides[idx].img->state &= ~(DRAWN | SCALED);
	xdraw();
}

int main(int argc, char *argv[])
{
	int i;
	FILE *fp = NULL;

	ARGBEGIN {
	case 'f':
		opt_font = EARGF(usage());
		break;
	case 'v':
	default:
		usage();
	} ARGEND;

	for (i = 0; i < argc; i++) {
		if ((fp = strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin)) {
			load(fp);
			fclose(fp);
		} else {
			eprintf("could not open file %s for reading:", argv[i]);
		}
	}

	if (!slides || !slides[0].text)
		usage();

	xinit();
	run();

	cleanup(&dc);
	return EXIT_SUCCESS;
}