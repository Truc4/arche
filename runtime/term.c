#define _POSIX_C_SOURCE 200112L
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Terminal raw-mode input + frame timing for interactive arche programs.
 * Plain C behind the usual extern pattern (no handles): raw mode disables line
 * buffering and echo and makes reads non-blocking (VMIN=0, VTIME=0), so a game
 * loop can poll a key each frame without stalling. */

static struct termios g_saved;
static int g_raw = 0;

void term_raw_enable(void) {
	if (tcgetattr(STDIN_FILENO, &g_saved) != 0)
		return; /* not a tty (e.g. piped) — leave as-is */
	struct termios raw = g_saved;
	raw.c_lflag &= (unsigned)~(ICANON | ECHO);
	raw.c_cc[VMIN] = 0; /* non-blocking: read returns immediately */
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	g_raw = 1;
}

void term_raw_restore(void) {
	if (g_raw) {
		tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
		g_raw = 0;
	}
}

/* Next buffered input byte, or 0 if none is available right now. */
int term_read_key(void) {
	unsigned char c;
	int n = (int)read(STDIN_FILENO, &c, 1);
	return (n == 1) ? (int)c : 0;
}
