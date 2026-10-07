#include <stdint.h>
#include <stddef.h>

#if !defined(CORPUS_VARIANT)
#define CORPUS_VARIANT 0
#endif

__attribute__((noinline)) uint32_t corpus_shared_mix(uint32_t value)
{
	value ^= value << 7;
	value += 0x13579bdu;
	return value ^ (value >> 11);
}

__attribute__((noinline)) uint32_t corpus_versioned(uint32_t value)
{
#if CORPUS_VARIANT == 0
	return corpus_shared_mix(value) + 7;
#else
	return corpus_shared_mix(value) + 11;
#endif
}

#if CORPUS_VARIANT != 3
__attribute__((noinline)) uint32_t corpus_removable(uint32_t value)
{
	return (value << 3) ^ 0x2468aceu;
}
#endif

#if CORPUS_VARIANT == 4
__attribute__((noinline)) uint32_t corpus_renamed(uint32_t value)
#else
__attribute__((noinline)) uint32_t corpus_stable_named(uint32_t value)
#endif
{
	return (value * 17u) ^ (value >> 5);
}

__attribute__((noinline)) uint32_t corpus_patch_target(
	uint8_t* destination, size_t capacity, uint32_t length)
{
#if CORPUS_VARIANT == 5
	for (uint32_t index = 0; index < length; index++)
		destination[index] = (uint8_t)(index ^ 0x5au);
	return length;
#elif CORPUS_VARIANT == 6
	uint32_t bounded = length > capacity ? (uint32_t)capacity : length;
	for (uint32_t index = 0; index < bounded; index++)
		destination[index] = (uint8_t)(index ^ 0x5au);
	return bounded;
#elif CORPUS_VARIANT == 7
	uint32_t bounded = length > capacity ? (uint32_t)capacity : length;
	for (uint32_t index = 0; index < bounded; index++)
		destination[index] = (uint8_t)((index * 3u) ^ 0xa5u);
	return bounded ^ 0x55u;
#elif CORPUS_VARIANT == 8
	(void)destination;
	return corpus_shared_mix(length ^ (uint32_t)capacity);
#else
	uint32_t bounded = length > capacity ? (uint32_t)capacity : length;
	for (uint32_t index = 0; index < bounded; index++)
		destination[index] = (uint8_t)(index + 1u);
	return bounded;
#endif
}

#if CORPUS_VARIANT == 2
__attribute__((noinline)) uint32_t corpus_added(uint32_t value)
{
	return (value * 33u) ^ 0xa5a5a5a5u;
}
#endif

#if CORPUS_VARIANT == 9
__attribute__((noinline)) uint32_t corpus_larger_component(uint32_t value)
{
	value = corpus_shared_mix(value);
	value ^= value * 0x9e3779b9u;
	return value + (value >> 13);
}
#endif

int main(int argc, char** argv)
{
	uint8_t destination[32] = {0};
	uint32_t value = corpus_versioned((uint32_t)argc + (argv != 0));
#if CORPUS_VARIANT != 3
	value ^= corpus_removable(value);
#endif
#if CORPUS_VARIANT == 4
	value ^= corpus_renamed(value);
#else
	value ^= corpus_stable_named(value);
#endif
	value ^= corpus_patch_target(destination, sizeof(destination), value & 63u);
#if CORPUS_VARIANT == 2
	value ^= corpus_added(value);
#endif
#if CORPUS_VARIANT == 9
	value ^= corpus_larger_component(value);
#endif
	return (int)(value & 0xffu);
}
