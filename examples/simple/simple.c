#include <string.h>
#include <unistd.h>

/* C equivalent for simple.arche */

int main() {
	int x = 10;
	int y = 20;
	int result = x + y;
	const char msg[] = "result: 30\n";
	write(1, msg, strlen(msg));
	return 0;
}
