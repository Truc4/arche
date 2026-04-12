#include <unistd.h>

/* C equivalent for hello_world.arche */

int main() {
	const char msg[] = "Hello, World!\n";
	write(1, msg, 14);
	return 0;
}
