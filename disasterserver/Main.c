#include <Lib.h>
#include <Console.h>
#include <io/Threads.h>

int main(void)
{
	if (!disaster_init())
		return 1;

	console_start();

	return disaster_run();
}
