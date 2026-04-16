#include <string.h>
#include <unistd.h>

/* C equivalent for hello_world.arche */

int main() {
	const char msg[] = "Hello, World!\n";
	write(1, msg, strlen(msg));
	return 0;
}
