#include <string.h>
#include <unistd.h>

/* C equivalent for with_params.arche */

void add(int a, int b) {
	int result = a + b;
	const char msg[] = "sum: 15\n";
	write(1, msg, strlen(msg));
}

int main() {
	add(5, 10);
	return 0;
}
