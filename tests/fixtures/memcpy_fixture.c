#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Keep the rejection text behind a data pointer. The extraction regression
// checks that function-to-string evidence follows this indirection instead of
// depending on a renderer annotation at the call site.
static const char* volatile rejection_message = "copy length rejected";

// Give the symbol-transfer test an unchanged local function. The changed copy
// wrapper is compared separately using the fixture's known symbol addresses.
static volatile unsigned symbol_port_observation;

__attribute__((noinline)) static unsigned symbol_port_fixture(unsigned value)
{
	return (value ^ 0x5aU) * 3U + 7U;
}

// This deliberately remains a local symbol so the stripped build presents it
// to Binary Ninja as sub_<address>, while the reference build retains the
// source-level name used to locate the changed-function comparison.
__attribute__((noinline)) static int copy_record(
    char* destination, size_t destination_size, const char* source, size_t length)
{
#ifndef MEMCPY_BUG
	if (length > destination_size)
	{
		fputs(rejection_message, stderr);
		return -1;
	}
#endif
	memcpy(destination, source, length);
	// Keep this as a real wrapper in optimized builds. Without an observable
	// post-copy operation, the vulnerable variant collapses to a j__memcpy
	// tail-call thunk and no longer provides a useful symbol-port fixture.
	(void)*(volatile unsigned char*)destination;
	return 0;
}

int main(int argc, char** argv)
{
	char destination[8] = {0};
	if (argc != 2)
		return 2;
	symbol_port_observation = symbol_port_fixture((unsigned char)argv[1][0]);
	return copy_record(destination, sizeof(destination), argv[1], strlen(argv[1]));
}
