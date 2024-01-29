/* See LICENSE file for copyright and license details. */

static char *fontfallbacks[] = {
	"dejavu sans",
	"roboto",
	"ubuntu",
};
#define NUMFONTSCALES 42
#define FONTSZ(x) ((int)(10.0 * powf(1.1288, (x)))) /* x in [0, NUMFONTSCALES-1] */

static const char *colors[] = {
	"#000000", /* foreground color */
	"#5f2529", /* background color */
};

static const float linespacing = 1.4;

/* how much screen estate is to be used at max for the content */
static const float usablewidth = 1;
static const float usableheight = 1;

/* Automatic advance interval in seconds */
static const unsigned int autoadvanceinterval = 10;

static Mousekey mshortcuts[] = {
	/* button         function        argument */
	{ Button1,        advance,        {.i = 0} },
};

static Shortcut shortcuts[] = {
	/* keysym         function        argument */
	{ XK_Escape,      quit,           {0} },
	{ XK_q,           quit,           {0} },
	{ XK_Right,       advance,        {.i = +1} },
	{ XK_Left,        advance,        {.i = -1} },
	{ XK_Return,      advance,        {.i = +1} },
	{ XK_space,       advance,        {.i = +1} },
	{ XK_BackSpace,   advance,        {.i = -1} },
	{ XK_l,           advance,        {.i = +1} },
	{ XK_h,           advance,        {.i = -1} },
	{ XK_j,           advance,        {.i = +1} },
	{ XK_k,           advance,        {.i = -1} },
	{ XK_Down,        advance,        {.i = +1} },
	{ XK_Up,          advance,        {.i = -1} },
	{ XK_Next,        advance,        {.i = +1} },
	{ XK_Prior,       advance,        {.i = -1} },
	{ XK_n,           advance,        {.i = +1} },
	{ XK_p,           advance,        {.i = -1} },
	{ XK_r,           reload,         {0} },
};

static Filter filters[] = {
	{ "\\.ff$", "cat" },
	{ "\\.ff.bz2$", "bunzip2" },
	{ "\\.[a-z0-9]+$", "2ff" },
};
